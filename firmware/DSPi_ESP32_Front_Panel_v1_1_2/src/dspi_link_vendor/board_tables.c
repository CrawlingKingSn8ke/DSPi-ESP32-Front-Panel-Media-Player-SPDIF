/*
 * board_tables.c - the pin table for each supported chip.
 *
 * Implements bridge spec 4.4. Each table's comment says which pins are left
 * out and why, so the lists can be checked against Espressif's datasheets.
 * test/host/test_board_tables.c checks the tables against those exclusions.
 *
 * Portable: no ESP-IDF headers.
 */

#include "board_table.h"

#define PIN_LIST(array) {.pins = (array), .count = sizeof(array) / sizeof((array)[0])}

bool board_pin_list_contains(board_pin_list_t list, uint8_t pin)
{
    for (size_t i = 0; i < list.count; i++) {
        if (list.pins[i] == pin) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * ESP32 (original)
 *
 * Left out: 0, 2, 5, 12, 15 (strapping); 1, 3 (boot console UART0);
 * 6 to 11 (flash); 16, 17 (PSRAM on WROVER, flash on PICO-D4); 37, 38 (not
 * broken out on WROOM or WROVER). 20, 24 and 28 to 31 do not exist.
 * 34, 35, 36 and 39 are input only and have no pull-up, so an RX line on
 * one floats and reads noise whenever the DSPi is unplugged or off; they
 * are not offered at all.
 * 2 is offered for an LED because many boards have one there.
 * ------------------------------------------------------------------------- */

static const uint8_t ESP32_UART_TX_PINS[] = {4, 13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
static const uint8_t ESP32_UART_RX_PINS[] = {4, 13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
static const uint8_t ESP32_LED_PINS[] = {2, 4, 13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};

const board_table_t BOARD_TABLE_ESP32 = {
    .chip_name = "esp32",
    .default_uart_tx_pin = 19,
    .default_uart_rx_pin = 18,
    .boot_button_pin = 0,
    .uart_tx_pins = PIN_LIST(ESP32_UART_TX_PINS),
    .uart_rx_pins = PIN_LIST(ESP32_UART_RX_PINS),
    .led_pins = PIN_LIST(ESP32_LED_PINS),
};

/* ---------------------------------------------------------------------------
 * ESP32-S3
 *
 * Left out: 0, 3, 45, 46 (strapping); 19, 20 (USB); 26 to 32 (flash and
 * PSRAM); 33 to 37 (octal flash or PSRAM on many modules); 43, 44 (boot
 * console UART0); 47, 48 for the UART (1.8 V on modules with 1.8 V octal
 * PSRAM, too low for the DSPi). 22 to 25 do not exist.
 * 48 is offered for an LED: the ESP32-S3-DevKitC-1 v1.0 has its RGB LED
 * there (v1.1 uses 38).
 * ------------------------------------------------------------------------- */

static const uint8_t ESP32S3_UART_PINS[] = {1,  2,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                                            14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42};
static const uint8_t ESP32S3_LED_PINS[] = {1,  2,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                                           14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 48};

const board_table_t BOARD_TABLE_ESP32S3 = {
    .chip_name = "esp32s3",
    .default_uart_tx_pin = 17,
    .default_uart_rx_pin = 18,
    .boot_button_pin = 0,
    .uart_tx_pins = PIN_LIST(ESP32S3_UART_PINS),
    .uart_rx_pins = PIN_LIST(ESP32S3_UART_PINS),
    .led_pins = PIN_LIST(ESP32S3_LED_PINS),
};

/* ---------------------------------------------------------------------------
 * ESP32-C3
 *
 * Left out: 2, 8, 9 (strapping); 11 (flash supply on most modules); 12 to 17
 * (flash); 18, 19 (USB); 20, 21 (boot console UART0).
 * 8 is offered for an LED: the ESP32-C3-DevKitM-1 and the C3 SuperMini have
 * theirs there.
 * ------------------------------------------------------------------------- */

static const uint8_t ESP32C3_UART_PINS[] = {0, 1, 3, 4, 5, 6, 7, 10};
static const uint8_t ESP32C3_LED_PINS[] = {0, 1, 3, 4, 5, 6, 7, 8, 10};

const board_table_t BOARD_TABLE_ESP32C3 = {
    .chip_name = "esp32c3",
    .default_uart_tx_pin = 5,
    .default_uart_rx_pin = 4,
    .boot_button_pin = 9,
    .uart_tx_pins = PIN_LIST(ESP32C3_UART_PINS),
    .uart_rx_pins = PIN_LIST(ESP32C3_UART_PINS),
    .led_pins = PIN_LIST(ESP32C3_LED_PINS),
};

/* ---------------------------------------------------------------------------
 * ESP32-C6
 *
 * Left out: 4, 5, 8, 9, 15 (strapping); 12, 13 (USB); 16, 17 (boot console
 * UART0); 24 to 30 (flash); 10, 11, 14 (not broken out on the
 * ESP32-C6-WROOM-1).
 * 8 and 15 are offered for an LED: the ESP32-C6-DevKitC-1 has its RGB LED on
 * 8 and the XIAO ESP32C6 its user LED on 15.
 * ------------------------------------------------------------------------- */

static const uint8_t ESP32C6_UART_PINS[] = {0, 1, 2, 3, 6, 7, 18, 19, 20, 21, 22, 23};
static const uint8_t ESP32C6_LED_PINS[] = {0, 1, 2, 3, 6, 7, 8, 15, 18, 19, 20, 21, 22, 23};

const board_table_t BOARD_TABLE_ESP32C6 = {
    .chip_name = "esp32c6",
    .default_uart_tx_pin = 22,
    .default_uart_rx_pin = 23,
    .boot_button_pin = 9,
    .uart_tx_pins = PIN_LIST(ESP32C6_UART_PINS),
    .uart_rx_pins = PIN_LIST(ESP32C6_UART_PINS),
    .led_pins = PIN_LIST(ESP32C6_LED_PINS),
};
