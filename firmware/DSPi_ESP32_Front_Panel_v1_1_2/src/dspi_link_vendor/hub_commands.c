/*
 * hub_commands.c - CMD frames, the queue of requests to the uart task, and
 * origin attribution.
 *
 * Implements Link spec 8.2 and the origin rule of 8.3, as bridge spec 8.3
 * and 8.4 describe. Every request the hub sends the DSPi, whether a client's
 * command, a poll or a snapshot read, goes through one queue here. The uart
 * task answers in the order it was asked, so the oldest pending request is
 * always the one being answered.
 *
 * Portable: no ESP-IDF headers.
 */

#include <string.h>

#include "hub_internal.h"

/* REQ_SET_ALL_PARAMS: the bulk SET, which the UART carries in one frame and
 * the firmware accepts above the 64-byte limit for other SETs. */
#define REQ_SET_ALL_PARAMS 0xA1

/* Notification sources that a Link client's write produces (Link spec 8.3):
 * HOST_SET, BULK_SET and UART. */
#define SOURCE_HOST_SET 1
#define SOURCE_BULK_SET 2
#define SOURCE_UART 8

/* ---------------------------------------------------------------------------
 * The pending queue
 * ------------------------------------------------------------------------- */

/* The free entry at the back of the queue, or NULL if full. It joins the
 * queue only on commit_pending(), so a request the uart task refuses leaves
 * no trace. */
static hub_pending_t *reserve_pending(hub_t *hub)
{
    if (hub->pending_count == HUB_PENDING_CAPACITY) {
        return NULL;
    }
    const size_t back = (hub->pending_head + hub->pending_count) % HUB_PENDING_CAPACITY;
    hub_pending_t *entry = &hub->pending[back];
    memset(entry, 0, sizeof(*entry));
    entry->tag = hub->next_tag++;
    if (entry->tag == 0) {
        entry->tag = hub->next_tag++; /* 0 means "no request" (hub_submit_own()) */
    }
    entry->submitted_ms = hub_now(hub);
    return entry;
}

static void origin_taken_up(hub_t *hub, uint32_t tag);

/* The oldest request has just become the one the uart task works on: its
 * wait starts now, and so does a write's origin window. */
static void head_changed(hub_t *hub)
{
    hub->pending_head_since_ms = hub_now(hub);
    if (hub->pending_count > 0) {
        origin_taken_up(hub, hub->pending[hub->pending_head].tag);
    }
}

static void commit_pending(hub_t *hub)
{
    hub->pending_count++;
    if (hub->pending_count == 1) {
        head_changed(hub);
    }
}

/* Takes the oldest entry off the queue, copying it out so the caller may
 * queue new work while handling it. The uart task works on one request at
 * a time, so the next one starts now. */
static void pop_pending(hub_t *hub, hub_pending_t *out)
{
    *out = hub->pending[hub->pending_head];
    hub->pending_head = (hub->pending_head + 1) % HUB_PENDING_CAPACITY;
    hub->pending_count--;
    head_changed(hub);
}

/* True when tag `a` was issued before tag `b`. Tags count up and wrap, so
 * this compares their distance rather than their values. */
static bool tag_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

uint32_t hub_submit_own(hub_t *hub, hub_work_kind_t kind, const dspi_request_t *request,
                        bool may_retry, const hub_poll_key_t *poll)
{
    hub_pending_t *entry = reserve_pending(hub);
    if (entry == NULL) {
        return 0;
    }
    entry->kind = kind;
    if (poll != NULL) {
        entry->poll = *poll;
    }
    const uint32_t tag = entry->tag;
    if (!hub->io.submit(hub->io.context, request, may_retry, tag)) {
        return 0;
    }
    commit_pending(hub);
    return tag;
}

/* ---------------------------------------------------------------------------
 * Origin attribution (Link spec 8.3)
 * ------------------------------------------------------------------------- */

