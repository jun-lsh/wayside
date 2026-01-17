#include "hnr26_badge.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "aw9523.h"
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "hal/i2c_types.h"
#include "soc/clk_tree_defs.h"
#include "soc/gpio_num.h"

/**
 * @brief Logging tag to use for this component
 */
static const char* TAG = "hnr26_badge";

/**
 * @brief I2C master bus handle
 */
static i2c_master_bus_handle_t hnr26_badge_bus_handle;
/**
 * @brief AW9523 GPIO expander's I2C device handle
 */
static aw9523_t hnr26_badge_dev;

/**
 * @brief Circular buffer to keep track of the last 2 states of all the AW9523
 * GPIO expander virtual pins
 */
static aw9523_pins_data_digital_t hnr26_badge_virtual_pins_state_partitions[2];
/**
 * @brief Pointer to circular buffer to the latest virtual pins' states
 */
static aw9523_pins_data_digital_t* hnr26_badge_virtual_pins_state_current =
    hnr26_badge_virtual_pins_state_partitions;
/**
 * @brief Pointer to circular buffer to the second latest virtual pins' states
 */
static aw9523_pins_data_digital_t* hnr26_badge_virtual_pins_state_previous =
    &(hnr26_badge_virtual_pins_state_partitions[1]);

/**
 * @brief Track which array index of the circular buffer contains the latest
 * virtual pins' states
 */
static bool hnr26_badge_virtual_pins_state_partition_active = 0;

/**
 * @brief I2C master bus configuration for the HnR'26 hardware badge
 */
static const i2c_master_bus_config_t HNR26_BADGE_BUS_CONFIG = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = GPIO_NUM_7,
    .scl_io_num = GPIO_NUM_6,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .intr_priority = 0,
    .flags.enable_internal_pullup = true,
    .flags.allow_pd = false,
};

/**
 * @brief AW9523 GPIO expander pins mode to set
 */
static const aw9523_pins_mode_t HNR26_BADGE_VIRTUAL_PINS_MODE = {
    AW9523_PIN_GPIO_OUTPUT, /*!< LED for dice face number 5 */
    AW9523_PIN_GPIO_OUTPUT, /*!< LED for dice face number 6 */
    AW9523_PIN_GPIO_OUTPUT, /*!< LED for dice face number 7 */
    AW9523_PIN_GPIO_OUTPUT, /*!< LED for dice face number 8 */
    AW9523_PIN_GPIO_OUTPUT, /*!< LED for dice face number 9 */
    AW9523_PIN_GPIO_OUTPUT, /*!< LED for dice face number 10 */
    AW9523_PIN_GPIO_INPUT,  /*!< Button A (removed) */
    AW9523_PIN_GPIO_INPUT,  /*!< Button B (removed) */
    AW9523_PIN_GPIO_OUTPUT, /*!< LED for dice face number 1 */
    AW9523_PIN_GPIO_OUTPUT, /*!< LED for dice face number 2 */
    AW9523_PIN_GPIO_OUTPUT, /*!< LED for dice face number 3 */
    AW9523_PIN_GPIO_OUTPUT, /*!< LED for dice face number 4 */
    AW9523_PIN_GPIO_INPUT,  /*!< Button up */
    AW9523_PIN_GPIO_INPUT,  /*!< Button down */
    AW9523_PIN_GPIO_INPUT,  /*!< Button left */
    AW9523_PIN_GPIO_INPUT   /*!< Button right */
};

/**
 * @brief Update the circular buffer pointers and state tracker to start storing
 * a new state of the virtual pins
 */
static void hnr26_badge_swap_virtual_pins_state_active_partition() {
  if (hnr26_badge_virtual_pins_state_partition_active == 0) {
    hnr26_badge_virtual_pins_state_current =
        &(hnr26_badge_virtual_pins_state_partitions[1]);
    hnr26_badge_virtual_pins_state_previous =
        hnr26_badge_virtual_pins_state_partitions;
  } else {
    hnr26_badge_virtual_pins_state_current =
        hnr26_badge_virtual_pins_state_partitions;
    hnr26_badge_virtual_pins_state_previous =
        &(hnr26_badge_virtual_pins_state_partitions[1]);
  }

  hnr26_badge_virtual_pins_state_partition_active =
      !hnr26_badge_virtual_pins_state_partition_active;
}

esp_err_t hnr26_badge_get_virtual_pin_from_dice_num(
    const hnr26_badge_dice_t dice_num, aw9523_pin_num_t* pin_num) {
  ESP_RETURN_ON_FALSE(dice_num > 0 && dice_num <= 10, ESP_FAIL, TAG,
                      "Invalid dice number %" PRIu8 " given", dice_num);

  *pin_num = dice_num < 5 ? dice_num + 7 : dice_num - 5;

  return ESP_OK;
}

