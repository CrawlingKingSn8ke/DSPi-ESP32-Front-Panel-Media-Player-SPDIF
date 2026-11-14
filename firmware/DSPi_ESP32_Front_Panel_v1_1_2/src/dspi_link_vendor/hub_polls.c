/*
 * hub_polls.c - poll subscriptions and their schedule.
 *
 * Implements Link spec 7.6 and 8.4 as bridge spec 7.7 and 8.5 describe.
 * Sessions subscribe slots; identical polls from several sessions form one
 * group that runs once and fans out. A poll runs only when no client
 * command is waiting for the uart task, so commands always go first.
 *
 * Portable: no ESP-IDF headers.
 */

#include <string.h>

#include "hub_internal.h"

/* After this many failures in a row a poll reports poll.error and slows to
 * HUB_POLL_FAILED_PERIOD_MS until the device recovers (Link spec 8.4). */
#define POLL_FAILURES_BEFORE_ERROR 3

/* ---------------------------------------------------------------------------
 * Groups
 * ------------------------------------------------------------------------- */

static bool same_key(const hub_poll_key_t *a, const hub_poll_key_t *b)
{
    return a->request == b->request && a->value == b->value && a->index == b->index &&
           a->length == b->length;
}

static hub_poll_group_t *find_group(hub_t *hub, const hub_poll_key_t *key)
{
    for (size_t i = 0; i < hub->poll_group_count; i++) {
        if (same_key(&hub->poll_groups[i].key, key)) {
            return &hub->poll_groups[i];
        }
    }
    return NULL;
}

/* Gives a new group the schedule and failure count of the group with the
 * same key before a rebuild, if there was one. */
static void inherit_schedule(hub_poll_group_t *group, const hub_poll_group_t *previous,
                             size_t previous_count)
{
    for (size_t old = 0; old < previous_count; old++) {
        if (same_key(&previous[old].key, &group->key)) {
            group->next_due_ms = previous[old].next_due_ms;
            group->failures_in_a_row = previous[old].failures_in_a_row;
            return;
        }
    }
}

/* Adds one subscriber's poll to the groups: it joins its group, which runs
 * at the fastest rate any subscriber was granted, or starts a new one. */
static void join_group(hub_t *hub, const hub_poll_slot_t *poll, const hub_poll_group_t *previous,
                       size_t previous_count)
{
    const uint32_t period_ms = 1000u / poll->hz;
    hub_poll_group_t *group = find_group(hub, &poll->key);
    if (group != NULL) {
        group->period_ms = period_ms < group->period_ms ? period_ms : group->period_ms;
        return;
    }
    if (hub->poll_group_count == HUB_MAX_POLL_GROUPS) {
        return; /* subscribe refuses before this can happen */
    }
    group = &hub->poll_groups[hub->poll_group_count++];
    *group =
        (hub_poll_group_t){.key = poll->key, .period_ms = period_ms, .next_due_ms = hub_now(hub)};
    inherit_schedule(group, previous, previous_count);
}

/* Rebuilds the groups from every session's slots, keeping each surviving
 * group's schedule. */
static void rebuild_groups(hub_t *hub)
{
    hub_poll_group_t previous[HUB_MAX_POLL_GROUPS];
    const size_t previous_count = hub->poll_group_count;
    memcpy(previous, hub->poll_groups, sizeof(previous));
    hub->poll_group_count = 0;

    for (size_t s = 0; s < hub->config.max_sessions; s++) {
        const hub_session_t *session = &hub->sessions[s];
        if (session->state != HUB_SESSION_OPEN) {
            continue;
        }
        for (size_t p = 0; p < HUB_POLL_SLOTS; p++) {
            if (session->polls[p].active) {
                join_group(hub, &session->polls[p], previous, previous_count);
            }
        }
    }
}

/* Adds `key` to the `count` distinct keys in `keys` unless it is already
 * there. `keys` has room for every key a caller can add. */
static void add_distinct(hub_poll_key_t *keys, size_t *count, const hub_poll_key_t *key)
{
    for (size_t k = 0; k < *count; k++) {
        if (same_key(&keys[k], key)) {
            return;
        }
    }
    keys[(*count)++] = *key;
}

/* The groups needed if session `slot` polls `keys` (`count` of them, which
 * may repeat) and every other session keeps what it polls. */