/* The origin queue holds the writes in dispatch order. A write is entered
 * when it is queued, its window restarts when the uart task takes it up
 * (a long queue would otherwise use the window up before the DSPi even
 * sees the write), and it leaves if the DSPi refuses it, since a refused
 * write changes nothing and must not claim someone else's notification. */

static hub_origin_t *origin_at(hub_t *hub, size_t position)
{
    return &hub->origins[(hub->origin_head + position) % HUB_ORIGIN_QUEUE_ENTRIES];
}

static void remember_write(hub_t *hub, uint16_t session_id, uint32_t tag)
{
    if (hub->origin_count == HUB_ORIGIN_QUEUE_ENTRIES) {
        /* Full: forget the oldest write, which is the least likely to match. */
        hub->origin_head = (hub->origin_head + 1) % HUB_ORIGIN_QUEUE_ENTRIES;
        hub->origin_count--;
    }
    *origin_at(hub, hub->origin_count) =
        (hub_origin_t){.session_id = session_id, .tag = tag, .sent_ms = hub_now(hub)};
    hub->origin_count++;
}

static void origin_taken_up(hub_t *hub, uint32_t tag)
{
    for (size_t i = 0; i < hub->origin_count; i++) {
        hub_origin_t *origin = origin_at(hub, i);
        if (origin->tag == tag) {
            origin->sent_ms = hub_now(hub);
            return;
        }
    }
}

static void forget_write(hub_t *hub, uint32_t tag)
{
    for (size_t i = 0; i < hub->origin_count; i++) {
        if (origin_at(hub, i)->tag != tag) {
            continue;
        }
        for (size_t later = i + 1; later < hub->origin_count; later++) {
            *origin_at(hub, later - 1) = *origin_at(hub, later);
        }
        hub->origin_count--;
        return;
    }
}

