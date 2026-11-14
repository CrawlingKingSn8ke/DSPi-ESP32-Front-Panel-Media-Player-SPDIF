/*
 * hub_device.c - the device record and its events, notifications,
 * snapshots, locks and names.
 *
 * Implements Link spec 7.4 (inventory), 7.5 (snapshot), 7.7 (locks) and the
 * relay of 8.3, as bridge spec 7.6 and 8.4 to 8.7 describe. The bridge has
 * one device, always at handle 0.
 *
 * Portable: no ESP-IDF headers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub_internal.h"
#include "link_base64.h"

/* Commands the hub sends for itself (firmware commands.md). */
#define REQ_GET_STATUS 0x50
#define REQ_GET_ALL_PARAMS 0xA0
#define STATUS_COMBINED_FRAME 9 /* REQ_GET_STATUS wValue for the meter frame */

/* The v2 notification packet (notification_protocol_v2_spec.md). */
#define PACKET_VERSION_2 2
#define PACKET_HEADER_BYTES 4 /* version, event, flags, seq */
#define EVENT_PARAM_CHANGED 0x02
#define EVENT_BULK_INVALIDATED 0x03
#define PARAM_CHANGED_SOURCE_OFFSET 8
#define BULK_INVALIDATED_SOURCE_OFFSET 4

/* The bulk header (WireHeader), stable across wire formats. */
#define BULK_HEADER_BYTES 16
#define BULK_WIRE_VERSION_OFFSET 0
#define BULK_CHANNELS_OFFSET 2
#define BULK_INPUTS_OFFSET 4

/* lock.acquire without a timeout gets the longest one allowed (Link spec
 * 7.7). */
#define LOCK_MAX_TIMEOUT_MS 60000

/* "255.255.255" and its NUL. */
#define FIRMWARE_VERSION_TEXT_BYTES 12

/* ---------------------------------------------------------------------------
 * The device object and its events (Link spec 7.4)
 * ------------------------------------------------------------------------- */

bool hub_device_online(const hub_t *hub)
{
    return hub->device.state == HUB_DEVICE_ONLINE;
}

bool hub_device_notifies(const hub_t *hub)
{
    const dspi_identity_t *identity = &hub->device.identity;
    return hub->device.state == HUB_DEVICE_ABSENT || !identity->has_uart_config ||
           identity->notify_enabled;
}

/* "major.minor.patch" from the platform reply: bytes 1, 4 and 5 on current
 * firmware, or the nibbles of byte 2 on firmware that sends only 4 bytes. */
static void format_firmware_version(const dspi_identity_t *identity, char *out, size_t size)
{
    const uint8_t *platform = identity->platform;
    if (identity->platform_length >= 6) {
        snprintf(out, size, "%u.%u.%u", platform[1], platform[4], platform[5]);
    } else {
        snprintf(out, size, "%u.%u.%u", platform[1], platform[2] >> 4, platform[2] & 0x0F);
    }
}

cJSON *hub_device_json(const hub_t *hub)
{
    const hub_device_t *device = &hub->device;
    char firmware[FIRMWARE_VERSION_TEXT_BYTES];
    format_firmware_version(&device->identity, firmware, sizeof(firmware));

    cJSON *object = cJSON_CreateObject();
    if (object == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(object, "handle", HUB_DEVICE_HANDLE);
    cJSON_AddStringToObject(object, "serial", device->identity.serial);
    cJSON_AddStringToObject(object, "name", device->name);
    cJSON_AddNumberToObject(object, "platform", device->identity.platform[0]);
    cJSON_AddStringToObject(object, "fw", firmware);
    if (device->identity.platform_length >= 7) {
        cJSON_AddNumberToObject(object, "fw_beta", device->identity.platform[6]);
    }
    cJSON_AddNumberToObject(object, "outputs", device->identity.platform[3]);
    if (device->header_known) {
        cJSON_AddNumberToObject(object, "inputs", device->inputs);
        cJSON_AddNumberToObject(object, "wire_version", device->wire_version);
    }
    cJSON_AddStringToObject(object, "state", hub_device_online(hub) ? "online" : "offline");
    cJSON_AddStringToObject(object, "link", "uart");
    if (device->locked_by != 0) {
        cJSON_AddNumberToObject(object, "locked_by", device->locked_by);
    } else {
        cJSON_AddNullToObject(object, "locked_by");
    }
    return object;
}

/* Sends device.added or device.changed with the current device object. */
static void broadcast_device(hub_t *hub, const char *event)
{
    cJSON *message = cJSON_CreateObject();
    if (message == NULL) {
        return;
    }
    cJSON_AddStringToObject(message, "t", event);
    hub_json_add(message, "device", hub_device_json(hub));
    hub_broadcast_json(hub, message);
}

static void broadcast_resync(hub_t *hub, uint8_t reason)
{
    const size_t length =
        link_encode_resync(hub->frame, sizeof(hub->frame), HUB_DEVICE_HANDLE, reason);
    for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
        if (hub->sessions[slot].state == HUB_SESSION_OPEN &&
            !hub_send_binary(hub, (int)slot, hub->frame, length)) {
            hub->sessions[slot].resync_owed = true;
        }
    }
}

