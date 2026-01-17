#include "aw9523.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "portmacro.h"

/**
 * @brief Logging tag to use for this component
 */
static const char* TAG = "aw9523";

/**
 * @brief Physical port number (0 or 1), where port 0 has pins 0 to 7, and port
 * 1 has pins 8 to 15
 */
typedef uint8_t _aw9523_port_num_t;  // Not a `bool` to allow for use in loops
                                     // without overflowing
/**
 * @brief Physical pin number of a port `n`, only ranges from 0 to 7 inclusive
 * (`Pn_0..7`)
 */
typedef uint8_t _aw9523_port_pin_num_t;

/**
 * @brief Convert a virtual pin to a physical port and pin
 *
 * @param[in] pin Virtual pin number to convert
 * @param[out] port_num Equivalent physical port number
 * @param[out] port_pin Equivalent physical pin number for port
 */
static void _aw9523_get_port_pin_num(const aw9523_pin_num_t pin,
                                     _aw9523_port_num_t* port_num,
                                     _aw9523_port_pin_num_t* port_pin) {
  *port_num = pin / 8;
  *port_pin = pin % 8;
}

static void _aw9523_calc_regs_pin_mode_update(const aw9523_pin_num_t pin,
                                              const aw9523_pin_mode_t pin_mode,
                                              aw9523_reg_value_t* port_mode,
                                              aw9523_reg_value_t* gpio_dir) {
  _aw9523_port_num_t _port_num = 0;
  _aw9523_port_pin_num_t port_pin = 0;
  _aw9523_get_port_pin_num(pin, &_port_num, &port_pin);

  switch (pin_mode) {
    case AW9523_PIN_GPIO_INPUT:
      *port_mode |= 0x1 << port_pin;
      *gpio_dir |= 0x1 << port_pin;
      break;
    case AW9523_PIN_GPIO_OUTPUT:
      *port_mode |= 0x1 << port_pin;
      *gpio_dir &= ~(0x1 << port_pin);
      break;
    case AW9523_PIN_LED:
      *port_mode &= ~(0x1 << port_pin);
      *gpio_dir &= ~(0x1 << port_pin);
      break;
    default:
      ESP_LOGE(TAG,
               "Invalid pin mode supplied for pin %" PRIu8
               ", defaulting to GPIO input",
               pin);
      *port_mode |= 0x1 << port_pin;
      *gpio_dir |= 0x1 << port_pin;
      break;
  }
}

static aw9523_reg_addr_t _aw9523_get_led_brightness_reg_from_pin_num(
    const aw9523_pin_num_t pin) {
  if (pin < 8) {
    return 0x24 + pin;
  }

  if (pin < 12) {
    return 0x18 + pin;
  }

  return 0x20 + pin;
}

esp_err_t aw9523_read_reg(const aw9523_t* dev, const aw9523_reg_addr_t reg,
                          aw9523_reg_value_t* value) {
  return i2c_master_transmit_receive(*dev, (const uint8_t*)(&reg), 1, value, 1,
                                     1000 / portTICK_PERIOD_MS);
}

esp_err_t aw9523_write_reg(const aw9523_t* dev, const aw9523_reg_addr_t reg,
                           const aw9523_reg_value_t value) {
  const uint8_t data[2] = {reg, value};
  return i2c_master_transmit(*dev, data, 2, 1000 / portTICK_PERIOD_MS);
}

esp_err_t aw9523_init(const i2c_master_bus_handle_t* bus_handle,
                      const aw9523_i2c_addr_t address, aw9523_t* dev) {
  const i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = address,
      .scl_speed_hz = 400000,
      .flags.disable_ack_check = false,
  };

  ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(*bus_handle, &dev_cfg, dev),
                      TAG, "Failed to add I2C device");

  aw9523_reg_value_t res_id = 0x00;
  ESP_RETURN_ON_ERROR(aw9523_read_reg(dev, AW9523_REG_ID, &res_id), TAG,
                      "Failed to read I2C device ID");

  ESP_RETURN_ON_FALSE(res_id == AW9523_ID, ESP_FAIL, TAG,
                      "I2C device is not a AW9523 GPIO expander (got %#04x)",
                      res_id);

  ESP_RETURN_ON_ERROR(aw9523_soft_reset(dev), TAG,
                      "Failed to soft reset AW9523 GPIO expander");

  return ESP_OK;
}

