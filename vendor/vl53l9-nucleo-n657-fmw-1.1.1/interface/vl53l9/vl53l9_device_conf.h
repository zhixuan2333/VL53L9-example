/**
 ******************************************************************************
 * @file    vl53l9_device_conf.h
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

#ifndef VL53L9_DEVICE_CONF_H
#define VL53L9_DEVICE_CONF_H

#include "vl53l9_interface.h"

// NOTE: set the correct hardware configuration by defining symbols according to your setup
#if !defined(CONFIG_HW_STEVAL_CAM) && !defined(CONFIG_HW_X_NUCLEO_ARD)
#define CONFIG_HW_STEVAL_CAM // fallback configuration if no symbol is defined
#endif

// NOTE: in case multiple devices, make sure NB_DEVICES is correctly set
#ifndef NB_DEVICES
#if defined(CONFIG_HW_STEVAL_CAM) && defined(CONFIG_HW_X_NUCLEO_ARD)
#define NB_DEVICES (2U)
#elif defined(CONFIG_HW_STEVAL_CAM) || defined(CONFIG_HW_X_NUCLEO_ARD)
#define NB_DEVICES (1U)
#endif
#endif // NB_DEVICES

vl53l9_device_t *vl53l9_get_device(uint8_t id);

#endif // VL53L9_DEVICE_CONF_H