/* Answers every snapshot waiter with an error. */
static void fail_snapshot_waiters(hub_t *hub, const char *code, const char *msg)
{
    for (size_t i = 0; i < hub->snapshot.waiter_count; i++) {
        const hub_snapshot_waiter_t *waiter = &hub->snapshot.waiters[i];
        const hub_session_t *session = hub_open_session(hub, waiter->slot);
        if (session != NULL && session->id == waiter->session_id) {
            hub_reply_error(hub, waiter->slot, waiter->request_id, code, msg);
        }
    }
    hub->snapshot.waiter_count = 0;
}

static void release_lock(hub_t *hub)
{
    hub->device.locked_by = 0;
    hub->device.lock_expires_ms = 0;
}

/* The device is gone for good: tell everyone and start again empty. */
static void forget_device(hub_t *hub)
{
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "t", "device.removed");
    cJSON_AddNumberToObject(message, "handle", HUB_DEVICE_HANDLE);
    cJSON_AddStringToObject(message, "serial", hub->device.identity.serial);
    hub_broadcast_json(hub, message);

    hub_polls_clear(hub);
    fail_snapshot_waiters(hub, "no_device", NULL);
    memset(&hub->device, 0, sizeof(hub->device));
    hub->device.state = HUB_DEVICE_ABSENT;
    hub->snapshot.valid = false;
    hub->snapshot.read_tag = 0; /* An answer still on its way is for the old board */
    hub->io.advertise(hub->io.context);
}

/* Starts the next snapshot read if none is under way: the settings blob and
 * then the status frame, or only the status frame while the cached blob can
 * be trusted (bridge spec 8.6). The blob read also gives the device object
 * its wire_version and inputs. */
static void start_snapshot_read(hub_t *hub);

void hub_device_found(hub_t *hub, const dspi_identity_t *identity, uint32_t baud)
{
    hub_device_t *device = &hub->device;
    const bool known = device->state != HUB_DEVICE_ABSENT;
    const bool same_device = known && strcmp(device->identity.serial, identity->serial) == 0;

    if (known && !same_device) {
        forget_device(hub); /* Another board was swapped in (bridge spec 7.6) */
    }

    device->identity = *identity;
    device->baud = baud;
    device->state = HUB_DEVICE_ONLINE;
    device->seq_known = false;

    if (same_device) {
        broadcast_device(hub, "device.changed");
        broadcast_resync(hub, LINK_RESYNC_REATTACHED);
    } else {
        if (!hub->io.load_device_name(hub->io.context, identity->serial, device->name,
                                      sizeof(device->name))) {
            /* "DSPi" and the serial's last four characters (bridge spec 8.7). */
            const size_t serial_length = strlen(identity->serial);
            const char *tail = identity->serial + (serial_length > 4 ? serial_length - 4 : 0);
            snprintf(device->name, sizeof(device->name), "DSPi %s", tail);
        }
        broadcast_device(hub, "device.added");
    }
    hub->io.advertise(hub->io.context);

    if (baud != hub->config.settings.uart_baud) {
        /* Detection found the DSPi at another rate: keep it, so the next
         * start begins there, and show it to the settings page (bridge spec
         * 8.11). The UART already runs at this rate and stays as it is. If
         * it cannot be saved (the I/O logs why), the next start simply
         * detects it again. */
        bridge_settings_t settings = hub->config.settings;
        settings.uart_baud = baud;
        if (hub_apply_settings(hub, &settings)) {
            hub_broadcast_config(hub);
        }
    }
    hub->snapshot.valid = false;
    hub->snapshot.read_tag = 0;
    start_snapshot_read(hub);
}