uint16_t hub_take_origin(hub_t *hub, uint8_t source)
{
    if (source != SOURCE_HOST_SET && source != SOURCE_BULK_SET && source != SOURCE_UART) {
        return 0; /* A preset, a GPIO or the firmware itself: no Link client */
    }

    /* Order, not recency: the oldest write still inside the window claims
     * the notification, so a second client's later write cannot take the
     * first client's attribution. */
    const uint32_t now = hub_now(hub);
    while (hub->origin_count > 0) {
        const hub_origin_t oldest = hub->origins[hub->origin_head];
        hub->origin_head = (hub->origin_head + 1) % HUB_ORIGIN_QUEUE_ENTRIES;
        hub->origin_count--;
        if (now - oldest.sent_ms <= LINK_ORIGIN_WINDOW_MS) {
            return oldest.session_id;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Client commands
 * ------------------------------------------------------------------------- */

static void respond(hub_t *hub, int slot, uint16_t tag, uint8_t status, const uint8_t *payload,
                    uint16_t length)
{
    const size_t frame_length =
        link_encode_cmd_response(hub->frame, sizeof(hub->frame), tag, status, payload, length);
    /* A response lost to a full queue is covered by the client's own
     * command timeout (Link spec 8.2); the hub has nothing better to do. */
    (void)hub_send_binary(hub, slot, hub->frame, frame_length);
}

/* The checks of bridge spec 8.3, in order. Returns the status to answer
 * with, or DSPI_STATUS_OK if the command may go to the DSPi. */
static uint8_t admission_status(const hub_t *hub, const hub_session_t *session,
                                const link_cmd_request_t *command, link_class_t class_)
{
    if (command->handle != HUB_DEVICE_HANDLE || !hub_device_online(hub)) {
        return LINK_STATUS_NO_DEVICE;
    }
    if (command->length > LINK_MAX_PAYLOAD_BYTES) {
        return LINK_STATUS_TOO_LARGE;
    }
    if (!link_policy_role_allows(session->role, class_)) {
        return LINK_STATUS_DENIED;
    }
    if (hub->device.locked_by != 0 && hub->device.locked_by != session->id) {
        return LINK_STATUS_LOCKED;
    }
    if (session->commands_in_flight >= LINK_MAX_INFLIGHT) {
        return LINK_STATUS_RATE_LIMITED;
    }
    if (command->is_get && command->length > HUB_UPDATE_MAX_GET_BYTES &&
        hub_update_in_progress(hub)) {
        return DSPI_STATUS_BUSY; /* Not run; retry once the update is over */
    }
    if (!command->is_get && command->request != REQ_SET_ALL_PARAMS &&
        command->length > DSPI_UART_MAX_SET_PAYLOAD_BYTES) {
        /* The firmware refuses this on its UART (Link spec 9.2); answer as
         * it would, without spending the link on it. */
        return DSPI_STATUS_OVERSIZE;
    }
    if (!command->is_get && command->request == REQ_SET_ALL_PARAMS && hub->bulk_set_busy) {
        return LINK_STATUS_RATE_LIMITED; /* The one bulk SET buffer is in use */
    }
    return DSPI_STATUS_OK;
}

/* Queues an admitted command for the uart task. Returns DSPI_STATUS_OK once
 * it is on its way, or the status to answer with now. */
static uint8_t enqueue_command(hub_t *hub, int slot, hub_session_t *session,
                               const link_cmd_request_t *command, link_class_t class_)
{
    hub_pending_t *entry = reserve_pending(hub);
    if (entry == NULL) {
        return LINK_STATUS_RATE_LIMITED;
    }
    const bool is_bulk_set = !command->is_get && command->request == REQ_SET_ALL_PARAMS;
    entry->kind = HUB_WORK_CLIENT_COMMAND;
    entry->slot = slot;
    entry->session_id = session->id;
    entry->client_tag = command->tag;
    /* A write-as-read GET changes the device too (Link spec 6.3). */
    entry->writes = !command->is_get || class_ != LINK_CLASS_READ;
    entry->is_bulk_set = is_bulk_set;

    /* Where the SET payload lives until the uart task has sent it. */
    uint8_t *payload_store = is_bulk_set ? hub->bulk_set_buffer : entry->payload;
    if (command->payload != NULL) {
        memcpy(payload_store, command->payload, command->length);
    }
    const dspi_request_t request = {
        .is_get = command->is_get,
        .request = command->request,
        .value = command->value,
        .index = command->index,
        .length = command->length,
        .payload = command->is_get ? NULL : payload_store,
    };
    /* Only a read is safe to repeat after no answer (bridge spec 7.3). */
    const bool may_retry = command->is_get && class_ == LINK_CLASS_READ;
    if (!hub->io.submit(hub->io.context, &request, may_retry, entry->tag)) {
        return LINK_STATUS_RATE_LIMITED;
    }

    if (entry->writes) {
        remember_write(hub, session->id, entry->tag); /* Before it can become the head */
    }
    commit_pending(hub);
    session->commands_in_flight++;
    hub->client_commands_pending++;
    hub->bulk_set_busy = hub->bulk_set_busy || is_bulk_set;
    return DSPI_STATUS_OK;
}

void hub_commands_handle_frame(hub_t *hub, int slot, const uint8_t *data, size_t length)
{
    hub_session_t *session = hub_open_session(hub, slot);
    if (session == NULL) {
        return;
    }
    link_cmd_request_t command;
    if (!link_decode_cmd_request(data, length, &command)) {
        respond(hub, slot, link_frame_tag(data, length), LINK_STATUS_BAD_FRAME, NULL, 0);
        return;
    }

    const link_class_t class_ = link_policy_class(command.request, command.is_get);
    uint8_t status = admission_status(hub, session, &command, class_);
    if (status == DSPI_STATUS_OK) {
        status = enqueue_command(hub, slot, session, &command, class_);
    }
    if (status != DSPI_STATUS_OK) {
        respond(hub, slot, command.tag, status, NULL, 0);
    }
}

/* The Link status for how a request ended (Link spec 8.1, bridge spec 7.3). */
static uint8_t link_status_for(const dspi_reply_t *reply)
{
    switch (reply->kind) {
    case DSPI_REPLY_ANSWERED:
        return reply->status; /* The DSPi's own status, unchanged */
    case DSPI_REPLY_TIMEOUT:
    case DSPI_REPLY_CORRUPT:
        /* A damaged answer is no answer: the command may or may not have
         * run. Clients retry on CRC_ERROR, so that status is passed on only
         * when the DSPi itself says it did not run the frame (above). */
        return LINK_STATUS_TIMEOUT;
    case DSPI_REPLY_NO_DEVICE:
        return LINK_STATUS_NO_DEVICE;
    }
    return LINK_STATUS_TIMEOUT;
}

static void finish_client_command(hub_t *hub, const hub_pending_t *work, const dspi_reply_t *reply)
{
    const uint8_t status = link_status_for(reply);
    const uint32_t round_trip_ms = hub_now(hub) - work->submitted_ms;

    hub->device.commands++;
    hub->device.errors += status == DSPI_STATUS_OK ? 0 : 1;
    hub->device.average_rtt_ms =
        (hub->device.average_rtt_ms * (HUB_RTT_AVERAGE_WEIGHT - 1) + round_trip_ms) /
        HUB_RTT_AVERAGE_WEIGHT;
    if (work->writes && status == DSPI_STATUS_OK) {
        hub_snapshot_mark_stale(hub);
    } else if (work->writes) {
        forget_write(hub, work->tag);
    }

    /* The session may have closed, and its slot been reused, meanwhile. */
    hub_session_t *session = hub_open_session(hub, work->slot);
    if (session == NULL || session->id != work->session_id) {
        return;
    }
    session->commands_in_flight--;
    respond(hub, work->slot, work->client_tag, status, reply->payload,
            status == DSPI_STATUS_OK ? reply->length : 0);
}

/* Finishes one piece of work with its answer. */
static void complete_work(hub_t *hub, const hub_pending_t *work, const dspi_reply_t *reply)
{
    switch (work->kind) {
    case HUB_WORK_CLIENT_COMMAND:
        hub->client_commands_pending--;
        if (work->is_bulk_set) {
            hub->bulk_set_busy = false;
        }
        finish_client_command(hub, work, reply);
        break;
    case HUB_WORK_POLL:
        hub->poll_pending = false;
        hub_polls_on_reply(hub, work, reply);
        break;
    case HUB_WORK_SNAPSHOT_BULK:
    case HUB_WORK_SNAPSHOT_STATUS:
        hub_snapshot_on_reply(hub, work, reply);
        break;
    }
}

/* Takes the oldest request off the queue and finishes it as a timeout, so
 * its client hears back and the hub's counts stay true. */
static void finish_oldest_as_lost(hub_t *hub)
{
    static const dspi_reply_t LOST_REPLY = {.kind = DSPI_REPLY_TIMEOUT};
    hub_pending_t lost;
    pop_pending(hub, &lost);
    complete_work(hub, &lost, &LOST_REPLY);
}

void hub_reply(hub_t *hub, uint32_t tag, const dspi_reply_t *reply)
{
    /* An answer older than the oldest request belongs to one already given
     * up on (hub_commands_tick()); its client has heard back. */
    if (hub->pending_count == 0 || tag_before(tag, hub->pending[hub->pending_head].tag)) {
        return;
    }
    /* Answers arrive in order, so anything older than `tag` lost its answer
     * on the way (a full event queue). */
    while (hub->pending_count > 0 && tag_before(hub->pending[hub->pending_head].tag, tag)) {
        finish_oldest_as_lost(hub);
    }
    if (hub->pending_count == 0 || hub->pending[hub->pending_head].tag != tag) {
        return; /* Not a request the hub is waiting for */
    }

    hub_pending_t work;
    pop_pending(hub, &work);
    complete_work(hub, &work, reply);

    /* Keep the link busy: the next due poll can start now. */
    hub_polls_tick(hub);
}

void hub_commands_tick(hub_t *hub)
{
    if (hub->pending_count > 0 &&
        hub_now(hub) - hub->pending_head_since_ms >= HUB_REPLY_LOST_AFTER_MS) {
        finish_oldest_as_lost(hub);
        hub_polls_tick(hub);
    }
}
