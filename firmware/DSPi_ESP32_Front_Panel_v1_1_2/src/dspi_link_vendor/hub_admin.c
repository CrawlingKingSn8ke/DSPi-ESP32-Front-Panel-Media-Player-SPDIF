/*
 * hub_admin.c - administering the hub: its statistics and name, its
 * settings, its Wi-Fi, its PIN and paired clients, and restarting or
 * resetting it.
 *
 * Implements Link spec 7.3 (auth.list, auth.revoke, auth.set_role), 7.9
 * (hub.stats, hub.rename) and 7.10 to 7.13, as bridge spec 8.7 and 8.9 to
 * 8.11 describe, and the setup page's PIN calls (bridge spec 9.3). The
 * messages that change something require admin; on the bridge every
 * authenticated session is admin.
 *
 * Portable: no ESP-IDF headers.
 */

#include <stdio.h>
#include <string.h>

#include "hub_config.h"
#include "hub_internal.h"

bool hub_require_admin(hub_t *hub, int slot, int id)
{
    if (hub->sessions[slot].role != LINK_ROLE_ADMIN) {
        hub_reply_error(hub, slot, id, "denied", "admin role required");
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Statistics and the hub's name (Link spec 7.9)
 * ------------------------------------------------------------------------- */

void hub_handle_hub_stats(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    if (!hub_require_admin(hub, slot, id)) {
        return;
    }
    unsigned open_sessions = 0;
    for (size_t i = 0; i < hub->config.max_sessions; i++) {
        open_sessions += hub->sessions[i].state == HUB_SESSION_OPEN ? 1 : 0;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "sessions", open_sessions);
    cJSON_AddNumberToObject(body, "uptime_s", (double)(hub_uptime_ms(hub) / 1000));

    cJSON *devices = cJSON_AddArrayToObject(body, "devices");
    if (hub->device.state != HUB_DEVICE_ABSENT) {
        cJSON *device = cJSON_CreateObject();
        cJSON_AddNumberToObject(device, "handle", HUB_DEVICE_HANDLE);
        cJSON_AddNumberToObject(device, "cmds", hub->device.commands);
        cJSON_AddNumberToObject(device, "errors", hub->device.errors);
        cJSON_AddNumberToObject(device, "avg_rtt_ms", hub->device.average_rtt_ms);
        cJSON_AddNumberToObject(device, "notify_dropped", hub->device.notifications_dropped);
        hub_json_append(devices, device);
    }

    cJSON *bridge = cJSON_AddObjectToObject(body, "bridge");
    if (hub->io.describe_bridge != NULL && bridge != NULL) {
        hub->io.describe_bridge(hub->io.context, bridge);
    }
    hub_reply_ok(hub, slot, id, body);
}

hub_change_result_t hub_rename(hub_t *hub, const char *name)
{
    if (!settings_name_is_valid(name)) {
        return HUB_CHANGE_INVALID;
    }
    bridge_settings_t settings = hub->config.settings;
    snprintf(settings.name, sizeof(settings.name), "%s", name);
    if (!hub_apply_settings(hub, &settings)) {
        return HUB_CHANGE_FAILED;
    }
    hub_broadcast_config(hub);
    return HUB_CHANGE_OK;
}

void hub_handle_hub_rename(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!hub_require_admin(hub, slot, id)) {
        return;
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(message, "name");
    const hub_change_result_t result =
        cJSON_IsString(name) ? hub_rename(hub, name->valuestring) : HUB_CHANGE_INVALID;
    if (result == HUB_CHANGE_INVALID) {
        hub_reply_error(hub, slot, id, "bad_request", "name must be 1 to 63 bytes of UTF-8");
    } else if (result == HUB_CHANGE_FAILED) {
        hub_reply_error(hub, slot, id, "internal", "the name could not be saved");
    } else {
        hub_reply_ok(hub, slot, id, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Settings (Link spec 7.10)
 * ------------------------------------------------------------------------- */

bool hub_apply_settings(hub_t *hub, const bridge_settings_t *settings)
{
    if (!hub->io.apply_settings(hub->io.context, settings)) {
        /* The I/O may have saved them, or put some into effect, before it
         * failed (hub_io.h). Putting the old ones back, saved and running,
         * keeps the promise that a failed change leaves everything as it
         * was (bridge spec 8.10); if even that fails, the I/O logs it. */
        (void)hub->io.apply_settings(hub->io.context, &hub->config.settings);
        return false;
    }
    hub->config.settings = *settings;
    hub->io.advertise(hub->io.context);
    return true;
}

void hub_broadcast_config(hub_t *hub)
{
    cJSON *event = cJSON_CreateObject();
    if (event == NULL) {
        return;
    }
    cJSON_AddStringToObject(event, "t", "hub.config.changed");
    hub_json_add(event, "config", hub_config_to_json(&hub->config.settings));
    hub_broadcast_json(hub, event);
}

void hub_handle_config_get(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    if (!hub_require_admin(hub, slot, id)) {
        return;
    }
    cJSON *body = cJSON_CreateObject();
    hub_json_add(body, "config", hub_config_to_json(&hub->config.settings));
    hub_json_add(body, "options", hub_config_options(hub->config.board));
    hub_reply_ok(hub, slot, id, body);
}

void hub_handle_config_set(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!hub_require_admin(hub, slot, id)) {
        return;
    }
    bridge_settings_t updated;
    const char *problem = hub_config_apply(cJSON_GetObjectItemCaseSensitive(message, "config"),
                                           &hub->config.settings, hub->config.board, &updated);
    if (problem != NULL) {
        hub_reply_error_field(hub, slot, id, "bad_request", "a setting is unknown or not allowed",
                              problem);
        return;
    }
    if (!hub_apply_settings(hub, &updated)) {
        hub_reply_error(hub, slot, id, "internal", "the settings could not be saved or applied");
        return;
    }

    /* The reply leads hub.config.changed (Link spec 7.10). */
    cJSON *body = cJSON_CreateObject();
    hub_json_add(body, "config", hub_config_to_json(&updated));
    hub_reply_ok(hub, slot, id, body);
    hub_broadcast_config(hub);
}

/* ---------------------------------------------------------------------------
 * Wi-Fi (Link spec 7.11)
 * ------------------------------------------------------------------------- */

void hub_handle_wifi_scan(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    if (!hub_require_admin(hub, slot, id)) {
        return;
    }
    /* A scan takes seconds; the I/O runs it in the background and answers
     * through hub_reply_later(). */
    const hub_io_result_t started =
        hub->io.start_wifi_scan(hub->io.context, slot, hub->sessions[slot].id, id);
    if (started == HUB_IO_BUSY) {
        hub_reply_error(hub, slot, id, "busy", "a scan is already running");
    } else if (started != HUB_IO_OK) {
        hub_reply_error(hub, slot, id, "internal", "the bridge could not start a scan");
    }
}

void hub_handle_wifi_join(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!hub_require_admin(hub, slot, id)) {
        return;
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(message, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(message, "password");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password) ||
        !hub->io.wifi_join(hub->io.context, ssid->valuestring, password->valuestring)) {
        hub_reply_error(hub, slot, id, "bad_request",
                        "ssid must be 1 to 32 bytes and password empty or 8 to 63 characters");
        return;
    }
    /* `ok` means accepted. Every session closes while the bridge moves;
     * clients find it again by its hub id (Link spec 7.11). */
    hub_reply_ok(hub, slot, id, NULL);
    hub_close_sessions(hub, CLOSE_SHUTTING_DOWN, -1);
}

/* ---------------------------------------------------------------------------
 * The PIN and paired clients (Link spec 7.3 and 7.12)
 * ------------------------------------------------------------------------- */

/* Room for an ISO-8601 time; generous, because the compiler cannot see that
 * the year has four digits. */
#define TIME_TEXT_BYTES 32

/* Writes `unix_time` as ISO-8601 UTC ("2026-09-08T10:12:00Z") using the
 * civil-from-days algorithm, which needs no time library. */
static void format_time(uint32_t unix_time, char out[TIME_TEXT_BYTES])
{
    const int64_t days = unix_time / 86400;
    const uint32_t seconds = unix_time % 86400;
    const int64_t shifted = days + 719468;
    const int64_t era = shifted / 146097;
    const int64_t day_of_era = shifted - era * 146097;
    const int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t month_index = (5 * day_of_year + 2) / 153;
    const int day = (int)(day_of_year - (153 * month_index + 2) / 5 + 1);
    const int month = (int)(month_index < 10 ? month_index + 3 : month_index - 9);
    const int year = (int)(year_of_era + era * 400 + (month <= 2 ? 1 : 0));
    snprintf(out, TIME_TEXT_BYTES, "%04d-%02d-%02dT%02u:%02u:%02uZ", year, month, day,
             (unsigned)(seconds / 3600), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60));
}

static bool client_online(const hub_t *hub, uint16_t cid)
{
    for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
        if (hub->sessions[slot].state == HUB_SESSION_OPEN && hub->sessions[slot].cid == cid) {
            return true;
        }
    }
    return false;
}

void hub_handle_auth_list(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    if (!hub_require_admin(hub, slot, id)) {
        return;
    }
    size_t count = 0;
    const hub_auth_client_t *clients = hub_auth_clients(&hub->auth, &count);
    cJSON *body = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(body, "clients");
    for (size_t i = 0; i < count; i++) {
        if (!clients[i].used) {
            continue;
        }
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "cid", clients[i].cid);
        cJSON_AddStringToObject(entry, "name", clients[i].name);
        cJSON_AddStringToObject(entry, "role", "admin");
        char when[TIME_TEXT_BYTES];
        /* A time taken before the clock was set is left out (bridge spec 8.9). */
        if (clients[i].created != 0) {
            format_time(clients[i].created, when);
            cJSON_AddStringToObject(entry, "created", when);
        }
        if (clients[i].last_seen != 0) {
            format_time(clients[i].last_seen, when);
            cJSON_AddStringToObject(entry, "last_seen", when);
        }
        cJSON_AddBoolToObject(entry, "online", client_online(hub, clients[i].cid));
        hub_json_append(list, entry);
    }
    hub_reply_ok(hub, slot, id, body);
}