esp_err_t aw9523_set_pins(const aw9523_t* dev,
                          const aw9523_pins_mode_t* pins_mode) {
  for (_aw9523_port_num_t port_num = 0; port_num < 2; port_num++) {
    aw9523_reg_value_t port_mode = 0x00;
    aw9523_reg_value_t gpio_dir = 0x00;

    for (_aw9523_port_pin_num_t port_pin = 0; port_pin < 8; port_pin++) {
      const aw9523_pin_num_t pin = port_num * 8 + port_pin;
      const aw9523_pin_mode_t pin_mode = (*pins_mode)[pin];

      _aw9523_calc_regs_pin_mode_update(pin, pin_mode, &port_mode, &gpio_dir);
    }

    const aw9523_reg_addr_t port_mode_reg =
        port_num == 0 ? AW9523_REG_MODE_P0 : AW9523_REG_MODE_P1;
    const aw9523_reg_addr_t gpio_dir_reg =
        port_num == 0 ? AW9523_REG_GPIO_DIR_P0 : AW9523_REG_GPIO_DIR_P1;

    ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, port_mode_reg, port_mode), TAG,
                        "Failed to write port mode for port %" PRIu8, port_num);
    ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, gpio_dir_reg, gpio_dir), TAG,
                        "Failed to write GPIO direction for port %" PRIu8,
                        port_num);
  }

  return ESP_OK;
}

esp_err_t aw9523_set_pin(const aw9523_t* dev, const aw9523_pin_num_t pin,
                         const aw9523_pin_mode_t pin_mode) {
  _aw9523_port_num_t port_num = 0;
  _aw9523_port_pin_num_t port_pin = 0;

  _aw9523_get_port_pin_num(pin, &port_num, &port_pin);

  aw9523_reg_value_t port_mode = 0x00;
  aw9523_reg_value_t gpio_dir = 0x00;
  const aw9523_reg_addr_t port_mode_reg =
      port_num == 0 ? AW9523_REG_MODE_P0 : AW9523_REG_MODE_P1;
  const aw9523_reg_addr_t gpio_dir_reg =
      port_num == 0 ? AW9523_REG_GPIO_DIR_P0 : AW9523_REG_GPIO_DIR_P1;

  ESP_RETURN_ON_ERROR(aw9523_read_reg(dev, port_mode_reg, &port_mode), TAG,
                      "Failed to read port mode of pin %" PRIu8, pin);
  ESP_RETURN_ON_ERROR(aw9523_read_reg(dev, gpio_dir_reg, &gpio_dir), TAG,
                      "Failed to read GPIO direction of pin %" PRIu8, pin);

  _aw9523_calc_regs_pin_mode_update(pin, pin_mode, &port_mode, &gpio_dir);

  ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, port_mode_reg, port_mode), TAG,
                      "Failed to write port mode of pin %" PRIu8, pin);
  ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, gpio_dir_reg, gpio_dir), TAG,
                      "Failed to write GPIO direction of pin %" PRIu8, pin);

  return ESP_OK;
}

esp_err_t aw9523_gpio_read_pins(const aw9523_t* dev,
                                aw9523_pins_data_digital_t* pins_data) {
  for (_aw9523_port_num_t port_num = 0; port_num < 2; port_num++) {
    const aw9523_reg_addr_t gpio_dir_reg =
        port_num == 0 ? AW9523_REG_GPIO_DIR_P0 : AW9523_REG_GPIO_DIR_P1;

    aw9523_reg_value_t gpio_dir;
    ESP_RETURN_ON_ERROR(aw9523_read_reg(dev, gpio_dir_reg, &gpio_dir), TAG,
                        "Failed to read GPIO direction for port %" PRIu8,
                        port_num);

    const aw9523_reg_addr_t gpio_input_reg =
        port_num == 0 ? AW9523_REG_GPIO_INPUT_P0 : AW9523_REG_GPIO_INPUT_P1;

    aw9523_reg_value_t gpio_input;
    ESP_RETURN_ON_ERROR(aw9523_read_reg(dev, gpio_input_reg, &gpio_input), TAG,
                        "Failed to read GPIO input for port %" PRIu8, port_num);

    const aw9523_reg_addr_t gpio_output_reg =
        port_num == 0 ? AW9523_REG_GPIO_OUTPUT_P0 : AW9523_REG_GPIO_OUTPUT_P1;

    aw9523_reg_value_t gpio_output;
    ESP_RETURN_ON_ERROR(aw9523_read_reg(dev, gpio_output_reg, &gpio_output),
                        TAG, "Failed to read GPIO output for port %" PRIu8,
                        port_num);

    for (_aw9523_port_pin_num_t port_pin = 0; port_pin < 8; port_pin++) {
      const aw9523_pin_mode_t pin_dir = (gpio_dir >> port_pin) & 0b1;
      aw9523_pin_data_digital_t pin_data;

      if (pin_dir == AW9523_PIN_GPIO_INPUT) {
        pin_data = (gpio_input >> port_pin) & 0b1;
      } else {
        pin_data = (gpio_output >> port_pin) & 0b1;
      }

      const aw9523_pin_num_t pin = port_num * 8 + port_pin;
      (*pins_data)[pin] = pin_data;
    }
  }

  return ESP_OK;
}

