/*
 * hub_config.h - the bridge's settings as hub.config.get and hub.config.set
 * see them (Link spec 7.10, bridge spec 8.11).
 *
 *   {"name": "...", "hostname": "...",
 *    "uart": {"tx_pin": 17, "rx_pin": 18, "baud": 1000000, "detect_baud": true},
 *    "led":  {"type": "none", "pin": null}}
 *
 * A change names only the keys it changes; nested objects merge. The result
 * is checked as a whole with settings_model_find_problem(), and an unknown
 * key is an error rather than ignored, so a typo never passes silently.
 *
 * Called only by the hub, on the hub task. Portable: no ESP-IDF headers.
 */

#ifndef HUB_CONFIG_H
#define HUB_CONFIG_H

#include "board_table.h"
#include "cJSON.h"
#include "settings_model.h"

/* The `config` object for `settings`. The caller owns the result; NULL if
 * memory ran out. */
cJSON *hub_config_to_json(const bridge_settings_t *settings);

/* The `options` object: the values a client may choose from on `board`.
 * The caller owns the result; NULL if memory ran out. */
cJSON *hub_config_options(const board_table_t *board);

/*
 * Applies `changes` to `current`, writing the result to `result`.
 * Returns NULL when the result is valid, or the first offending key as a
 * dotted path ("uart.tx_pin") when a key is unknown, has the wrong type, or
 * breaks a rule. The path is a static string, valid until the next call.
 * `result` is only meaningful on success.
 */
const char *hub_config_apply(const cJSON *changes, const bridge_settings_t *current,
                             const board_table_t *board, bridge_settings_t *result);

#endif /* HUB_CONFIG_H */
