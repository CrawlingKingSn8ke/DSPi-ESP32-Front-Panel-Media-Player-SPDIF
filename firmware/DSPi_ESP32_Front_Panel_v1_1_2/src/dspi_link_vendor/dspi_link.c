/*
 * dspi_link.c - one request at a time to the DSPi, with notifications
 * relayed throughout. See dspi_link.h; implements bridge spec 7.2 to 7.6.
 *
 * Portable: no ESP-IDF headers. Every wait goes through the port's read
 * function with a timeout, so the code never blocks for longer than the
 * request's own deadline.
 */

#include "dspi_link.h"

#include <string.h>

#include "bridge_config.h"

/* Commands the bridge sends on its own behalf (firmware commands.md). */
#define REQ_GET_SERIAL 0x7E
#define REQ_GET_PLATFORM 0x7F
#define REQ_GET_UART_CONFIG 0xF6
#define REQ_GET_CTRL_IFACE_STATUS 0xF9

#define PLATFORM_MIN_BYTES 4 /* Old firmware returns 4; current returns 7 */
#define IFACE_STATUS_BYTES 8
#define UART_CONFIG_BYTES 8

/* ---------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */

static uint32_t now_ms(const dspi_link_t *link)
{
    return link->port.now_ms(link->port.context);
}

/* Milliseconds until `deadline`, or 0 if it has passed. Unsigned
 * subtraction keeps this right when the clock wraps. */
static uint32_t remaining_ms(const dspi_link_t *link, uint32_t deadline)
{
    const int32_t remaining = (int32_t)(deadline - now_ms(link));
    return remaining > 0 ? (uint32_t)remaining : 0;
}

/* Time to send `bytes` at 8N1, which is 10 bits per byte, rounded up. */
static uint32_t wire_time_ms(uint32_t bytes, uint32_t baud)
{
    return (bytes * 10u * 1000u + baud - 1) / baud;
}

/* How long to wait for the answer to `request` (bridge spec 7.2): a fixed
 * margin plus twice the wire time of the largest answer it could get. */
static uint32_t reply_timeout_ms(const dspi_link_t *link, const dspi_request_t *request)
{
    /* Front-panel compatibility: occupied native preset saves can need a
     * sector erase. Preserve the existing650ms window without replaying
     * an uncertain save. Other requests keep upstream wire-time sizing. */
    if (!request->is_get && request->request == 0x90) {
        return 650;
    }
    uint32_t largest_reply = DSPI_FRAME_RESPONSE_OVERHEAD_BYTES;
    if (request->is_get) {
        largest_reply += request->length == 0 ? LINK_MAX_PAYLOAD_BYTES : request->length;
    }
    return DSPI_UART_TIMEOUT_MARGIN_MS + 2 * wire_time_ms(largest_reply, link->baud);
}

/* ---------------------------------------------------------------------------
 * Receiving
 * ------------------------------------------------------------------------- */

/* Milliseconds since bytes last arrived. */
static uint32_t quiet_ms(const dspi_link_t *link)
{
    return now_ms(link) - link->last_byte_ms;
}

/*
 * Takes the next received byte, reading more from the UART if the chunk is
 * used up. Returns false if nothing arrives within `timeout_ms`.
 *
 * Part way through a frame it waits no longer than the rest of
 * DSPI_UART_FRAME_GAP_MS. If the line stays quiet that long, the frame is
 * abandoned and its bytes wait in the parser to be searched again, so a
 * false start (noise, a lost byte) cannot swallow the next real frame.
 */
static bool next_byte(dspi_link_t *link, uint32_t timeout_ms, uint8_t *byte)
{
    if (link->chunk_next == link->chunk_count) {
        uint32_t wait = timeout_ms;
        const bool in_frame = dspi_parser_in_frame(&link->parser);
        if (in_frame) {
            const uint32_t quiet = quiet_ms(link);
            const uint32_t gap_left =
                quiet < DSPI_UART_FRAME_GAP_MS ? DSPI_UART_FRAME_GAP_MS - quiet : 0;
            wait = gap_left < wait ? gap_left : wait;
        }
        link->chunk_count =
            link->port.read(link->port.context, link->chunk, sizeof(link->chunk), wait);
        link->chunk_next = 0;
        if (link->chunk_count == 0) {
            if (in_frame && quiet_ms(link) >= DSPI_UART_FRAME_GAP_MS) {
                dspi_parser_abandon(&link->parser);
            }
            return false;
        }
        link->last_byte_ms = now_ms(link);
    }
    *byte = link->chunk[link->chunk_next++];
    return true;
}

