/*
 * hub_internal.h - functions the hub's source files share. Not part of the
 * public API (hub.h); like it, everything here runs on the hub task only.
 *
 * The hub (bridge spec 8, implementing Link spec 5 to 8) is split by
 * subject so each file can be read on its own:
 *
 *   hub.c           sessions, the message dispatch table, hello, keepalive,
 *                   replies and the info document
 *   hub_commands.c  CMD frames, the queue of requests to the uart task and
 *                   its lost answers, origin attribution
 *   hub_device.c    the device record and its events, notifications,
 *                   snapshots, locks, names
 *   hub_polls.c     poll subscriptions and their schedule
 *   hub_admin.c     administering the hub: statistics, its name, settings,
 *                   Wi-Fi, the PIN and paired clients, restart and reset
 *   hub_config.c    the settings as hub.config JSON (hub_config.h)
 *   hub_update.c    the hub's own firmware update (Link spec 7.14)
 */

#ifndef HUB_INTERNAL_H
#define HUB_INTERNAL_H

#include "cJSON.h"
#include "hub.h"

/* WebSocket close codes the hub sends (Link spec 5). */
#define CLOSE_KEEPALIVE_FAILURE 1001
#define CLOSE_PROTOCOL_ERROR 4000
#define CLOSE_AUTH_TIMEOUT 4001
#define CLOSE_TOKEN_REVOKED 4002
#define CLOSE_SHUTTING_DOWN 4003
#define CLOSE_INTERNAL_ERROR 1011 /* The hub ran out of memory for the session */

/* ---------------------------------------------------------------------------
 * hub.c
 * ------------------------------------------------------------------------- */

uint32_t hub_now(const hub_t *hub);

/* The session on `slot` if it has said hello, else NULL. */
hub_session_t *hub_open_session(hub_t *hub, int slot);

/* Sends `message` to one session and deletes it. Returns false if the
 * session's queue was full. */
bool hub_send_json(hub_t *hub, int slot, cJSON *message);

/* Sends `message` to every open session and deletes it. */
void hub_broadcast_json(hub_t *hub, cJSON *message);

/* Sends a binary frame to one session. */
bool hub_send_binary(hub_t *hub, int slot, const uint8_t *data, size_t length);

/* Replies {"t":"ok","id":id, ...body}. Takes ownership of `body`, which may
 * be NULL for an empty reply. */
void hub_reply_ok(hub_t *hub, int slot, int id, cJSON *body);

/* Replies {"t":"err","id":id,"code":code,"msg":msg}. `msg` may be NULL. */
void hub_reply_error(hub_t *hub, int slot, int id, const char *code, const char *msg);

/* The same, with the offending setting's dotted path as "field" (Link spec
 * 7.10). */
void hub_reply_error_field(hub_t *hub, int slot, int id, const char *code, const char *msg,
                           const char *field);

/* Adds `item` to `object` as `name`, or to `array`. On failure (`item`
 * NULL, or no memory) `item` is deleted, so a half-built message never
 * leaks. Returns whether it was added. */
bool hub_json_add(cJSON *object, const char *name, cJSON *item);
bool hub_json_append(cJSON *array, cJSON *item);

/* Reads an integer member. False if absent or not a number. */
bool hub_json_int(const cJSON *object, const char *name, int *value);

/* Asks the network side to close a session with `code`, and stops serving
 * it at once. */
void hub_close_session(hub_t *hub, int slot, uint16_t code);

/* Closes every live session except `except_slot` (-1 for none). */
void hub_close_sessions(hub_t *hub, uint16_t code, int except_slot);

/* Closes the open sessions of paired client `cid` (none for 0), except
 * `except_slot` (-1 for none). */
void hub_close_client_sessions(hub_t *hub, uint16_t cid, uint16_t code, int except_slot);

/* Milliseconds since hub_init(), in 64 bits so hub.stats never wraps. */
uint64_t hub_uptime_ms(hub_t *hub);

/* "pin" or "none", as hello, the TXT record and the info endpoint say. */
const char *hub_auth_mode(const hub_t *hub);

/* ---------------------------------------------------------------------------
 * hub_admin.c
 * ------------------------------------------------------------------------- */

/* True when the session may administer the hub; otherwise replies `denied`
 * and returns false (Link spec 7.9). */
bool hub_require_admin(hub_t *hub, int slot, int id);

