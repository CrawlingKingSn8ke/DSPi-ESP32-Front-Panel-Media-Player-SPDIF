/*
 * ws_header.h - the header of a WebSocket frame sent by a server (RFC 6455
 * section 5.2).
 *
 * The HTTP server sends each session's waiting frames in one write (bridge
 * spec 8.2), so it builds the frames itself. A server's frames are never
 * masked, so a header is just the opcode and the payload length.
 *
 * Portable: no ESP-IDF headers.
 */

#ifndef WS_HEADER_H
#define WS_HEADER_H

#include <stddef.h>
#include <stdint.h>

/* The longest header: 2 bytes and a 64-bit length. */
#define WS_HEADER_MAX_BYTES 10

/* Opcodes (RFC 6455 section 5.2). */
#define WS_OPCODE_TEXT 0x1
#define WS_OPCODE_BINARY 0x2
#define WS_OPCODE_CLOSE 0x8
#define WS_OPCODE_PING 0x9
#define WS_OPCODE_PONG 0xA

/* Writes the header of one final, unmasked frame into `out` and returns its
 * length: 2 bytes up to 125 bytes of payload, 4 up to 65535, else 10. */
size_t ws_header_encode(uint8_t opcode, size_t payload_length, uint8_t out[WS_HEADER_MAX_BYTES]);

#endif /* WS_HEADER_H */
