/* Driver LCD HD44780 via PCF8574 (I2C), modo 4 bits. Ver lcd_i2c.h. */

#include "lcd_i2c.h"

#include "pico/stdlib.h"

/* Bits do PCF8574 (backpack padrao) */
#define LCD_RS 0x01
#define LCD_EN 0x04
#define LCD_BL 0x08 /* backlight ligado */

static i2c_inst_t *g_i2c;
static uint8_t g_addr;

static void pcf_write(uint8_t data)
{
    i2c_write_blocking(g_i2c, g_addr, &data, 1, false);
}

/* Pulso no EN para o LCD capturar os 4 bits. */
static void lcd_toggle_enable(uint8_t val)
{
    sleep_us(1);
    pcf_write(val | LCD_EN | LCD_BL);
    sleep_us(1);
    pcf_write((val & ~LCD_EN) | LCD_BL);
    sleep_us(50);
}

/* Envia um nibble (4 bits baixos) com o modo (0=comando, LCD_RS=dado). */
static void lcd_send_nibble(uint8_t nibble, uint8_t mode)
{
    uint8_t val = (uint8_t)((nibble << 4) | mode);
    pcf_write(val | LCD_BL);
    lcd_toggle_enable(val);
}

static void lcd_send_byte(uint8_t value, uint8_t mode)
{
    lcd_send_nibble(value >> 4, mode);
    lcd_send_nibble(value & 0x0F, mode);
}

uint8_t lcd_init(i2c_inst_t *i2c, uint8_t addr)
{
    g_i2c = i2c;

    /* Autodeteccao do endereco. Cobre PCF8574 (0x20..0x27) e
     * PCF8574A (0x38..0x3F); 0x27 e 0x3F sao os mais comuns, testados antes. */
    if (addr == 0) {
        const uint8_t cand[] = { 0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x24,
                                 0x25, 0x26, 0x38, 0x39, 0x3A, 0x3B, 0x3C,
                                 0x3D, 0x3E };
        for (unsigned i = 0; i < sizeof(cand); i++) {
            uint8_t d = 0;
            if (i2c_write_blocking(i2c, cand[i], &d, 1, false) >= 0) {
                addr = cand[i];
                break;
            }
        }
        if (addr == 0) {
            return 0; /* nenhum LCD respondeu */
        }
    }
    g_addr = addr;

    /* Sequencia de inicializacao em 4 bits (datasheet HD44780). */
    sleep_ms(50);
    lcd_send_nibble(0x03, 0);
    sleep_ms(5);
    lcd_send_nibble(0x03, 0);
    sleep_us(150);
    lcd_send_nibble(0x03, 0);
    sleep_us(150);
    lcd_send_nibble(0x02, 0); /* muda para 4 bits */

    lcd_send_byte(0x28, 0); /* function set: 4 bits, 2 linhas, 5x8 */
    lcd_send_byte(0x0C, 0); /* display on, cursor off, blink off */
    lcd_send_byte(0x06, 0); /* entry mode: incrementa, sem shift */
    lcd_clear();
    return addr;
}

void lcd_clear(void)
{
    lcd_send_byte(0x01, 0);
    sleep_ms(2);
}

void lcd_set_cursor(uint8_t col, uint8_t row)
{
    static const uint8_t row_off[] = { 0x00, 0x40 };
    if (row > 1) {
        row = 1;
    }
    lcd_send_byte((uint8_t)(0x80 | (col + row_off[row])), 0);
}

void lcd_print(const char *s)
{
    while (*s) {
        lcd_send_byte((uint8_t)*s++, LCD_RS);
    }
}