esp_err_t hnr26_badge_init() {
  ESP_LOGI(TAG, "Initialising AW9523 GPIO expander");

  // Set up I2C bus
  ESP_RETURN_ON_ERROR(
      i2c_new_master_bus(&HNR26_BADGE_BUS_CONFIG, &hnr26_badge_bus_handle), TAG,
      "Failed to initialise I2C master bus");

  // Initialise the GPIO expander
  ESP_RETURN_ON_ERROR(
      aw9523_init(&hnr26_badge_bus_handle, AW9523_I2C_ADDR_AD0_GND_AD1_GND,
                  &hnr26_badge_dev),
      TAG, "Failed to initialise AW9523 GPIO expander");

  // Set GPIO expander pin modes of LEDs and buttons
  ESP_RETURN_ON_ERROR(
      aw9523_set_pins(&hnr26_badge_dev, &HNR26_BADGE_VIRTUAL_PINS_MODE), TAG,
      "Failed to set pin modes");

  // Set P0 to be push-pull output instead of open-drain output
  ESP_RETURN_ON_ERROR(aw9523_set_gpio_output_mode_p0(
                          &hnr26_badge_dev, AW9523_GPIO_OUTPUT_MODE_PUSH_PULL),
                      TAG, "Failed to set port 0 to push-pull output");

  ESP_LOGI(TAG, "Successfully initialised AW9523 GPIO expander");

  return ESP_OK;
}

esp_err_t hnr26_badge_get_led(const hnr26_badge_dice_t dice_num,
                              aw9523_pin_data_digital_t* is_on) {
  aw9523_pin_num_t pin_num;
  ESP_RETURN_ON_ERROR(
      hnr26_badge_get_virtual_pin_from_dice_num(dice_num, &pin_num), TAG,
      "Cannot calculate pin number from given dice number %" PRIu8, dice_num);

  ESP_RETURN_ON_ERROR(aw9523_gpio_read_pin(&hnr26_badge_dev, pin_num,
                                           AW9523_PIN_GPIO_OUTPUT, is_on),
                      TAG, "Failed to read LED %" PRIu8, dice_num);

  return ESP_OK;
}

esp_err_t hnr26_badge_set_led(const hnr26_badge_dice_t dice_num,
                              const aw9523_pin_data_digital_t is_on) {
  aw9523_pin_num_t pin_num;
  ESP_RETURN_ON_ERROR(
      hnr26_badge_get_virtual_pin_from_dice_num(dice_num, &pin_num), TAG,
      "Cannot calculate pin number from given dice number %" PRIu8, dice_num);

  ESP_RETURN_ON_ERROR(aw9523_gpio_write_pin(&hnr26_badge_dev, pin_num, is_on),
                      TAG, "Failed to set LED %" PRIu8 " to %d", dice_num,
                      is_on);

  ESP_LOGV(TAG, "Successfully set dice number %" PRIu8 " to %d", dice_num,
           is_on);
  return ESP_OK;
}

esp_err_t hnr26_badge_update_virtual_pins_state() {
  hnr26_badge_swap_virtual_pins_state_active_partition();

  ESP_RETURN_ON_ERROR(
      aw9523_gpio_read_pins(&hnr26_badge_dev,
                            hnr26_badge_virtual_pins_state_current),
      TAG, "Failed to read GPIO pins");

  return ESP_OK;
}

bool hnr26_badge_get_button_state(const hnr26_badge_button_t button) {
  return (*hnr26_badge_virtual_pins_state_current)[button];
}

bool hnr26_badge_get_previous_button_state(const hnr26_badge_button_t button) {
  return (*hnr26_badge_virtual_pins_state_previous)[button];
}

bool hnr26_badge_get_button_is_pressed(const hnr26_badge_button_t button) {
  return !hnr26_badge_get_previous_button_state(button) &&
         hnr26_badge_get_button_state(button);
}

bool hnr26_badge_get_button_is_held(const hnr26_badge_button_t button) {
  return hnr26_badge_get_previous_button_state(button) &&
         hnr26_badge_get_button_state(button);
}

bool hnr26_badge_get_button_is_released(const hnr26_badge_button_t button) {
  return hnr26_badge_get_previous_button_state(button) &&
         !hnr26_badge_get_button_state(button);
}

bool hnr26_badge_get_button_is_idle(const hnr26_badge_button_t button) {
  return !hnr26_badge_get_previous_button_state(button) &&
         !hnr26_badge_get_button_state(button);
}

esp_err_t hnr26_badge_destroy() {
  ESP_RETURN_ON_ERROR(aw9523_destroy(&hnr26_badge_dev), TAG,
                      "Failed to remove AW9523 GPIO expander");
  return ESP_OK;
}
