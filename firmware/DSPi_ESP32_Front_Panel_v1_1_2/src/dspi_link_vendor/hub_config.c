/*
 * hub_config.c - the bridge's settings as hub.config JSON, and applying a
 * change to them (see hub_config.h). Kept apart from the hub's messages so
 * the JSON shape and its checks can be read and tested on their own.
 *
 * Implements Link spec 7.10 for the keys of bridge spec 8.11.
 *
 * Portable: no ESP-IDF headers.
 */

#include "hub_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bridge_config.h"

/* The LED types by their names on the wire, in settings_led_type_t order. */
static const char *const LED_TYPE_NAMES[] = {"none", "gpio", "gpio_active_low", "ws2812"};
#define LED_TYPE_COUNT (sizeof(LED_TYPE_NAMES) / sizeof(LED_TYPE_NAMES[0]))

/* The largest pin number a client may name: BOARD_PIN_NONE marks "none". */
#define MAX_PIN_NUMBER (BOARD_PIN_NONE - 1)

/* Adds `item` to `parent` (an object when `name` is set, else an array), or
 * deletes it when it cannot be added, so nothing leaks when memory runs
 * out. */
static void add_item(cJSON *parent, const char *name, cJSON *item)
{
    const bool added = name != NULL ? cJSON_AddItemToObject(parent, name, item)
                                    : cJSON_AddItemToArray(parent, item);
    if (!added) {
        cJSON_Delete(item);
    }
}

static cJSON *pin_list(board_pin_list_t list)
{
    cJSON *array = cJSON_CreateArray();
    for (size_t i = 0; array != NULL && i < list.count; i++) {
        add_item(array, NULL, cJSON_CreateNumber(list.pins[i]));
    }
    return array;
}

cJSON *hub_config_to_json(const bridge_settings_t *settings)
{
    cJSON *config = cJSON_CreateObject();
    if (config == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(config, "name", settings->name);
    cJSON_AddStringToObject(config, "hostname", settings->hostname);

    cJSON *uart = cJSON_AddObjectToObject(config, "uart");
    cJSON_AddNumberToObject(uart, "tx_pin", settings->uart_tx_pin);
    cJSON_AddNumberToObject(uart, "rx_pin", settings->uart_rx_pin);
    cJSON_AddNumberToObject(uart, "baud", settings->uart_baud);
    cJSON_AddBoolToObject(uart, "detect_baud", settings->uart_detect_baud);

    cJSON *led = cJSON_AddObjectToObject(config, "led");
    const unsigned type = (unsigned)settings->led_type;
    cJSON_AddStringToObject(led, "type", type < LED_TYPE_COUNT ? LED_TYPE_NAMES[type] : "none");
    if (settings->led_pin == BOARD_PIN_NONE) {
        cJSON_AddNullToObject(led, "pin");
    } else {
        cJSON_AddNumberToObject(led, "pin", settings->led_pin);
    }
    return config;
}

cJSON *hub_config_options(const board_table_t *board)
{
    static const uint32_t BAUDS[] = DSPI_UART_BAUD_CANDIDATES;
    cJSON *options = cJSON_CreateObject();
    if (options == NULL) {
        return NULL;
    }

    cJSON *uart = cJSON_AddObjectToObject(options, "uart");
    add_item(uart, "tx_pin", pin_list(board->uart_tx_pins));
    add_item(uart, "rx_pin", pin_list(board->uart_rx_pins));
    cJSON *bauds = cJSON_AddArrayToObject(uart, "baud");
    for (size_t i = 0; i < sizeof(BAUDS) / sizeof(BAUDS[0]); i++) {
        add_item(bauds, NULL, cJSON_CreateNumber(BAUDS[i]));
    }

    cJSON *led = cJSON_AddObjectToObject(options, "led");
    cJSON *types = cJSON_AddArrayToObject(led, "type");
    for (size_t i = 0; i < LED_TYPE_COUNT; i++) {
        add_item(types, NULL, cJSON_CreateString(LED_TYPE_NAMES[i]));
    }
    add_item(led, "pin", pin_list(board->led_pins));
    return options;
}

/* ---------------------------------------------------------------------------
 * Applying changes
 * ------------------------------------------------------------------------- */

/* Room for the longest dotted path a client can provoke, "uart." or "led."
 * and an unknown key; a longer key is cut, which still names it. */
#define PROBLEM_PATH_BYTES 48

/* Where a failed key's dotted path is written for the caller. Valid until
 * the next call; only the hub task calls this module. */
static char problem_path[PROBLEM_PATH_BYTES];

static const char *problem(const char *parent, const char *key)
{
    if (parent != NULL) {
        snprintf(problem_path, sizeof(problem_path), "%s.%s", parent, key);
    } else {
        snprintf(problem_path, sizeof(problem_path), "%s", key);
    }
    return problem_path;
}

/* A whole number from 0 to `max`, or false. */
static bool read_number(const cJSON *item, uint32_t max, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > max ||
        item->valuedouble != (double)(uint32_t)item->valuedouble) {
        return false;
    }
    *out = (uint32_t)item->valuedouble;
    return true;
}

