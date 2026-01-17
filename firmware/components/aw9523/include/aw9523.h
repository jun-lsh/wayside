/**
 * @file aw9523.h
 * @author Yik Jin (@yikjin) <yikjin@nushackers.org>
 * @brief AW9523 GPIO expander component
 * @version 0.0.3
 * @date 2026-01-05
 *
 * @copyright Copyright (c) 2026 NUS Hackers
 *
 * This component simplifies communication with the AW9523 GPIO expander module,
 * and abstracts away having to deal with the 2 physical ports (`P0` and `P1`)
 * of 8 pins (`Pn_0..7`) each in the device, representing them as 16 virtual
 * pins instead (`0..15`) for ease of use.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_types.h"
#include "esp_err.h"

//
// Constants
//

/**
 * @brief I2C address of the AW9523 GPIO expander
 */
typedef enum {
  AW9523_I2C_ADDR_AD0_GND_AD1_GND = 0x58, /*!< `AD0` and `AD1` connected to
                                             `GND` */
  AW9523_I2C_ADDR_AD0_VCC_AD1_GND = 0x59, /*!< `AD0` connected to `VCC`, and
                                             `AD1` connected to `GND` */
  AW9523_I2C_ADDR_AD0_GND_AD1_VCC = 0x5A, /*!< `AD0` connected to `GND`, and
                                             `AD1` connected to `VCC` */
  AW9523_I2C_ADDR_AD0_VCC_AD1_VCC = 0x5B  /*!< `AD0` and `AD1` connected to
                                             `VCC` */
} aw9523_i2c_addr_t;

/**
 * @brief ID returned by the AW9523 GPIO expander when reading from the
 * `AW9523_REG_ID` register
 */
#define AW9523_ID 0x23

/**
 * @brief AW9523 GPIO expander's I2C device handle
 */
typedef i2c_master_dev_handle_t aw9523_t;

/**
 * @brief Register address constants to read from and/or write to for the AW9523
 * GPIO expander
 */
typedef enum {
  AW9523_REG_GPIO_INPUT_P0 = 0x00, /*!< GPIO pins' input values for port 0 (pins
                                      0 to 7) (read-only) */
  AW9523_REG_GPIO_INPUT_P1 = 0x01, /*!< GPIO pins' input values for port 1 (pins
                                      8 to 15) (read-only) */
  AW9523_REG_GPIO_OUTPUT_P0 = 0x02, /*!< GPIO pins' output values for port 0
                                       (pins 0 to 7) (read/write) */
  AW9523_REG_GPIO_OUTPUT_P1 = 0x03, /*!< GPIO pins' output values for port 1
                                       (pins 8 to 15) (read/write) */
  AW9523_REG_GPIO_DIR_P0 = 0x04,    /*!< GPIO pins' direction (input/output) for
                                       port 0 (pins 0 to 7) (read/write) */
  AW9523_REG_GPIO_DIR_P1 = 0x05,    /*!< GPIO pins' direction (input/output) for
                                       port 1 (pins 8 to 15) (read/write) */
  AW9523_REG_GPIO_INTERRUPT_P0 = 0x06, /*!< GPIO input pins' interrupt status
                                          for port 0 (pins 0 to 7) (read/write)
                                          */
  AW9523_REG_GPIO_INTERRUPT_P1 = 0x07, /*!< GPIO input pins' interrupt status
                                          for port 1 (pins 8 to 15) (read/write)
                                          */
  AW9523_REG_ID = 0x10,                /*!< Constant ID value (read-only) */
  AW9523_REG_CONTROL = 0x11,           /*!< Control register (read/write) */
  AW9523_REG_MODE_P0 = 0x12, /*!< GPIO or LED pins mode for port 0 (pins 0 to 7)
                                (read/write) */
  AW9523_REG_MODE_P1 = 0x13, /*!< GPIO or LED pins mode for port 1 (pins 8 to
                                15) (read/write) */
  AW9523_REG_SOFT_RESET = 0x7F /*!< Soft reset to default state (write-only) */
} aw9523_reg_addr_t;

/**
 * @brief Register value read from and/or to write to the AW9523 GPIO expander
 */
typedef uint8_t aw9523_reg_value_t;

/**
 * @brief Pin mode of a specific pin
 */
typedef enum {
  AW9523_PIN_GPIO_OUTPUT = 0, /*!< GPIO output pin mode */
  AW9523_PIN_GPIO_INPUT = 1,  /*!< GPIO input pin mode */
  AW9523_PIN_LED = 2          /*!< LED pin mode */
} aw9523_pin_mode_t;

/**
 * @brief Pin mode of all 16 pins
 */
typedef aw9523_pin_mode_t aw9523_pins_mode_t[16];

/**
 * @brief Pin number, ranging from 0 to 15 inclusive
 */
typedef uint8_t aw9523_pin_num_t;

/**
 * @brief GPIO digital pin data to read/write
 */
typedef bool aw9523_pin_data_digital_t;

/**
 * @brief GPIO digital pin data to read/write for all 16 pins
 */
typedef aw9523_pin_data_digital_t aw9523_pins_data_digital_t[16];

/**
 * @brief Interrupt mode on/off for a GPIO input pin
 */
typedef enum {
  AW9523_PIN_INTERRUPT_MODE_ENABLE = 0b0,  /*!< Enable interrupt for GPIO input
                                              pin */
  AW9523_PIN_INTERRUPT_MODE_DISABLE = 0b1, /*!< Disable interrupt for GPIO input
                                              pin */
} aw9523_pin_interrupt_mode_t;

/**
 * @brief Interrupt mode for all 16 GPIO input pins
 */
typedef aw9523_pin_interrupt_mode_t aw9523_pins_interrupt_mode_t[16];

