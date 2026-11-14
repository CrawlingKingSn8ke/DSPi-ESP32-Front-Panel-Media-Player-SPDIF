/*
 * hub.h - the DSPi Link hub: sessions, commands, notifications, polls,
 * snapshots and locks for the one DSPi behind the bridge.
 *
 * Implements Link spec 5 to 8 as bridge spec 8 describes. The hub is a plain
 * state machine. Something calls one hub function per event (a WebSocket
 * message, a UART reply, a timer tick) and the hub reacts through hub_io_t.
 * On the ESP32 that something is the hub task (hub_task.h), which is the
 * only caller of every function below, so nothing here needs locking. The
 * host tests call the same functions directly.
 *
 * Portable: no ESP-IDF headers.
 */

#ifndef HUB_H
#define HUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_table.h"
#include "bridge_config.h"
#include "dspi_link.h"
#include "hub_auth.h"
#include "hub_io.h"
#include "link_frames.h"
#include "link_policy.h"
#include "settings_model.h"

/* The bridge serves one DSPi, always at handle 0 (bridge spec 1). */
#define HUB_DEVICE_HANDLE 0

#define HUB_MAX_SESSIONS LINK_MAX_SESSIONS_WITH_PSRAM
#define HUB_POLL_SLOTS 16 /* Slots 0 to 15 per session (Link spec 7.6) */
#define HUB_PENDING_CAPACITY DSPI_UART_QUEUE_DEPTH

/* HUB_TICK_MS, HUB_MAX_POLL_GROUPS and HUB_ORIGIN_QUEUE_ENTRIES are tunables
 * and live in bridge_config.h. */

/* ---------------------------------------------------------------------------
 * State. Treat every type below as opaque; they are public only so a hub
 * can live in static storage.
 * ------------------------------------------------------------------------- */

typedef enum {
    HUB_SESSION_FREE,
    HUB_SESSION_AWAITING_HELLO,
    HUB_SESSION_AUTHENTICATING, /* hello done; must pair or present a token (PIN on) */
    HUB_SESSION_OPEN,
    HUB_SESSION_CLOSING, /* Close requested; waiting for hub_session_closed() */
} hub_session_state_t;

/* What one poll asks the DSPi. Identical keys from several sessions run
 * once (Link spec 7.6). */
typedef struct {
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} hub_poll_key_t;

typedef struct {
    bool active;
    hub_poll_key_t key;
    uint8_t hz;            /* The granted rate */
    uint32_t last_sent_ms; /* So a faster group never exceeds that rate */
} hub_poll_slot_t;

typedef struct {
    hub_session_state_t state;
    uint16_t id;      /* Link session id, never 0 */
    uint32_t address; /* The client's IPv4 address, for rate limits */
    uint16_t cid;     /* The paired client this session belongs to, or 0 */
    char client_name[AUTH_CLIENT_NAME_BYTES];
    link_role_t role;
    uint32_t opened_ms;
    uint32_t last_activity_ms; /* Any frame from the client, pongs included */
    uint32_t last_touch_ms;    /* When the paired client was last marked seen */
    uint32_t last_ping_ms;
    uint8_t commands_in_flight;
    uint16_t notify_sequence;
    uint16_t poll_sequence;
    bool resync_owed; /* A notification was dropped; RESYNC when there is room */
    hub_poll_slot_t polls[HUB_POLL_SLOTS];
} hub_session_t;

/* What a request sent to the uart task was for. */
typedef enum {
    HUB_WORK_CLIENT_COMMAND,
    HUB_WORK_POLL,
    HUB_WORK_SNAPSHOT_BULK,
    HUB_WORK_SNAPSHOT_STATUS,
} hub_work_kind_t;

/* One request waiting for the uart task. The uart task answers in order,
 * so these form a queue. */
