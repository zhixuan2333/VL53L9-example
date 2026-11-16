/**
 ******************************************************************************
 * @file    platform_utils.c
 * @author  IMD Software Team
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

// private includes
#include "stm32n6xx_hal.h"

#include "stm32n6xx_hal_i3c.h"
#include "vl53l9_device_conf.h"
#include "vl53l9_interface.h"

#include <stdio.h>
#include <string.h>

// when updating use semantic versioning (https://semver.org/)
#define FW_MAJOR (1)
#define FW_MINOR (1)
#define FW_PATCH (1)

typedef enum {
    I3C_SCL_FREQ_1MHZ,
    I3C_SCL_FREQ_12_5MHZ,
} i3c_scl_freq_t;

typedef enum {
    I3C_INSTANCE_1 = 0u,
    I3C_INSTANCE_2 = 1u,
    I3C_INSTANCE_MAX, // NOTE: keep this entry as the last one
} i3c_instance_t;

extern DCMIPP_HandleTypeDef hdcmipp;
extern I3C_HandleTypeDef hi3c1;
extern I3C_HandleTypeDef hi3c2;

static I3C_HandleTypeDef *g_i3c_handles[] = { &hi3c1, &hi3c2, NULL };
static uint8_t g_i3c_daa_count = 0;            // debug purpose, number of devices detected during I3C DAA procedure
static uint64_t g_i3c_daa_payload[NB_DEVICES]; // debug purpose, payloads of devices detected during I3C DAA procedure

static volatile bool g_i3c_dma_rx_evt[I3C_INSTANCE_MAX];
static volatile bool g_i3c_daa_evt[I3C_INSTANCE_MAX];
static volatile bool g_i3c_ibi_evt[NB_DEVICES];
static volatile bool g_irq_evt[NB_DEVICES];
static volatile bool g_cam_pipe_frame_evt;

/* private functions prototypes */

static int _timeout_expire(uint32_t to_start, uint32_t to_value);
static int _get_i3c_instance(I3C_HandleTypeDef *p_hi3c);
static int _set_i3c_scl_freq(I3C_HandleTypeDef *p_hi3c, i3c_scl_freq_t freq);

/* exported functions */

int platform_delay(uint32_t delay_ms) {
    HAL_Delay(delay_ms);
    return 0;
}

int platform_get_version(platform_version_t *version) {

    version->interface = (_version_t){ .major = INTERFACE_MAJOR, .minor = INTERFACE_MINOR, .patch = INTERFACE_PATCH };
    version->firmware = (_version_t){ .major = FW_MAJOR, .minor = FW_MINOR, .patch = FW_PATCH };
    version->driver =
        (_version_t){ .major = VL53L9_CORE_MAJOR, .minor = VL53L9_CORE_MINOR, .patch = VL53L9_CORE_PATCH };

    strncpy(version->board_name, "nucleo-n657x0-q", BOARD_NAME_STR_SIZE);

    return 0;
}

/* device power management */

int platform_power_reset(uint8_t id) {
    vl53l9_device_t *p_dev = vl53l9_get_device(id);
    HAL_GPIO_WritePin((GPIO_TypeDef *)p_dev->xshut.port, p_dev->xshut.pin, GPIO_PIN_RESET);
    HAL_Delay(50);
    HAL_GPIO_WritePin((GPIO_TypeDef *)p_dev->xshut.port, p_dev->xshut.pin, GPIO_PIN_SET);
    HAL_Delay(50);
    return 0;
}

int platform_power_enable(uint8_t id) {
    vl53l9_device_t *p_dev = vl53l9_get_device(id);
    HAL_GPIO_WritePin((GPIO_TypeDef *)p_dev->xshut.port, p_dev->xshut.pin, GPIO_PIN_SET);
    HAL_Delay(50);
    return 0;
}

int platform_power_disable(uint8_t id) {
    vl53l9_device_t *p_dev = vl53l9_get_device(id);
    HAL_GPIO_WritePin((GPIO_TypeDef *)p_dev->xshut.port, p_dev->xshut.pin, GPIO_PIN_RESET);
    HAL_Delay(50);
    return 0;
}

/* i2c/i3c interfaces */

