/*
 * origin.h - whether a browser's Origin header names this bridge (Link
 * spec 4).
 *
 * A browser sends Origin with every WebSocket upgrade; native apps such as
 * DSPi Console send none. Without this check, any web page open in a
 * browser on the home network could open a Link session to the bridge and,
 * in `auth: none` mode, administer it. The bridge's own pages are served
 * from the bridge, so their origin names it: by the IP address the
 * connection arrived on, or by its .local hostname. Comparing against those
 * names, not against the request's Host header, also defeats DNS
 * rebinding, where a hostile name is pointed at the bridge's address.
 *
 * Portable: no ESP-IDF headers.
 */

#ifndef ORIGIN_H
#define ORIGIN_H

#include <stdbool.h>

/*
 * True when `origin`, an Origin header's value such as
 * "http://dspi-bridge-1b34.local:11915", names this bridge: an http or
 * https origin whose host is `local_address` (dotted IPv4, the address the
 * connection arrived on) or `hostname` followed by ".local", in any case,
 * on any port. "null", other schemes, IPv6 literals and anything malformed
 * are not. Any task.
 */
bool origin_is_own(const char *origin, const char *local_address, const char *hostname);

#endif /* ORIGIN_H */
