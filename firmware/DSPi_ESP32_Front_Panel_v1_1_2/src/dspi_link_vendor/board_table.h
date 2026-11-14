/*
 * board_table.h - the pins the bridge may use on each supported chip.
 *
 * Implements bridge spec 4.4. One table per chip lists, for each job a pin
 * can do, every pin the bridge offers for it. Anything not listed is never
 * offered: flash and PSRAM pins, USB pins, the boot console UART, strapping
 * pins, input-only pins (they have no pull-up, which an RX line needs), and
 * pins that some common modules do not break out.
 *
 * The lists are explicit arrays rather than bitmasks so a reader can check
 * them against a datasheet line by line. They are sorted ascending, which is
 * also the order the settings page shows them in.
 *
 * This header and board_tables.c are portable (no ESP-IDF headers), so the
 * host tests check all four tables in one run. board.h picks the table for
 * the chip the firmware is running on.
 */

#ifndef BOARD_TABLE_H
#define BOARD_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Marks "no pin", for example an LED that is not fitted. */
#define BOARD_PIN_NONE 0xFF

/* A read-only list of GPIO numbers, sorted ascending. */
typedef struct {
    const uint8_t *pins;
    size_t count;
} board_pin_list_t;

/* Everything the bridge needs to know about one chip's pins. */
typedef struct {
    const char *chip_name;       /* As in hub.stats: "esp32", "esp32s3", ... */
    uint8_t default_uart_tx_pin; /* Wired to the DSPi's UART RX */
    uint8_t default_uart_rx_pin; /* Wired to the DSPi's UART TX */
    uint8_t boot_button_pin;     /* The BOOT button on every common board */
    board_pin_list_t uart_tx_pins;
    board_pin_list_t uart_rx_pins; /* Pins with a pull-up, so an unwired RX idles high */
    board_pin_list_t led_pins;     /* Outputs, including on-board LED pins */
} board_table_t;

/* One table per chip. They are constant data fixed at build time, not
 * state, and are public so the host tests can check all four in one run;
 * firmware code reaches its own through board_table(). */
extern const board_table_t BOARD_TABLE_ESP32;
extern const board_table_t BOARD_TABLE_ESP32S3;
extern const board_table_t BOARD_TABLE_ESP32C3;
extern const board_table_t BOARD_TABLE_ESP32C6;

/* True when `pin` appears in `list`. Any task may call it. */
bool board_pin_list_contains(board_pin_list_t list, uint8_t pin);

#endif /* BOARD_TABLE_H */