esp_err_t aw9523_gpio_read_pin(const aw9523_t* dev, const aw9523_pin_num_t pin,
                               const aw9523_pin_mode_t pin_mode,
                               aw9523_pin_data_digital_t* data) {
  ESP_RETURN_ON_FALSE(
      pin_mode == AW9523_PIN_GPIO_INPUT || pin_mode == AW9523_PIN_GPIO_OUTPUT,
      ESP_FAIL, TAG,
      "Cannot read pin that is not in GPIO mode (requested pin mode %d)",
      pin_mode);

  _aw9523_port_num_t port_num = 0;
  _aw9523_port_pin_num_t port_pin = 0;

  _aw9523_get_port_pin_num(pin, &port_num, &port_pin);

  aw9523_reg_addr_t reg = 0x00;

  switch (pin_mode) {
    case AW9523_PIN_GPIO_INPUT:
      reg = port_num == 0 ? AW9523_REG_GPIO_INPUT_P0 : AW9523_REG_GPIO_INPUT_P1;
      break;
    case AW9523_PIN_GPIO_OUTPUT:
      reg =
          port_num == 0 ? AW9523_REG_GPIO_OUTPUT_P0 : AW9523_REG_GPIO_OUTPUT_P1;
      break;
    default:
      ESP_LOGE(
          TAG,
          "Cannot read pin that is not in GPIO mode (requested pin mode %d)",
          pin_mode);
      return ESP_FAIL;
  }

  aw9523_reg_value_t reg_value;
  ESP_RETURN_ON_ERROR(aw9523_read_reg(dev, reg, &reg_value), TAG,
                      "Failed to read current GPIO status of pin %" PRIu8, pin);

  *data = (reg_value >> port_pin) & 0x1;

  return ESP_OK;
}

esp_err_t aw9523_gpio_write_pins(const aw9523_t* dev,
                                 const aw9523_pins_data_digital_t* data) {
  for (_aw9523_port_num_t port_num = 0; port_num < 2; port_num++) {
    const aw9523_reg_addr_t reg =
        port_num == 0 ? AW9523_REG_GPIO_OUTPUT_P0 : AW9523_REG_GPIO_OUTPUT_P1;
    aw9523_reg_value_t reg_value = 0x00;

    for (_aw9523_port_pin_num_t port_pin = 0; port_pin < 8; port_pin++) {
      const aw9523_pin_num_t pin = port_num * 8 + port_pin;
      const aw9523_pin_data_digital_t pin_data = (*data)[pin];

      if (pin_data) {
        reg_value |= 0x1 << port_pin;
      }
    }

    ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, reg, reg_value), TAG,
                        "Failed to write GPIO output for port %" PRIu8,
                        port_num);
  }

  return ESP_OK;
}

esp_err_t aw9523_gpio_write_pin(const aw9523_t* dev, const aw9523_pin_num_t pin,
                                const aw9523_pin_data_digital_t data) {
  _aw9523_port_num_t port_num = 0;
  _aw9523_port_pin_num_t port_pin = 0;

  _aw9523_get_port_pin_num(pin, &port_num, &port_pin);

  const aw9523_reg_addr_t reg =
      port_num == 0 ? AW9523_REG_GPIO_OUTPUT_P0 : AW9523_REG_GPIO_OUTPUT_P1;

  aw9523_reg_value_t reg_value;
  ESP_RETURN_ON_ERROR(aw9523_read_reg(dev, reg, &reg_value), TAG,
                      "Failed to read current GPIO output of pin %" PRIu8, pin);

  if (data) {
    reg_value |= 0x1 << port_pin;
  } else {
    reg_value &= ~(0x1 << port_pin);
  }

  ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, reg, reg_value), TAG,
                      "Failed to write GPIO output of pin %" PRIu8, pin);

  return ESP_OK;
}

