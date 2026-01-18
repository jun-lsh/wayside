/*
 * definitions.h - hardware pin definitions
 */

#ifndef DEFINITIONS_H
#define DEFINITIONS_H

#include "driver/gpio.h"
#include "hal/adc_types.h"

/* nfc chip pins */
#define NFC_PWR_PIN     GPIO_NUM_0      /* power enable */
#define NFC_SDA_PIN     GPIO_NUM_7      /* i2c data */
#define NFC_SCL_PIN     GPIO_NUM_6      /* i2c clock */
#define NFC_FD_PIN      GPIO_NUM_1      /* field detect interrupt */

/* nfc i2c config */
#define NFC_I2C_FREQ_HZ 100000          /* 100khz - can try 400000 */

/* adc */
#define VBAT_ADC_CHANNEL ADC_CHANNEL_4  /* gpio4, adc1_ch4 */

#endif
