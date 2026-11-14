/*
 * settings_model.c - defaults and validation for the bridge's settings.
 *
 * Implements bridge spec 8.11 and 11. Kept apart from settings.c, which
 * stores the values, so the rules run in the host tests. Portable: no
 * ESP-IDF headers.
 */

#include "settings_model.h"

#include <stdio.h>
#include <string.h>

#include "bridge_config.h"

static const uint32_t SUPPORTED_BAUDS[] = DSPI_UART_BAUD_CANDIDATES;

/* Writes 16 random bytes as a version 4 UUID (RFC 9562): the version nibble
 * is 4 and the variant bits are 10. */
static void format_uuid_v4(const uint8_t random[16], char out[SETTINGS_HUB_ID_CHARS + 1])
{
    uint8_t bytes[16];
    memcpy(bytes, random, sizeof(bytes));
    bytes[6] = (uint8_t)((bytes[6] & 0x0F) | 0x40);
    bytes[8] = (uint8_t)((bytes[8] & 0x3F) | 0x80);

    snprintf(out, SETTINGS_HUB_ID_CHARS + 1,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
             bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
             bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

void settings_model_defaults(bridge_settings_t *out, const board_table_t *board,
                             const uint8_t mac[6], const uint8_t random[16])
{
    memset(out, 0, sizeof(*out));
    format_uuid_v4(random, out->hub_id);

    /* "XXXX" in bridge spec 2: the MAC's last two bytes tell bridges apart. */
    snprintf(out->name, sizeof(out->name), "DSPi Bridge %02X%02X", mac[4], mac[5]);
    snprintf(out->hostname, sizeof(out->hostname), "dspi-bridge-%02x%02x", mac[4], mac[5]);

    out->uart_tx_pin = board->default_uart_tx_pin;
    out->uart_rx_pin = board->default_uart_rx_pin;
    out->uart_baud = DSPI_UART_DEFAULT_BAUD;
    out->uart_detect_baud = true;
    out->led_type = SETTINGS_LED_NONE;
    out->led_pin = BOARD_PIN_NONE;
}

/* Length of the UTF-8 sequence that starts with `lead`, or 0 if `lead`
 * cannot start one. */
static size_t utf8_sequence_length(uint8_t lead)
{
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        return 2;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        return 4;
    }
    return 0; /* A continuation byte, an overlong lead or beyond U+10FFFF */
}

bool settings_name_is_valid(const char *name)
{
    const size_t length = strlen(name);
    if (length == 0 || length > SETTINGS_NAME_MAX_BYTES) {
        return false;
    }

    /* Structural UTF-8 check: every lead byte is followed by the right number
     * of continuation bytes. Good enough for a display name; it does not
     * reject every overlong 3- and 4-byte form. */
    for (size_t i = 0; i < length;) {
        const uint8_t lead = (uint8_t)name[i];
        if (lead < 0x20 || lead == 0x7F) {
            return false; /* Control characters would break DNS-SD and the UI */
        }
        const size_t sequence = utf8_sequence_length(lead);
        if (sequence == 0 || i + sequence > length) {
            return false;
        }
        for (size_t k = 1; k < sequence; k++) {
            if (((uint8_t)name[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += sequence;
    }
    return true;
}

bool settings_hostname_is_valid(const char *hostname)
{
    const size_t length = strlen(hostname);
    if (length == 0 || length > SETTINGS_HOSTNAME_MAX_CHARS) {
        return false;
    }
    if (hostname[0] == '-' || hostname[length - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        const char c = hostname[i];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool settings_hub_id_is_valid(const char *hub_id)
{
    if (strlen(hub_id) != SETTINGS_HUB_ID_CHARS) {
        return false;
    }
    for (size_t i = 0; i < SETTINGS_HUB_ID_CHARS; i++) {
        const char c = hub_id[i];
        const bool dash_position = (i == 8 || i == 13 || i == 18 || i == 23);
        if (dash_position) {
            if (c != '-') {
                return false;
            }
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool settings_baud_is_supported(uint32_t baud)
{
    for (size_t i = 0; i < sizeof(SUPPORTED_BAUDS) / sizeof(SUPPORTED_BAUDS[0]); i++) {
        if (SUPPORTED_BAUDS[i] == baud) {
            return true;
        }
    }
    return false;
}

static bool led_type_is_known(settings_led_type_t type)
{
    switch (type) {
    case SETTINGS_LED_NONE:
    case SETTINGS_LED_GPIO:
    case SETTINGS_LED_GPIO_ACTIVE_LOW:
    case SETTINGS_LED_WS2812:
        return true;
    }
    return false;
}

const char *settings_model_find_problem(const bridge_settings_t *settings,
                                        const board_table_t *board)
{
    if (!settings_hub_id_is_valid(settings->hub_id)) {
        return "hub_id";
    }
    if (!settings_name_is_valid(settings->name)) {
        return "name";
    }
    if (!settings_hostname_is_valid(settings->hostname)) {
        return "hostname";
    }
    if (!board_pin_list_contains(board->uart_tx_pins, settings->uart_tx_pin)) {
        return "uart.tx_pin";
    }
    if (!board_pin_list_contains(board->uart_rx_pins, settings->uart_rx_pin) ||
        settings->uart_rx_pin == settings->uart_tx_pin) {
        return "uart.rx_pin";
    }
    if (!settings_baud_is_supported(settings->uart_baud)) {
        return "uart.baud";
    }
    if (!led_type_is_known(settings->led_type)) {
        return "led.type";
    }

    if (settings->led_type == SETTINGS_LED_NONE) {
        if (settings->led_pin != BOARD_PIN_NONE) {
            return "led.pin";
        }
    } else {
        const bool shares_uart_pin = settings->led_pin == settings->uart_tx_pin ||
                                     settings->led_pin == settings->uart_rx_pin;
        if (!board_pin_list_contains(board->led_pins, settings->led_pin) || shares_uart_pin) {
            return "led.pin";
        }
    }
    return NULL;
}

/* How many keys settings_model_find_problem() can name: hub_id, name,
 * hostname, uart.tx_pin, uart.rx_pin, uart.baud, led.type and led.pin.
 * Repair fixes at least one per pass, so this bounds its loop. */
#define FIELD_COUNT 8

/* Puts back the default for the key named by `field`, a path returned by
 * settings_model_find_problem. Pins are reset in pairs so a half-changed
 * pair cannot leave both on one pin. Returns false for a key it does not
 * know, so a key added to find_problem without a repair here is caught. */
static bool restore_default(bridge_settings_t *settings, const bridge_settings_t *defaults,
                            const char *field)
{
    if (strcmp(field, "hub_id") == 0) {
        memcpy(settings->hub_id, defaults->hub_id, sizeof(settings->hub_id));
    } else if (strcmp(field, "name") == 0) {
        memcpy(settings->name, defaults->name, sizeof(settings->name));
    } else if (strcmp(field, "hostname") == 0) {
        memcpy(settings->hostname, defaults->hostname, sizeof(settings->hostname));
    } else if (strcmp(field, "uart.tx_pin") == 0 || strcmp(field, "uart.rx_pin") == 0) {
        settings->uart_tx_pin = defaults->uart_tx_pin;
        settings->uart_rx_pin = defaults->uart_rx_pin;
    } else if (strcmp(field, "uart.baud") == 0) {
        settings->uart_baud = defaults->uart_baud;
    } else if (strcmp(field, "led.type") == 0 || strcmp(field, "led.pin") == 0) {
        /* Switch the LED off rather than guess a pin. */
        settings->led_type = SETTINGS_LED_NONE;
        settings->led_pin = BOARD_PIN_NONE;
    } else {
        return false;
    }
    return true;
}

bool settings_model_repair(bridge_settings_t *settings, const bridge_settings_t *defaults,
                           const board_table_t *board, size_t *repaired)
{
    *repaired = 0;
    for (size_t pass = 0; pass < FIELD_COUNT; pass++) {
        const char *field = settings_model_find_problem(settings, board);
        if (field == NULL) {
            return true;
        }
        if (!restore_default(settings, defaults, field)) {
            return false;
        }
        (*repaired)++;
    }
    /* Every key was reset and something is still wrong: the defaults are. */
    return settings_model_find_problem(settings, board) == NULL;
}