void hub_device_lost(hub_t *hub)
{
    if (hub->device.state != HUB_DEVICE_ONLINE) {
        return;
    }
    hub->device.state = HUB_DEVICE_OFFLINE;
    hub->device.offline_since_ms = hub_now(hub);
    hub_polls_clear(hub); /* Subscriptions end when the device goes offline (Link spec 7.6) */
    fail_snapshot_waiters(hub, "no_device", NULL);
    hub->snapshot.read_tag = 0; /* An answer still on its way is ignored */
    broadcast_device(hub, "device.changed");
}

void hub_device_tick(hub_t *hub)
{
    const uint32_t now = hub_now(hub);
    hub_device_t *device = &hub->device;

    if (device->state == HUB_DEVICE_OFFLINE &&
        now - device->offline_since_ms >= LINK_OFFLINE_HANDLE_RETAIN_MS) {
        forget_device(hub);
        return;
    }
    if (device->locked_by != 0 && (int32_t)(now - device->lock_expires_ms) >= 0) {
        release_lock(hub);
        broadcast_device(hub, "device.changed");
    }
}

/* ---------------------------------------------------------------------------
 * Notifications (Link spec 8.3, bridge spec 8.4)
 * ------------------------------------------------------------------------- */

/* The packet's source byte, or -1 for an event that has none. */
static int packet_source(const uint8_t *packet, size_t length)
{
    const uint8_t event = packet[1];
    if (event == EVENT_PARAM_CHANGED && length > PARAM_CHANGED_SOURCE_OFFSET) {
        return packet[PARAM_CHANGED_SOURCE_OFFSET];
    }
    if (event == EVENT_BULK_INVALIDATED && length > BULK_INVALIDATED_SOURCE_OFFSET) {
        return packet[BULK_INVALIDATED_SOURCE_OFFSET];
    }
    return -1;
}

