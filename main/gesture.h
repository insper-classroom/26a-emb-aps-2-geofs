#ifndef GESTURE_H
#define GESTURE_H

/* =========================================================================
 * API C do classificador de gestos (Edge Impulse on-device).
 *
 * Sensor: HC-SR04 (serie temporal de distancia). O firmware (main.c) coleta
 * uma janela de amostras e chama gesture_classify(). A implementacao real
 * (Edge Impulse) so e compilada quando o build define HAS_EI_MODEL e o
 * modelo exportado existe em ei-model/. Sem o modelo, e um stub que retorna
 * "idle" - assim o projeto continua compilando enquanto o modelo nao foi
 * treinado/exportado.
 * ========================================================================= */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IDs de gesto (devem casar com o GESTUREMAP da ponte PC) */
#define GEST_IDLE        0
#define GEST_SWIPE_UP    1
#define GEST_SWIPE_DOWN  2
#define GEST_HOVER       3

/* Inicializa o classificador (no-op no stub). */
void gesture_init(void);

/* Classifica uma janela de amostras (ex.: distancias do HC-SR04 em cm).
 * Retorna o id do gesto vencedor (>= 0) ou -1 em erro. Se label_out != NULL,
 * escreve o rotulo textual (truncado em cap). */
int gesture_classify(const float *window, size_t n, char *label_out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GESTURE_H */
