/*
 * origin.c - whether a browser's Origin header names this bridge (Link
 * spec 4). See origin.h.
 */

#include "origin.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* True when the first `length` bytes of `text` equal `expected`, ignoring
 * case, and `expected` has exactly that length. */
static bool equals_ignoring_case(const char *text, size_t length, const char *expected)
{
    if (strlen(expected) != length) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)expected[i])) {
            return false;
        }
    }
    return true;
}

/* The host part of "scheme://host[:port]", or NULL for any other shape.
 * Sets `*length` to its length. */
static const char *origin_host(const char *origin, size_t *length)
{
    static const char *const SCHEMES[] = {"http://", "https://"};
    for (size_t i = 0; i < sizeof(SCHEMES) / sizeof(SCHEMES[0]); i++) {
        const size_t scheme_length = strlen(SCHEMES[i]);
        if (strlen(origin) > scheme_length &&
            equals_ignoring_case(origin, scheme_length, SCHEMES[i])) {
            const char *host = origin + scheme_length;
            *length = strcspn(host, ":/");
            /* An origin has no path; a port is digits only. */
            const char *after = host + *length;
            if (*after == '/') {
                return NULL;
            }
            if (*after == ':' &&
                (after[1] == '\0' || strspn(after + 1, "0123456789") != strlen(after + 1))) {
                return NULL;
            }
            return *length > 0 ? host : NULL;
        }
    }
    return NULL;
}

bool origin_is_own(const char *origin, const char *local_address, const char *hostname)
{
    if (origin == NULL) {
        return false;
    }
    size_t length = 0;
    const char *host = origin_host(origin, &length);
    if (host == NULL || host[0] == '[') {
        return false; /* Malformed, "null", or an IPv6 literal the bridge never has */
    }
    if (local_address != NULL && local_address[0] != '\0' &&
        equals_ignoring_case(host, length, local_address)) {
        return true;
    }
    static const char LOCAL_SUFFIX[] = ".local";
    const size_t suffix_length = sizeof(LOCAL_SUFFIX) - 1;
    if (hostname == NULL || hostname[0] == '\0' || length <= suffix_length ||
        !equals_ignoring_case(host + length - suffix_length, suffix_length, LOCAL_SUFFIX)) {
        return false;
    }
    return equals_ignoring_case(host, length - suffix_length, hostname);
}
