/* =========================================================================
 * Wrapper C++ do classificador de gestos (Edge Impulse on-device).
 *
 * Dois caminhos, selecionados no build:
 *   - HAS_EI_MODEL definido  -> usa o modelo exportado do Edge Impulse Studio
 *                               (biblioteca C++ em ei-model/).
 *   - HAS_EI_MODEL ausente   -> stub: estrutura pronta, retorna "idle".
 *
 * Para treinar o modelo: colete a serie de distancia do HC-SR04 com o
 * edge-impulse data-forwarder (build com EI_DATA_FORWARDER=1), rotule os
 * gestos no Studio, exporte a "C++ library" e descompacte em ei-model/.
 * Depois recompile com -DHAS_EI_MODEL=ON.
 * ========================================================================= */

#include "gesture.h"

#include <stdio.h>
#include <string.h>

#if defined(HAS_EI_MODEL)

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"
#include "model-parameters/model_metadata.h"

void gesture_init(void)
{
    /* run_classifier_init() nao e obrigatorio nas versoes recentes do SDK. */
}

/* Mapeia o rotulo textual do modelo para o id usado no protocolo. */
static int label_to_id(const char *label)
{
    if (strcmp(label, "swipe_up") == 0)   return GEST_SWIPE_UP;
    if (strcmp(label, "swipe_down") == 0) return GEST_SWIPE_DOWN;
    if (strcmp(label, "hover") == 0)      return GEST_HOVER;
    return GEST_IDLE;
}

int gesture_classify(const float *window, size_t n, char *label_out, size_t cap)
{
    /* O Edge Impulse espera EI_CLASSIFIER_RAW_SAMPLE_COUNT amostras. */
    size_t count = (n < EI_CLASSIFIER_RAW_SAMPLE_COUNT)
                       ? n
                       : EI_CLASSIFIER_RAW_SAMPLE_COUNT;

    ei::signal_t signal;
    if (numpy::signal_from_buffer(window, count, &signal) != 0) {
        return -1;
    }

    ei_impulse_result_t result = { 0 };
    if (run_classifier(&signal, &result, false) != EI_IMPULSE_OK) {
        return -1;
    }

    size_t best = 0;
    float best_v = 0.0f;
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (result.classification[i].value > best_v) {
            best_v = result.classification[i].value;
            best = i;
        }
    }

    const char *label = result.classification[best].label;
    if (label_out && cap) {
        snprintf(label_out, cap, "%s", label);
    }
    return label_to_id(label);
}

#else /* ---------- stub (sem modelo treinado ainda) ---------- */

void gesture_init(void)
{
}

int gesture_classify(const float *window, size_t n, char *label_out, size_t cap)
{
    (void)window;
    (void)n;
    if (label_out && cap) {
        snprintf(label_out, cap, "idle");
    }
    return GEST_IDLE;
}

#endif