// NOTE: in case of multiple devices, this method requires that instance id have been correctly set for each of them
// TODO: enable ibi notifications (up to 4 devices only)
int platform_assign_dynamic_address() {

    HAL_StatusTypeDef status;
    g_i3c_daa_count = 0; // reset device payload count before entering assigning dynamic addresses

    for (int i = 0; g_i3c_handles[i] != NULL; i++) {

        uint64_t payload;
        I3C_ENTDAAPayloadTypeDef payload_info;
        I3C_HandleTypeDef *p_hi3c = g_i3c_handles[i];

        // set i3c bus frequency to 1 MHz before dynamic address assignment
        _set_i3c_scl_freq(p_hi3c, I3C_SCL_FREQ_1MHZ);

        do {
            payload = 0;
            status = HAL_I3C_Ctrl_DynAddrAssign(p_hi3c, &payload, I3C_RSTDAA_THEN_ENTDAA, 5000);

            if (status != HAL_BUSY) {
                continue;
            }

            HAL_I3C_Get_ENTDAA_Payload_Info(p_hi3c, payload, &payload_info);

            for (int i = 0; i < NB_DEVICES; i++) {
                vl53l9_device_t *p_dev = vl53l9_get_device(i);
                if (payload_info.PID.MIPIID == p_dev->instance_id) {
                    // TODO: check if dynamic address assignment was successful
                    HAL_I3C_Ctrl_SetDynAddr(p_hi3c, p_dev->address); // use static address as dynamic address
                    HAL_Delay(1);
                    g_i3c_daa_payload[g_i3c_daa_count++] = payload;
                    break;
                }
            }
        } while (status == HAL_BUSY);

        // mark that dynamic address assignment has been completed for this i3c instance
        i3c_instance_t instance = _get_i3c_instance(p_hi3c);
        g_i3c_daa_evt[instance] = true;

        // set i3c bus frequency to 12.5 MHz after dynamic address assignement
        _set_i3c_scl_freq(p_hi3c, I3C_SCL_FREQ_12_5MHZ);
    }

    return 0;
}

int platform_ctrl_gpio(platform_gpio_t gpio, platform_gpio_state_t state) {
    switch (state) {
    case PLATFORM_GPIO_STATE_RESET:
        HAL_GPIO_WritePin(gpio.port, gpio.pin, GPIO_PIN_RESET);
        break;
    case PLATFORM_GPIO_STATE_SET:
        HAL_GPIO_WritePin(gpio.port, gpio.pin, GPIO_PIN_SET);
        break;
    case PLATFORM_GPIO_STATE_TOGGLE:
        HAL_GPIO_TogglePin(gpio.port, gpio.pin);
        break;
    default:
        return -1;
        break;
    }
    return 0;
}

/* profiling */

int platform_profiler_enable() {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk; // enable counter
    return 0;
}

int platform_profiler_disable() {
    // TODO: clear CoreDebug_DEMCR_TRCENA_Msk in DEMCR register
    // TODO: disable counter
    return 0;
}

uint32_t platform_profiler_get_timestamp() {
    return DWT->CYCCNT;
}

uint32_t platform_profiler_convert_to_us(uint32_t timestamp) {
    uint32_t tick_per_1us = SystemCoreClock / 1000000;
    return timestamp / tick_per_1us;
}

/* event handling */

int platform_enable_event(uint8_t id, platform_event_t event) {
    int res = 0;
    switch (event) {
    case PLATFORM_I3C_IBI_EVT:
        g_i3c_ibi_evt[id] = false;
        // TODO: abstract this
        // HAL_I3C_ActivateNotification(&hi3c2, NULL, LL_I3C_IER_IBIIE);
        // HAL_NVIC_EnableIRQ(I3C2_EV_IRQn);
        break;
    case PLATFORM_GPIO_IT_EVT:
        g_irq_evt[id] = false;
        // TODO: abstract this
        // HAL_NVIC_EnableIRQ(EXTI0_IRQn);
        // HAL_NVIC_EnableIRQ(EXTI6_IRQn);
        break;
    case PLATFORM_CAM_PIPE_FRAME_EVT:
        g_cam_pipe_frame_evt = false;
        HAL_NVIC_EnableIRQ(DCMIPP_IRQn);
        HAL_NVIC_EnableIRQ(CSI_IRQn);
        break;
    default:
        res = -1;
        break;
    }
    return res;
}

int platform_disable_event(uint8_t id, platform_event_t event) {

    int res = 0;
    vl53l9_device_t *p_dev = vl53l9_get_device(id);
    i3c_instance_t instance = _get_i3c_instance(p_dev->bus); // TODO: check if instance is valid
    I3C_HandleTypeDef *hi3c = g_i3c_handles[instance];

    switch (event) {
    case PLATFORM_I3C_IBI_EVT:
        HAL_I3C_DeactivateNotification(hi3c, LL_I3C_IER_IBIIE);
        // HAL_NVIC_DisableIRQ(I3C2_EV_IRQn); // TODO: abstract this call
        g_i3c_ibi_evt[id] = false;
        break;
    case PLATFORM_GPIO_IT_EVT:
        // HAL_NVIC_DisableIRQ(EXTI0_IRQn);
        // HAL_NVIC_DisableIRQ(EXTI6_IRQn);
        g_irq_evt[id] = false;
        break;
    case PLATFORM_CAM_PIPE_FRAME_EVT:
        HAL_NVIC_DisableIRQ(DCMIPP_IRQn);
        HAL_NVIC_DisableIRQ(CSI_IRQn);
        g_cam_pipe_frame_evt = false;
        break;
    default:
        res = -1;
        break;
    }
    return res;
}

