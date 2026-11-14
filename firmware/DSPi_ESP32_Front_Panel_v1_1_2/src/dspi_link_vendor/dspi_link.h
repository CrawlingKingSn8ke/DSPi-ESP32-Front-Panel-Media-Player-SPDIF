/*
 * dspi_link.h - talks to the DSPi over its UART, one request at a time.
 *
 * Implements bridge spec 7.2 to 7.6: the idle gap that lets notifications
 * through, timeouts sized to the baud rate, the one retry a safe read may
 * get, quarantine after a timeout, baud detection and reading the device's
 * identity. Notifications that arrive at any point are handed to a callback.
 *
 * This is the portable core of the uart task. It reaches the UART and the
 * clock only through dspi_port_t, so the same code runs on the ESP32
 * (dspi_uart.c supplies the real UART) and in the host tests (a simulated
 * DSPi). A dspi_link_t belongs to one task; nothing here is thread-safe.
 */

#ifndef DSPI_LINK_H
#define DSPI_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_config.h"
#include "dspi_frame.h"

/* ---------------------------------------------------------------------------
 * What the link runs on
 * ------------------------------------------------------------------------- */

/* The UART and clock, supplied by the caller. Every function receives
 * `context` first. */
typedef struct {
    void *context;

    /* Sends `length` bytes and returns once they have left the UART. */
    bool (*write)(void *context, const uint8_t *data, size_t length);

    /* Waits up to `timeout_ms` for received bytes and returns how many it
     * copied into `data` (at most `capacity`); 0 means none arrived. */
    size_t (*read)(void *context, uint8_t *data, size_t capacity, uint32_t timeout_ms);

    /* Changes the baud rate. */
    bool (*set_baud)(void *context, uint32_t baud);

    /* Reinstalls the UART driver, for a receiver stuck after repeated
     * timeouts (bridge spec 7.4). */
    bool (*restart)(void *context);

    /* Milliseconds from any fixed starting point; wraps after 49 days. */
    uint32_t (*now_ms)(void *context);
} dspi_port_t;

/* Called with each notification packet, verbatim (Link spec 8.3). The
 * packet is valid only during the call. */
typedef void (*dspi_notification_handler_t)(void *context, const uint8_t *packet, size_t length);

/* ---------------------------------------------------------------------------
 * The link
 * ------------------------------------------------------------------------- */

/* Running totals, reported in hub.stats (bridge spec 8.7). */
typedef struct {
    uint32_t requests;
    uint32_t timeouts;
    uint32_t crc_errors; /* Frames that failed their CRC or claimed more than the buffer holds */
    uint32_t retries;
    uint32_t notifications;
    uint32_t restarts;
} dspi_link_counters_t;

/* Link state. Treat as opaque; use the functions below. */
typedef struct {
    dspi_port_t port;
    uint8_t chunk[DSPI_UART_READ_CHUNK_BYTES]; /* Read ahead; the rest waits for the next call */
    size_t chunk_count;                        /* Bytes in `chunk` */
    size_t chunk_next;                         /* Next unread byte in `chunk` */
    dspi_notification_handler_t on_notification;
    void *notification_context;
    dspi_parser_t parser;
    uint32_t baud;
    uint32_t last_reply_ms;      /* When the last response ended, for the idle gap */
    uint32_t last_byte_ms;       /* When bytes last arrived, to spot a frame that stopped */
    bool probing;                /* Looking for the device: no UART restarts */
    bool quarantined;            /* A timeout happened; drain before sending */
    uint8_t unanswered_in_a_row; /* Attempts with no valid answer, towards a UART restart */
    uint8_t failures_in_a_row;   /* Towards declaring the device lost */
    dspi_link_counters_t counters;
} dspi_link_t;

/*
 * Prepares a link.
 *
 * port:     the UART and clock; copied, so it may be a local.
 * baud:     the rate the port is already running at.
 * buffer:   holds one received frame; DSPI_PARSER_BUFFER_BYTES(
 *           LINK_MAX_PAYLOAD_BYTES) fits a bulk read. Must outlive the link.
 * capacity: the size of `buffer`; longer frames are discarded.
 * handler:  receives notifications; may be NULL to discard them.
 */
void dspi_link_init(dspi_link_t *link, const dspi_port_t *port, uint32_t baud, uint8_t *buffer,
                    size_t capacity, dspi_notification_handler_t handler, void *handler_context);

/* Switches to `baud` and forgets any partial frame. Returns false, leaving
 * the link at its old rate, if the port could not change. */
bool dspi_link_set_baud(dspi_link_t *link, uint32_t baud);

