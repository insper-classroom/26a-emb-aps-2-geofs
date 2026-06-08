#ifndef PINS_H
#define PINS_H

/* =========================================================================
 * Mapa de pinos - Controle GeoFS (Raspberry Pi Pico 2 / RP2350)
 * Ajustar conforme a fiacao real do hardware.
 * ========================================================================= */

/* ---------------- Entradas analogicas (ADC) ---------------- */
/* Joystick: dois eixos no ADC. Potenciometro linear: throttle. */
#define ADC_JOY_X_PIN   26   /* ADC0 - eixo X (roll)  */
#define ADC_JOY_X_CH    0
#define ADC_JOY_Y_PIN   27   /* ADC1 - eixo Y (pitch) */
#define ADC_JOY_Y_CH    1
#define ADC_POT_PIN     28   /* ADC2 - potenciometro (throttle) */
#define ADC_POT_CH      2
#define ADC_BAT_PIN     29   /* ADC3 - bateria (fase 3) */
#define ADC_BAT_CH      3

/* ---------------- Entradas digitais (botoes, com IRQ) ---------------- */
/* Ativos em baixo (pull-up interno; apertado = 0). */
#define BTN_GEAR_PIN    10   /* trem de pouso (G) */
#define BTN_FLAPS_PIN   11   /* flaps (F)         */
#define BTN_BRAKE_PIN   12   /* freio (B)         */
#define BTN_VIEW_PIN    13   /* troca de view (V) */
#define BTN_JOY_PIN      9   /* clique do joystick */

/* IDs logicos dos botoes (enviados no datagrama) */
#define BTN_ID_GEAR     0
#define BTN_ID_FLAPS    1
#define BTN_ID_BRAKE    2
#define BTN_ID_VIEW     3
#define BTN_ID_JOY      4

/* ---------------- Sensor ultrassonico HC-SR04 (sensor da IA) ---------------- */
#define TRIG_PIN        14
#define ECHO_PIN        15   /* echo via divisor de tensao p/ 3V3 */

/* ---------------- Saidas de feedback (fase 3) ---------------- */
#define BUZZER_PIN      18   /* PWM */
#define VIBRA_PIN       19   /* motor de vibracao (GPIO + transistor) */

/* ---------------- LED RGB de status de conexao ---------------- */
#define LED_R_PIN       20
#define LED_G_PIN       21
#define LED_B_PIN       22
/* 1 = LED RGB catodo comum (gpio_put(pin,1) acende). 0 = anodo comum. */
#define LED_ACTIVE_HIGH 1

/* ---------------- I2C0 (OLED SSD1306 + IMU opcional) ---------------- */
#define I2C_SDA_PIN      4
#define I2C_SCL_PIN      5

#endif /* PINS_H */