/* Parses one byte: the next one waiting to be rescanned, or else the next
 * received one. Returns false if neither came within `timeout_ms`.
 * `received` says which it was. */
static bool parse_next(dspi_link_t *link, uint32_t timeout_ms, dspi_parse_result_t *result,
                       dspi_frame_t *frame, bool *received)
{
    *received = false;
    if (dspi_parser_rescan(&link->parser, result, frame)) {
        return true;
    }
    uint8_t byte;
    if (!next_byte(link, timeout_ms, &byte)) {
        return false;
    }
    *received = true;
    *result = dspi_parser_feed(&link->parser, byte, frame);
    return true;
}

/* Forgets every byte received but not yet parsed, and any partial frame. */
static void discard_received(dspi_link_t *link)
{
    link->chunk_count = 0;
    link->chunk_next = 0;
    dspi_parser_reset(&link->parser);
}

static void relay_notification(dspi_link_t *link, const dspi_frame_t *frame)
{
    link->counters.notifications++;
    if (link->on_notification != NULL) {
        link->on_notification(link->notification_context, frame->payload, frame->length);
    }
}

/* How waiting for an answer ended. */
typedef enum {
    WAIT_ANSWERED,
    WAIT_TIMED_OUT,
} wait_result_t;

/* What an answer to the request being waited on must look like. A type of
 * 0 matches nothing: used while only listening for notifications. */
typedef struct {
    uint8_t type;        /* A response type, or 0 when no answer is expected */
    uint16_t max_length; /* Longest payload it can carry; 0 for no limit */
} expected_answer_t;

/* True when `frame` can be the expected answer. The firmware never returns
 * more than a GET's wLength, so a longer answer, like one of the wrong type,
 * belongs to an earlier request that timed out (bridge spec 7.4). */
static bool matches(const dspi_frame_t *frame, expected_answer_t expected)
{
    if (expected.type == 0 || frame->type != expected.type) {
        return false;
    }
    return expected.max_length == 0 || frame->length <= expected.max_length;
}

/*
 * Parses received bytes until the expected answer arrives or `deadline`
 * passes, relaying every notification on the way. Any other answer is a
 * late one to an earlier request and is dropped. Once the deadline has
 * passed, bytes already received are still parsed, up to
 * DSPI_UART_LATE_PARSE_BYTES, since the answer may have arrived in time
 * while the uart task was held up.
 *
 * saw_corrupt: set when a damaged frame of the expected type arrived, which
 *              may have been the answer. A damaged notification is not.
 */
static wait_result_t receive_until(dspi_link_t *link, expected_answer_t expected, uint32_t deadline,
                                   dspi_frame_t *answer, bool *saw_corrupt)
{
    size_t late_bytes_left = DSPI_UART_LATE_PARSE_BYTES;
    for (;;) {
        const uint32_t remaining = remaining_ms(link, deadline);
        if (remaining == 0 && late_bytes_left == 0) {
            return WAIT_TIMED_OUT;
        }
        dspi_parse_result_t result;
        dspi_frame_t frame;
        bool received;
        if (!parse_next(link, remaining, &result, &frame, &received)) {
            if (remaining == 0 && !dspi_parser_rescan_pending(&link->parser)) {
                return WAIT_TIMED_OUT; /* Nothing more had arrived */
            }
            continue; /* The loop re-checks the deadline */
        }
        if (received && remaining == 0) {
            late_bytes_left--;
        }

        switch (result) {
        case DSPI_PARSE_FRAME:
            if (frame.type == DSPI_FRAME_TYPE_NOTIFICATION) {
                relay_notification(link, &frame);
            } else if (matches(&frame, expected)) {
                *answer = frame;
                return WAIT_ANSWERED;
            }
            break;
        case DSPI_PARSE_BAD_CRC:
        case DSPI_PARSE_TOO_LONG:
            /* Keep waiting: a damaged notification can come before the
             * real answer. If no answer follows, the caller reports it. */
            link->counters.crc_errors++;
            if (expected.type != 0 && frame.type == expected.type) {
                *saw_corrupt = true;
            }
            break;
        case DSPI_PARSE_NOTHING:
            break;
        }
    }
}

void dspi_link_listen(dspi_link_t *link, uint32_t duration_ms)
{
    const expected_answer_t nothing = {.type = 0};
    dspi_frame_t unused;
    bool corrupt = false;
    /* Always times out: nothing matches `nothing`. */
    (void)receive_until(link, nothing, now_ms(link) + duration_ms, &unused, &corrupt);
}