typedef struct {
    uint32_t tag;
    hub_work_kind_t kind;
    int slot;            /* Client command: who sent it */
    uint16_t session_id; /* Client command: detects a slot reused meanwhile */
    uint16_t client_tag; /* Client command: the CMD frame's tag */
    bool writes;         /* Client command: a SET, or a GET that changes the device
                            (classed other than read), for origins and the cache */
    bool is_bulk_set;    /* Client command: its payload is in bulk_set_buffer */
    hub_poll_key_t poll; /* Poll: which one */
    uint32_t submitted_ms;
    uint8_t payload[DSPI_UART_MAX_SET_PAYLOAD_BYTES]; /* A small SET's payload */
} hub_pending_t;

typedef struct {
    hub_poll_key_t key;
    uint32_t period_ms;
    uint32_t next_due_ms;
    uint8_t failures_in_a_row;
} hub_poll_group_t;

typedef enum {
    HUB_DEVICE_ABSENT,
    HUB_DEVICE_ONLINE,
    HUB_DEVICE_OFFLINE, /* Known but not answering; handle kept for 60 s */
} hub_device_state_t;

typedef struct {
    hub_device_state_t state;
    dspi_identity_t identity;
    char name[SETTINGS_NAME_MAX_BYTES + 1];
    uint32_t baud;
    uint32_t offline_since_ms;
    bool header_known; /* The bulk header below has been read */
    uint8_t wire_version;
    uint8_t channels;
    uint8_t inputs;
    bool seq_known;
    uint8_t last_seq;   /* Of the last notification, to spot gaps */
    uint16_t locked_by; /* Session id, or 0 */
    uint32_t lock_expires_ms;
    uint32_t commands;
    uint32_t errors;
    uint32_t average_rtt_ms;
    uint32_t notifications_dropped;
} hub_device_t;

typedef struct {
    int slot;
    uint16_t session_id;
    int request_id;
    bool covered; /* A status read was asked for after this request */
} hub_snapshot_waiter_t;

typedef struct {
    uint8_t bulk[LINK_MAX_PAYLOAD_BYTES];
    uint16_t bulk_length;
    uint8_t status[DSPI_UART_MAX_GET_REPLY_BYTES];
    uint16_t status_length;
    bool valid;        /* bulk and status hold a complete read */
    bool stale;        /* The blob may have changed since it was read */
    uint32_t read_tag; /* The read in flight (hub_pending_t tag), or 0 */
    uint32_t read_ms;  /* When the blob was read */
    hub_snapshot_waiter_t waiters[HUB_MAX_SESSIONS];
    size_t waiter_count;
} hub_snapshot_t;

typedef struct {
    uint16_t session_id;
    uint32_t tag;     /* The write's hub_pending_t tag */
    uint32_t sent_ms; /* Queued, then when the uart task took it up */
} hub_origin_t;

typedef enum {
    HUB_UPDATE_IDLE,
    HUB_UPDATE_RECEIVING, /* Chunks are arriving */
    HUB_UPDATE_FINISHING, /* Every chunk has arrived; waiting for the check */
} hub_update_phase_t;

/* The firmware update in progress, if any (Link spec 7.14). */
typedef struct {
    hub_update_phase_t phase;
    int slot;            /* Who is sending it */
    uint16_t session_id; /* Detects a slot reused meanwhile */
    uint16_t transfer;   /* The xfer id its FWDATA chunks carry */
    uint16_t next_transfer;
    uint32_t size;
    uint32_t received;         /* Bytes handed to the I/O: the next chunk's offset */
    uint32_t last_progress_ms; /* A chunk arriving or being written */
    uint32_t generation;       /* The I/O's id for this update (hub_io_t) */
    bool sender_left;          /* The sender closed after the last chunk */
} hub_update_t;

typedef struct {
    bridge_settings_t settings; /* The hub id, name and everything hub.config shows */
    const board_table_t *board; /* For the choices hub.config offers */
    const char *version;        /* The bridge firmware's version */
    size_t max_sessions;        /* At most HUB_MAX_SESSIONS */
    uint32_t reserved_poll_bps; /* Combined panel's essential local reads; 0 upstream */

    /* The saved PIN and paired clients exist but could not be read. The
     * hub then fails safe rather than open: the PIN counts as on (hello
     * says `auth: pin`, `pin_source: fixed`), yet every auth.pair and
     * auth.token is answered `internal` and hub_check_pin() returns
     * HUB_AUTH_UNAVAILABLE, because no PIN or token can be checked.
     * hub_set_pin() works as usual and ends this, so the setup page after
     * a factory reset, or a factory reset itself, recovers the bridge. */
    bool auth_unreadable;
} hub_config_t;

