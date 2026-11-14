/*
 * link_frames.h - the Link binary data plane (Link spec 8).
 *
 * Every binary WebSocket message is one frame: a 4-byte header (type,
 * flags, tag) and a type-specific body. Multi-byte fields are little-endian.
 * The hub decodes the frames clients send (CMD requests, FWDATA chunks) and
 * encodes the ones it sends (CMD responses, NOTIFY, POLL, FWDATA acks,
 * RESYNC).
 *
 * Portable: no ESP-IDF headers. Encoders write into a caller's buffer and
 * return the frame length, or 0 if the buffer is too small. Nothing here
 * keeps state, so any task may call these; on the bridge it is the hub task.
 */

#ifndef LINK_FRAMES_H
#define LINK_FRAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Frame types (Link spec 8). */
#define LINK_FRAME_CMD_REQUEST 0x01
#define LINK_FRAME_CMD_RESPONSE 0x81
#define LINK_FRAME_NOTIFY 0x02
#define LINK_FRAME_POLL 0x03
#define LINK_FRAME_FWDATA 0x04
#define LINK_FRAME_FWDATA_ACK 0x84
#define LINK_FRAME_RESYNC 0x05

#define LINK_FRAME_HEADER_BYTES 4
#define LINK_CMD_REQUEST_HEADER_BYTES 14 /* Everything before a SET's payload */
#define LINK_CMD_RESPONSE_HEADER_BYTES 8
#define LINK_NOTIFY_HEADER_BYTES 8
#define LINK_POLL_HEADER_BYTES 8
#define LINK_FWDATA_HEADER_BYTES 8 /* Everything before a chunk's data */
#define LINK_FWDATA_ACK_BYTES 9
#define LINK_RESYNC_BYTES 6

/* Link-level CMD statuses (Link spec 8.1). 0x00 to 0x07 are the DSPi's own,
 * passed through unchanged (see dspi_frame.h). */
#define LINK_STATUS_NO_DEVICE 0x80
#define LINK_STATUS_DENIED 0x81
#define LINK_STATUS_TIMEOUT 0x82
#define LINK_STATUS_TOO_LARGE 0x83
#define LINK_STATUS_RATE_LIMITED 0x84
#define LINK_STATUS_LOCKED 0x85
#define LINK_STATUS_UNSUPPORTED 0x86
#define LINK_STATUS_BAD_FRAME 0x87

/* RESYNC reasons (Link spec 8.6). */
#define LINK_RESYNC_DROPPED 0
#define LINK_RESYNC_REATTACHED 1
#define LINK_RESYNC_CACHE_REBUILT 2

/* The type byte of a frame, or -1 if it is too short to have one. */
int link_frame_type(const uint8_t *frame, size_t length);

/* The tag of a frame, or 0 if it is too short to have one. */
uint16_t link_frame_tag(const uint8_t *frame, size_t length);

/* A CMD request (Link spec 8.2). `payload` points into the frame. */
typedef struct {
    uint16_t tag;
    uint8_t handle;
    bool is_get;
    uint8_t request; /* bRequest */
    uint16_t value;  /* wValue */
    uint16_t index;  /* wIndex */
    uint16_t length; /* wLength: SET payload bytes, or GET bytes wanted */
    const uint8_t *payload;
} link_cmd_request_t;

/* Decodes a CMD request. False if the frame is malformed: the wrong type, a
 * direction other than 0 or 1, or a length that disagrees with wLength. */
bool link_decode_cmd_request(const uint8_t *frame, size_t length, link_cmd_request_t *out);

/* A FWDATA chunk (Link spec 8.5). `data` points into the frame. */
typedef struct {
    uint16_t transfer; /* The tag: the xfer id from fw.install or hub.update */
    uint32_t offset;
    const uint8_t *data;
    size_t length;
} link_fwdata_chunk_t;

/* Decodes a FWDATA chunk. False if the frame is of another type or too
 * short for its offset; a chunk with no data is valid. */
bool link_decode_fwdata(const uint8_t *frame, size_t length, link_fwdata_chunk_t *out);

/* Encodes a CMD response (Link spec 8.2): `status` and, for an OK GET,
 * `length` bytes of `payload` (which may be NULL when `length` is 0).
 * Returns the frame length, or 0 if `capacity` is too small. */
size_t link_encode_cmd_response(uint8_t *out, size_t capacity, uint16_t tag, uint8_t status,
                                const uint8_t *payload, uint16_t length);

/* Encodes a NOTIFY (Link spec 8.3) carrying `packet` verbatim, with the
 * session's `sequence` as its tag and `origin` the session id the hub
 * credits, or 0. Returns the frame length, or 0 if `capacity` is too small. */
size_t link_encode_notify(uint8_t *out, size_t capacity, uint16_t sequence, uint8_t handle,
                          uint16_t origin, const uint8_t *packet, size_t length);

/* Encodes a POLL (Link spec 8.4): the answer to the GET subscribed in
 * `slot`. Returns the frame length, or 0 if `capacity` is too small. */
size_t link_encode_poll(uint8_t *out, size_t capacity, uint16_t sequence, uint8_t handle,
                        uint8_t slot, const uint8_t *payload, uint16_t length);

/* Encodes a FWDATA ack (Link spec 8.5) for the chunk at `offset`; status 0
 * means accepted. Returns LINK_FWDATA_ACK_BYTES, or 0 if `capacity` is too
 * small. */
size_t link_encode_fwdata_ack(uint8_t *out, size_t capacity, uint16_t transfer, uint32_t offset,
                              uint8_t status);

/* Encodes a RESYNC (Link spec 8.6) with one of the LINK_RESYNC_* reasons.
 * Returns LINK_RESYNC_BYTES, or 0 if `capacity` is too small. */
size_t link_encode_resync(uint8_t *out, size_t capacity, uint8_t handle, uint8_t reason);

#endif /* LINK_FRAMES_H */
