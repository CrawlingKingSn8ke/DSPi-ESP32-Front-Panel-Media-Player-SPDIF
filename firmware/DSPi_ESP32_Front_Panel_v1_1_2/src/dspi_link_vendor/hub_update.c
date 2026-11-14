/*
 * hub_update.c - the hub's own firmware update (Link spec 7.14, bridge spec
 * 14.2).
 *
 * One update at a time, from one admin session:
 *
 *   hub.update {size, sha256}   -> ok {xfer}, and the I/O prepares
 *   FWDATA chunks, in order     -> each handed to the I/O to write
 *   the I/O writes a chunk      -> FWDATA ack to the client
 *   the I/O checks the image    -> hub.update.done, then a restart if good
 *   anything goes wrong         -> hub.update.done with the reason
 *
 * A chunk is acknowledged only once it is in flash, so the client, which
 * keeps few chunks unanswered, never sends faster than the flash takes
 * them. The flash work itself happens in the I/O, off the hub task.
 *
 * Portable: no ESP-IDF headers.
 */

#include <string.h>

#include "hub_internal.h"

/* FWDATA ack statuses (Link spec 8.5). */
#define FWDATA_ACCEPTED 0
#define FWDATA_REJECTED 1

bool hub_update_available(const hub_t *hub)
{
    return hub->io.update_capacity != NULL && hub->io.update_capacity(hub->io.context) > 0;
}

bool hub_update_in_progress(const hub_t *hub)
{
    return hub->update.phase != HUB_UPDATE_IDLE;
}

/* Ends the update without a restart, and lets the reads it held back go. */
static void end_update(hub_t *hub)
{
    hub->update.phase = HUB_UPDATE_IDLE;
    hub_snapshot_resume(hub);
}

/* The session sending the update, if it is still connected. */
static bool sender_is_connected(hub_t *hub)
{
    const hub_update_t *update = &hub->update;
    const hub_session_t *session = hub_open_session(hub, update->slot);
    return session != NULL && session->id == update->session_id;
}

static void send_ack(hub_t *hub, int slot, uint16_t transfer, uint32_t offset, uint8_t status)
{
    const size_t length =
        link_encode_fwdata_ack(hub->frame, sizeof(hub->frame), transfer, offset, status);
    /* A lost ack stalls the sender, and the update is then abandoned after
     * OTA_IDLE_TIMEOUT_MS with hub.update.done saying so. */
    (void)hub_send_binary(hub, slot, hub->frame, length);
}

/* hub.update.done to the sender: {"ok": true, "version"} or {"ok": false,
 * "msg"}. */
static void send_done(hub_t *hub, bool ok, const char *text)
{
    if (!sender_is_connected(hub)) {
        return;
    }
    cJSON *done = cJSON_CreateObject();
    cJSON_AddStringToObject(done, "t", "hub.update.done");
    cJSON_AddBoolToObject(done, "ok", ok);
    cJSON_AddStringToObject(done, ok ? "version" : "msg", text);
    /* Lost to a full queue, the outcome still shows: a good image restarts
     * the bridge, and a bad one leaves the old version in hello. */
    (void)hub_send_json(hub, hub->update.slot, done);
}

/* Abandons the update, says why, and keeps the current firmware. */
static void fail(hub_t *hub, const char *why)
{
    hub->io.update_abort(hub->io.context);
    send_done(hub, false, why);
    end_update(hub);
}

/* The value of one hex digit, or -1. */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Reads exactly 2 x HUB_IO_SHA256_BYTES hex digits into a digest. */
static bool parse_sha256(const char *hex, uint8_t digest[HUB_IO_SHA256_BYTES])
{
    if (hex == NULL || strlen(hex) != HUB_IO_SHA256_BYTES * 2) {
        return false;
    }
    for (size_t i = 0; i < HUB_IO_SHA256_BYTES; i++) {
        const int high = hex_digit(hex[i * 2]);
        const int low = hex_digit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        digest[i] = (uint8_t)(high << 4 | low);
    }
    return true;
}