/**
 * @brief GPIO output mode for all port 0 pins (pins 0 to 7)
 */
typedef enum {
  AW9523_GPIO_OUTPUT_MODE_OPEN_DRAIN = 0b0, /*!< Open drain GPIO output mode */
  AW9523_GPIO_OUTPUT_MODE_PUSH_PULL = 0b1   /*!< Push pull GPIO output mode */
} aw9523_gpio_output_mode_t;

/**
 * @brief Maximum current for all LED output pins
 */
typedef enum {
  AW9523_LED_MAX_mA_37 = 0x0, /*!< 37 mA max current for all LED output pins */
  AW9523_LED_MAX_mA_27_75 = 0x1, /*!< 27.75 mA max current for all LED output
                                    pins */
  AW9523_LED_MAX_mA_18_5 = 0x2, /*!< 18.5 mA max current for all LED output pins
                                 */
  AW9523_LED_MAX_mA_9_25 = 0x3  /*!< 9.25 mA max current for all LED output pins
                                 */
} aw9523_led_max_current_t;

//
// Functions
//

/**
 * @brief Read the specified register from the AW9523 GPIO expander
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] reg Register address to read from
 * @param[out] value Value read from the specified register
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_read_reg(const aw9523_t* dev, const aw9523_reg_addr_t reg,
                          aw9523_reg_value_t* value);

/**
 * @brief Write the specified register to the AW9523 GPIO expander
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] reg Register address to write to
 * @param[in] value Value to write to the specified register
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_write_reg(const aw9523_t* dev, const aw9523_reg_addr_t reg,
                           const aw9523_reg_value_t value);

/**
 * @brief Initialise the AW9523 GPIO expander
 *
 * @param[in] bus_handle I2C master bus handle
 * @param[in] address I2C address of the AW9523 GPIO expander
 * @param[out] dev I2C device handle of the AW9523 GPIO expander
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_init(const i2c_master_bus_handle_t* bus_handle,
                      const aw9523_i2c_addr_t address, aw9523_t* dev);

/**
 * @brief Set pin mode of all 16 pins
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] pins_mode Pin mode of all 16 pins
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_set_pins(const aw9523_t* dev,
                          const aw9523_pins_mode_t* pins_mode);

/**
 * @brief Set pin mode of a specific pin
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] pin Pin number to update the pin mode of
 * @param[in] pin_mode New pin mode for the specified pin number
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_set_pin(const aw9523_t* dev, const aw9523_pin_num_t pin,
                         const aw9523_pin_mode_t pin_mode);

/**
 * @brief Read all GPIO pins
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[out] pins_data Pin mode read from all 16 pins
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_gpio_read_pins(const aw9523_t* dev,
                                aw9523_pins_data_digital_t* pins_data);

/**
 * @brief Read digital pin data from a specific GPIO pin
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] pin Pin number to read from
 * @param[in] pin_mode If the specified pin is in GPIO input or output mode
 * @param[out] data GPIO digital pin data read from the pin number
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_gpio_read_pin(const aw9523_t* dev, const aw9523_pin_num_t pin,
                               const aw9523_pin_mode_t pin_mode,
                               aw9523_pin_data_digital_t* data);

/**
 * @brief Write digital pin data to all GPIO output pins
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] data Digital data to write to each GPIO output pin
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_gpio_write_pins(const aw9523_t* dev,
                                 const aw9523_pins_data_digital_t* data);

/**
 * @brief Write digital pin data to specific GPIO output pin
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] pin GPIO output pin number to write to
 * @param[in] data Digital data to write to the specified GPIO output pin
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_gpio_write_pin(const aw9523_t* dev, const aw9523_pin_num_t pin,
                                const aw9523_pin_data_digital_t data);

/**
 * @brief Set interrupt pin mode for all GPIO input pins
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] pins_interrupt_mode Interrupt mode to set for each GPIO input pin
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_set_gpio_interrupt_pins(
    const aw9523_t* dev,
    const aw9523_pins_interrupt_mode_t* pins_interrupt_mode);

/**
 * @brief Set interrupt pin mode for specific GPIO input pin
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] pin GPIO input pin to set interrupt pin mode of
 * @param[in] pin_interrupt_mode Interrupt mode to set the specified pin to
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_set_gpio_interrupt_pin(
    const aw9523_t* dev, const aw9523_pin_num_t pin,
    const aw9523_pin_interrupt_mode_t pin_interrupt_mode);

/**
 * @brief Set GPIO output mode of all pins in port 0 (pins 0 to 7)
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] gpio_output_mode GPIO output mode to set all of pins 0 to 7 to
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_set_gpio_output_mode_p0(
    const aw9523_t* dev, const aw9523_gpio_output_mode_t gpio_output_mode);

/**
 * @brief Set max current output of LED pins in LED mode
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] max_current Max current output of LED pins
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_set_led_max_current(
    const aw9523_t* dev, const aw9523_led_max_current_t max_current);

/**
 * @brief Set LED brightness of specific LED pin in LED mode
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @param[in] pin LED pin number to set LED brightness of
 * @param[in] brightness LED brightness value from 0 (0x00) to 255 (0xFF)
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_set_led_brightness(const aw9523_t* dev,
                                    const aw9523_pin_num_t pin,
                                    const aw9523_reg_value_t brightness);

/**
 * @brief Soft reset the AW9523 GPIO expander
 *
 * @param[in] dev I2C device handle of the AW9523 GPIO expander
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_soft_reset(const aw9523_t* dev);

/**
 * @brief Soft reset and remove the I2C instance of the AW9523 GPIO expander
 *
 * @param[in,out] dev I2C device handle of the AW9523 GPIO expander
 * @return esp_err_t ESP error constants
 */
esp_err_t aw9523_destroy(aw9523_t* dev);