esp_err_t aw9523_set_gpio_interrupt_pins(
    const aw9523_t* dev,
    const aw9523_pins_interrupt_mode_t* pins_interrupt_mode) {
  aw9523_reg_value_t reg_value;

  for (_aw9523_port_num_t port_num = 0; port_num < 2; port_num++) {
    reg_value = 0x00;

    for (_aw9523_port_pin_num_t port_pin = 0; port_pin < 8; port_pin++) {
      const aw9523_pin_num_t pin = port_num * 8 + port_pin;
      const aw9523_pin_interrupt_mode_t interrupt_mode =
          (*pins_interrupt_mode)[pin];

      if (interrupt_mode) {
        reg_value |= 0x1 << port_pin;
      }
    }

    const aw9523_reg_addr_t reg = port_num == 0 ? AW9523_REG_GPIO_INTERRUPT_P0
                                                : AW9523_REG_GPIO_INTERRUPT_P1;

    ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, reg, reg_value), TAG,
                        "Failed to write interrupt register for port %" PRIu8,
                        port_num);
  }

  return ESP_OK;
}

esp_err_t aw9523_set_gpio_interrupt_pin(
    const aw9523_t* dev, const aw9523_pin_num_t pin,
    const aw9523_pin_interrupt_mode_t pin_interrupt_mode) {
  _aw9523_port_num_t port_num = 0;
  _aw9523_port_pin_num_t port_pin = 0;

  _aw9523_get_port_pin_num(pin, &port_num, &port_pin);

  aw9523_reg_value_t reg_value = 0x00;
  const aw9523_reg_addr_t reg = port_num == 0 ? AW9523_REG_GPIO_INTERRUPT_P0
                                              : AW9523_REG_GPIO_INTERRUPT_P1;

  ESP_RETURN_ON_ERROR(aw9523_read_reg(dev, reg, &reg_value), TAG,
                      "Failed to read interrupt register of pin %" PRIu8, pin);

  if (pin_interrupt_mode) {
    reg_value |= 0x1 << port_pin;
  } else {
    reg_value &= ~(0x1 << port_pin);
  }

  ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, reg, reg_value), TAG,
                      "Failed to write interrupt register of pin %" PRIu8, pin);

  return ESP_OK;
}

esp_err_t aw9523_set_gpio_output_mode_p0(
    const aw9523_t* dev, const aw9523_gpio_output_mode_t gpio_output_mode) {
  aw9523_reg_value_t reg_value = 0x00;
  ESP_RETURN_ON_ERROR(aw9523_read_reg(dev, AW9523_REG_CONTROL, &reg_value), TAG,
                      "Failed to read device control register");

  if (gpio_output_mode) {
    reg_value |= 0x1 << 4;
  } else {
    reg_value &= ~(0x1 << 4);
  }

  ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, AW9523_REG_CONTROL, reg_value), TAG,
                      "Failed to write device control register");

  return ESP_OK;
}

esp_err_t aw9523_set_led_max_current(
    const aw9523_t* dev, const aw9523_led_max_current_t max_current) {
  aw9523_reg_value_t reg_value = 0x00;
  ESP_RETURN_ON_ERROR(aw9523_read_reg(dev, AW9523_REG_CONTROL, &reg_value), TAG,
                      "Failed to read device control register");

  reg_value &= ~0x3;
  reg_value |= max_current;

  ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, AW9523_REG_CONTROL, reg_value), TAG,
                      "Failed to write device control register");

  return ESP_OK;
}

esp_err_t aw9523_set_led_brightness(const aw9523_t* dev,
                                    const aw9523_pin_num_t pin,
                                    const aw9523_reg_value_t brightness) {
  const aw9523_reg_addr_t reg =
      _aw9523_get_led_brightness_reg_from_pin_num(pin);
  ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, reg, brightness), TAG,
                      "Failed to write LED brightness register");

  return ESP_OK;
}

esp_err_t aw9523_soft_reset(const aw9523_t* dev) {
  ESP_RETURN_ON_ERROR(aw9523_write_reg(dev, AW9523_REG_SOFT_RESET, 0x00), TAG,
                      "Failed to reset AW9523 GPIO expander");

  vTaskDelay(2 / portTICK_PERIOD_MS);

  return ESP_OK;
}

esp_err_t aw9523_destroy(aw9523_t* dev) {
  ESP_RETURN_ON_ERROR(aw9523_soft_reset(dev), TAG,
                      "Failed to soft reset AW9523 GPIO expander");

  ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(*dev), TAG,
                      "Failed to remove GPIO device from I2C master bus");

  *dev = NULL;

  return ESP_OK;
}
