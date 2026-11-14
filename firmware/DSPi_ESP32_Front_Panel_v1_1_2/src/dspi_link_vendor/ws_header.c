/*
 * ws_header.c - encodes the header of a server's WebSocket frame (RFC 6455
 * section 5.2). See ws_header.h. http_server.c uses it to write a session's
 * frames in batches (bridge spec 8.2).
 *
 * Portable: no ESP-IDF headers.
 */

#include "ws_header.h"

#define FINAL_FRAGMENT 0x80
#define LENGTH_16_BIT 126
#define LENGTH_64_BIT 127

size_t ws_header_encode(uint8_t opcode, size_t payload_length, uint8_t out[WS_HEADER_MAX_BYTES])
{
    out[0] = (uint8_t)(FINAL_FRAGMENT | (opcode & 0x0F));
    if (payload_length < LENGTH_16_BIT) {
        out[1] = (uint8_t)payload_length;
        return 2;
    }
    if (payload_length <= 0xFFFF) {
        out[1] = LENGTH_16_BIT;
        out[2] = (uint8_t)(payload_length >> 8);
        out[3] = (uint8_t)(payload_length & 0xFF);
        return 4;
    }
    out[1] = LENGTH_64_BIT;
    const uint64_t length = payload_length;
    for (int i = 0; i < 8; i++) {
        out[2 + i] = (uint8_t)(length >> (56 - 8 * i)); /* Network byte order */
    }
    return 10;
}