void hub_notification(hub_t *hub, const uint8_t *packet, size_t length)
{
    hub_device_t *device = &hub->device;
    if (length < PACKET_HEADER_BYTES || packet[0] != PACKET_VERSION_2) {
        return; /* v1 packets are never relayed (Link spec 8.3) */
    }

    /* A gap in the firmware's seq means the DSPi dropped events for the
     * bridge: every client must re-read (bridge spec 8.4). */
    const uint8_t seq = packet[3];
    if (device->seq_known && (uint8_t)(seq - device->last_seq) != 1) {
        broadcast_resync(hub, LINK_RESYNC_DROPPED);
    }
    device->last_seq = seq;
    device->seq_known = true;

    /* Any notification may describe a change to the settings blob. */
    hub_snapshot_mark_stale(hub);

    const int source = packet_source(packet, length);
    const uint16_t origin = source >= 0 ? hub_take_origin(hub, (uint8_t)source) : 0;

    for (size_t i = 0; i < hub->config.max_sessions; i++) {
        hub_session_t *session = &hub->sessions[i];
        if (session->state != HUB_SESSION_OPEN) {
            continue;
        }
        /* The sequence advances even if the frame is dropped, so the client
         * sees the gap and re-reads (Link spec 8.3). */
        const size_t frame_length =
            link_encode_notify(hub->frame, sizeof(hub->frame), session->notify_sequence++,
                               HUB_DEVICE_HANDLE, origin, packet, length);
        if (!hub_send_binary(hub, (int)i, hub->frame, frame_length)) {
            device->notifications_dropped++;
            session->resync_owed = true;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Snapshot (Link spec 7.5, bridge spec 8.6)
 * ------------------------------------------------------------------------- */

void hub_snapshot_mark_stale(hub_t *hub)
{
    hub->snapshot.stale = true;
}

/* True while the cached blob is known to be current. Never when the DSPi
 * sends no notifications: then nothing would tell the cache it changed. */
static bool blob_trusted(const hub_t *hub)
{
    return hub->snapshot.valid && !hub->snapshot.stale && hub_device_notifies(hub);
}

/* Queues the status read, which every waiter now present is to receive. */
static uint32_t submit_status_read(hub_t *hub)
{
    /* The meter frame is 2 x channels + 7 bytes (Link spec 7.6). */
    const dspi_request_t status_read = {
        .is_get = true,
        .request = REQ_GET_STATUS,
        .value = STATUS_COMBINED_FRAME,
        .length = (uint16_t)(2 * hub->device.channels + 7),
    };
    const uint32_t tag = hub_submit_own(hub, HUB_WORK_SNAPSHOT_STATUS, &status_read, true, NULL);
    if (tag != 0) {
        for (size_t i = 0; i < hub->snapshot.waiter_count; i++) {
            hub->snapshot.waiters[i].covered = true;
        }
    }
    return tag;
}

static void start_snapshot_read(hub_t *hub)
{
    hub_snapshot_t *snapshot = &hub->snapshot;
    if (snapshot->read_tag != 0 || !hub_device_online(hub)) {
        return;
    }
    if (hub_update_in_progress(hub)) {
        return; /* The blob is too large to read now; hub_snapshot_resume() */
    }
    if (blob_trusted(hub)) {
        snapshot->read_tag = submit_status_read(hub);
    } else {
        /* Clear `stale` now: a change during the read marks it stale again. */
        snapshot->stale = false;
        const dspi_request_t bulk_read = {
            .is_get = true, .request = REQ_GET_ALL_PARAMS, .length = 0};
        snapshot->read_tag = hub_submit_own(hub, HUB_WORK_SNAPSHOT_BULK, &bulk_read, true, NULL);
    }
    if (snapshot->read_tag == 0) {
        snapshot->stale = true;
        fail_snapshot_waiters(hub, "busy", "the bridge's queue to the DSPi is full");
    }
}

void hub_snapshot_resume(hub_t *hub)
{
    if (hub->snapshot.waiter_count > 0 || !hub->snapshot.valid) {
        start_snapshot_read(hub);
    }
}

/* Sends the cached snapshot to one waiter. */
static void send_snapshot(hub_t *hub, int slot, int id)
{
    const hub_snapshot_t *snapshot = &hub->snapshot;
    char *bulk_b64 = malloc(link_base64_encoded_length(snapshot->bulk_length) + 1);
    char *status_b64 = malloc(link_base64_encoded_length(snapshot->status_length) + 1);
    if (bulk_b64 == NULL || status_b64 == NULL) {
        free(bulk_b64);
        free(status_b64);
        hub_reply_error(hub, slot, id, "internal", "out of memory");
        return;
    }
    link_base64_encode(snapshot->bulk, snapshot->bulk_length, bulk_b64);
    link_base64_encode(snapshot->status, snapshot->status_length, status_b64);

    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        free(bulk_b64);
        free(status_b64);
        hub_reply_error(hub, slot, id, "internal", "out of memory");
        return;
    }
    cJSON_AddNumberToObject(body, "handle", HUB_DEVICE_HANDLE);
    if (hub->device.header_known) {
        cJSON_AddNumberToObject(body, "wire_version", hub->device.wire_version);
    }
    cJSON_AddNumberToObject(body, "age_ms", hub_now(hub) - snapshot->read_ms);
    cJSON_AddStringToObject(body, "bulk_b64", bulk_b64);
    cJSON_AddStringToObject(body, "status_b64", status_b64);
    free(bulk_b64);
    free(status_b64);
    hub_reply_ok(hub, slot, id, body);
}

/* Adds `slot`'s request to those waiting for the snapshot. A session has
 * at most one place, so one client cannot crowd out the rest: a second
 * request while its first waits is answered `busy`. False then. */
static bool add_snapshot_waiter(hub_t *hub, int slot, int id)
{
    hub_snapshot_t *snapshot = &hub->snapshot;
    for (size_t i = 0; i < snapshot->waiter_count; i++) {
        if (snapshot->waiters[i].slot == slot) {
            hub_reply_error(hub, slot, id, "busy", "this session already waits for a snapshot");
            return false;
        }
    }
    snapshot->waiters[snapshot->waiter_count++] = (hub_snapshot_waiter_t){
        .slot = slot, .session_id = hub->sessions[slot].id, .request_id = id};
    return true;
}

void hub_handle_device_snapshot(hub_t *hub, int slot, int id, const cJSON *message)
{
    int handle = -1;
    (void)hub_json_int(message, "handle", &handle); /* Absent: -1, no such device */
    if (handle != HUB_DEVICE_HANDLE || !hub_device_online(hub)) {
        hub_reply_error(hub, slot, id, "no_device", NULL);
        return;
    }
    /* The status frame is read fresh for every snapshot, and the blob too
     * unless the cache is current (bridge spec 8.6). A request that comes
     * while a status read is already on its way waits for the next one. */
    if (add_snapshot_waiter(hub, slot, id)) { /* Room for one per session */
        start_snapshot_read(hub);
    }
}

/* Reads the bulk header for the device object. The first time, the device
 * object gains wire_version and inputs, so clients hear device.changed. */
static void learn_bulk_header(hub_t *hub)
{
    if (hub->snapshot.bulk_length < BULK_HEADER_BYTES) {
        return;
    }
    hub_device_t *device = &hub->device;
    const bool first_time = !device->header_known;
    device->wire_version = hub->snapshot.bulk[BULK_WIRE_VERSION_OFFSET];
    device->channels = hub->snapshot.bulk[BULK_CHANNELS_OFFSET];
    device->inputs = hub->snapshot.bulk[BULK_INPUTS_OFFSET];
    device->header_known = true;
    if (first_time) {
        broadcast_device(hub, "device.changed");
    }
}

/* Answers the waiters a status read was asked for, and starts another read
 * for any that came while it ran. */
static void serve_snapshot_waiters(hub_t *hub)
{
    hub_snapshot_t *snapshot = &hub->snapshot;
    size_t kept = 0;
    for (size_t i = 0; i < snapshot->waiter_count; i++) {
        const hub_snapshot_waiter_t waiter = snapshot->waiters[i];
        if (!waiter.covered) {
            snapshot->waiters[kept++] = waiter;
            continue;
        }
        const hub_session_t *session = hub_open_session(hub, waiter.slot);
        if (session != NULL && session->id == waiter.session_id) {
            send_snapshot(hub, waiter.slot, waiter.request_id);
        }
    }
    snapshot->waiter_count = kept;
    if (kept > 0) {
        start_snapshot_read(hub);
    }
}

void hub_snapshot_on_reply(hub_t *hub, const hub_pending_t *work, const dspi_reply_t *reply)
{
    hub_snapshot_t *snapshot = &hub->snapshot;
    if (work->tag != snapshot->read_tag) {
        return; /* For a read given up on: the device went away meanwhile */
    }
    snapshot->read_tag = 0;
    if (reply->kind != DSPI_REPLY_ANSWERED || reply->status != DSPI_STATUS_OK) {
        snapshot->stale = true;
        fail_snapshot_waiters(hub, "busy", "the DSPi did not answer the snapshot read");
        return;
    }

    if (work->kind == HUB_WORK_SNAPSHOT_BULK) {
        snapshot->bulk_length =
            reply->length < sizeof(snapshot->bulk) ? reply->length : sizeof(snapshot->bulk);
        memcpy(snapshot->bulk, reply->payload, snapshot->bulk_length);
        snapshot->read_ms = hub_now(hub);
        learn_bulk_header(hub);
        snapshot->read_tag = submit_status_read(hub);
        if (snapshot->read_tag == 0) {
            snapshot->valid = false; /* The blob and the status no longer match */
            fail_snapshot_waiters(hub, "busy", "the bridge's queue to the DSPi is full");
        }
        return;
    }

    snapshot->status_length =
        reply->length < sizeof(snapshot->status) ? reply->length : sizeof(snapshot->status);
    memcpy(snapshot->status, reply->payload, snapshot->status_length);
    snapshot->valid = true;
    serve_snapshot_waiters(hub);
}

/* ---------------------------------------------------------------------------
 * Inventory and names (Link spec 7.4)
 * ------------------------------------------------------------------------- */

void hub_handle_device_list(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    cJSON *body = cJSON_CreateObject();
    cJSON *devices = cJSON_AddArrayToObject(body, "devices");
    if (hub->device.state != HUB_DEVICE_ABSENT) {
        hub_json_append(devices, hub_device_json(hub));
    }
    hub_reply_ok(hub, slot, id, body);
}

void hub_handle_device_rename(hub_t *hub, int slot, int id, const cJSON *message)
{
    int handle = -1;
    (void)hub_json_int(message, "handle", &handle); /* Absent: -1, no such device */
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(message, "name");

    if (hub->sessions[slot].role == LINK_ROLE_VIEWER) {
        hub_reply_error(hub, slot, id, "denied", "control or admin role required"); /* Link 7.4 */
        return;
    }
    if (handle != HUB_DEVICE_HANDLE || hub->device.state == HUB_DEVICE_ABSENT) {
        hub_reply_error(hub, slot, id, "no_device", NULL);
        return;
    }
    if (!cJSON_IsString(name) || !settings_name_is_valid(name->valuestring)) {
        hub_reply_error(hub, slot, id, "bad_request", "name must be 1 to 63 bytes of UTF-8");
        return;
    }
    if (!hub->io.save_device_name(hub->io.context, hub->device.identity.serial,
                                  name->valuestring)) {
        hub_reply_error(hub, slot, id, "internal", "the name could not be saved");
        return;
    }
    snprintf(hub->device.name, sizeof(hub->device.name), "%s", name->valuestring);
    hub_reply_ok(hub, slot, id, NULL);
    broadcast_device(hub, "device.changed");
    hub->io.advertise(hub->io.context);
}

/* ---------------------------------------------------------------------------
 * Locks (Link spec 7.7)
 * ------------------------------------------------------------------------- */

void hub_handle_lock_acquire(hub_t *hub, int slot, int id, const cJSON *message)
{
    const hub_session_t *session = &hub->sessions[slot];
    int handle = -1;
    (void)hub_json_int(message, "handle", &handle); /* Absent: -1, no such device */
    if (handle != HUB_DEVICE_HANDLE || !hub_device_online(hub)) {
        hub_reply_error(hub, slot, id, "no_device", NULL);
        return;
    }
    if (session->role == LINK_ROLE_VIEWER) {
        hub_reply_error(hub, slot, id, "denied", "a viewer cannot write");
        return;
    }

    int timeout_ms = LOCK_MAX_TIMEOUT_MS;
    if (cJSON_GetObjectItemCaseSensitive(message, "timeout_ms") != NULL &&
        (!hub_json_int(message, "timeout_ms", &timeout_ms) || timeout_ms < 0)) {
        hub_reply_error(hub, slot, id, "bad_request", "timeout_ms must be 0 or more");
        return;
    }
    if (timeout_ms > LOCK_MAX_TIMEOUT_MS) {
        timeout_ms = LOCK_MAX_TIMEOUT_MS;
    }
    if (hub->device.locked_by != 0 && hub->device.locked_by != session->id) {
        hub_reply_error(hub, slot, id, "locked", NULL);
        return;
    }

    hub->device.locked_by = session->id;
    hub->device.lock_expires_ms = hub_now(hub) + (uint32_t)timeout_ms;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "timeout_ms", timeout_ms);
    hub_reply_ok(hub, slot, id, body);
    broadcast_device(hub, "device.changed");
}

void hub_handle_lock_release(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    const uint16_t session_id = hub->sessions[slot].id;
    if (hub->device.locked_by != 0 && hub->device.locked_by != session_id) {
        hub_reply_error(hub, slot, id, "locked", "another session holds the lock");
        return;
    }
    const bool was_held = hub->device.locked_by == session_id;
    release_lock(hub);
    hub_reply_ok(hub, slot, id, NULL);
    if (was_held) {
        broadcast_device(hub, "device.changed");
    }
}

void hub_device_session_closed(hub_t *hub, int slot, uint16_t session_id)
{
    hub_snapshot_t *snapshot = &hub->snapshot;
    size_t kept = 0;
    for (size_t i = 0; i < snapshot->waiter_count; i++) {
        if (snapshot->waiters[i].slot != slot) {
            snapshot->waiters[kept++] = snapshot->waiters[i];
        }
    }
    snapshot->waiter_count = kept;

    /* Its lock ends with it (Link spec 7.7). */
    if (session_id != 0 && hub->device.locked_by == session_id) {
        release_lock(hub);
        broadcast_device(hub, "device.changed");
    }
}
