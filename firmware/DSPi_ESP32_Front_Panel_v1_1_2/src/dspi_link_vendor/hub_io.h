/*
 * hub_io.h - everything the hub reaches outside itself.
 *
 * The hub is a portable state machine (bridge spec 6.2). It never touches a
 * socket, the UART, flash or a clock directly; it calls these functions,
 * which hub_task.c supplies on the ESP32 and the host tests supply as
 * fakes. Every function receives `context` first, and the hub calls them
 * only from the task that runs it (the hub task on the ESP32).
 *
 * Sessions are numbered by slot, 0 to max_sessions - 1. The network side
 * owns the mapping from slots to sockets.
 */

#ifndef HUB_IO_H
#define HUB_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "dspi_frame.h"
#include "settings_model.h"

/* A SHA-256 digest, as hub.update gives it (Link spec 7.14). */
#define HUB_IO_SHA256_BYTES 32

/* What hub.restart and hub.reset erase before restarting (Link spec 7.13),
 * in rising order: a later request may raise the level, never lower it. */
typedef enum {
    HUB_RESTART_ONLY,    /* hub.restart: erase nothing */
    HUB_RESTART_NETWORK, /* hub.reset "network": forget the home network */
    HUB_RESTART_FACTORY, /* hub.reset "factory": erase everything */
} hub_restart_level_t;

/* How a request to start background work went. */
typedef enum {
    HUB_IO_OK,     /* Started; the answer comes later */
    HUB_IO_BUSY,   /* The same work is already running */
    HUB_IO_FAILED, /* Could not be started (out of memory, no task) */
} hub_io_result_t;

typedef struct {
    void *context;

    /* Queues one WebSocket message for a session and returns at once. The
     * bytes are copied. Returns false when the session's outbound queue is
     * full (bridge spec 8.2), in which case nothing was queued. */
    bool (*send_text)(void *context, int slot, const char *text, size_t length);
    bool (*send_binary)(void *context, int slot, const uint8_t *data, size_t length);

    /* Sends a WebSocket ping (Link spec 4). */
    void (*ping)(void *context, int slot);

    /* Closes a session's socket with a WebSocket close code (Link spec 5).
     * The network side later reports it with hub_session_closed(). */
    void (*close)(void *context, int slot, uint16_t code);

    /* Queues a request for the uart task; the answer comes back through
     * hub_reply() with `tag`. A SET's payload must stay valid until then.
     * Returns false if the queue is full. */
    bool (*submit)(void *context, const dspi_request_t *request, bool may_retry, uint32_t tag);

    /* Milliseconds from any fixed point; wraps after 49 days. */
    uint32_t (*now_ms)(void *context);

    /* Saves new settings and puts them into effect: the UART is restarted on
     * new pins or rate, mDNS takes a new hostname (bridge spec 8.11). Called
     * only with settings that pass settings_model_find_problem(). Returns
     * false, having logged why, if they could not be saved (nothing
     * changed) or not put into effect (saved; they apply at the next
     * start). */
    bool (*apply_settings)(void *context, const bridge_settings_t *settings);

    /* Starts a Wi-Fi scan in the background (Link spec 7.11), one at a
     * time. On HUB_IO_OK the result comes back through hub_reply_later()
     * with these three values; HUB_IO_BUSY while another scan runs. */
    hub_io_result_t (*start_wifi_scan)(void *context, int slot, uint16_t session_id,
                                       int request_id);

    /* Tries a new Wi-Fi network, keeping the old one to return to (Link spec
     * 7.11). False if the name or password cannot be used. */
    bool (*wifi_join)(void *context, const char *ssid, const char *password);

    /* Restarts the bridge shortly, after erasing what `level` says (Link
     * spec 7.13). Called once every session has been told. */
    void (*restart)(void *context, hub_restart_level_t level);

    /* Saves and loads device names by serial (bridge spec 8.7). save returns
     * false, having logged why, when the name could not be saved; load
     * returns false when no name is saved. */
    bool (*save_device_name)(void *context, const char *serial, const char *name);
    bool (*load_device_name)(void *context, const char *serial, char *name, size_t size);

    /* The hub's public description changed (name, devices): refresh the
     * DNS-SD TXT record (bridge spec 8.8). */
    void (*advertise)(void *context);

    /* Adds the `bridge` object's fields to a hub.stats reply (bridge spec
     * 8.7). May be NULL. */
    void (*describe_bridge)(void *context, cJSON *bridge);

    /* Firmware updates (Link spec 7.14, bridge spec 14.2). The I/O writes
     * the image beside the running firmware, away from the hub task, and
     * reports back through hub_update_written() for each chunk in flash and
     * hub_update_finished() once, at the end or on a failure. Each report
     * carries the generation update_begin returned, so a report from an
     * abandoned update is never taken for the next one's.
     *
     *   update_capacity  the largest image it can take; 0 for no updates
     *   update_begin     prepares for `size` bytes whose SHA-256 is `sha256`;
     *                    returns the update's generation, or 0 if it cannot
     *   update_write     queues the chunk at `offset`, copying it. False when
     *                    its queue is full. Once the last byte is written it
     *                    checks the whole image and, if it is good, makes it
     *                    the one that starts next.
     *   update_abort     drops the update in progress, if any: nothing more
     *                    of it is written or switched to */
    uint32_t (*update_capacity)(void *context);
    uint32_t (*update_begin)(void *context, uint32_t size,
                             const uint8_t sha256[HUB_IO_SHA256_BYTES]);
    bool (*update_write)(void *context, uint32_t offset, const uint8_t *data, size_t length);
    void (*update_abort)(void *context);
} hub_io_t;

#endif /* HUB_IO_H */
