/* =========================================================================
 * APS-2 - Controle para o flight simulator GeoFS
 * Raspberry Pi Pico 2 (RP2350) + FreeRTOS
 *
 * Arquitetura RTOS (sem variaveis globais de dados - a comunicacao entre
 * tasks/ISR e feita SOMENTE por filas e semaforos):
 *
 *   ISR GPIO (botoes) --q_btn--> task_buttons --+
 *   task_analog (joystick X/Y, potenciometro) --+--q_input--> task_comm --USB--> PC
 *                                                |
 *   task_comm --q_status--> task_status_led (LED de conexao)
 *
 * Protocolo device -> PC (4 bytes, sync no inicio):
 *   [0xFF][TYPE][VAL_LO][VAL_HI]     (VAL = int16 little-endian)
 * Protocolo PC -> device (1 byte): 0xA0 heartbeat, 0xA1 crash, 0xA2 stall.
 * ========================================================================= */

#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <task.h>

#include <stdio.h>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

#include "gesture.h"
#include "pins.h"
#include "ssd1306.h"

/* Build de coleta de dados p/ o edge-impulse data-forwarder (default: off) */
#ifndef EI_DATA_FORWARDER
#define EI_DATA_FORWARDER 0
#endif

/* ---------------- Protocolo ---------------- */
#define PKT_SYNC 0xFF
#define TYPE_AXIS_X 0
#define TYPE_AXIS_Y 1
#define TYPE_THROTTLE 2
#define TYPE_BUTTON 3
#define TYPE_GESTURE 4
#define TYPE_BATTERY 5 /* interno: nao enviado ao PC */
#define TYPE_HEARTBEAT 6

#define CMD_HEARTBEAT 0xA0
#define CMD_CRASH 0xA1
#define CMD_STALL 0xA2

/* Estados de conexao (para o LED de status) */
#define CONN_DISCONNECTED 0
#define CONN_CONNECTED 1
#define CONN_CALIBRATING 2

/* Janela morta do joystick (em contagens de ADC, centro ~2048) */
#define JOY_CENTER 2048
#define JOY_DEADZONE 200

/* Timeout do heartbeat do PC: sem heartbeat por mais que isso = desconectado */
#define HEARTBEAT_TIMEOUT_MS 1500

/* HC-SR04: janela de amostras p/ o classificador e alcance maximo (cm).
 * GESTURE_WINDOW deve casar com EI_CLASSIFIER_RAW_SAMPLE_COUNT do modelo. */
#define GESTURE_WINDOW 50
#define HCSR04_MAX_CM 200.0f

/* Eventos de feedback (q_event -> task_feedback) */
#define EV_BTN_CLICK 1
#define EV_THROTTLE_MAX 2
#define EV_LOW_BATT 3
#define EV_CRASH 4
#define EV_STALL 5

/* Campos do display (q_disp -> task_display) */
#define DISP_CONN 0
#define DISP_THROTTLE 1
#define DISP_GESTURE 2
#define DISP_BATT 3
#define DISP_PROFILE 4

/* Perfis multiusuario: presets de sensibilidade do joystick. */
typedef struct {
    const char *name;
    int scale; /* % aplicado a deflexao */
} profile_t;
static const profile_t PROFILES[] = {
    { "Suave", 60 }, { "Normal", 100 }, { "Sport", 150 }
};
#define NUM_PROFILES 3
#define DEFAULT_PROFILE 1

/* Macro: gravacao/reproducao de sequencia de comandos no botao do joystick. */
#define MACRO_MAX 32
#define LONG_PRESS_MS 700
typedef struct {
    uint8_t id;
    uint16_t dt_ms;
} macro_step_t;

/* Bateria: lida via VSYS/ADC3 (divisor interno da placa ~3.0). LiPo 1S. */
#define VBAT_DIVIDER 3.0f
#define VBAT_MIN_V 3.0f
#define VBAT_MAX_V 4.2f
#define VBAT_LOW_PCT 15

/* ---------------- Mensagens das filas ---------------- */
typedef struct {
    uint8_t type;
    int16_t value;
} input_msg_t;

typedef struct {
    uint8_t field;
    int16_t value;
} disp_msg_t;