void hub_handle_auth_revoke(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!hub_require_admin(hub, slot, id)) {
        return;
    }
    int cid = 0;
    if (!hub_json_int(message, "cid", &cid) || cid <= 0 || cid > 0xFFFF ||
        !hub_auth_revoke(&hub->auth, (uint16_t)cid)) {
        hub_reply_error(hub, slot, id, "bad_request", "no such client");
        return;
    }
    hub_reply_ok(hub, slot, id, NULL);
    /* The revoked client's live sessions close at once (Link spec 6.2). */
    hub_close_client_sessions(hub, (uint16_t)cid, CLOSE_TOKEN_REVOKED, -1);
}

void hub_handle_auth_set_role(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    /* With a fixed PIN every paired client is admin (bridge spec 8.9). */
    hub_reply_error(hub, slot, id, "unsupported", "every client of this hub is admin");
}

bool hub_pin_enabled(const hub_t *hub)
{
    return hub_auth_pin_enabled(&hub->auth);
}

hub_auth_result_t hub_check_pin(hub_t *hub, uint32_t address, const char *pin)
{
    return hub_auth_check_pin(&hub->auth, address, pin);
}

/* Sets the PIN (or clears it, `pin` NULL) for hub.auth.set and the setup
 * page alike. Every paired client is forgotten, so no session keeps a
 * client id; with a new PIN every session but `keep_slot` (-1 for none) is
 * closed to pair again. */
