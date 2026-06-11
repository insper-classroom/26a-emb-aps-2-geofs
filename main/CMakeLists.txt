
if (NOT FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message("Skipping FreeRTOS examples as FREERTOS_KERNEL_PATH not defined")
    return()
endif()

include(FreeRTOS_Kernel_import.cmake)

# Opcoes do build:
#   -DHAS_EI_MODEL=ON      compila o classificador Edge Impulse (precisa ei-model/)
#   -DEI_DATA_FORWARDER=ON build de coleta p/ o edge-impulse data-forwarder
# HAS_EI_MODEL liga sozinho se a pasta ei-model/ existir (assim o "Run" do
# VS Code ja grava a versao com IA, sem precisar passar a flag na mao).
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../ei-model/edge-impulse-sdk)
    set(_ei_default ON)
else()
    set(_ei_default OFF)
endif()
option(HAS_EI_MODEL "Compilar com o modelo Edge Impulse (ei-model/)" ${_ei_default})
option(EI_DATA_FORWARDER "Build de coleta de dados (data-forwarder)" OFF)

add_executable(pico_emb
    main.c
    gesture.cpp
    lcd_i2c.c
    )

target_include_directories(pico_emb PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    )

# Linking to FreeRTOS-Kernel-Heap4 means we use a dynamic heap for allocations
target_link_libraries(pico_emb PRIVATE
    pico_async_context_freertos
    FreeRTOS-Kernel-Heap4
    pico_stdlib
    hardware_adc
    hardware_i2c
    hardware_pwm
    )

# Protocolo binario do controle pela serial USB (CDC). Sem stdio UART para
# nao misturar texto de debug com o stream binario.
pico_enable_stdio_usb(pico_emb 1)
pico_enable_stdio_uart(pico_emb 0)
# SMP: 2 cores (lab RTOS expert). A task de gesto (IA) roda no core 1 e as
# tasks de controle no core 0, via vTaskCoreAffinitySet (configUSE_CORE_AFFINITY
# e habilitado automaticamente pela FreeRTOSConfig.h no modo SMP).
target_compile_definitions(pico_emb PRIVATE
    configNUMBER_OF_CORES=2
    )

if(EI_DATA_FORWARDER)
    target_compile_definitions(pico_emb PRIVATE EI_DATA_FORWARDER=1)
endif()

# Modelo Edge Impulse (gestos do HC-SR04). Os fontes do SDK + modelo ficam em
# ei-model/ (exportado do Edge Impulse Studio). Enquanto nao existir, o build
# usa o stub em gesture.cpp e tudo continua compilando.
if(HAS_EI_MODEL)
    set(EI_MODEL_DIR ${CMAKE_CURRENT_LIST_DIR}/../ei-model)
    set(EI_SDK_FOLDER ${EI_MODEL_DIR}/edge-impulse-sdk)

    target_compile_definitions(pico_emb PRIVATE
        HAS_EI_MODEL=1
        EIDSP_QUANTIZE_FILTERBANK=0
        EI_CLASSIFIER_ALLOCATION_STATIC=1
        ARM_MATH_LOOPUNROLL
        )

    target_include_directories(pico_emb PRIVATE
        ${EI_MODEL_DIR}
        ${EI_MODEL_DIR}/tflite-model
        ${EI_MODEL_DIR}/model-parameters
        ${EI_SDK_FOLDER}
        ${EI_SDK_FOLDER}/third_party/ruy
        ${EI_SDK_FOLDER}/third_party/gemmlowp
        ${EI_SDK_FOLDER}/third_party/flatbuffers/include
        ${EI_SDK_FOLDER}/third_party
        ${EI_SDK_FOLDER}/tensorflow
        ${EI_SDK_FOLDER}/dsp
        ${EI_SDK_FOLDER}/classifier
        ${EI_SDK_FOLDER}/anomaly
        ${EI_SDK_FOLDER}/CMSIS/NN/Include
        ${EI_SDK_FOLDER}/CMSIS/DSP/PrivateInclude
        ${EI_SDK_FOLDER}/CMSIS/DSP/Include
        ${EI_SDK_FOLDER}/CMSIS/Core/Include
        ${EI_SDK_FOLDER}/porting
        )

    file(GLOB_RECURSE EI_CORE_CPP  ${EI_SDK_FOLDER}/*.cpp)
    file(GLOB_RECURSE EI_CORE_CC   ${EI_SDK_FOLDER}/*.cc)
    file(GLOB_RECURSE EI_CORE_C    ${EI_SDK_FOLDER}/*.c)
    file(GLOB_RECURSE EI_CORE_S    ${EI_SDK_FOLDER}/*.s)
    file(GLOB_RECURSE EI_MODEL_CPP ${EI_MODEL_DIR}/tflite-model/*.cpp)
    target_sources(pico_emb PRIVATE
        ${EI_CORE_CPP} ${EI_CORE_CC} ${EI_CORE_C} ${EI_CORE_S} ${EI_MODEL_CPP})

    target_link_libraries(pico_emb PRIVATE hardware_pio hardware_dma)
endif()

if(PICO_CYW43_SUPPORTED)
    # For led support on pico_w
    target_link_libraries(pico_emb PRIVATE
        pico_cyw43_arch_none
        )
endif()
pico_add_extra_outputs(pico_emb)

set_target_properties(pico_emb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