int platform_wait_for_event(uint8_t id, platform_event_t event, uint32_t timeout_ms) {

    int res = 0;
    uint64_t to_startlong = HAL_GetTick();
    uint32_t to_start = (uint32_t)to_startlong;
    vl53l9_device_t *p_dev = vl53l9_get_device(id);
    i3c_instance_t instance = _get_i3c_instance(p_dev->bus); // TODO: check if instance is valid

    volatile bool *p_event_flag = NULL;

    switch (event) {
    case PLATFORM_GPIO_IT_EVT:
        p_event_flag = &g_irq_evt[id];
        break;
    case PLATFORM_I3C_IBI_EVT:
        p_event_flag = &g_i3c_ibi_evt[id];
        break;
    case PLATFORM_I3C_DAA_EVT:
        p_event_flag = &g_i3c_daa_evt[instance];
        break;
    case PLATFORM_I3C_DMA_RX_EVT:
        p_event_flag = &g_i3c_dma_rx_evt[instance];
        break;
    case PLATFORM_CAM_PIPE_FRAME_EVT:
        p_event_flag = &g_cam_pipe_frame_evt;
        break;
    default:
        return -1;
    }

    while (!(*p_event_flag) && !_timeout_expire(to_start, timeout_ms))
        ;

    if (!(*p_event_flag)) {
        res = -1;
    }

    return res;
}

int platform_acknowledge_event(uint8_t id, platform_event_t event) {

    int res = 0;
    vl53l9_device_t *p_dev = vl53l9_get_device(id);
    i3c_instance_t instance = _get_i3c_instance(p_dev->bus); // TODO: check if instance is valid

    switch (event) {
    case PLATFORM_GPIO_IT_EVT:
        g_irq_evt[id] = false;
        break;
    case PLATFORM_I3C_IBI_EVT:
        g_i3c_ibi_evt[id] = false;
        break;
    case PLATFORM_I3C_DAA_EVT:
        g_i3c_daa_evt[instance] = false;
        break;
    case PLATFORM_I3C_DMA_RX_EVT:
        g_i3c_dma_rx_evt[instance] = false;
        break;
    case PLATFORM_CAM_PIPE_FRAME_EVT:
        g_cam_pipe_frame_evt = false;
        break;
    default:
        res = -1;
        break;
    }
    return res;
}

int platform_get_event_status(uint8_t id, platform_event_t event, bool *active) {

    vl53l9_device_t *p_dev = vl53l9_get_device(id);
    i3c_instance_t instance = _get_i3c_instance(p_dev->bus); // TODO: check if instance is valid

    switch (event) {
    case PLATFORM_GPIO_IT_EVT:
        *active = g_irq_evt[id] ? true : false;
        break;
    case PLATFORM_I3C_IBI_EVT:
        *active = g_i3c_ibi_evt[id] ? true : false;
        break;
    case PLATFORM_I3C_DAA_EVT:
        *active = g_i3c_daa_evt[instance] ? true : false;
        break;
    case PLATFORM_I3C_DMA_RX_EVT:
        *active = g_i3c_dma_rx_evt[instance] ? true : false;
        break;
    case PLATFORM_CAM_PIPE_FRAME_EVT:
        *active = g_cam_pipe_frame_evt ? true : false;
        break;
    default:
        return -1;
    }

    return 0;
}

/* HAL callbacks */

void HAL_I3C_NotifyCallback(I3C_HandleTypeDef *hi3c, uint32_t eventId) {
    if ((eventId & EVENT_ID_IBI) == EVENT_ID_IBI) {
        I3C_CCCInfoTypeDef CCCInfo;
        if (HAL_I3C_GetCCCInfo(hi3c, EVENT_ID_IBI, &CCCInfo) != HAL_OK) {
            return; // error in handling event
        }
        if (CCCInfo.IBITgtPayload == 0) {
            ; // TODO: retrieve instance, instance id from payload and set flag accordingly
        }
    }
}

void HAL_I3C_CtrlRxCpltCallback(I3C_HandleTypeDef *hi3c) {
    i3c_instance_t instance = _get_i3c_instance(hi3c);
    g_i3c_dma_rx_evt[instance] = true;
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin) {
    for (int i = 0; i < NB_DEVICES; i++) {
        vl53l9_device_t *p_dev = vl53l9_get_device(i);
        if (GPIO_Pin == p_dev->intr.pin) {
            g_irq_evt[i] = true;
        }
    }
}