/* Makes `settings` current: saves and applies them through the I/O and
 * advertises. False, keeping the old settings, if the I/O could not. The
 * caller then announces them with hub_broadcast_config(), after any reply
 * that should lead it. */
bool hub_apply_settings(hub_t *hub, const bridge_settings_t *settings);

/* Tells every session the current settings with hub.config.changed. */
void hub_broadcast_config(hub_t *hub);

void hub_handle_hub_stats(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_hub_rename(hub_t *hub, int slot, int id, const cJSON *message);

void hub_handle_auth_list(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_auth_revoke(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_auth_set_role(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_hub_auth_set(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_config_get(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_config_set(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_wifi_scan(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_wifi_join(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_restart(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_reset(hub_t *hub, int slot, int id, const cJSON *message);

/* ---------------------------------------------------------------------------
 * hub_commands.c
 * ------------------------------------------------------------------------- */

/* Handles one CMD request frame (Link spec 8.2, bridge spec 8.3). */
void hub_commands_handle_frame(hub_t *hub, int slot, const uint8_t *data, size_t length);

/* Queues a request the hub makes for itself (a poll or a snapshot read).
 * Returns its tag, which is never 0, or 0 if the uart task's queue is
 * full. */
uint32_t hub_submit_own(hub_t *hub, hub_work_kind_t kind, const dspi_request_t *request,
                        bool may_retry, const hub_poll_key_t *poll);

/* The session that most likely caused a notification from `source`, or 0
 * (Link spec 8.3). */
uint16_t hub_take_origin(hub_t *hub, uint8_t source);

/* Finishes the oldest request as a timeout if its answer is overdue by
 * HUB_REPLY_LOST_AFTER_MS, so a lost answer cannot hold the queue. */
void hub_commands_tick(hub_t *hub);

/* ---------------------------------------------------------------------------
 * hub_device.c
 * ------------------------------------------------------------------------- */

/* The device object of Link spec 7.4. */
cJSON *hub_device_json(const hub_t *hub);

/* True when handle 0 is a device that is online. */
bool hub_device_online(const hub_t *hub);

/* False when the DSPi is known to have notifications turned off (bridge
 * spec 5): then `notify` is not offered and the snapshot cache cannot see
 * changes, so every snapshot reads the blob again. */
bool hub_device_notifies(const hub_t *hub);

void hub_handle_device_list(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_device_rename(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_device_snapshot(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_lock_acquire(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_lock_release(hub_t *hub, int slot, int id, const cJSON *message);

/* A forwarded SET or a notification may have changed the settings blob. */
void hub_snapshot_mark_stale(hub_t *hub);

/* Starts the snapshot read that waited for an update to end, if one is
 * wanted. */
void hub_snapshot_resume(hub_t *hub);

void hub_snapshot_on_reply(hub_t *hub, const hub_pending_t *work, const dspi_reply_t *reply);
void hub_device_session_closed(hub_t *hub, int slot, uint16_t session_id);
void hub_device_tick(hub_t *hub);

/* ---------------------------------------------------------------------------
 * hub_update.c
 * ------------------------------------------------------------------------- */

/* True when the I/O can take an update, so hello offers `hub_update`. */
bool hub_update_available(const hub_t *hub);

/* True from hub.update until the update ends. Meanwhile the hub keeps
 * large answers off the UART (HUB_UPDATE_MAX_GET_BYTES). */
bool hub_update_in_progress(const hub_t *hub);

void hub_handle_update(hub_t *hub, int slot, int id, const cJSON *message);

/* Handles one FWDATA frame (Link spec 8.5). */
void hub_update_chunk(hub_t *hub, int slot, const uint8_t *data, size_t length);

void hub_update_session_closed(hub_t *hub, int slot);

/* Gives up on an update that has stopped making progress. */
void hub_update_tick(hub_t *hub);

/* ---------------------------------------------------------------------------
 * hub_polls.c
 * ------------------------------------------------------------------------- */

void hub_handle_poll_subscribe(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_poll_unsubscribe(hub_t *hub, int slot, int id, const cJSON *message);
void hub_polls_session_closed(hub_t *hub, int slot);

/* Ends every subscription: the device went offline (Link spec 7.6). */
void hub_polls_clear(hub_t *hub);

/* Starts the most overdue poll if the link is free. */
void hub_polls_tick(hub_t *hub);

void hub_polls_on_reply(hub_t *hub, const hub_pending_t *work, const dspi_reply_t *reply);

#endif /* HUB_INTERNAL_H */