typedef struct {
    hub_io_t io;
    hub_config_t config;
    hub_auth_t auth;
    uint64_t uptime_ms;      /* Since hub_init(), for hub.stats; never wraps */
    uint32_t uptime_seen_ms; /* The clock when uptime_ms was last brought up to date */

    hub_session_t sessions[HUB_MAX_SESSIONS];
    uint16_t next_session_id;

    hub_pending_t pending[HUB_PENDING_CAPACITY];
    size_t pending_head;
    size_t pending_count;
    uint32_t pending_head_since_ms; /* When the oldest request became the oldest */
    uint32_t next_tag;
    size_t client_commands_pending;
    bool poll_pending;

    uint8_t bulk_set_buffer[LINK_MAX_PAYLOAD_BYTES]; /* The one bulk SET in flight */
    bool bulk_set_busy;

    hub_origin_t origins[HUB_ORIGIN_QUEUE_ENTRIES];
    size_t origin_head;
    size_t origin_count;

    hub_poll_group_t poll_groups[HUB_MAX_POLL_GROUPS];
    size_t poll_group_count;

    hub_device_t device;
    hub_snapshot_t snapshot;
    hub_update_t update;

    /* Scratch for encoding one outbound binary frame; the largest is a CMD
     * response carrying a whole bulk blob. */
    uint8_t frame[LINK_CMD_RESPONSE_HEADER_BYTES + LINK_MAX_PAYLOAD_BYTES];
} hub_t;

/* ---------------------------------------------------------------------------
 * Lifecycle and time
 * ------------------------------------------------------------------------- */

/* Starts `hub` with no sessions and no device. `io` and `config` are
 * copied; `auth_io` and `saved_auth` set up the PIN and paired clients
 * (hub_auth.h), and `saved_auth` may be NULL for none. Called once, before
 * any other function. */
void hub_init(hub_t *hub, const hub_io_t *io, const hub_config_t *config,
              const hub_auth_io_t *auth_io, const hub_auth_store_t *saved_auth);

/* Runs timers: keepalive, hello timeout, lock expiry, offline retention,
 * lost answers, a stalled update and due polls. Call every HUB_TICK_MS. */
void hub_tick(hub_t *hub);

/* ---------------------------------------------------------------------------
 * Network events
 * ------------------------------------------------------------------------- */

/* True when another session may connect; checked before the WebSocket
 * handshake so a full bridge answers HTTP 503 (bridge spec 8.2). */
bool hub_has_room(const hub_t *hub);

/* A WebSocket session finished its handshake on `slot`, from `address`
 * (IPv4, network byte order; used for rate limits). */
void hub_session_opened(hub_t *hub, int slot, uint32_t address);

/* A text (JSON) or binary message of `length` bytes arrived on `slot`.
 * The hub reads it only during the call; the caller keeps ownership. */
void hub_session_text(hub_t *hub, int slot, const char *text, size_t length);
void hub_session_binary(hub_t *hub, int slot, const uint8_t *data, size_t length);

/* A pong or other control frame arrived: the client is alive. */
void hub_session_activity(hub_t *hub, int slot);

/* The socket on `slot` closed, for whatever reason. Once this returns the
 * slot may be reused. */
void hub_session_closed(hub_t *hub, int slot);

/* ---------------------------------------------------------------------------
 * Device events, from the uart task
 * ------------------------------------------------------------------------- */

/* The uart task found a DSPi with `identity` (copied) at `baud`: a new
 * device is announced, the same one coming back is a re-attach (bridge
 * spec 7.6). */
void hub_device_found(hub_t *hub, const dspi_identity_t *identity, uint32_t baud);

/* The DSPi stopped answering: it goes offline, and is removed if it does
 * not return within LINK_OFFLINE_HANDLE_RETAIN_MS. */
void hub_device_lost(hub_t *hub);

