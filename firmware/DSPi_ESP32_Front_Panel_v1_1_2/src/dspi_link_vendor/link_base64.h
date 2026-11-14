/*
 * link_base64.h - standard base64 with padding, for the `_b64` fields of
 * the control plane (Link spec 7.1).
 *
 * Portable: no ESP-IDF headers, and no state, so any task may call these;
 * on the bridge the hub task builds device.snapshot replies with them.
 */

#ifndef LINK_BASE64_H
#define LINK_BASE64_H

#include <stddef.h>
#include <stdint.h>

/* Characters needed to encode `length` bytes, not counting the NUL. */
size_t link_base64_encoded_length(size_t length);

/* Writes the base64 of `data` and a NUL into `out`, which needs
 * link_base64_encoded_length(length) + 1 bytes. Returns the characters
 * written, not counting the NUL. */
size_t link_base64_encode(const uint8_t *data, size_t length, char *out);

#endif /* LINK_BASE64_H */
