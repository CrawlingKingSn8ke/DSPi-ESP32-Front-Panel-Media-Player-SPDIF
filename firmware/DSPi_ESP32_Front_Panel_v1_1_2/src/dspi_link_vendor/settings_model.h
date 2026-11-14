/*
 * settings_model.h - the bridge's saved settings and the rules they obey.
 *
 * Implements the `bridge` namespace of bridge spec 11 and the validation
 * rules of bridge spec 8.11. The struct here is what settings.h loads and
 * saves; the functions decide defaults and validity.
 *
 * Portable: no ESP-IDF headers, so the rules are covered by host tests.
 * settings.c applies them when loading and saving, and the hub applies the
 * same checks to hub.config.set. Nothing here keeps state, so any task may
 * call these functions.
 */

#ifndef SETTINGS_MODEL_H
#define SETTINGS_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_table.h"

/* Bumped whenever the saved layout changes; settings.c migrates older
 * layouts step by step (bridge spec 11). */
#define SETTINGS_SCHEMA_VERSION 1

#define SETTINGS_HUB_ID_CHARS 36       /* A UUID: 8-4-4-4-12 hex digits */
#define SETTINGS_NAME_MAX_BYTES 63     /* DNS-SD instance name limit */
#define SETTINGS_HOSTNAME_MAX_CHARS 63 /* DNS label limit */

/* How the optional status LED is driven (bridge spec 8.11 and 13). */
typedef enum {
    SETTINGS_LED_NONE = 0,
    SETTINGS_LED_GPIO = 1,            /* On when the pin is high */
    SETTINGS_LED_GPIO_ACTIVE_LOW = 2, /* On when the pin is low */
    SETTINGS_LED_WS2812 = 3,          /* One addressable RGB LED */
} settings_led_type_t;

/* Everything in the `bridge` namespace. Strings are NUL-terminated. */
typedef struct {
    /* Link `hid`: a lower-case UUID, stable until a factory reset. */
    char hub_id[SETTINGS_HUB_ID_CHARS + 1];

    /* The hub's name in DNS-SD and hello, in UTF-8. */
    char name[SETTINGS_NAME_MAX_BYTES + 1];

    /* The mDNS hostname, without ".local". */
    char hostname[SETTINGS_HOSTNAME_MAX_CHARS + 1];

    /* The ESP32 pins wired to the DSPi: TX goes to the DSPi's RX, RX to its TX. */
    uint8_t uart_tx_pin;
    uint8_t uart_rx_pin;

    /* The rate in use. When detection is on, the bridge overwrites it with
     * the rate it finds the DSPi at. */
    uint32_t uart_baud;
    bool uart_detect_baud;

    /* The optional status LED; led_pin is BOARD_PIN_NONE when led_type is
     * SETTINGS_LED_NONE. */
    settings_led_type_t led_type;
    uint8_t led_pin;
} bridge_settings_t;

/*
 * Fills `out` with factory settings for this board.
 *
 * mac:    the Wi-Fi MAC address; its last two bytes name the bridge.
 * random: 16 random bytes for the hub id.
 */
void settings_model_defaults(bridge_settings_t *out, const board_table_t *board,
                             const uint8_t mac[6], const uint8_t random[16]);

/*
 * Checks every rule in bridge spec 8.11.
 *
 * Returns NULL when the settings are valid, or the first invalid key as the
 * dotted path Link spec 7.10 uses for `field` ("uart.tx_pin").
 */
const char *settings_model_find_problem(const bridge_settings_t *settings,
                                        const board_table_t *board);

/*
 * Replaces each invalid value with its default from `defaults`.
 *
 * repaired: receives how many keys were replaced; 0 means nothing was wrong.
 *
 * Returns true when `settings` are valid afterwards. False means `defaults`
 * are themselves invalid on this board, a bug in the board table; the
 * settings are then still invalid and must not be saved.
 */
bool settings_model_repair(bridge_settings_t *settings, const bridge_settings_t *defaults,
                           const board_table_t *board, size_t *repaired);

/* True for 1 to 63 bytes of valid UTF-8 with no control characters. */
bool settings_name_is_valid(const char *name);

/* True for a DNS label: 1 to 63 of a-z, 0-9 and '-', no '-' at either end. */
bool settings_hostname_is_valid(const char *hostname);

/* True for a lower-case UUID in 8-4-4-4-12 form. */
bool settings_hub_id_is_valid(const char *hub_id);

/* True when baud is one of DSPI_UART_BAUD_CANDIDATES. */
bool settings_baud_is_supported(uint32_t baud);

#endif /* SETTINGS_MODEL_H */