/* ---------------- Handles RTOS (file-scope p/ acesso da ISR) ---------------- */
static QueueHandle_t q_input;  /* input_msg_t : sensores  -> task_comm        */
static QueueHandle_t q_btn;    /* uint8_t     : ISR botoes -> task_buttons     */
static QueueHandle_t q_status; /* uint8_t     : task_comm  -> task_status_led  */
static QueueHandle_t q_echo;   /* uint32_t    : ISR echo HC-SR04 -> task_gesture */
static QueueHandle_t q_event;  /* uint8_t     : eventos -> task_feedback        */
static QueueHandle_t q_disp;   /* disp_msg_t  : -> task_display                 */
static QueueHandle_t q_profile; /* uint8_t (overwrite) : perfil ativo (botoes->analog) */
static SemaphoreHandle_t mtx_i2c; /* protege o barramento I2C (OLED + IMU)     */

/* =========================================================================
 * ISR - callback unico de GPIO (dispatch por pino).
 * Botoes ativos em baixo: borda de descida = apertou.
 * ========================================================================= */
static void gpio_callback(uint gpio, uint32_t events)
{
    /* HC-SR04: mede a largura do pulso de echo (rise -> fall). */
    if (gpio == ECHO_PIN) {
        static uint64_t t_rise = 0;
        if (events & GPIO_IRQ_EDGE_RISE) {
            t_rise = time_us_64();
        } else if (events & GPIO_IRQ_EDGE_FALL) {
            uint32_t dur = (uint32_t)(time_us_64() - t_rise);
            BaseType_t woken = pdFALSE;
            xQueueSendFromISR(q_echo, &dur, &woken);
            portYIELD_FROM_ISR(woken);
        }
        return;
    }

    /* Botoes (ativos em baixo): FALL = apertou, RISE = soltou.
     * RISE so esta habilitado no botao do joystick (deteccao de long-press);
     * o bit 0x80 marca "soltou". */
    uint8_t base = (uint8_t)gpio;
    BaseType_t woken = pdFALSE;
    if (events & GPIO_IRQ_EDGE_FALL) {
        uint8_t c = base;
        xQueueSendFromISR(q_btn, &c, &woken);
    }
    if (events & GPIO_IRQ_EDGE_RISE) {
        uint8_t c = base | 0x80u;
        xQueueSendFromISR(q_btn, &c, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

/* Envia um datagrama de 4 bytes pela serial USB (sem traducao de texto). */
static void send_packet(uint8_t type, int16_t value)
{
    putchar_raw(PKT_SYNC);
    putchar_raw(type);
    putchar_raw((uint8_t)(value & 0xFF));
    putchar_raw((uint8_t)((value >> 8) & 0xFF));
}

/* Janela morta em torno do centro calibrado + sensibilidade do perfil. */
static int16_t process_axis(int raw, int center, int scale_pct)
{
    int delta = raw - center;
    if (delta > -JOY_DEADZONE && delta < JOY_DEADZONE) {
        return 0;
    }
    long v = (long)delta * scale_pct / 100;
    if (v > 32767) {
        v = 32767;
    } else if (v < -32768) {
        v = -32768;
    }
    return (int16_t)v;
}

static int btn_pin_to_id(uint8_t pin)
{
    switch (pin) {
    case BTN_GEAR_PIN:  return BTN_ID_GEAR;
    case BTN_FLAPS_PIN: return BTN_ID_FLAPS;
    case BTN_BRAKE_PIN: return BTN_ID_BRAKE;
    case BTN_VIEW_PIN:  return BTN_ID_VIEW;
    case BTN_JOY_PIN:   return BTN_ID_JOY;
    default:            return -1;
    }
}

static void status_led_set(uint8_t state)
{
    int r = 0, g = 0, b = 0;
    switch (state) {
    case CONN_CONNECTED:    g = 1; break; /* verde   = conectado    */
    case CONN_CALIBRATING:  b = 1; break; /* azul    = calibrando   */
    case CONN_DISCONNECTED:
    default:                r = 1; break; /* vermelho = desconectado */
    }
#if !LED_ACTIVE_HIGH
    r = !r; g = !g; b = !b;
#endif
    gpio_put(LED_R_PIN, r);
    gpio_put(LED_G_PIN, g);
    gpio_put(LED_B_PIN, b);
}

/* Toca um tom no buzzer (PWM) por 'ms' milissegundos e desliga. */
static void buzzer_tone(uint freq_hz, uint ms)
{
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    if (freq_hz == 0) {
        pwm_set_enabled(slice, false);
        return;
    }
    /* f = clk_sys / (clkdiv * (wrap+1)); clkdiv fixo = 64. */
    uint32_t wrap = clock_get_hz(clk_sys) / (64u * freq_hz);
    if (wrap > 65535u) {
        wrap = 65535u;
    }
    pwm_set_wrap(slice, (uint16_t)wrap);
    pwm_set_gpio_level(BUZZER_PIN, (uint16_t)(wrap / 2)); /* 50% duty */
    pwm_set_enabled(slice, true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    pwm_set_enabled(slice, false);
}

/* =========================================================================
 * Tasks
 * ========================================================================= */

/* Le joystick (X/Y) e potenciometro (throttle); publica em q_input. */
static void task_analog(void *p)
{
    (void)p;

    adc_init();
    adc_gpio_init(ADC_JOY_X_PIN);
    adc_gpio_init(ADC_JOY_Y_PIN);
    adc_gpio_init(ADC_POT_PIN);
    adc_gpio_init(ADC_BAT_PIN);

    /* --- Wizard de calibracao: assume o joystick centrado no boot ---
     * LED azul (CALIBRATING) enquanto amostra; mede o centro real de X/Y. */
    uint8_t calib = CONN_CALIBRATING;
    xQueueOverwrite(q_status, &calib);
    int32_t sx = 0, sy = 0;
    const int NCAL = 64;
    for (int i = 0; i < NCAL; i++) {
        adc_select_input(ADC_JOY_X_CH);
        sx += adc_read();
        adc_select_input(ADC_JOY_Y_CH);
        sy += adc_read();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    int center_x = (int)(sx / NCAL);
    int center_y = (int)(sy / NCAL);
    uint8_t done = CONN_DISCONNECTED;
    xQueueOverwrite(q_status, &done);

    int scale = PROFILES[DEFAULT_PROFILE].scale;
    uint8_t tick = 0;
    uint16_t bat_tick = 0;
    bool throttle_was_max = false;

    for (;;) {
        input_msg_t m;

        /* Perfil ativo (atualizado pelo botao do joystick) */
        uint8_t pidx;
        if (xQueuePeek(q_profile, &pidx, 0) == pdTRUE && pidx < NUM_PROFILES) {
            scale = PROFILES[pidx].scale;
        }

        /* Eixo X (roll) */
        adc_select_input(ADC_JOY_X_CH);
        m.type = TYPE_AXIS_X;
        m.value = process_axis(adc_read(), center_x, scale);
        xQueueSend(q_input, &m, 0);

        /* Eixo Y (pitch) */
        adc_select_input(ADC_JOY_Y_CH);
        m.type = TYPE_AXIS_Y;
        m.value = process_axis(adc_read(), center_y, scale);
        xQueueSend(q_input, &m, 0);

        /* Throttle a ~10 Hz (a cada 5 ciclos), valor absoluto 0..1000 */
        if (++tick >= 5) {
            tick = 0;
            adc_select_input(ADC_POT_CH);
            int16_t thr = (int16_t)((adc_read() * 1000) / 4095);
            m.type = TYPE_THROTTLE;
            m.value = thr;
            xQueueSend(q_input, &m, 0);

            /* Haptics local: avisa ao cruzar para o maximo. */
            bool now_max = (thr >= 980);
            if (now_max && !throttle_was_max) {
                uint8_t ev = EV_THROTTLE_MAX;
                xQueueSend(q_event, &ev, 0);
            }
            throttle_was_max = now_max;
        }

        /* Bateria (~a cada 1 s): VSYS via ADC3 -> percentual. */
        if (++bat_tick >= 50) {
            bat_tick = 0;
            adc_select_input(ADC_BAT_CH);
            float v = (adc_read() * 3.3f / 4095.0f) * VBAT_DIVIDER;
            int pct = (int)((v - VBAT_MIN_V) / (VBAT_MAX_V - VBAT_MIN_V) * 100.0f);
            if (pct < 0) {
                pct = 0;
            } else if (pct > 100) {
                pct = 100;
            }
            m.type = TYPE_BATTERY;
            m.value = (int16_t)pct;
            xQueueSend(q_input, &m, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(20)); /* ~50 Hz */
    }
}

/* Trata os botoes (vindos da ISR):
 *  - GEAR/FLAPS/BRAKE -> comando de voo (q_input -> PC)
 *  - VIEW            -> cicla o perfil de sensibilidade (local)
 *  - JOY (clique)    -> macro: clique curto reproduz, clique longo grava/para
 */
static void task_buttons(void *p)
{
    (void)p;

    TickType_t last[30] = { 0 }; /* debounce por pino (na borda de press) */
    uint8_t profile_idx = DEFAULT_PROFILE;

    macro_step_t macro[MACRO_MAX];
    int macro_len = 0;
    bool recording = false;
    TickType_t rec_last = 0;
    TickType_t joy_press = 0;

    for (;;) {
        uint8_t raw;
        if (xQueueReceive(q_btn, &raw, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        TickType_t now = xTaskGetTickCount();
        bool release = (raw & 0x80u) != 0;
        uint8_t pin = raw & 0x7Fu;

        /* ---- Botao do joystick = macro ---- */
        if (pin == BTN_JOY_PIN) {
            if (!release) {
                joy_press = now;
            } else if ((now - joy_press) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
                /* clique longo: liga/desliga gravacao */
                recording = !recording;
                uint8_t ev = recording ? EV_BTN_CLICK : EV_THROTTLE_MAX;
                if (recording) {
                    macro_len = 0;
                    rec_last = now;
                }
                xQueueSend(q_event, &ev, 0);
            } else if (!recording && macro_len > 0) {
                /* clique curto: reproduz a macro gravada */
                for (int i = 0; i < macro_len; i++) {
                    uint16_t dt = macro[i].dt_ms > 2000 ? 2000 : macro[i].dt_ms;
                    vTaskDelay(pdMS_TO_TICKS(dt));
                    input_msg_t mm = { .type = TYPE_BUTTON,
                                       .value = (int16_t)macro[i].id };
                    xQueueSend(q_input, &mm, 0);
                }
            }
            continue;
        }

        /* Demais botoes: somente borda de press, com debounce. */
        if (release) {
            continue;
        }
        if (pin < 30 && (now - last[pin]) < pdMS_TO_TICKS(200)) {
            continue;
        }
        if (pin < 30) {
            last[pin] = now;
        }

        /* ---- VIEW = cicla perfil (nao vai ao PC) ---- */
        if (pin == BTN_VIEW_PIN) {
            profile_idx = (uint8_t)((profile_idx + 1) % NUM_PROFILES);
            xQueueOverwrite(q_profile, &profile_idx);
            disp_msg_t d = { .field = DISP_PROFILE, .value = profile_idx };
            xQueueSend(q_disp, &d, 0);
            uint8_t ev = EV_BTN_CLICK;
            xQueueSend(q_event, &ev, 0);
            continue;
        }

        /* ---- Botoes de voo -> PC (e gravados se em modo macro) ---- */
        int id = btn_pin_to_id(pin);
        if (id >= 0) {
            input_msg_t m = { .type = TYPE_BUTTON, .value = (int16_t)id };
            xQueueSend(q_input, &m, 0);
            if (recording && macro_len < MACRO_MAX) {
                uint32_t dt = (uint32_t)((now - rec_last) * portTICK_PERIOD_MS);
                macro[macro_len].id = (uint8_t)id;
                macro[macro_len].dt_ms = (uint16_t)(dt > 65535 ? 65535 : dt);
                macro_len++;
                rec_last = now;
            }
        }
    }
}

/* Mede o HC-SR04, monta a janela e roda o classificador (Edge Impulse).
 * Rodara no core 1 (afinidade) - lab RTOS expert. */
static void task_gesture(void *p)
{
    (void)p;

    gpio_init(TRIG_PIN);
    gpio_set_dir(TRIG_PIN, GPIO_OUT);
    gpio_put(TRIG_PIN, 0);

    gpio_init(ECHO_PIN);
    gpio_set_dir(ECHO_PIN, GPIO_IN);
    gpio_set_irq_enabled(ECHO_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);

    gesture_init();

    float window[GESTURE_WINDOW];
    size_t idx = 0;
    int last_gesture = GEST_IDLE;

    for (;;) {
        /* Dispara o pulso de trigger (10 us). */
        gpio_put(TRIG_PIN, 1);
        sleep_us(10);
        gpio_put(TRIG_PIN, 0);

        /* Espera a largura do echo (medida na ISR). */
        uint32_t dur;
        float dist = HCSR04_MAX_CM;
        if (xQueueReceive(q_echo, &dur, pdMS_TO_TICKS(30)) == pdTRUE) {
            dist = dur / 58.0f; /* us -> cm */
            if (dist > HCSR04_MAX_CM) {
                dist = HCSR04_MAX_CM;
            }
        }

#if EI_DATA_FORWARDER
        /* Modo coleta: imprime amostras p/ o edge-impulse data-forwarder. */
        printf("%.1f\n", dist);
#else
        window[idx++] = dist;
        if (idx >= GESTURE_WINDOW) {
            idx = 0;
            char label[16];
            int g = gesture_classify(window, GESTURE_WINDOW, label, sizeof(label));
            if (g >= 0 && g != GEST_IDLE && g != last_gesture) {
                last_gesture = g;
                input_msg_t m = { .type = TYPE_GESTURE, .value = (int16_t)g };
                xQueueSend(q_input, &m, 0);
            } else if (g == GEST_IDLE) {
                last_gesture = GEST_IDLE;
            }
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(20)); /* ~50 Hz */
    }
}

/* Consome q_input -> serial USB; le bytes do PC e gerencia estado de conexao. */
static void task_comm(void *p)
{
    (void)p;

    TickType_t last_hb = 0;
    uint8_t conn = CONN_DISCONNECTED;
    status_led_set(conn);
    xQueueOverwrite(q_status, &conn);

    for (;;) {
        /* 1) Consome a fila de entrada: envia ao PC e alimenta display/feedback */
        input_msg_t m;
        if (xQueueReceive(q_input, &m, pdMS_TO_TICKS(10)) == pdTRUE) {
            disp_msg_t d;
            switch (m.type) {
            case TYPE_THROTTLE:
                send_packet(m.type, m.value);
                d.field = DISP_THROTTLE;
                d.value = m.value / 10; /* 0..100 % */
                xQueueSend(q_disp, &d, 0);
                break;
            case TYPE_GESTURE:
                send_packet(m.type, m.value);
                d.field = DISP_GESTURE;
                d.value = m.value;
                xQueueSend(q_disp, &d, 0);
                break;
            case TYPE_BUTTON: {
                send_packet(m.type, m.value);
                uint8_t ev = EV_BTN_CLICK;
                xQueueSend(q_event, &ev, 0);
                break;
            }
            case TYPE_BATTERY: { /* interno: NAO enviar ao PC */
                d.field = DISP_BATT;
                d.value = m.value;
                xQueueSend(q_disp, &d, 0);
                if (m.value <= VBAT_LOW_PCT) {
                    uint8_t ev = EV_LOW_BATT;
                    xQueueSend(q_event, &ev, 0);
                }
                break;
            }
            default: /* eixos */
                send_packet(m.type, m.value);
                break;
            }
        }

        /* 2) Le comandos do PC (nao-bloqueante) */
        int c = getchar_timeout_us(0);
        while (c != PICO_ERROR_TIMEOUT) {
            if ((uint8_t)c == CMD_HEARTBEAT) {
                last_hb = xTaskGetTickCount();
            } else if ((uint8_t)c == CMD_CRASH) {
                uint8_t ev = EV_CRASH;
                xQueueSend(q_event, &ev, 0);
            } else if ((uint8_t)c == CMD_STALL) {
                uint8_t ev = EV_STALL;
                xQueueSend(q_event, &ev, 0);
            }
            c = getchar_timeout_us(0);
        }

        /* 3) Atualiza estado de conexao pelo heartbeat */
        uint8_t now_conn =
            ((xTaskGetTickCount() - last_hb) < pdMS_TO_TICKS(HEARTBEAT_TIMEOUT_MS))
                ? CONN_CONNECTED
                : CONN_DISCONNECTED;
        if (now_conn != conn) {
            conn = now_conn;
            xQueueOverwrite(q_status, &conn);
            disp_msg_t d = { .field = DISP_CONN, .value = conn };
            xQueueSend(q_disp, &d, 0);
        }
    }
}

/* Aciona buzzer + motor de vibracao em eventos locais e do jogo (haptics). */
static void task_feedback(void *p)
{
    (void)p;

    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    pwm_set_clkdiv(pwm_gpio_to_slice_num(BUZZER_PIN), 64.0f);
    pwm_set_enabled(pwm_gpio_to_slice_num(BUZZER_PIN), false);

    gpio_init(VIBRA_PIN);
    gpio_set_dir(VIBRA_PIN, GPIO_OUT);
    gpio_put(VIBRA_PIN, 0);

    for (;;) {
        uint8_t ev;
        if (xQueueReceive(q_event, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (ev) {
        case EV_BTN_CLICK:
            buzzer_tone(2000, 25);
            break;
        case EV_THROTTLE_MAX:
            buzzer_tone(2600, 60);
            break;
        case EV_LOW_BATT:
            buzzer_tone(700, 200);
            break;
        case EV_CRASH:
            gpio_put(VIBRA_PIN, 1);
            buzzer_tone(250, 450);
            gpio_put(VIBRA_PIN, 0);
            break;
        case EV_STALL:
            for (int i = 0; i < 3; i++) {
                gpio_put(VIBRA_PIN, 1);
                buzzer_tone(1200, 80);
                gpio_put(VIBRA_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(70));
            }
            break;
        default:
            break;
        }
    }
}

/* Nome curto do gesto p/ exibir no OLED. */
static const char *gesture_name(int id)
{
    switch (id) {
    case GEST_SWIPE_UP:   return "swipe up";
    case GEST_SWIPE_DOWN: return "swipe down";
    case GEST_HOVER:      return "hover";
    default:              return "idle";
    }
}

/* OLED SSD1306: dashboard do controle (conexao, throttle, gesto, bateria). */
static void task_display(void *p)
{
    (void)p;

    i2c_init(i2c0, 400 * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    ssd1306_t disp;
    disp.external_vcc = false;
    ssd1306_init(&disp, 128, 64, 0x3C, i2c0);

    uint8_t conn = CONN_DISCONNECTED;
    int throttle = 0;
    int batt = 0;
    int gesture = GEST_IDLE;
    int profile = DEFAULT_PROFILE;
    char buf[24];

    for (;;) {
        disp_msg_t d;
        if (xQueueReceive(q_disp, &d, pdMS_TO_TICKS(500)) == pdTRUE) {
            switch (d.field) {
            case DISP_CONN:     conn = (uint8_t)d.value; break;
            case DISP_THROTTLE: throttle = d.value; break;
            case DISP_GESTURE:  gesture = d.value; break;
            case DISP_BATT:     batt = d.value; break;
            case DISP_PROFILE:  profile = d.value; break;
            default: break;
            }
        }

        xSemaphoreTake(mtx_i2c, portMAX_DELAY);
        ssd1306_clear(&disp);
        snprintf(buf, sizeof(buf), "GeoFS [%s]",
                 PROFILES[profile < NUM_PROFILES ? profile : 0].name);
        ssd1306_draw_string(&disp, 0, 0, 1, buf);
        ssd1306_draw_string(&disp, 0, 12, 1,
                            conn == CONN_CONNECTED ? "PC: conectado"
                                                   : "PC: ---");

        snprintf(buf, sizeof(buf), "Throttle: %3d%%", throttle);
        ssd1306_draw_string(&disp, 0, 24, 1, buf);
        /* barra de throttle */
        int bar = throttle * 120 / 100;
        ssd1306_draw_empty_square(&disp, 0, 34, 124, 6);
        if (bar > 0) {
            ssd1306_draw_square(&disp, 2, 36, (uint32_t)(bar > 120 ? 120 : bar), 2);
        }

        snprintf(buf, sizeof(buf), "Gesto: %s", gesture_name(gesture));
        ssd1306_draw_string(&disp, 0, 44, 1, buf);
        snprintf(buf, sizeof(buf), "Bateria: %3d%%", batt);
        ssd1306_draw_string(&disp, 0, 54, 1, buf);

        ssd1306_show(&disp);
        xSemaphoreGive(mtx_i2c);
    }
}

/* LED RGB de status: reage a mudancas de estado de conexao. */
static void task_status_led(void *p)
{
    (void)p;

    const uint pins[] = { LED_R_PIN, LED_G_PIN, LED_B_PIN };
    for (int i = 0; i < 3; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
    }
    uint8_t state = CONN_DISCONNECTED;
    status_led_set(state);

    for (;;) {
        if (xQueueReceive(q_status, &state, portMAX_DELAY) == pdTRUE) {
            status_led_set(state);
        }
    }
}

/* =========================================================================
 * Setup de hardware e main
 * ========================================================================= */

/* Configura botoes como entrada com pull-up e habilita IRQ por borda. */
static void buttons_init(void)
{
    const uint pins[] = { BTN_GEAR_PIN, BTN_FLAPS_PIN, BTN_BRAKE_PIN,
                          BTN_VIEW_PIN, BTN_JOY_PIN };
    for (int i = 0; i < 5; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
    /* Registra o callback unico no primeiro pino... */
    gpio_set_irq_enabled_with_callback(pins[0], GPIO_IRQ_EDGE_FALL, true,
                                       &gpio_callback);
    /* ...e habilita os demais pinos no mesmo callback. */
    for (int i = 1; i < 5; i++) {
        gpio_set_irq_enabled(pins[i], GPIO_IRQ_EDGE_FALL, true);
    }
    /* Botao do joystick tambem detecta a borda de subida (soltar) para
     * distinguir clique curto x longo (macro). */
    gpio_set_irq_enabled(BTN_JOY_PIN, GPIO_IRQ_EDGE_RISE, true);
}

int main(void)
{
    stdio_init_all();

    q_input = xQueueCreate(16, sizeof(input_msg_t));
    q_btn = xQueueCreate(16, sizeof(uint8_t));
    q_status = xQueueCreate(1, sizeof(uint8_t));
    q_echo = xQueueCreate(1, sizeof(uint32_t));
    q_event = xQueueCreate(8, sizeof(uint8_t));
    q_disp = xQueueCreate(8, sizeof(disp_msg_t));
    q_profile = xQueueCreate(1, sizeof(uint8_t));
    mtx_i2c = xSemaphoreCreateMutex();

    if (!q_input || !q_btn || !q_status || !q_echo || !q_event || !q_disp ||
        !q_profile || !mtx_i2c) {
        for (;;) { /* falha de alocacao */
        }
    }

    /* Perfil default disponivel desde o boot (xQueuePeek em task_analog). */
    uint8_t def_profile = DEFAULT_PROFILE;
    xQueueOverwrite(q_profile, &def_profile);

    buttons_init();

    TaskHandle_t h_analog = NULL, h_buttons = NULL, h_comm = NULL;
    TaskHandle_t h_led = NULL, h_gesture = NULL, h_feedback = NULL;
    TaskHandle_t h_display = NULL;

    xTaskCreate(task_gesture, "gesture", 1024, NULL, 2, &h_gesture);
#if !EI_DATA_FORWARDER
    xTaskCreate(task_analog, "analog", 512, NULL, 2, &h_analog);
    xTaskCreate(task_buttons, "buttons", 512, NULL, 3, &h_buttons);
    xTaskCreate(task_comm, "comm", 512, NULL, 2, &h_comm);
    xTaskCreate(task_status_led, "led", 256, NULL, 1, &h_led);
    xTaskCreate(task_feedback, "feedback", 512, NULL, 2, &h_feedback);
    xTaskCreate(task_display, "display", 1024, NULL, 1, &h_display);
#endif

#if (configNUMBER_OF_CORES > 1)
    /* Lab RTOS expert: a IA (DSP + classificador) fica isolada no core 1;
     * as tasks de controle de tempo-real ficam no core 0. */
    vTaskCoreAffinitySet(h_gesture, (UBaseType_t)(1u << 1));
    if (h_analog)   vTaskCoreAffinitySet(h_analog,   (UBaseType_t)(1u << 0));
    if (h_buttons)  vTaskCoreAffinitySet(h_buttons,  (UBaseType_t)(1u << 0));
    if (h_comm)     vTaskCoreAffinitySet(h_comm,     (UBaseType_t)(1u << 0));
    if (h_led)      vTaskCoreAffinitySet(h_led,      (UBaseType_t)(1u << 0));
    if (h_feedback) vTaskCoreAffinitySet(h_feedback, (UBaseType_t)(1u << 0));
    if (h_display)  vTaskCoreAffinitySet(h_display,  (UBaseType_t)(1u << 0));
#endif

    vTaskStartScheduler();

    for (;;) {
    }
}
