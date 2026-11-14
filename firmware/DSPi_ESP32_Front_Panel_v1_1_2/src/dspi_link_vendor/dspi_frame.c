/*
 * dspi_frame.c - builds DSPi UART requests and parses responses and
 * notifications. Implements bridge spec 7.1; see dspi_frame.h for the frame
 * layout. The uart task's link engine (dspi_link.c) is its only user on the
 * bridge; the host tests drive it directly.
 *
 * Portable: no ESP-IDF headers.
 */

#include "dspi_frame.h"

#include <string.h>

uint16_t dspi_crc16_update(uint16_t crc, const uint8_t *data, size_t length)
{
    /* CRC-16/CCITT-FALSE: polynomial 0x1021, most significant bit first, no
     * reflection and no final XOR. Bitwise rather than table-driven: frames
     * are short and the 512-byte table is not worth its flash. */
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void put_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFF);
    out[1] = (uint8_t)(value >> 8);
}

void dspi_frame_encode_request(const dspi_request_t *request,
                               uint8_t header[DSPI_FRAME_REQUEST_HEADER_BYTES],
                               uint8_t crc[DSPI_FRAME_CRC_BYTES])
{
    header[0] = DSPI_FRAME_SYNC;
    header[1] = request->is_get ? DSPI_FRAME_TYPE_GET_REQUEST : DSPI_FRAME_TYPE_SET_REQUEST;
    header[2] = request->request;
    put_u16_le(&header[3], request->value);
    put_u16_le(&header[5], request->index);
    put_u16_le(&header[7], request->length);

    /* The CRC covers everything after the sync byte, payload included. */
    uint16_t sum =
        dspi_crc16_update(DSPI_CRC_INITIAL, &header[1], DSPI_FRAME_REQUEST_HEADER_BYTES - 1);
    if (!request->is_get && request->length > 0) {
        sum = dspi_crc16_update(sum, request->payload, request->length);
    }
    put_u16_le(crc, sum);
}

void dspi_parser_init(dspi_parser_t *parser, uint8_t *buffer, size_t capacity)
{
    parser->buffer = buffer;
    parser->capacity = capacity;
    dspi_parser_reset(parser);
}

void dspi_parser_reset(dspi_parser_t *parser)
{
    parser->stage = DSPI_PARSER_WAIT_SYNC;
    parser->length = 0;
    parser->crc = DSPI_CRC_INITIAL;
    parser->stored = 0;
    parser->rescan_next = 0;
    parser->rescan_end = 0;
}

bool dspi_parser_in_frame(const dspi_parser_t *parser)
{
    return parser->stage != DSPI_PARSER_WAIT_SYNC;
}

static bool is_device_frame_type(uint8_t type)
{
    return type == DSPI_FRAME_TYPE_SET_RESPONSE || type == DSPI_FRAME_TYPE_GET_RESPONSE ||
           type == DSPI_FRAME_TYPE_NOTIFICATION;
}

/* The longest payload a frame in the buffer can carry. */
static size_t max_payload(const dspi_parser_t *parser)
{
    return parser->capacity > DSPI_PARSER_OVERHEAD_BYTES
               ? parser->capacity - DSPI_PARSER_OVERHEAD_BYTES
               : 0;
}

/* Keeps one byte of the current frame. `checked` adds it to the CRC, which
 * covers everything after the sync byte except the CRC itself. */
static void store(dspi_parser_t *parser, uint8_t byte, bool checked)
{
    parser->buffer[parser->stored++] = byte;
    if (checked) {
        parser->crc = dspi_crc16_update(parser->crc, &byte, 1);
    }
}

/* Describes the frame in the parser, finished or discarded. Its bytes after
 * the sync byte are type, status, length (2) and then the payload. */
static void describe_frame(const dspi_parser_t *parser, dspi_frame_t *frame)
{
    frame->type = parser->buffer[0];
    frame->status = parser->buffer[1];
    frame->length = parser->length;
    frame->payload = &parser->buffer[4];
}

/*
 * Drops the current frame and queues its bytes for rescanning, ahead of any
 * bytes still waiting from an earlier rescan. The type byte is left out: it
 * followed the sync byte that turned out to be false, and is never 0xA5.
 *
 * Both moves go towards the start of the buffer. A frame found while
 * rescanning is stored behind the byte being read, so its bytes always end
 * before the bytes still waiting.
 */
static void keep_for_rescan(dspi_parser_t *parser)
{
    const size_t skip = parser->stored > 0 ? 1 : 0;
    const size_t kept = parser->stored - skip;
    const size_t waiting = parser->rescan_end - parser->rescan_next;
    memmove(parser->buffer, parser->buffer + skip, kept);
    memmove(parser->buffer + kept, parser->buffer + parser->rescan_next, waiting);
    parser->rescan_next = 0;
    parser->rescan_end = kept + waiting;
    parser->stored = 0;
    parser->stage = DSPI_PARSER_WAIT_SYNC;
}