void HAL_DCMIPP_PIPE_FrameEventCallback(DCMIPP_HandleTypeDef *hdcmipp, uint32_t Pipe) {
    if (Pipe == DCMIPP_PIPE0) {
        g_cam_pipe_frame_evt = true;
    }
}

/* csi interface */

int platform_start_csi_pipe(uint8_t *buff_csi) {
    if (HAL_OK != HAL_DCMIPP_CSI_PIPE_Start(&hdcmipp, DCMIPP_PIPE0, DCMIPP_VIRTUAL_CHANNEL0, (uint32_t)buff_csi,
                                            DCMIPP_MODE_CONTINUOUS)) {
        return -1;
    }
    return 0;
}

int platform_stop_csi_pipe(void) {
    if (HAL_OK != HAL_DCMIPP_CSI_PIPE_Stop(&hdcmipp, DCMIPP_PIPE0, DCMIPP_VIRTUAL_CHANNEL0)) {
        return -1;
    }
    return 0;
}

/* display */

int platform_display_enable() {
    return 0;
}

int platform_display_disable() {
    return 0;
}

int platform_display_set_color_lut(uint8_t layer_id) {
    return 0;
}

int platform_display_configure_layer(platform_display_layer_config_t *config, uint8_t layer_id) {
    return 0;
}

/* private functions **************************************************************************************************/

static int _timeout_expire(uint32_t to_start, uint32_t to_value) {
    uint64_t currentlong = HAL_GetTick();
    uint32_t current = (uint32_t)currentlong;

    return current >= to_start ? (current - to_start) >= to_value : (current + (0xffffffff - to_start) + 1) >= to_value;
}

static int _get_i3c_instance(I3C_HandleTypeDef *p_hi3c) {

    if (p_hi3c->Instance == I3C1) {
        return I3C_INSTANCE_1;
    } else if (p_hi3c->Instance == I3C2) {
        return I3C_INSTANCE_2;
    }
    return -1; // unsupported instance
}

static int _set_i3c_scl_freq(I3C_HandleTypeDef *p_hi3c, i3c_scl_freq_t freq) {

    I3C_TypeDef *p_instance = p_hi3c->Instance;
    HAL_I3C_ModeTypeDef mode = p_hi3c->Mode;

    if (HAL_I3C_DeInit(p_hi3c) != HAL_OK) {
        return -1;
    }

    p_hi3c->Instance = p_instance;
    p_hi3c->Mode = mode;

    switch (freq) {
    case I3C_SCL_FREQ_1MHZ:
        // Set SCL frequency to 1 MHz
        p_hi3c->Init.CtrlBusCharacteristic.SDAHoldTime = HAL_I3C_SDA_HOLD_TIME_1_5;
        p_hi3c->Init.CtrlBusCharacteristic.WaitTime = HAL_I3C_OWN_ACTIVITY_STATE_0;
        p_hi3c->Init.CtrlBusCharacteristic.SCLPPLowDuration = 0x63;
        p_hi3c->Init.CtrlBusCharacteristic.SCLI3CHighDuration = 0x63;
        p_hi3c->Init.CtrlBusCharacteristic.SCLODLowDuration = 0x63;
        p_hi3c->Init.CtrlBusCharacteristic.SCLI2CHighDuration = 0x63;
        p_hi3c->Init.CtrlBusCharacteristic.BusFreeDuration = 0x27;
        p_hi3c->Init.CtrlBusCharacteristic.BusIdleDuration = 0xc6;
        break;
    case I3C_SCL_FREQ_12_5MHZ:
        // Set SCL frequency to 12.5 MHz
        p_hi3c->Init.CtrlBusCharacteristic.SDAHoldTime = HAL_I3C_SDA_HOLD_TIME_1_5;
        p_hi3c->Init.CtrlBusCharacteristic.WaitTime = HAL_I3C_OWN_ACTIVITY_STATE_0;
        p_hi3c->Init.CtrlBusCharacteristic.SCLPPLowDuration = 0x07;
        p_hi3c->Init.CtrlBusCharacteristic.SCLI3CHighDuration = 0x07;
        p_hi3c->Init.CtrlBusCharacteristic.SCLODLowDuration = 0x47;
        p_hi3c->Init.CtrlBusCharacteristic.SCLI2CHighDuration = 0x00; // NOTE: not relevant
        p_hi3c->Init.CtrlBusCharacteristic.BusFreeDuration = 0x28;
        p_hi3c->Init.CtrlBusCharacteristic.BusIdleDuration = 0xc6;
        break;
    default:
        return -1; // Unsupported frequency
    }

    if (HAL_I3C_Init(p_hi3c) != HAL_OK) {
        return -1;
    }

    return 0;
}