static hub_auth_result_t change_pin(hub_t *hub, const char *pin, int keep_slot)
{
    const hub_auth_result_t result = hub_auth_set_pin(&hub->auth, pin);
    if (result != HUB_AUTH_OK) {
        return result;
    }
    for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
        hub->sessions[slot].cid = 0;
    }
    if (pin != NULL) {
        hub_close_sessions(hub, CLOSE_TOKEN_REVOKED, keep_slot);
    }
    hub->io.advertise(hub->io.context);
    return HUB_AUTH_OK;
}

hub_auth_result_t hub_set_pin(hub_t *hub, const char *pin)
{
    return change_pin(hub, pin, -1);
}

/* Replies to a failed PIN change. */
static void reply_pin_failure(hub_t *hub, int slot, int id, hub_auth_result_t result)
{
    if (result == HUB_AUTH_INVALID_PIN) {
        hub_reply_error(hub, slot, id, "bad_request", "the PIN must be six digits");
    } else {
        hub_reply_error(hub, slot, id, "internal", "the bridge could not store the PIN");
    }
}

void hub_handle_hub_auth_set(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!hub_require_admin(hub, slot, id)) {
        return;
    }
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(message, "mode");
    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(message, "pin");
    const bool mode_none = cJSON_IsString(mode) && strcmp(mode->valuestring, "none") == 0;
    const bool mode_pin = cJSON_IsString(mode) && strcmp(mode->valuestring, "pin") == 0;
    if (!mode_none && !mode_pin) {
        hub_reply_error(hub, slot, id, "bad_request", "mode must be none or pin");
        return;
    }
    if (mode_pin && pin == NULL) {
        hub_reply_error(hub, slot, id, "unsupported", "this hub uses a fixed PIN only");
        return;
    }
    if (mode_pin && !cJSON_IsString(pin)) {
        reply_pin_failure(hub, slot, id, HUB_AUTH_INVALID_PIN);
        return;
    }

    /* `none`: everyone stays connected and is admin. `pin`: every token is
     * revoked; the client that set the PIN stays paired with a fresh token
     * and everyone else pairs again (Link spec 7.12). */
    const hub_auth_result_t changed = change_pin(hub, mode_pin ? pin->valuestring : NULL, slot);
    if (changed != HUB_AUTH_OK) {
        reply_pin_failure(hub, slot, id, changed);
        return;
    }
    if (mode_none) {
        hub_reply_ok(hub, slot, id, NULL);
        return;
    }
    hub_session_t *session = &hub->sessions[slot];
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    if (hub_auth_issue(&hub->auth, session->client_name, token, &session->cid, NULL) !=
        HUB_AUTH_OK) {
        hub_reply_error(hub, slot, id, "internal", "the PIN is set, but pair again to get a token");
        return;
    }
    session->last_touch_ms = hub_now(hub);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "token", token);
    hub_reply_ok(hub, slot, id, body);
}

/* ---------------------------------------------------------------------------
 * Restart and reset (Link spec 7.13)
 * ------------------------------------------------------------------------- */

void hub_restart(hub_t *hub, hub_restart_level_t level)
{
    hub_close_sessions(hub, CLOSE_SHUTTING_DOWN, -1);
    hub->io.restart(hub->io.context, level);
}

static void restart(hub_t *hub, int slot, int id, hub_restart_level_t level)
{
    hub_reply_ok(hub, slot, id, NULL);
    hub_restart(hub, level);
}

void hub_handle_restart(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    if (hub_require_admin(hub, slot, id)) {
        restart(hub, slot, id, HUB_RESTART_ONLY);
    }
}

void hub_handle_reset(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!hub_require_admin(hub, slot, id)) {
        return;
    }
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(message, "level");
    if (cJSON_IsString(level) && strcmp(level->valuestring, "network") == 0) {
        restart(hub, slot, id, HUB_RESTART_NETWORK);
    } else if (cJSON_IsString(level) && strcmp(level->valuestring, "factory") == 0) {
        restart(hub, slot, id, HUB_RESTART_FACTORY);
    } else {
        hub_reply_error(hub, slot, id, "bad_request", "level must be network or factory");
    }
}