/* A string that fits `size` bytes with its NUL, copied into `out`. */
static bool read_string(const cJSON *item, char *out, size_t size)
{
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= size) {
        return false;
    }
    snprintf(out, size, "%s", item->valuestring);
    return true;
}

static const char *apply_uart(const cJSON *uart, bridge_settings_t *result)
{
    if (!cJSON_IsObject(uart)) {
        return problem(NULL, "uart");
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, uart)
    {
        uint32_t number = 0;
        /* Only the type and range are checked here; which pins and rates
         * are allowed is settings_model_find_problem()'s to say. */
        if (strcmp(item->string, "tx_pin") == 0 && read_number(item, MAX_PIN_NUMBER, &number)) {
            result->uart_tx_pin = (uint8_t)number;
        } else if (strcmp(item->string, "rx_pin") == 0 &&
                   read_number(item, MAX_PIN_NUMBER, &number)) {
            result->uart_rx_pin = (uint8_t)number;
        } else if (strcmp(item->string, "baud") == 0 && read_number(item, UINT32_MAX, &number)) {
            result->uart_baud = number;
        } else if (strcmp(item->string, "detect_baud") == 0 && cJSON_IsBool(item)) {
            result->uart_detect_baud = cJSON_IsTrue(item);
        } else {
            return problem("uart", item->string); /* Unknown, or the wrong type */
        }
    }
    return NULL;
}

static const char *apply_led(const cJSON *led, bridge_settings_t *result)
{
    if (!cJSON_IsObject(led)) {
        return problem(NULL, "led");
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, led)
    {
        uint32_t number = 0;
        if (strcmp(item->string, "type") == 0 && cJSON_IsString(item)) {
            size_t type = 0;
            while (type < LED_TYPE_COUNT && strcmp(item->valuestring, LED_TYPE_NAMES[type]) != 0) {
                type++;
            }
            if (type == LED_TYPE_COUNT) {
                return problem("led", "type");
            }
            result->led_type = (settings_led_type_t)type;
        } else if (strcmp(item->string, "pin") == 0 && cJSON_IsNull(item)) {
            result->led_pin = BOARD_PIN_NONE;
        } else if (strcmp(item->string, "pin") == 0 && read_number(item, MAX_PIN_NUMBER, &number)) {
            result->led_pin = (uint8_t)number;
        } else {
            return problem("led", item->string);
        }
    }
    /* Switching the LED off needs no pin; save the client from sending null. */
    if (result->led_type == SETTINGS_LED_NONE) {
        result->led_pin = BOARD_PIN_NONE;
    }
    return NULL;
}

const char *hub_config_apply(const cJSON *changes, const bridge_settings_t *current,
                             const board_table_t *board, bridge_settings_t *result)
{
    *result = *current;
    if (!cJSON_IsObject(changes)) {
        return problem(NULL, "config");
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, changes)
    {
        const char *failed = NULL;
        if (strcmp(item->string, "name") == 0) {
            failed = read_string(item, result->name, sizeof(result->name)) ? NULL
                                                                           : problem(NULL, "name");
        } else if (strcmp(item->string, "hostname") == 0) {
            failed = read_string(item, result->hostname, sizeof(result->hostname))
                         ? NULL
                         : problem(NULL, "hostname");
        } else if (strcmp(item->string, "uart") == 0) {
            failed = apply_uart(item, result);
        } else if (strcmp(item->string, "led") == 0) {
            failed = apply_led(item, result);
        } else {
            failed = problem(NULL, item->string); /* Unknown keys are errors (Link spec 7.10) */
        }
        if (failed != NULL) {
            return failed;
        }
    }
    return settings_model_find_problem(result, board);
}
