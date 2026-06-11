#ifndef LCD_I2C_H
#define LCD_I2C_H

/* Driver minimo para LCD de caracteres HD44780 com backpack I2C (PCF8574).
 * Modo 4 bits. Mapa de bits padrao do backpack:
 *   P0=RS  P1=RW  P2=EN  P3=Backlight  P4..P7=D4..D7 */

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"

/* Inicializa o LCD. Se addr==0, tenta autodetectar (0x27 e 0x3F).
 * Retorna o endereco usado, ou 0 se nenhum LCD respondeu. */
uint8_t lcd_init(i2c_inst_t *i2c, uint8_t addr);

void lcd_clear(void);
void lcd_set_cursor(uint8_t col, uint8_t row);
void lcd_print(const char *s);

#endif /* LCD_I2C_H */