static size_t groups_needed(const hub_t *hub, int slot, const hub_poll_key_t *keys, size_t count)
{
    /* Other sessions use at most HUB_MAX_POLL_GROUPS distinct keys, and
     * one session at most HUB_POLL_SLOTS. */
    hub_poll_key_t distinct[HUB_MAX_POLL_GROUPS + HUB_POLL_SLOTS];
    size_t distinct_count = 0;
    for (size_t s = 0; s < hub->config.max_sessions; s++) {
        if ((int)s == slot || hub->sessions[s].state != HUB_SESSION_OPEN) {
            continue;
        }
        for (size_t p = 0; p < HUB_POLL_SLOTS; p++) {
            if (hub->sessions[s].polls[p].active) {
                add_distinct(distinct, &distinct_count, &hub->sessions[s].polls[p].key);
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        add_distinct(distinct, &distinct_count, &keys[i]);
    }
    return distinct_count;
}

/* ---------------------------------------------------------------------------
 * Subscribing (Link spec 7.6)
 * ------------------------------------------------------------------------- */

/* UART bytes one poll costs: its request and its answer (bridge spec 7.7). */
static uint32_t poll_cost_bytes(const hub_poll_key_t *key)
{
    return DSPI_FRAME_REQUEST_HEADER_BYTES + DSPI_FRAME_CRC_BYTES +
           DSPI_FRAME_RESPONSE_OVERHEAD_BYTES + key->length;
}

/* One entry of a poll.subscribe request, checked. */
typedef struct {
    int slot;
    hub_poll_key_t key;
    int hz;
} poll_request_t;

/* Reads and checks one entry. Returns NULL if it is fine, or the reason it
 * is not. */
static const char *parse_poll(const cJSON *item, poll_request_t *out)
{
    int request = -1;
    int value = -1;
    int index = -1;
    int length = -1;
    if (!hub_json_int(item, "slot", &out->slot) || !hub_json_int(item, "req", &request) ||
        !hub_json_int(item, "val", &value) || !hub_json_int(item, "idx", &index) ||
        !hub_json_int(item, "len", &length) || !hub_json_int(item, "hz", &out->hz)) {
        return "each poll needs slot, req, val, idx, len and hz";
    }
    if (out->slot < 0 || out->slot >= HUB_POLL_SLOTS) {
        return "slot must be 0 to 15";
    }
    if (request < 0 || request > 0xFF || value < 0 || value > 0xFFFF || index < 0 ||
        index > 0xFFFF || length < 1 || length > LINK_MAX_PAYLOAD_BYTES || out->hz < 1) {
        return "req, val, idx, len or hz is out of range";
    }
    out->key = (hub_poll_key_t){.request = (uint8_t)request,
                                .value = (uint16_t)value,
                                .index = (uint16_t)index,
                                .length = (uint16_t)length};
    return NULL;
}

/* Reads and checks every entry of `polls` into `requests`, before anything
 * changes. Returns their number, or 0 after replying with the reason. */
static int read_requests(hub_t *hub, int slot, int id, const cJSON *polls,
                         poll_request_t requests[HUB_POLL_SLOTS])
{
    const int count = cJSON_GetArraySize(polls);
    if (!cJSON_IsArray(polls) || count < 1 || count > HUB_POLL_SLOTS) {
        hub_reply_error(hub, slot, id, "bad_request", "polls must list 1 to 16 polls");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        const char *problem = parse_poll(cJSON_GetArrayItem(polls, i), &requests[i]);
        if (problem != NULL) {
            hub_reply_error(hub, slot, id, "bad_request", problem);
            return 0;
        }
        if (link_policy_class(requests[i].key.request, true) != LINK_CLASS_READ) {
            hub_reply_error(hub, slot, id, "denied", "only commands classed read may be polled");
            return 0;
        }
    }
    return count;
}

/* True when one of the `count` requests takes slot number `p`. */
static bool slot_is_requested(size_t p, const poll_request_t *requests, int count)
{
    for (int i = 0; i < count; i++) {
        if ((size_t)requests[i].slot == p) {
            return true;
        }
    }
    return false;
}

/* The keys `session` polls once `requests` replace any of its slots with
 * the same numbers: those it keeps, then the new ones. Returns how many. */
static size_t keys_after(const hub_session_t *session, const poll_request_t *requests, int count,
                         hub_poll_key_t keys[2 * HUB_POLL_SLOTS])
{
    size_t key_count = 0;
    for (size_t p = 0; p < HUB_POLL_SLOTS; p++) {
        if (session->polls[p].active && !slot_is_requested(p, requests, count)) {
            keys[key_count++] = session->polls[p].key;
        }
    }
    for (int i = 0; i < count; i++) {
        keys[key_count++] = requests[i].key;
    }
    return key_count;
}

/* One distinct poll and the rate its group runs at. */
typedef struct {
    hub_poll_key_t key;
    uint32_t hz;
} poll_rate_t;

/* Raises `key`'s rate in `rates` to at least `hz`, adding it if it is new.
 * `rates` has room for every distinct key a caller can add. */
static poll_rate_t *raise_rate(poll_rate_t *rates, size_t *count, const hub_poll_key_t *key,
                               uint32_t hz)
{
    for (size_t i = 0; i < *count; i++) {
        if (same_key(&rates[i].key, key)) {
            rates[i].hz = hz > rates[i].hz ? hz : rates[i].hz;
            return &rates[i];
        }
    }
    rates[*count] = (poll_rate_t){.key = *key, .hz = hz};
    return &rates[(*count)++];
}

/*
 * Grants rates for `requests` against one budget for the whole link
 * (bridge spec 7.7): every group, whoever subscribed it, costs its bytes
 * at its fastest rate, and a poll identical to one already running costs
 * only the rate it adds. Writes each grant to `granted_hz`. Returns -1 if
 * every request gets at least 1 Hz, or the index of the first that cannot,
 * in which case nothing may be taken.
 */
static int grant_rates(const hub_t *hub, int slot, const poll_request_t *requests, int count,
                       uint32_t granted_hz[HUB_POLL_SLOTS])
{
    /* Every other session's polls, and this one's that stay. The groups
     * check before this keeps them to HUB_MAX_POLL_GROUPS distinct keys. */
    poll_rate_t rates[HUB_MAX_POLL_GROUPS + HUB_POLL_SLOTS];
    size_t rate_count = 0;
    for (size_t s = 0; s < hub->config.max_sessions; s++) {
        const hub_session_t *session = &hub->sessions[s];
        if (session->state != HUB_SESSION_OPEN) {
            continue;
        }
        for (size_t p = 0; p < HUB_POLL_SLOTS; p++) {
            const hub_poll_slot_t *poll = &session->polls[p];
            if (poll->active && !((int)s == slot && slot_is_requested(p, requests, count))) {
                (void)raise_rate(rates, &rate_count, &poll->key, poll->hz);
            }
        }
    }
    uint32_t used = 0;
    for (size_t i = 0; i < rate_count; i++) {
        used += poll_cost_bytes(&rates[i].key) * rates[i].hz;
    }
    const uint32_t budget = hub_poll_budget(hub);

    for (int i = 0; i < count; i++) {
        const uint32_t cost = poll_cost_bytes(&requests[i].key);
        poll_rate_t *rate = raise_rate(rates, &rate_count, &requests[i].key, 0);
        const uint32_t running_hz = rate->hz;
        uint32_t hz = (uint32_t)requests[i].hz;
        hz = hz > LINK_POLL_MAX_HZ ? LINK_POLL_MAX_HZ : hz;
        if (hz > running_hz) {
            /* Only the rate above what already runs costs anything. */
            const uint32_t affordable = used < budget ? (budget - used) / cost : 0;
            hz = hz > running_hz + affordable ? running_hz + affordable : hz;
        }
        if (hz < 1) {
            return i;
        }
        if (hz > running_hz) {
            used += cost * (hz - running_hz);
            rate->hz = hz;
        }
        granted_hz[i] = hz;
    }
    return -1;
}

/* Takes the requests into the session's slots at the rates granted. Each
 * grant is added to `granted`. */
static void take_requests(hub_t *hub, hub_session_t *session, const poll_request_t *requests,
                          int count, const uint32_t granted_hz[HUB_POLL_SLOTS], cJSON *granted)
{
    for (int i = 0; i < count; i++) {
        const uint32_t hz = granted_hz[i];
        /* last_sent_ms starts a period back, so the first answer goes out. */
        session->polls[requests[i].slot] = (hub_poll_slot_t){
            .active = true,
            .key = requests[i].key,
            .hz = (uint8_t)hz,
            .last_sent_ms = hub_now(hub) - 1000u / hz,
        };
        cJSON *grant = cJSON_CreateObject();
        cJSON_AddNumberToObject(grant, "slot", requests[i].slot);
        cJSON_AddNumberToObject(grant, "hz", hz);
        hub_json_append(granted, grant);
    }
}

void hub_handle_poll_subscribe(hub_t *hub, int slot, int id, const cJSON *message)
{
    hub_session_t *session = &hub->sessions[slot];
    int handle = -1;
    (void)hub_json_int(message, "handle", &handle); /* Absent: -1, no such device */
    if (handle != HUB_DEVICE_HANDLE || !hub_device_online(hub)) {
        hub_reply_error(hub, slot, id, "no_device", NULL);
        return;
    }
    poll_request_t requests[HUB_POLL_SLOTS];
    const int count =
        read_requests(hub, slot, id, cJSON_GetObjectItemCaseSensitive(message, "polls"), requests);
    if (count == 0) {
        return;
    }
    hub_poll_key_t keys[2 * HUB_POLL_SLOTS];
    const size_t key_count = keys_after(session, requests, count, keys);
    if (groups_needed(hub, slot, keys, key_count) > HUB_MAX_POLL_GROUPS) {
        hub_reply_error(hub, slot, id, "busy", "the bridge is already running its most polls");
        return; /* Nothing has changed */
    }
    uint32_t granted_hz[HUB_POLL_SLOTS];
    if (grant_rates(hub, slot, requests, count, granted_hz) >= 0) {
        hub_reply_error(hub, slot, id, "busy",
                        "the poll budget cannot fit these polls even at 1 Hz");
        return; /* Nothing has changed */
    }

    cJSON *body = cJSON_CreateObject();
    take_requests(hub, session, requests, count, granted_hz,
                  cJSON_AddArrayToObject(body, "granted"));
    rebuild_groups(hub);
    hub_reply_ok(hub, slot, id, body);
}

void hub_handle_poll_unsubscribe(hub_t *hub, int slot, int id, const cJSON *message)
{
    hub_session_t *session = &hub->sessions[slot];
    const cJSON *slots = cJSON_GetObjectItemCaseSensitive(message, "slots");
    if (!cJSON_IsArray(slots)) {
        hub_reply_error(hub, slot, id, "bad_request", "slots must be an array");
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, slots)
    {
        if (cJSON_IsNumber(item) && item->valueint >= 0 && item->valueint < HUB_POLL_SLOTS) {
            session->polls[item->valueint].active = false;
        }
    }
    rebuild_groups(hub);
    hub_reply_ok(hub, slot, id, NULL);
}

void hub_polls_session_closed(hub_t *hub, int slot)
{
    memset(hub->sessions[slot].polls, 0, sizeof(hub->sessions[slot].polls));
    rebuild_groups(hub);
}

void hub_polls_clear(hub_t *hub)
{
    for (size_t s = 0; s < hub->config.max_sessions; s++) {
        memset(hub->sessions[s].polls, 0, sizeof(hub->sessions[s].polls));
    }
    hub->poll_group_count = 0;
}

/* ---------------------------------------------------------------------------
 * Running polls
 * ------------------------------------------------------------------------- */

void hub_polls_tick(hub_t *hub)
{
    /* Client commands first, one poll at a time (bridge spec 7.7), and
     * none while an update is written (HUB_UPDATE_MAX_GET_BYTES). */
    if (!hub_device_online(hub) || hub->client_commands_pending > 0 || hub->poll_pending ||
        hub_update_in_progress(hub)) {
        return;
    }

    const uint32_t now = hub_now(hub);
    hub_poll_group_t *due = NULL;
    for (size_t i = 0; i < hub->poll_group_count; i++) {
        hub_poll_group_t *group = &hub->poll_groups[i];
        const bool is_due = (int32_t)(now - group->next_due_ms) >= 0;
        if (is_due && (due == NULL || (int32_t)(group->next_due_ms - due->next_due_ms) < 0)) {
            due = group;
        }
    }
    if (due == NULL) {
        return;
    }

    const uint32_t period = due->failures_in_a_row >= POLL_FAILURES_BEFORE_ERROR
                                ? HUB_POLL_FAILED_PERIOD_MS
                                : due->period_ms;
    /* Fallen more than a period behind: skip ahead rather than burst. */
    due->next_due_ms = (int32_t)(now - due->next_due_ms) > (int32_t)period
                           ? now + period
                           : due->next_due_ms + period;

    const dspi_request_t request = {
        .is_get = true,
        .request = due->key.request,
        .value = due->key.value,
        .index = due->key.index,
        .length = due->key.length,
    };
    /* No retry: the next tick is the retry, and it is never stale. A full
     * queue is the same: the poll simply runs at its next due time. */
    hub->poll_pending = hub_submit_own(hub, HUB_WORK_POLL, &request, false, &due->key) != 0;
}

/* Sends poll.error to every session subscribed to `key` (Link spec 8.4). */
static void report_poll_error(hub_t *hub, const hub_poll_key_t *key, const dspi_reply_t *reply)
{
    const char *code = reply->kind == DSPI_REPLY_NO_DEVICE ? "no_device" : "busy";
    for (size_t s = 0; s < hub->config.max_sessions; s++) {
        if (hub->sessions[s].state != HUB_SESSION_OPEN) {
            continue;
        }
        for (size_t p = 0; p < HUB_POLL_SLOTS; p++) {
            const hub_poll_slot_t *poll = &hub->sessions[s].polls[p];
            if (!poll->active || !same_key(&poll->key, key)) {
                continue;
            }
            cJSON *event = cJSON_CreateObject();
            cJSON_AddStringToObject(event, "t", "poll.error");
            cJSON_AddNumberToObject(event, "handle", HUB_DEVICE_HANDLE);
            cJSON_AddNumberToObject(event, "slot", (double)p);
            cJSON_AddStringToObject(event, "code", code);
            /* Lost to a full queue, it is missed: the poll's silence says
             * the same. */
            (void)hub_send_json(hub, (int)s, event);
        }
    }
}

void hub_polls_on_reply(hub_t *hub, const hub_pending_t *work, const dspi_reply_t *reply)
{
    hub_poll_group_t *group = find_group(hub, &work->poll);
    if (group == NULL) {
        return; /* Everyone unsubscribed while it ran */
    }

    if (reply->kind != DSPI_REPLY_ANSWERED || reply->status != DSPI_STATUS_OK) {
        /* Nothing is sent for a failed tick; the third in a row is reported
         * once. The count stops there, so it never wraps round. */
        if (group->failures_in_a_row < POLL_FAILURES_BEFORE_ERROR &&
            ++group->failures_in_a_row == POLL_FAILURES_BEFORE_ERROR) {
            report_poll_error(hub, &group->key, reply);
        }
        return;
    }
    group->failures_in_a_row = 0;

    /* Fan out the verbatim answer to every subscriber, under each one's own
     * slot number and sequence. The group runs at its fastest subscriber's
     * rate, so a slower subscriber is sent an answer only when its own
     * period is due, give or take half the group's period. */
    const uint32_t now = hub_now(hub);
    for (size_t s = 0; s < hub->config.max_sessions; s++) {
        hub_session_t *session = &hub->sessions[s];
        if (session->state != HUB_SESSION_OPEN) {
            continue;
        }
        for (size_t p = 0; p < HUB_POLL_SLOTS; p++) {
            hub_poll_slot_t *poll = &session->polls[p];
            if (!poll->active || !same_key(&poll->key, &group->key)) {
                continue;
            }
            const uint32_t own_period_ms = 1000u / poll->hz;
            if (now - poll->last_sent_ms + group->period_ms / 2 < own_period_ms) {
                continue;
            }
            poll->last_sent_ms = now;
            const size_t length =
                link_encode_poll(hub->frame, sizeof(hub->frame), session->poll_sequence++,
                                 HUB_DEVICE_HANDLE, (uint8_t)p, reply->payload, reply->length);
            /* A dropped POLL frame shows as a gap in the poll sequence. */
            (void)hub_send_binary(hub, (int)s, hub->frame, length);
        }
    }
}