/* The baud rate in use. */
uint32_t dspi_link_baud(const dspi_link_t *link);

/* How one request ended. */
typedef enum {
    DSPI_REPLY_ANSWERED,  /* The DSPi answered; see `status` */
    DSPI_REPLY_TIMEOUT,   /* No answer in time: Link status 0x82 TIMEOUT */
    DSPI_REPLY_CORRUPT,   /* The answer failed its CRC. It may have run, so Link status 0x82 TIMEOUT
                           */
    DSPI_REPLY_NO_DEVICE, /* Never sent: no DSPi is present. Set by the uart task (dspi_uart.h) */
} dspi_reply_kind_t;

typedef struct {
    dspi_reply_kind_t kind;
    uint8_t status;         /* The DSPi's status byte when ANSWERED */
    const uint8_t *payload; /* A GET's payload; valid until the next call on the link */
    uint16_t length;
    uint8_t attempts; /* 1, or 2 after a retry */
} dspi_reply_t;

/*
 * Sends one request and waits for its response, relaying notifications that
 * arrive meanwhile.
 *
 * Any request is sent once more if the DSPi answers CRC_ERROR or
 * FRAME_ERROR, because the DSPi did not run it.
 *
 * may_retry: true only for a GET that `link_policy` classes as read, which
 *            is safe to repeat (bridge spec 7.3). Such a request is also
 *            sent once more after a timeout or a corrupt answer.
 */
dspi_reply_t dspi_link_transact(dspi_link_t *link, const dspi_request_t *request, bool may_retry);

/* Reads and relays notifications for `duration_ms`. Called while idle. */
void dspi_link_listen(dspi_link_t *link, uint32_t duration_ms);

/* How a heartbeat ended. */
typedef enum {
    DSPI_HEARTBEAT_SAME_DEVICE,  /* It answered, with the expected serial or an error status */
    DSPI_HEARTBEAT_NO_ANSWER,    /* No valid answer, even after the retry */
    DSPI_HEARTBEAT_OTHER_DEVICE, /* A DSPi with another serial answered: it was swapped */
} dspi_heartbeat_t;

/* Reads the serial number to check the device is still there, and is still
 * the one whose serial is `serial` (bridge spec 7.6). */
dspi_heartbeat_t dspi_link_heartbeat(dspi_link_t *link, const char *serial);

/* Requests in a row that got no valid answer. The caller declares the
 * device lost at DSPI_UART_FAILURES_BEFORE_LOST (bridge spec 7.6). */
uint8_t dspi_link_failures_in_a_row(const dspi_link_t *link);

/* Milliseconds since the last response ended. */
uint32_t dspi_link_idle_ms(const dspi_link_t *link);

/* The running totals; the pointer stays valid as long as the link. */
const dspi_link_counters_t *dspi_link_counters(const dspi_link_t *link);

/* ---------------------------------------------------------------------------
 * Finding the device
 * ------------------------------------------------------------------------- */

/*
 * Looks for the DSPi by asking for its interface status at each rate in
 * turn: `first_baud`, then each of `candidates` not already tried (bridge
 * spec 7.5). Leaves the link at the rate that answered and returns true, or
 * returns false with the link back at `first_baud`.
 */
bool dspi_link_find_baud(dspi_link_t *link, uint32_t first_baud, const uint32_t *candidates,
                         size_t count);

/* Characters in a DSPi serial number: REQ_GET_SERIAL returns 16 bytes. */
#define DSPI_SERIAL_MAX_CHARS 16

/* What the bridge reads from a device when it appears (bridge spec 7.6). */
typedef struct {
    char serial[DSPI_SERIAL_MAX_CHARS + 1]; /* REQ_GET_SERIAL (0x7E), NUL-terminated */
    uint8_t platform[7];                    /* REQ_GET_PLATFORM (0x7F), as returned */
    uint8_t platform_length;                /* 7 on current firmware, fewer on old */
    bool uart_live;                         /* REQ_GET_CTRL_IFACE_STATUS (0xF9) byte 1 */
    bool notify_enabled;                    /* REQ_GET_UART_CONFIG (0xF6) byte 3 */
    uint32_t configured_baud;               /* REQ_GET_UART_CONFIG bytes 4 to 7 */
    bool has_uart_config; /* False if 0xF6 failed; the two fields above are then unknown */
} dspi_identity_t;

/* Reads the serial, platform, interface status and UART configuration.
 * Returns false if the serial or platform could not be read. */
bool dspi_link_read_identity(dspi_link_t *link, dspi_identity_t *identity);

#endif /* DSPI_LINK_H */
