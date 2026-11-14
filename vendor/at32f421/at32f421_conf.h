/*
 * Minimal AT32F421 configuration header for the standalone 1-Wire driver.
 * Only the register-definition headers needed by the driver are pulled in;
 * no BSP driver source files are used.
 */
#ifndef __AT32F421_CONF_H
#define __AT32F421_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HEXT_VALUE
#define HEXT_VALUE              ((uint32_t)8000000)
#endif
#ifndef HICK_VALUE
#define HICK_VALUE              ((uint32_t)8000000)
#endif

#define CRM_MODULE_ENABLED
#define TMR_MODULE_ENABLED
#define GPIO_MODULE_ENABLED
#define DMA_MODULE_ENABLED
#define FLASH_MODULE_ENABLED
#define I2C_MODULE_ENABLED

#ifdef CRM_MODULE_ENABLED
#include "at32f421_crm.h"
#endif
#ifdef TMR_MODULE_ENABLED
#include "at32f421_tmr.h"
#endif
#ifdef GPIO_MODULE_ENABLED
#include "at32f421_gpio.h"
#endif
#ifdef DMA_MODULE_ENABLED
#include "at32f421_dma.h"
#endif
#ifdef FLASH_MODULE_ENABLED
#include "at32f421_flash.h"
#endif
#ifdef I2C_MODULE_ENABLED
#include "at32f421_i2c.h"
#endif

#ifdef __cplusplus
}
#endif

#endif /* __AT32F421_CONF_H */
