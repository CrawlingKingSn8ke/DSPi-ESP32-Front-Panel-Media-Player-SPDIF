/*
 * hub.c - sessions, the message dispatch table, hello and keepalive.
 *
 * Implements Link spec 4 (keepalive), 5 (session lifecycle), 6.1 (`auth:
 * none`), 7.1 (the JSON envelope) and 7.2 (hello), and bridge spec 8.1 and
 * 8.2. See hub_internal.h for how the other hub files divide the rest.
 *
 * Portable: no ESP-IDF headers.
 */

#include <string.h>

#include "hub_internal.h"

/* The protocol version this hub speaks (Link spec 10). */
#define PROTOCOL_MAJOR 1
#define PROTOCOL_MINOR 2

/* An `id` of -1 means the request carried none, so replies leave it out. */
#define NO_ID (-1)

/* ---------------------------------------------------------------------------
 * Helpers shared with the other hub files
 * ------------------------------------------------------------------------- */

uint32_t hub_now(const hub_t *hub)
{
    return hub->io.now_ms(hub->io.context);
}

static bool valid_slot(const hub_t *hub, int slot)
{
    return slot >= 0 && (size_t)slot < hub->config.max_sessions;
}

hub_session_t *hub_open_session(hub_t *hub, int slot)
{
    if (!valid_slot(hub, slot) || hub->sessions[slot].state != HUB_SESSION_OPEN) {
        return NULL;
    }
    return &hub->sessions[slot];
}

bool hub_send_json(hub_t *hub, int slot, cJSON *message)
{
    char *text = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (text == NULL) {
        return false;
    }
    const bool sent = hub->io.send_text(hub->io.context, slot, text, strlen(text));
    cJSON_free(text);
    return sent;
}

void hub_broadcast_json(hub_t *hub, cJSON *message)
{
    /* Printed once, sent to everyone. */
    char *text = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (text == NULL) {
        return;
    }
    const size_t length = strlen(text);
    for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
        if (hub->sessions[slot].state == HUB_SESSION_OPEN) {
            /* A session whose queue is full misses this event; the I/O logs
             * it, and the next event of its kind carries the whole state. */
            (void)hub->io.send_text(hub->io.context, (int)slot, text, length);
        }
    }
    cJSON_free(text);
}

bool hub_send_binary(hub_t *hub, int slot, const uint8_t *data, size_t length)
{
    return hub->io.send_binary(hub->io.context, slot, data, length);
}