/* After a timeout, a late answer could be mistaken for the next request's.
 * Drop everything except notifications until the line has been silent for
 * DSPI_UART_QUARANTINE_SILENCE_MS, within DSPI_UART_QUARANTINE_MAX_MS
 * (bridge spec 7.4). */
static void drain_until_silent(dspi_link_t *link)
{
    const uint32_t give_up_at = now_ms(link) + DSPI_UART_QUARANTINE_MAX_MS;
    dspi_parser_reset(&link->parser);

    while (remaining_ms(link, give_up_at) > 0) {
        dspi_parse_result_t result;
        dspi_frame_t frame;
        bool received;
        if (!parse_next(link, DSPI_UART_QUARANTINE_SILENCE_MS, &result, &frame, &received)) {
            if (dspi_parser_rescan_pending(&link->parser) ||
                quiet_ms(link) < DSPI_UART_QUARANTINE_SILENCE_MS) {
                continue; /* An abandoned frame to search, or not yet silent */
            }
            break; /* Silent long enough */
        }
        if (result == DSPI_PARSE_FRAME && frame.type == DSPI_FRAME_TYPE_NOTIFICATION) {
            relay_notification(link, &frame);
        }
    }
    dspi_parser_reset(&link->parser);
    link->quarantined = false;
}

/* ---------------------------------------------------------------------------
 * Requests
 * ------------------------------------------------------------------------- */

void dspi_link_init(dspi_link_t *link, const dspi_port_t *port, uint32_t baud, uint8_t *buffer,
                    size_t capacity, dspi_notification_handler_t handler, void *handler_context)
{
    memset(link, 0, sizeof(*link));
    link->port = *port;
    link->baud = baud;
    link->on_notification = handler;
    link->notification_context = handler_context;
    dspi_parser_init(&link->parser, buffer, capacity);
    link->last_reply_ms = now_ms(link);
    link->last_byte_ms = link->last_reply_ms;
}

bool dspi_link_set_baud(dspi_link_t *link, uint32_t baud)
{
    if (!link->port.set_baud(link->port.context, baud)) {
        return false;
    }
    link->baud = baud;
    discard_received(link);
    return true;
}

uint32_t dspi_link_baud(const dspi_link_t *link)
{
    return link->baud;
}

/* Leaves at least DSPI_UART_IDLE_GAP_MS after the last answer, so the
 * firmware can send queued notifications (bridge spec 7.2). */
static void wait_for_idle_gap(dspi_link_t *link)
{
    const uint32_t idle = now_ms(link) - link->last_reply_ms;
    if (idle < DSPI_UART_IDLE_GAP_MS) {
        dspi_link_listen(link, DSPI_UART_IDLE_GAP_MS - idle);
    }
}

static bool send_request(dspi_link_t *link, const dspi_request_t *request)
{
    uint8_t header[DSPI_FRAME_REQUEST_HEADER_BYTES];
    uint8_t crc[DSPI_FRAME_CRC_BYTES];
    dspi_frame_encode_request(request, header, crc);

    /* Three writes, so a 6 KB bulk payload is never copied. */
    const bool has_payload = !request->is_get && request->length > 0;
    return link->port.write(link->port.context, header, sizeof(header)) &&
           (!has_payload ||
            link->port.write(link->port.context, request->payload, request->length)) &&
           link->port.write(link->port.context, crc, sizeof(crc));
}

/* Records an attempt that got no valid answer: quarantine before the next
 * one, and restart the UART after too many in a row. A corrupt answer counts
 * towards the restart as a timeout does, because a receiver stuck on noise
 * produces exactly those (bridge spec 7.4). */
static void note_no_answer(dspi_link_t *link, bool timed_out)
{
    link->quarantined = true;
    dspi_parser_reset(&link->parser);
    if (timed_out) {
        link->counters.timeouts++;
    }
    if (link->probing) {
        return; /* Each probe changes the rate, which already flushes the receiver */
    }
    if (++link->unanswered_in_a_row < DSPI_UART_UNANSWERED_BEFORE_REINSTALL) {
        return;
    }
    link->counters.restarts++;
    discard_received(link);
    if (link->port.restart(link->port.context)) {
        link->unanswered_in_a_row = 0;
    }
    /* On failure the count stays at the limit, so the next attempt that
     * gets no answer tries the restart again. */
}

