/*
 * link_frames.c - encodes and decodes Link binary frames. See
 * link_frames.h; implements Link spec 8.
 *
 * Portable: no ESP-IDF headers.
 */

#include "link_frames.h"

#include <string.h>

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)(in[0] | (in[1] << 8));
}

static uint32_t get_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFF);
    out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    put_u16(out, (uint16_t)(value & 0xFFFF));
    put_u16(out + 2, (uint16_t)(value >> 16));
}

/* Writes the 4-byte header shared by every frame. Flags are always 0. */
static void put_header(uint8_t *out, uint8_t type, uint16_t tag)
{
    out[0] = type;
    out[1] = 0;
    put_u16(&out[2], tag);
}

int link_frame_type(const uint8_t *frame, size_t length)
{
    return length >= 1 ? frame[0] : -1;
}

uint16_t link_frame_tag(const uint8_t *frame, size_t length)
{
    return length >= LINK_FRAME_HEADER_BYTES ? get_u16(&frame[2]) : 0;
}

bool link_decode_cmd_request(const uint8_t *frame, size_t length, link_cmd_request_t *out)
{
    if (length < LINK_CMD_REQUEST_HEADER_BYTES || frame[0] != LINK_FRAME_CMD_REQUEST) {
        return false;
    }
    const uint8_t direction = frame[5];
    if (direction > 1) {
        return false;
    }

    out->tag = get_u16(&frame[2]);
    out->handle = frame[4];
    out->is_get = direction == 1;
    out->request = frame[6];
    out->value = get_u16(&frame[8]);
    out->index = get_u16(&frame[10]);
    out->length = get_u16(&frame[12]);

    /* A SET carries exactly wLength bytes; a GET carries none. */
    const size_t payload_length = length - LINK_CMD_REQUEST_HEADER_BYTES;
    const size_t expected = out->is_get ? 0 : out->length;
    if (payload_length != expected) {
        return false;
    }
    out->payload = payload_length > 0 ? &frame[LINK_CMD_REQUEST_HEADER_BYTES] : NULL;
    return true;
}

bool link_decode_fwdata(const uint8_t *frame, size_t length, link_fwdata_chunk_t *out)
{
    if (length < LINK_FWDATA_HEADER_BYTES || frame[0] != LINK_FRAME_FWDATA) {
        return false;
    }
    out->transfer = get_u16(&frame[2]);
    out->offset = get_u32(&frame[4]);
    out->data = &frame[LINK_FWDATA_HEADER_BYTES];
    out->length = length - LINK_FWDATA_HEADER_BYTES;
    return true;
}

size_t link_encode_cmd_response(uint8_t *out, size_t capacity, uint16_t tag, uint8_t status,
                                const uint8_t *payload, uint16_t length)
{
    const size_t total = LINK_CMD_RESPONSE_HEADER_BYTES + length;
    if (capacity < total) {
        return 0;
    }
    put_header(out, LINK_FRAME_CMD_RESPONSE, tag);
    out[4] = status;
    out[5] = 0;
    put_u16(&out[6], length);
    if (length > 0) {
        memcpy(&out[8], payload, length);
    }
    return total;
}

size_t link_encode_notify(uint8_t *out, size_t capacity, uint16_t sequence, uint8_t handle,
                          uint16_t origin, const uint8_t *packet, size_t length)
{
    const size_t total = LINK_NOTIFY_HEADER_BYTES + length;
    if (capacity < total) {
        return 0;
    }
    put_header(out, LINK_FRAME_NOTIFY, sequence);
    out[4] = handle;
    out[5] = 0;
    put_u16(&out[6], origin);
    if (length > 0) {
        memcpy(&out[8], packet, length);
    }
    return total;
}

size_t link_encode_poll(uint8_t *out, size_t capacity, uint16_t sequence, uint8_t handle,
                        uint8_t slot, const uint8_t *payload, uint16_t length)
{
    const size_t total = LINK_POLL_HEADER_BYTES + length;
    if (capacity < total) {
        return 0;
    }
    put_header(out, LINK_FRAME_POLL, sequence);
    out[4] = handle;
    out[5] = slot;
    put_u16(&out[6], length);
    if (length > 0) {
        memcpy(&out[8], payload, length);
    }
    return total;
}

size_t link_encode_fwdata_ack(uint8_t *out, size_t capacity, uint16_t transfer, uint32_t offset,
                              uint8_t status)
{
    const size_t total = LINK_FWDATA_ACK_BYTES;
    if (capacity < total) {
        return 0;
    }
    put_header(out, LINK_FRAME_FWDATA_ACK, transfer);
    put_u32(&out[4], offset);
    out[8] = status;
    return total;
}

size_t link_encode_resync(uint8_t *out, size_t capacity, uint8_t handle, uint8_t reason)
{
    const size_t total = LINK_RESYNC_BYTES;
    if (capacity < total) {
        return 0;
    }
    put_header(out, LINK_FRAME_RESYNC, 0); /* The tag is unused (Link spec 8.6) */
    out[4] = handle;
    out[5] = reason;
    return total;
}