bool hub_json_add(cJSON *object, const char *name, cJSON *item)
{
    if (!cJSON_AddItemToObject(object, name, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

bool hub_json_append(cJSON *array, cJSON *item)
{
    if (!cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

/* Starts a reply envelope: {"t": type, "id": id}, or NULL if memory ran
 * out. */
static cJSON *new_reply(const char *type, int id)
{
    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(reply, "t", type);
    if (id != NO_ID) {
        cJSON_AddNumberToObject(reply, "id", id);
    }
    return reply;
}

/* Replies and events are sent once: a session whose queue is full misses
 * one, the I/O logs it, and the client's own timeout covers the request.
 * So the results of hub_send_json() below are deliberately not acted on. */

void hub_reply_ok(hub_t *hub, int slot, int id, cJSON *body)
{
    cJSON *reply = new_reply("ok", id);
    if (reply == NULL) {
        cJSON_Delete(body);
        return;
    }
    if (body != NULL) {
        cJSON *member = body->child;
        while (member != NULL) {
            cJSON *next = member->next;
            cJSON_DetachItemViaPointer(body, member);
            /* `string` is the member's own name, freed with it on failure. */
            if (!cJSON_AddItemToObject(reply, member->string, member)) {
                cJSON_Delete(member);
            }
            member = next;
        }
        cJSON_Delete(body);
    }
    (void)hub_send_json(hub, slot, reply);
}

void hub_reply_error_field(hub_t *hub, int slot, int id, const char *code, const char *msg,
                           const char *field)
{
    cJSON *reply = new_reply("err", id);
    if (reply == NULL) {
        return;
    }
    cJSON_AddStringToObject(reply, "code", code);
    if (msg != NULL) {
        cJSON_AddStringToObject(reply, "msg", msg);
    }
    if (field != NULL) {
        cJSON_AddStringToObject(reply, "field", field);
    }
    (void)hub_send_json(hub, slot, reply);
}

void hub_reply_error(hub_t *hub, int slot, int id, const char *code, const char *msg)
{
    hub_reply_error_field(hub, slot, id, code, msg, NULL);
}

bool hub_json_int(const cJSON *object, const char *name, int *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *value = item->valueint;
    return true;
}

/* ---------------------------------------------------------------------------
 * Sessions
 * ------------------------------------------------------------------------- */

void hub_init(hub_t *hub, const hub_io_t *io, const hub_config_t *config,
              const hub_auth_io_t *auth_io, const hub_auth_store_t *saved_auth)
{
    memset(hub, 0, sizeof(*hub));
    hub->io = *io;
    hub->config = *config;
    hub_auth_init(&hub->auth, auth_io, saved_auth);
    if (config->auth_unreadable) {
        hub_auth_mark_unreadable(&hub->auth); /* Fail safe: see hub_config_t */
    }
    if (hub->config.max_sessions > HUB_MAX_SESSIONS) {
        hub->config.max_sessions = HUB_MAX_SESSIONS;
    }
    hub->uptime_seen_ms = hub_now(hub);
    hub->next_session_id = 1;
    hub->next_tag = 1;
    hub->device.state = HUB_DEVICE_ABSENT;
    hub->update.next_transfer = 1;
}

bool hub_has_room(const hub_t *hub)
{
    for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
        if (hub->sessions[slot].state == HUB_SESSION_FREE) {
            return true;
        }
    }
    return false;
}

static bool not_yet_in(const hub_session_t *session)
{
    return session->state == HUB_SESSION_AWAITING_HELLO ||
           session->state == HUB_SESSION_AUTHENTICATING;
}

/* With the PIN on, one address may hold only HUB_UNAUTHENTICATED_PER_ADDRESS
 * sessions that have not authenticated: beyond that its oldest such session
 * is closed, so a stranger cannot fill every slot with sessions that never
 * pair. With `auth: none` a session is in as soon as it says hello. */
static void limit_unauthenticated(hub_t *hub, int slot, uint32_t address)
{
    if (!hub_auth_pin_enabled(&hub->auth)) {
        return;
    }
    size_t count = 0;
    int oldest = -1;
    for (size_t i = 0; i < hub->config.max_sessions; i++) {
        const hub_session_t *other = &hub->sessions[i];
        if ((int)i == slot || !not_yet_in(other) || other->address != address) {
            continue;
        }
        count++;
        if (oldest < 0 || (int32_t)(other->opened_ms - hub->sessions[oldest].opened_ms) < 0) {
            oldest = (int)i;
        }
    }
    if (count >= HUB_UNAUTHENTICATED_PER_ADDRESS) {
        hub_close_session(hub, oldest, CLOSE_AUTH_TIMEOUT);
    }
}

void hub_session_opened(hub_t *hub, int slot, uint32_t address)
{
    if (!valid_slot(hub, slot)) {
        return;
    }
    hub_session_t *session = &hub->sessions[slot];
    memset(session, 0, sizeof(*session));
    session->state = HUB_SESSION_AWAITING_HELLO;
    session->address = address;
    session->opened_ms = hub_now(hub);
    session->last_activity_ms = session->opened_ms;
    session->last_ping_ms = session->opened_ms;
    limit_unauthenticated(hub, slot, address);
}

/* Releases everything a session held: its polls, its lock and any snapshot
 * it waits for. Safe to call twice. */
static void release_session(hub_t *hub, int slot)
{
    const uint16_t session_id = hub->sessions[slot].id;
    hub_polls_session_closed(hub, slot);
    hub_device_session_closed(hub, slot, session_id);
    hub_update_session_closed(hub, slot);
}

void hub_close_session(hub_t *hub, int slot, uint16_t code)
{
    release_session(hub, slot);
    hub->sessions[slot].state = HUB_SESSION_CLOSING;
    hub->io.close(hub->io.context, slot, code);
}

void hub_close_sessions(hub_t *hub, uint16_t code, int except_slot)
{
    for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
        const hub_session_state_t state = hub->sessions[slot].state;
        const bool live = state == HUB_SESSION_AWAITING_HELLO ||
                          state == HUB_SESSION_AUTHENTICATING || state == HUB_SESSION_OPEN;
        if (live && (int)slot != except_slot) {
            hub_close_session(hub, (int)slot, code);
        }
    }
}

void hub_close_client_sessions(hub_t *hub, uint16_t cid, uint16_t code, int except_slot)
{
    if (cid == 0) {
        return;
    }
    for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
        if (hub->sessions[slot].state == HUB_SESSION_OPEN && hub->sessions[slot].cid == cid &&
            (int)slot != except_slot) {
            hub_close_session(hub, (int)slot, code);
        }
    }
}

void hub_session_closed(hub_t *hub, int slot)
{
    if (!valid_slot(hub, slot) || hub->sessions[slot].state == HUB_SESSION_FREE) {
        return;
    }
    release_session(hub, slot);
    hub->sessions[slot].state = HUB_SESSION_FREE;
}

void hub_session_activity(hub_t *hub, int slot)
{
    if (valid_slot(hub, slot) && hub->sessions[slot].state != HUB_SESSION_FREE) {
        hub->sessions[slot].last_activity_ms = hub_now(hub);
    }
}

/* A session id not in use, never 0 (Link spec 7.3). */
static uint16_t allocate_session_id(hub_t *hub)
{
    for (;;) {
        const uint16_t candidate = hub->next_session_id++;
        if (candidate == 0) {
            continue;
        }
        bool in_use = false;
        for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
            in_use = in_use || (hub->sessions[slot].state != HUB_SESSION_FREE &&
                                hub->sessions[slot].id == candidate);
        }
        if (!in_use) {
            return candidate;
        }
    }
}

/* ---------------------------------------------------------------------------
 * hello (Link spec 7.2) and authentication (Link spec 6)
 * ------------------------------------------------------------------------- */

const char *hub_auth_mode(const hub_t *hub)
{
    return hub_auth_pin_enabled(&hub->auth) ? "pin" : "none";
}

uint32_t hub_poll_budget(const hub_t *hub)
{
    const uint32_t baud =
        hub->device.state == HUB_DEVICE_ABSENT ? hub->config.settings.uart_baud : hub->device.baud;
    const uint32_t available=baud / LINK_POLL_BUDGET_BAUD_DIVISOR;
    return available>hub->config.reserved_poll_bps?available-hub->config.reserved_poll_bps:0;
}

static cJSON *new_proto_object(void)
{
    cJSON *proto = cJSON_CreateObject();
    if (proto == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(proto, "major", PROTOCOL_MAJOR);
    cJSON_AddNumberToObject(proto, "minor", PROTOCOL_MINOR);
    return proto;
}

static cJSON *new_hub_object(const hub_t *hub)
{
    cJSON *object = cJSON_CreateObject();
    if (object == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(object, "id", hub->config.settings.hub_id);
    cJSON_AddStringToObject(object, "name", hub->config.settings.name);
    cJSON_AddStringToObject(object, "kind", "bridge");
    cJSON_AddStringToObject(object, "version", hub->config.version);
    return object;
}

/* The capabilities of bridge spec 8.1. `notify` is left out when the DSPi
 * has notifications turned off (bridge spec 5), `hub_update` when the I/O
 * cannot take an update. */
static cJSON *new_caps_array(const hub_t *hub)
{
    static const char *const ALWAYS[] = {"poll",       "snapshot", "lock",        "web",  "rename",
                                         "hub_config", "wifi",     "auth_config", "reset"};
    const bool notify_off = !hub_device_notifies(hub);
    cJSON *caps = cJSON_CreateArray();
    if (caps == NULL) {
        return NULL;
    }
    hub_json_append(caps, cJSON_CreateString("cmd"));
    if (!notify_off) {
        hub_json_append(caps, cJSON_CreateString("notify"));
    }
    for (size_t i = 0; i < sizeof(ALWAYS) / sizeof(ALWAYS[0]); i++) {
        hub_json_append(caps, cJSON_CreateString(ALWAYS[i]));
    }
    if (hub_update_available(hub)) {
        hub_json_append(caps, cJSON_CreateString("hub_update"));
    }
    return caps;
}

static void handle_hello(hub_t *hub, int slot, const cJSON *message)
{
    hub_session_t *session = &hub->sessions[slot];

    int major = 0;
    const cJSON *proto = cJSON_GetObjectItemCaseSensitive(message, "proto");
    if (!hub_json_int(proto, "major", &major) || major != PROTOCOL_MAJOR) {
        hub_close_session(hub, slot, CLOSE_PROTOCOL_ERROR);
        return;
    }

    const cJSON *client = cJSON_GetObjectItemCaseSensitive(message, "client");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(client, "name");
    if (cJSON_IsString(name)) {
        hub_auth_copy_name(session->client_name, sizeof(session->client_name), name->valuestring);
    }

    /* With `auth: none` the session opens as soon as the hub answers. With
     * the PIN on, it must pair or present a token first. Either way every
     * session ends up admin (Link spec 6.1, bridge spec 8.9). */
    const bool pin = hub_auth_pin_enabled(&hub->auth);
    cJSON *hello = cJSON_CreateObject();
    if (hello == NULL) {
        hub_close_session(hub, slot, CLOSE_INTERNAL_ERROR); /* Out of memory */
        return;
    }
    session->id = allocate_session_id(hub);
    session->role = LINK_ROLE_ADMIN;
    session->state = pin ? HUB_SESSION_AUTHENTICATING : HUB_SESSION_OPEN;

    cJSON_AddStringToObject(hello, "t", "hello");
    hub_json_add(hello, "proto", new_proto_object());
    hub_json_add(hello, "hub", new_hub_object(hub));
    cJSON_AddStringToObject(hello, "auth", hub_auth_mode(hub));
    if (pin) {
        cJSON_AddStringToObject(hello, "pin_source", "fixed"); /* Link spec 6.2 */
    }
    hub_json_add(hello, "caps", new_caps_array(hub));
    cJSON *limits = cJSON_AddObjectToObject(hello, "limits");
    cJSON_AddNumberToObject(limits, "max_frame", LINK_MAX_FRAME_BYTES);
    cJSON_AddNumberToObject(limits, "max_payload", LINK_MAX_PAYLOAD_BYTES);
    cJSON_AddNumberToObject(limits, "max_inflight", LINK_MAX_INFLIGHT);
    cJSON_AddNumberToObject(limits, "poll_max_hz", LINK_POLL_MAX_HZ);
    cJSON_AddNumberToObject(limits, "poll_budget_bps", hub_poll_budget(hub));
    /* A hello lost to a full queue leaves the client to time out and
     * reconnect, as it would after any lost first message. */
    (void)hub_send_json(hub, slot, hello);
}

/* The `ok` that opens a session: its id, role and empty denial list, plus
 * `token` when one was just issued (Link spec 7.3). */
static void reply_authenticated(hub_t *hub, int slot, int id, const char *token)
{
    hub_session_t *session = &hub->sessions[slot];
    session->state = HUB_SESSION_OPEN;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "session", session->id);
    cJSON_AddStringToObject(body, "role", link_policy_role_name(session->role));
    if (token != NULL) {
        cJSON_AddStringToObject(body, "token", token);
    }
    cJSON *policy = cJSON_AddObjectToObject(body, "policy");
    cJSON_AddArrayToObject(policy, "denied"); /* Admin is denied nothing */
    hub_reply_ok(hub, slot, id, body);
}

static void reply_auth_failure(hub_t *hub, int slot, int id, hub_auth_result_t result)
{
    if (result == HUB_AUTH_RATE_LIMITED) {
        hub_reply_error(hub, slot, id, "rate_limited", "too many failures; try again in a minute");
    } else if (result == HUB_AUTH_UNAVAILABLE && hub_auth_unreadable(&hub->auth)) {
        hub_reply_error(
            hub, slot, id, "internal",
            "the bridge could not read its saved PIN; reset the bridge to set a new one");
    } else if (result == HUB_AUTH_UNAVAILABLE) {
        hub_reply_error(hub, slot, id, "internal", "the bridge could not check credentials");
    } else {
        hub_reply_error(hub, slot, id, "unauthenticated", NULL);
    }
}

static void remember_client_name(hub_session_t *session, const cJSON *message)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(message, "name");
    if (cJSON_IsString(name)) {
        hub_auth_copy_name(session->client_name, sizeof(session->client_name), name->valuestring);
    }
}

/* auth.pair. With `auth: none` a client may still pair, with any PIN, to
 * learn its session id for echo suppression (Link spec 6.1). With the PIN
 * on, the PIN is checked and a token issued (Link spec 6.2). */
static void handle_auth_pair(hub_t *hub, int slot, int id, const cJSON *message)
{
    hub_session_t *session = &hub->sessions[slot];
    remember_client_name(session, message);
    if (!hub_auth_pin_enabled(&hub->auth)) {
        reply_authenticated(hub, slot, id, NULL);
        return;
    }

    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(message, "pin");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t replaced = 0;
    const hub_auth_result_t result =
        hub_auth_pair(&hub->auth, session->address, cJSON_IsString(pin) ? pin->valuestring : "",
                      session->client_name, token, &session->cid, &replaced);
    if (result != HUB_AUTH_OK) {
        reply_auth_failure(hub, slot, id, result);
        return;
    }
    session->last_touch_ms = hub_now(hub);
    reply_authenticated(hub, slot, id, token);
    /* A full list forgot the client seen least recently; its token is
     * gone, so its sessions go too, as if it had been revoked. */
    hub_close_client_sessions(hub, replaced, CLOSE_TOKEN_REVOKED, slot);
}

/* auth.token: a client paired before presents its token (Link spec 6.2). */
static void handle_auth_token(hub_t *hub, int slot, int id, const cJSON *message)
{
    hub_session_t *session = &hub->sessions[slot];
    if (!hub_auth_pin_enabled(&hub->auth)) {
        reply_authenticated(hub, slot, id, NULL);
        return;
    }
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(message, "token");
    const hub_auth_result_t result =
        hub_auth_token(&hub->auth, session->address,
                       cJSON_IsString(token) ? token->valuestring : "", &session->cid);
    if (result != HUB_AUTH_OK) {
        reply_auth_failure(hub, slot, id, result);
        return;
    }
    session->last_touch_ms = hub_now(hub); /* hub_auth_token() marked it seen */
    reply_authenticated(hub, slot, id, NULL);
}

/* ---------------------------------------------------------------------------
 * Answers finished elsewhere
 * ------------------------------------------------------------------------- */

void hub_reply_later(hub_t *hub, int slot, uint16_t session_id, int request_id, cJSON *body)
{
    const hub_session_t *session = hub_open_session(hub, slot);
    if (session == NULL || session->id != session_id) {
        cJSON_Delete(body);
        return;
    }
    hub_reply_ok(hub, slot, request_id, body);
}

/* ---------------------------------------------------------------------------
 * Incoming messages
 * ------------------------------------------------------------------------- */

typedef void (*message_handler_t)(hub_t *hub, int slot, int id, const cJSON *message);

/* Every request type the hub answers. Anything else gets `unknown_type`
 * (Link spec 7.1). */
static const struct {
    const char *type;
    message_handler_t handle;
} MESSAGE_HANDLERS[] = {
    {"auth.pair", handle_auth_pair},
    {"auth.token", handle_auth_token},
    {"auth.list", hub_handle_auth_list},
    {"auth.revoke", hub_handle_auth_revoke},
    {"auth.set_role", hub_handle_auth_set_role},
    {"hub.auth.set", hub_handle_hub_auth_set},
    {"hub.config.get", hub_handle_config_get},
    {"hub.config.set", hub_handle_config_set},
    {"hub.wifi.scan", hub_handle_wifi_scan},
    {"hub.wifi.join", hub_handle_wifi_join},
    {"hub.restart", hub_handle_restart},
    {"hub.reset", hub_handle_reset},
    {"hub.update", hub_handle_update},
    {"device.list", hub_handle_device_list},
    {"device.rename", hub_handle_device_rename},
    {"device.snapshot", hub_handle_device_snapshot},
    {"poll.subscribe", hub_handle_poll_subscribe},
    {"poll.unsubscribe", hub_handle_poll_unsubscribe},
    {"lock.acquire", hub_handle_lock_acquire},
    {"lock.release", hub_handle_lock_release},
    {"hub.stats", hub_handle_hub_stats},
    {"hub.rename", hub_handle_hub_rename},
};

static void dispatch_request(hub_t *hub, int slot, const char *type, int id, const cJSON *message)
{
    for (size_t i = 0; i < sizeof(MESSAGE_HANDLERS) / sizeof(MESSAGE_HANDLERS[0]); i++) {
        if (strcmp(type, MESSAGE_HANDLERS[i].type) == 0) {
            MESSAGE_HANDLERS[i].handle(hub, slot, id, message);
            return;
        }
    }
    hub_reply_error(hub, slot, id, "unknown_type", NULL);
}

void hub_session_text(hub_t *hub, int slot, const char *text, size_t length)
{
    if (!valid_slot(hub, slot)) {
        return;
    }
    hub_session_t *session = &hub->sessions[slot];
    if (session->state != HUB_SESSION_AWAITING_HELLO &&
        session->state != HUB_SESSION_AUTHENTICATING && session->state != HUB_SESSION_OPEN) {
        return;
    }
    session->last_activity_ms = hub_now(hub);

    cJSON *message = cJSON_ParseWithLength(text, length);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(message, "t");
    const bool well_formed = cJSON_IsObject(message) && cJSON_IsString(type);

    if (session->state == HUB_SESSION_AWAITING_HELLO) {
        /* The first message must be hello (Link spec 5). */
        if (well_formed && strcmp(type->valuestring, "hello") == 0) {
            handle_hello(hub, slot, message);
        } else {
            hub_close_session(hub, slot, CLOSE_PROTOCOL_ERROR);
        }
    } else if (!well_formed) {
        hub_reply_error(hub, slot, NO_ID, "bad_request", "not a JSON object with a string t");
    } else {
        int id = NO_ID;
        (void)hub_json_int(message, "id", &id); /* Optional: NO_ID stands if absent */
        const bool is_auth = strcmp(type->valuestring, "auth.pair") == 0 ||
                             strcmp(type->valuestring, "auth.token") == 0;
        if (strcmp(type->valuestring, "hello") == 0) {
            hub_reply_error(hub, slot, id, "bad_request", "hello was already exchanged");
        } else if (session->state == HUB_SESSION_AUTHENTICATING && !is_auth) {
            /* Until it authenticates, a session may only pair or present a
             * token (Link spec 5). */
            hub_reply_error(hub, slot, id, "unauthenticated", NULL);
        } else {
            dispatch_request(hub, slot, type->valuestring, id, message);
        }
    }
    cJSON_Delete(message);
}

void hub_session_binary(hub_t *hub, int slot, const uint8_t *data, size_t length)
{
    hub_session_activity(hub, slot);
    if (hub_open_session(hub, slot) == NULL) {
        return; /* Binary frames before authentication are discarded (Link spec 5) */
    }

    const int type = link_frame_type(data, length);
    if (type == LINK_FRAME_CMD_REQUEST) {
        hub_commands_handle_frame(hub, slot, data, length);
    } else if (type == LINK_FRAME_FWDATA) {
        hub_update_chunk(hub, slot, data, length);
    } else if (type >= 0 && type < 0x80) {
        /* An unknown request type gets UNSUPPORTED with its tag (Link spec 8). */
        const size_t frame_length =
            link_encode_cmd_response(hub->frame, sizeof(hub->frame), link_frame_tag(data, length),
                                     LINK_STATUS_UNSUPPORTED, NULL, 0);
        (void)hub_send_binary(hub, slot, hub->frame, frame_length); /* Best effort, as any CMD */
    }
    /* Types from 0x80 are hub-to-client; a hub ignores them. */
}

/* ---------------------------------------------------------------------------
 * Timers
 * ------------------------------------------------------------------------- */

/* Hello timeout, keepalive and owed RESYNCs (Link spec 4 and 5, bridge spec
 * 8.2). */
static void tick_sessions(hub_t *hub, uint32_t now)
{
    for (size_t i = 0; i < hub->config.max_sessions; i++) {
        hub_session_t *session = &hub->sessions[i];
        const int slot = (int)i;

        if (not_yet_in(session) && now - session->opened_ms >= LINK_AUTH_TIMEOUT_MS) {
            hub_close_session(hub, slot, CLOSE_AUTH_TIMEOUT);
            continue;
        }
        if (session->state != HUB_SESSION_OPEN) {
            continue;
        }
        if (now - session->last_activity_ms >= LINK_PING_TIMEOUT_MS) {
            hub_close_session(hub, slot, CLOSE_KEEPALIVE_FAILURE);
            continue;
        }
        if (now - session->last_ping_ms >= LINK_PING_INTERVAL_MS) {
            hub->io.ping(hub->io.context, slot);
            session->last_ping_ms = now;
        }
        /* A paired client stays "seen" while it is connected, so a client
         * that never disconnects is not the one a full list forgets. */
        if (session->cid != 0 && now - session->last_touch_ms >= HUB_AUTH_TOUCH_INTERVAL_MS) {
            hub_auth_touch(&hub->auth, session->cid);
            session->last_touch_ms = now;
        }
        if (session->resync_owed) {
            const size_t length = link_encode_resync(hub->frame, sizeof(hub->frame),
                                                     HUB_DEVICE_HANDLE, LINK_RESYNC_DROPPED);
            session->resync_owed = !hub_send_binary(hub, slot, hub->frame, length);
        }
    }
}

uint64_t hub_uptime_ms(hub_t *hub)
{
    const uint32_t now = hub_now(hub);
    hub->uptime_ms += now - hub->uptime_seen_ms;
    hub->uptime_seen_ms = now;
    return hub->uptime_ms;
}

void hub_tick(hub_t *hub)
{
    (void)hub_uptime_ms(hub); /* Often enough that the clock never wraps unseen */
    hub_auth_tick(&hub->auth);
    tick_sessions(hub, hub_now(hub));
    hub_commands_tick(hub);
    hub_device_tick(hub);
    hub_polls_tick(hub);
    hub_update_tick(hub);
}

/* ---------------------------------------------------------------------------
 * What the hub publishes (Link spec 3)
 * ------------------------------------------------------------------------- */

char *hub_info_json(const hub_t *hub)
{
    cJSON *info = cJSON_CreateObject();
    if (info == NULL) {
        return NULL;
    }
    hub_json_add(info, "proto", new_proto_object());
    hub_json_add(info, "hub", new_hub_object(hub));
    cJSON_AddStringToObject(info, "auth", hub_auth_mode(hub));
    cJSON_AddBoolToObject(info, "tls", false);
    cJSON_AddStringToObject(info, "ws", "/dspi/v1");

    cJSON *devices = cJSON_AddArrayToObject(info, "devices");
    if (hub->device.state != HUB_DEVICE_ABSENT) {
        cJSON *device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "serial", hub->device.identity.serial);
        cJSON_AddStringToObject(device, "name", hub->device.name);
        hub_json_append(devices, device);
    }

    char *text = cJSON_PrintUnformatted(info);
    cJSON_Delete(info);
    return text;
}

void hub_advert(const hub_t *hub, hub_advert_t *advert)
{
    const bool known = hub->device.state != HUB_DEVICE_ABSENT;
    advert->hub_id = hub->config.settings.hub_id;
    advert->name = hub->config.settings.name;
    advert->auth = hub_auth_mode(hub);
    advert->devices = known ? 1 : 0;
    advert->serial = known ? hub->device.identity.serial : NULL;
}