/* One attempt at a request, without retries. */
static dspi_reply_t attempt(dspi_link_t *link, const dspi_request_t *request)
{
    dspi_reply_t reply = {.kind = DSPI_REPLY_TIMEOUT};

    if (link->quarantined) {
        drain_until_silent(link);
    }
    wait_for_idle_gap(link);

    if (!send_request(link, request)) {
        note_no_answer(link, true);
        return reply;
    }

    const expected_answer_t expected = {
        .type = request->is_get ? DSPI_FRAME_TYPE_GET_RESPONSE : DSPI_FRAME_TYPE_SET_RESPONSE,
        .max_length = request->is_get ? request->length : 0,
    };
    const uint32_t deadline = now_ms(link) + reply_timeout_ms(link, request);
    dspi_frame_t answer;
    bool saw_corrupt = false;

    if (receive_until(link, expected, deadline, &answer, &saw_corrupt) == WAIT_TIMED_OUT) {
        /* A corrupt frame with no answer after it was most likely the answer. */
        reply.kind = saw_corrupt ? DSPI_REPLY_CORRUPT : DSPI_REPLY_TIMEOUT;
        note_no_answer(link, !saw_corrupt);
        return reply;
    }

    link->last_reply_ms = now_ms(link);
    link->unanswered_in_a_row = 0;
    reply.kind = DSPI_REPLY_ANSWERED;
    reply.status = answer.status;
    if (request->is_get && answer.status == DSPI_STATUS_OK) {
        reply.payload = answer.payload;
        reply.length = answer.length;
    }
    return reply;
}

/*
 * True when a reply is worth one more attempt (bridge spec 7.3).
 *
 * A CRC_ERROR or FRAME_ERROR status means the DSPi rejected the frame itself
 * and never ran the request, so any request may be resent. With no valid
 * answer the request may or may not have run, so only a safe read is
 * repeated. BUSY and BULK_LOCKED are passed on: the firmware has already
 * retried those for 50 ms.
 */
static bool worth_retrying(const dspi_reply_t *reply, bool may_retry)
{
    if (reply->kind == DSPI_REPLY_ANSWERED) {
        return reply->status == DSPI_STATUS_CRC_ERROR || reply->status == DSPI_STATUS_FRAME_ERROR;
    }
    return may_retry;
}

dspi_reply_t dspi_link_transact(dspi_link_t *link, const dspi_request_t *request, bool may_retry)
{
    const uint8_t max_attempts = 2;
    dspi_reply_t reply = {.kind = DSPI_REPLY_TIMEOUT};

    link->counters.requests++;
    for (uint8_t number = 1; number <= max_attempts; number++) {
        if (number > 1) {
            link->counters.retries++;
        }
        reply = attempt(link, request);
        reply.attempts = number;
        if (!worth_retrying(&reply, may_retry)) {
            break;
        }
    }

    link->failures_in_a_row =
        reply.kind == DSPI_REPLY_ANSWERED ? 0 : (uint8_t)(link->failures_in_a_row + 1);
    return reply;
}

uint8_t dspi_link_failures_in_a_row(const dspi_link_t *link)
{
    return link->failures_in_a_row;
}

uint32_t dspi_link_idle_ms(const dspi_link_t *link)
{
    return now_ms(link) - link->last_reply_ms;
}

const dspi_link_counters_t *dspi_link_counters(const dspi_link_t *link)
{
    return &link->counters;
}

/* ---------------------------------------------------------------------------
 * Finding the device
 * ------------------------------------------------------------------------- */

/* The baud probe: the interface status, the cheapest read there is. */
static const dspi_request_t IFACE_STATUS_REQUEST = {
    .is_get = true, .request = REQ_GET_CTRL_IFACE_STATUS, .length = IFACE_STATUS_BYTES};

static const dspi_request_t SERIAL_REQUEST = {
    .is_get = true, .request = REQ_GET_SERIAL, .length = DSPI_SERIAL_MAX_CHARS};

/* Copies a serial number's characters into `out` (DSPI_SERIAL_MAX_CHARS + 1
 * bytes): raw bytes padded with NULs, so it stops at the first NUL. */
static void copy_serial(const uint8_t *bytes, size_t length, char *out)
{
    size_t count = 0;
    while (count < length && count < DSPI_SERIAL_MAX_CHARS && bytes[count] != 0) {
        out[count] = (char)bytes[count];
        count++;
    }
    out[count] = '\0';
}