void hub_handle_update(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!hub_require_admin(hub, slot, id)) {
        return;
    }
    if (!hub_update_available(hub)) {
        hub_reply_error(hub, slot, id, "unsupported", "this bridge cannot update itself");
        return;
    }
    hub_update_t *update = &hub->update;
    if (update->phase != HUB_UPDATE_IDLE) {
        hub_reply_error(hub, slot, id, "busy", "another update is running");
        return;
    }

    int size = 0;
    if (!hub_json_int(message, "size", &size) || size <= 0) {
        hub_reply_error(hub, slot, id, "bad_request", "size must be a positive number");
        return;
    }
    if ((uint32_t)size > hub->io.update_capacity(hub->io.context)) {
        hub_reply_error(hub, slot, id, "bad_request", "the image is too large for this bridge");
        return;
    }
    uint8_t digest[HUB_IO_SHA256_BYTES];
    const cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(message, "sha256");
    if (!parse_sha256(cJSON_IsString(sha256) ? sha256->valuestring : NULL, digest)) {
        hub_reply_error(hub, slot, id, "bad_request", "sha256 must be 64 hex digits");
        return;
    }
    const uint32_t generation = hub->io.update_begin(hub->io.context, (uint32_t)size, digest);
    if (generation == 0) {
        hub_reply_error(hub, slot, id, "internal", "the bridge could not prepare for an update");
        return;
    }

    if (update->next_transfer == 0) {
        update->next_transfer = 1; /* 0 is never a transfer id */
    }
    update->phase = HUB_UPDATE_RECEIVING;
    update->slot = slot;
    update->session_id = hub->sessions[slot].id;
    update->transfer = update->next_transfer++;
    update->size = (uint32_t)size;
    update->received = 0;
    update->last_progress_ms = hub_now(hub);
    update->generation = generation;
    update->sender_left = false;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "xfer", update->transfer);
    hub_reply_ok(hub, slot, id, body);
}

void hub_update_chunk(hub_t *hub, int slot, const uint8_t *data, size_t length)
{
    link_fwdata_chunk_t chunk;
    if (!link_decode_fwdata(data, length, &chunk)) {
        return; /* Too short to answer: there is no offset to echo */
    }
    hub_update_t *update = &hub->update;
    const bool for_this_update = update->phase == HUB_UPDATE_RECEIVING && slot == update->slot &&
                                 hub->sessions[slot].id == update->session_id &&
                                 chunk.transfer == update->transfer;
    if (!for_this_update) {
        send_ack(hub, slot, chunk.transfer, chunk.offset, FWDATA_REJECTED);
        return;
    }

    /* Chunks run from offset 0 with no gaps or overlaps (Link spec 8.5). */
    const bool in_order = chunk.offset == update->received && chunk.length > 0 &&
                          chunk.length <= update->size - update->received;
    if (!in_order) {
        send_ack(hub, slot, chunk.transfer, chunk.offset, FWDATA_REJECTED);
        fail(hub, "the image arrived out of order");
        return;
    }
    if (!hub->io.update_write(hub->io.context, chunk.offset, chunk.data, chunk.length)) {
        send_ack(hub, slot, chunk.transfer, chunk.offset, FWDATA_REJECTED);
        fail(hub, "the image arrived faster than the bridge could write it");
        return;
    }

    update->received += (uint32_t)chunk.length;
    update->last_progress_ms = hub_now(hub);
    if (update->received == update->size) {
        update->phase = HUB_UPDATE_FINISHING;
    }
}

void hub_update_written(hub_t *hub, uint32_t generation, uint32_t offset)
{
    hub_update_t *update = &hub->update;
    if (update->phase == HUB_UPDATE_IDLE || generation != update->generation) {
        return; /* From an update abandoned meanwhile */
    }
    update->last_progress_ms = hub_now(hub);
    if (sender_is_connected(hub)) {
        send_ack(hub, update->slot, update->transfer, offset, FWDATA_ACCEPTED);
    }
}

void hub_update_finished(hub_t *hub, uint32_t generation, bool ok, const char *text)
{
    if (hub->update.phase == HUB_UPDATE_IDLE || generation != hub->update.generation) {
        return; /* From an update abandoned meanwhile */
    }
    send_done(hub, ok, text);
    if (ok) {
        /* The new image starts next; restart into it (Link spec 7.14). */
        hub->update.phase = HUB_UPDATE_IDLE;
        hub_restart(hub, HUB_RESTART_ONLY);
    } else {
        end_update(hub);
    }
}

void hub_update_session_closed(hub_t *hub, int slot)
{
    hub_update_t *update = &hub->update;
    if (update->phase == HUB_UPDATE_IDLE || slot != update->slot || update->sender_left) {
        return;
    }
    if (update->phase == HUB_UPDATE_FINISHING) {
        /* Every byte is here; only the check is left. Let it finish: a good
         * image then starts as Link spec 7.14 says, and the client confirms
         * the outcome from hello when it reconnects. */
        update->sender_left = true;
        return;
    }
    /* Part of the image is missing, so nobody is left to finish it. */
    hub->io.update_abort(hub->io.context);
    end_update(hub);
}

void hub_update_tick(hub_t *hub)
{
    const hub_update_t *update = &hub->update;
    if (update->phase != HUB_UPDATE_IDLE &&
        hub_now(hub) - update->last_progress_ms > OTA_IDLE_TIMEOUT_MS) {
        fail(hub, "the update stalled and was abandoned");
    }
}