/* A notification packet of `length` bytes from the DSPi, relayed to every
 * session (Link spec 8.3). Read only during the call. */
void hub_notification(hub_t *hub, const uint8_t *packet, size_t length);

/* The answer to the request submitted with `tag`. Answers come in the
 * order requests were submitted; one older than an answer already given
 * up on is ignored. `reply->payload` is read only during the call. */
void hub_reply(hub_t *hub, uint32_t tag, const dspi_reply_t *reply);

/* How a change asked of the hub went. */
typedef enum {
    HUB_CHANGE_OK,
    HUB_CHANGE_INVALID, /* The value is not allowed; nothing changed */
    HUB_CHANGE_FAILED,  /* It could not be saved or applied (logged by the I/O) */
} hub_change_result_t;

/* Renames the hub, as hub.rename does: saves the name and advertises it.
 * HUB_CHANGE_INVALID if the name is not 1 to 63 bytes of UTF-8 (bridge
 * spec 8.11). */
hub_change_result_t hub_rename(hub_t *hub, const char *name);

/* Answers a request that the hub handed to its I/O to finish, such as
 * hub.wifi.scan, if the session that asked is still there. Takes ownership
 * of `body`. */
void hub_reply_later(hub_t *hub, int slot, uint16_t session_id, int request_id, cJSON *body);

/* True when the PIN is on; the setup page asks for it then (bridge spec
 * 9.2). */
bool hub_pin_enabled(const hub_t *hub);

/* Checks a PIN typed on the setup page at IPv4 `address`, under the same
 * rate limits as pairing (bridge spec 9.3). Returns what
 * hub_auth_check_pin() does. */
hub_auth_result_t hub_check_pin(hub_t *hub, uint32_t address, const char *pin);

/* Sets or clears the PIN from the setup page (bridge spec 9.3): `pin` NULL
 * turns it off. Every paired client is forgotten either way, and with a
 * new PIN every session is closed so it pairs again. Returns the result of
 * hub_auth_set_pin(): HUB_AUTH_OK, HUB_AUTH_INVALID_PIN or
 * HUB_AUTH_UNAVAILABLE. */
hub_auth_result_t hub_set_pin(hub_t *hub, const char *pin);

/* Tells every session the hub is going (close code 4003), then restarts
 * through the I/O after erasing what `level` says. The BOOT button uses
 * this; hub.restart and hub.reset do the same after replying. */
void hub_restart(hub_t *hub, hub_restart_level_t level);

/* ---------------------------------------------------------------------------
 * Firmware update events, from the I/O (hub_io_t's update functions)
 * ------------------------------------------------------------------------- */

/* The chunk at `offset` is in flash. `generation` is the one update_begin
 * returned; a report for any other update is ignored. Hub task only. */
void hub_update_written(hub_t *hub, uint32_t generation, uint32_t offset);

/* The update is over: the whole image was checked (`ok`), or something
 * went wrong on the way. `text` is the new firmware's version when `ok`,
 * or a sentence saying what was wrong. A report for any update but the
 * current one is ignored. Hub task only. */
void hub_update_finished(hub_t *hub, uint32_t generation, bool ok, const char *text);

/* ---------------------------------------------------------------------------
 * What the hub publishes
 * ------------------------------------------------------------------------- */

/* The /dspi/v1/info document (Link spec 3.2). The caller frees it with
 * cJSON_free(). NULL if memory ran out. */
char *hub_info_json(const hub_t *hub);

/* The values the DNS-SD TXT record needs (Link spec 3.1). The strings
 * point into the hub and stay valid until its next event. */
typedef struct {
    const char *hub_id;
    const char *name;
    const char *auth;   /* "none" or "pin" */
    unsigned devices;   /* `n` */
    const char *serial; /* `d`: the one device's serial, or NULL */
} hub_advert_t;

/* Fills `advert` with the hub's current DNS-SD values. */
void hub_advert(const hub_t *hub, hub_advert_t *advert);

/* Poll budget in bytes per second at the current baud rate (bridge spec
 * 7.7). */
uint32_t hub_poll_budget(const hub_t *hub);

#endif /* HUB_H */