dspi_heartbeat_t dspi_link_heartbeat(dspi_link_t *link, const char *serial)
{
    const dspi_reply_t reply = dspi_link_transact(link, &SERIAL_REQUEST, true);
    if (reply.kind != DSPI_REPLY_ANSWERED) {
        return DSPI_HEARTBEAT_NO_ANSWER;
    }
    if (reply.status != DSPI_STATUS_OK) {
        return DSPI_HEARTBEAT_SAME_DEVICE; /* Busy, say: it is there, and nothing says it changed */
    }
    char answered[DSPI_SERIAL_MAX_CHARS + 1];
    copy_serial(reply.payload, reply.length, answered);
    return strcmp(answered, serial) == 0 ? DSPI_HEARTBEAT_SAME_DEVICE : DSPI_HEARTBEAT_OTHER_DEVICE;
}

/* Asks for the interface status at `baud`. Any answer at all, even an error
 * status, proves a DSPi is listening at that rate. */
static bool answers_at(dspi_link_t *link, uint32_t baud)
{
    if (!dspi_link_set_baud(link, baud)) {
        return false;
    }
    return dspi_link_transact(link, &IFACE_STATUS_REQUEST, false).kind == DSPI_REPLY_ANSWERED;
}

/* Tries `first_baud`, then each candidate not already tried. */
static bool answers_at_any(dspi_link_t *link, uint32_t first_baud, const uint32_t *candidates,
                           size_t count)
{
    if (answers_at(link, first_baud)) {
        return true;
    }
    for (size_t i = 0; i < count; i++) {
        if (candidates[i] != first_baud && answers_at(link, candidates[i])) {
            return true;
        }
    }
    return false;
}

bool dspi_link_find_baud(dspi_link_t *link, uint32_t first_baud, const uint32_t *candidates,
                         size_t count)
{
    link->probing = true;
    const bool found = answers_at_any(link, first_baud, candidates, count);
    link->probing = false;
    if (!found) {
        /* If the port refuses, the link stays at the last rate tried, and
         * dspi_link_baud() still reports it truthfully. */
        (void)dspi_link_set_baud(link, first_baud);
    }
    return found;
}

/* Reads up to `capacity` bytes of a GET into `out`. Returns the number of
 * bytes read, or -1 if the DSPi did not answer OK. */
static int read_get(dspi_link_t *link, uint8_t request_code, uint8_t *out, size_t capacity)
{
    const dspi_request_t request = {
        .is_get = true, .request = request_code, .length = (uint16_t)capacity};
    const dspi_reply_t reply = dspi_link_transact(link, &request, true);
    if (reply.kind != DSPI_REPLY_ANSWERED || reply.status != DSPI_STATUS_OK) {
        return -1;
    }
    const size_t length = reply.length < capacity ? reply.length : capacity;
    memcpy(out, reply.payload, length);
    return (int)length;
}

bool dspi_link_read_identity(dspi_link_t *link, dspi_identity_t *identity)
{
    memset(identity, 0, sizeof(*identity));

    uint8_t serial[DSPI_SERIAL_MAX_CHARS];
    const int serial_length = read_get(link, REQ_GET_SERIAL, serial, sizeof(serial));
    if (serial_length <= 0) {
        return false;
    }
    copy_serial(serial, (size_t)serial_length, identity->serial);
    if (identity->serial[0] == '\0') {
        return false;
    }

    const int platform_length =
        read_get(link, REQ_GET_PLATFORM, identity->platform, sizeof(identity->platform));
    if (platform_length < PLATFORM_MIN_BYTES) {
        return false;
    }
    identity->platform_length = (uint8_t)platform_length;

    uint8_t status[IFACE_STATUS_BYTES];
    identity->uart_live =
        read_get(link, REQ_GET_CTRL_IFACE_STATUS, status, sizeof(status)) >= 2 && status[1] != 0;

    /* UartCtrlConfig: enabled, tx_pin, rx_pin, notify_enable, baud (u32 LE). */
    uint8_t config[UART_CONFIG_BYTES];
    if (read_get(link, REQ_GET_UART_CONFIG, config, sizeof(config)) == UART_CONFIG_BYTES) {
        identity->has_uart_config = true;
        identity->notify_enabled = config[3] != 0;
        identity->configured_baud = (uint32_t)config[4] | ((uint32_t)config[5] << 8) |
                                    ((uint32_t)config[6] << 16) | ((uint32_t)config[7] << 24);
    }
    return true;
}
