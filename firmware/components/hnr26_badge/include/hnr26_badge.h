/**
 * @file hnr26_badge.h
 * @author Yik Jin (@yikjin) <yikjin@nushackers.org>
 * @brief Hack&Roll 2026 hardware badge I/O library
 * @version 0.0.3
 * @date 2026-01-05
 *
 * @copyright Copyright (c) 2026 NUS Hackers
 *
 * This component simplifies communicating with the I/O peripherals on the
 * Hack&Roll 2026 hardware badge.
 *
 * The buttons and LEDs are connected via the AW9523 GPIO expander module to its
 * 16 virtual pins (consisting of 2 ports of 8 physical pins each).
 * Communication with the AW9523 GPIO expander is handled via the `aw9523`
 * component.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "aw9523.h"
#include "esp_err.h"

/**
 * @brief Dice face number from 1 to 10 inclusive
 */
typedef uint8_t hnr26_badge_dice_t;

/**
 * @brief AW9523 GPIO expander virtual pin number of a button
 */
typedef enum {
  HNR26_BADGE_BTN_UP = 12,    /*!< Virtual pin number of up button */
  HNR26_BADGE_BTN_DOWN = 13,  /*!< Virtual pin number of down button */
  HNR26_BADGE_BTN_LEFT = 14,  /*!< Virtual pin number of left button */
  HNR26_BADGE_BTN_RIGHT = 15, /*!< Virtual pin number of right button */
  HNR26_BADGE_BTN_A = 6,      /*!< Virtual pin number of removed A button */
  HNR26_BADGE_BTN_B = 7       /*!< Virtual pin number of removed B button */
} hnr26_badge_button_t;

/**
 * @brief Calculate the AW9523 GPIO expander virtual pin number from the given
 * dice face number
 *
 * @param[in] dice_num Given dice face number
 * @param[out] pin_num Calculated virtual pin number
 * @return esp_err_t ESP error constants
 */
esp_err_t hnr26_badge_get_virtual_pin_from_dice_num(
    const hnr26_badge_dice_t dice_num, aw9523_pin_num_t* pin_num);

/**
 * @brief Initialise the I/O library
 *
 * @return esp_err_t ESP error constants
 */
esp_err_t hnr26_badge_init();

/**
 * @brief Get the LED status of a given dice face number
 *
 * @param[in] dice_num Given dice face number
 * @param[out] is_on Whether the LED is on or off
 * @return esp_err_t ESP error constants
 */
esp_err_t hnr26_badge_get_led(const hnr26_badge_dice_t dice_num,
                              aw9523_pin_data_digital_t* is_on);

/**
 * @brief Set the LED status of a given dice face number
 *
 * @param[in] dice_num Given dice face number
 * @param[in] is_on Whether to set the LED on or off
 * @return esp_err_t ESP error constants
 */
esp_err_t hnr26_badge_set_led(const hnr26_badge_dice_t dice_num,
                              const aw9523_pin_data_digital_t is_on);

/**
 * @brief Update the GPIO status of all virtual pins on the AW9523 GPIO expander
 *
 * @return esp_err_t ESP error constants
 */
esp_err_t hnr26_badge_update_virtual_pins_state();

/**
 * @brief Get the last known state of the given button
 *
 * @param[in] button Button to get the last known state of
 * @return true Button is pressed
 * @return false Button is released
 */
bool hnr26_badge_get_button_state(const hnr26_badge_button_t button);

/**
 * @brief Get the second last known state of the given button
 *
 * @param[in] button Button to get the second last known state of
 * @return true Button was pressed
 * @return false Button was released
 */
bool hnr26_badge_get_previous_button_state(const hnr26_badge_button_t button);

/**
 * @brief Check if the given button has just been pressed
 *
 * @param[in] button Button to check if it is just pressed
 * @return true Button is just pressed
 * @return false Button has not just been pressed
 */
bool hnr26_badge_get_button_is_pressed(const hnr26_badge_button_t button);

/**
 * @brief Check if the given button is being held down
 *
 * @param[in] button Button to check if it is being held down
 * @return true Button is being held down
 * @return false Button is not being held down
 */
bool hnr26_badge_get_button_is_held(const hnr26_badge_button_t button);

/**
 * @brief Check if the given button has just been released
 *
 * @param[in] button Button to check if it is just released
 * @return true Button is just released
 * @return false Button has not just been released
 */
bool hnr26_badge_get_button_is_released(const hnr26_badge_button_t button);

/**
 * @brief Check if the given button has not been pressed during the last 2
 * checked states
 *
 * @param[in] button Button to check if it has not been pressed recently
 * @return true Button has not been pressed recently
 * @return false Button has been pressed recently
 */
bool hnr26_badge_get_button_is_idle(const hnr26_badge_button_t button);

/**
 * @brief Destructor function to remove resources when no longer used
 *
 * @return esp_err_t ESP error constants
 */
esp_err_t hnr26_badge_destroy();