/* Handles the bytes before the payload: type, status and length. */
static dspi_parse_result_t parse_header(dspi_parser_t *parser, uint8_t byte, dspi_frame_t *frame)
{
    switch (parser->stage) {
    case DSPI_PARSER_WAIT_TYPE:
        if (!is_device_frame_type(byte)) {
            /* Noise. A second 0xA5 may be the real start of a frame. */
            parser->stage =
                (byte == DSPI_FRAME_SYNC) ? DSPI_PARSER_WAIT_TYPE : DSPI_PARSER_WAIT_SYNC;
            return DSPI_PARSE_NOTHING;
        }
        store(parser, byte, true);
        parser->stage = DSPI_PARSER_WAIT_STATUS;
        return DSPI_PARSE_NOTHING;
    case DSPI_PARSER_WAIT_STATUS:
        store(parser, byte, true);
        parser->stage = DSPI_PARSER_WAIT_LENGTH_LOW;
        return DSPI_PARSE_NOTHING;
    case DSPI_PARSER_WAIT_LENGTH_LOW:
        store(parser, byte, true);
        parser->stage = DSPI_PARSER_WAIT_LENGTH_HIGH;
        return DSPI_PARSE_NOTHING;
    default: /* DSPI_PARSER_WAIT_LENGTH_HIGH */
        store(parser, byte, true);
        parser->length = (uint16_t)(parser->buffer[2] | (parser->buffer[3] << 8));
        if (parser->length > max_payload(parser)) {
            describe_frame(parser, frame);
            frame->payload = NULL;
            keep_for_rescan(parser);
            return DSPI_PARSE_TOO_LONG;
        }
        parser->stage = parser->length > 0 ? DSPI_PARSER_WAIT_PAYLOAD : DSPI_PARSER_WAIT_CRC_LOW;
        return DSPI_PARSE_NOTHING;
    }
}

/* Parses one byte, received or rescanned. */
static dspi_parse_result_t parse(dspi_parser_t *parser, uint8_t byte, dspi_frame_t *frame)
{
    switch (parser->stage) {
    case DSPI_PARSER_WAIT_SYNC:
        if (byte == DSPI_FRAME_SYNC) {
            parser->crc = DSPI_CRC_INITIAL;
            parser->stored = 0;
            parser->stage = DSPI_PARSER_WAIT_TYPE;
        }
        return DSPI_PARSE_NOTHING;

    case DSPI_PARSER_WAIT_TYPE:
    case DSPI_PARSER_WAIT_STATUS:
    case DSPI_PARSER_WAIT_LENGTH_LOW:
    case DSPI_PARSER_WAIT_LENGTH_HIGH:
        return parse_header(parser, byte, frame);

    case DSPI_PARSER_WAIT_PAYLOAD:
        store(parser, byte, true);
        if (parser->stored == 4u + parser->length) {
            parser->stage = DSPI_PARSER_WAIT_CRC_LOW;
        }
        return DSPI_PARSE_NOTHING;

    case DSPI_PARSER_WAIT_CRC_LOW:
        store(parser, byte, false);
        parser->stage = DSPI_PARSER_WAIT_CRC_HIGH;
        return DSPI_PARSE_NOTHING;

    case DSPI_PARSER_WAIT_CRC_HIGH: {
        store(parser, byte, false);
        const uint8_t *crc = &parser->buffer[parser->stored - DSPI_FRAME_CRC_BYTES];
        const uint16_t received_crc = (uint16_t)(crc[0] | (crc[1] << 8));
        describe_frame(parser, frame);
        if (received_crc != parser->crc) {
            frame->payload = NULL;
            keep_for_rescan(parser);
            return DSPI_PARSE_BAD_CRC;
        }
        parser->stage = DSPI_PARSER_WAIT_SYNC;
        return DSPI_PARSE_FRAME;
    }
    }

    /* Unreachable with a valid stage; recover rather than trust bad state. */
    dspi_parser_reset(parser);
    return DSPI_PARSE_NOTHING;
}

dspi_parse_result_t dspi_parser_feed(dspi_parser_t *parser, uint8_t byte, dspi_frame_t *frame)
{
    /* A caller that feeds while bytes wait to be rescanned has broken the
     * order of the stream; drop them rather than let the new frame overwrite
     * them half read. */
    parser->rescan_next = parser->rescan_end;
    return parse(parser, byte, frame);
}

bool dspi_parser_rescan(dspi_parser_t *parser, dspi_parse_result_t *result, dspi_frame_t *frame)
{
    if (!dspi_parser_rescan_pending(parser)) {
        return false;
    }
    const uint8_t byte = parser->buffer[parser->rescan_next++];
    *result = parse(parser, byte, frame);
    return true;
}

bool dspi_parser_rescan_pending(const dspi_parser_t *parser)
{
    return parser->rescan_next != parser->rescan_end;
}

void dspi_parser_abandon(dspi_parser_t *parser)
{
    if (dspi_parser_in_frame(parser)) {
        keep_for_rescan(parser);
    }
}
