/*
 * hub_auth.c - the PIN, paired clients' tokens and the pairing rate limits
 * (see hub_auth.h for the rules). Kept apart from the hub so the security
 * logic is small enough to review on its own and tested with fake crypto.
 *
 * Implements Link spec 6.2 for a fixed PIN and bridge spec 8.9.
 *
 * Portable: no ESP-IDF headers.
 */

#include "hub_auth.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static uint32_t now_ms(const hub_auth_t *auth)
{
    return auth->io.now_ms(auth->io.context);
}

/* True once `duration` ms have passed since `since`. Exact as long as no
 * more than 49 days pass between the two, which expire() guarantees by
 * ending everything timed long before that. */
static bool elapsed(uint32_t now, uint32_t since, uint32_t duration)
{
    return now - since >= duration;
}

/* Compares two hashes in time that does not depend on where they differ,
 * so response times reveal nothing about a stored hash. */
static bool same_hash(const uint8_t *a, const uint8_t *b)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < HUB_AUTH_HASH_BYTES; i++) {
        difference |= (uint8_t)(a[i] ^ b[i]);
    }
    return difference == 0;
}

/* AUTH_TOKEN_BYTES bytes as base64url without padding (RFC 4648 section
 * 5). */
static void encode_token(const uint8_t bytes[AUTH_TOKEN_BYTES], char out[HUB_AUTH_TOKEN_CHARS + 1])
{
    static const char ALPHABET[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t written = 0;
    for (size_t i = 0; i < AUTH_TOKEN_BYTES; i += 3) {
        const size_t left = AUTH_TOKEN_BYTES - i;
        const uint32_t group = ((uint32_t)bytes[i] << 16) |
                               (left > 1 ? (uint32_t)bytes[i + 1] << 8 : 0) |
                               (left > 2 ? (uint32_t)bytes[i + 2] : 0);
        out[written++] = ALPHABET[(group >> 18) & 0x3F];
        out[written++] = ALPHABET[(group >> 12) & 0x3F];
        if (left > 1) {
            out[written++] = ALPHABET[(group >> 6) & 0x3F];
        }
        if (left > 2) {
            out[written++] = ALPHABET[group & 0x3F];
        }
    }
    out[written] = '\0';
}

static bool save(const hub_auth_t *auth)
{
    if (auth->io.save_checked != NULL)
        return auth->io.save_checked(auth->io.context, &auth->store);
    auth->io.save(auth->io.context, &auth->store);
    return true;
}

/* ---------------------------------------------------------------------------
 * Rate limits (Link spec 6.2)
 * ------------------------------------------------------------------------- */

/* Ends the lockouts, failure windows and hourly save limits whose time has
 * passed, and frees the records of addresses with nothing left against
 * them. Run on every check and every tick, so every time kept here is
 * recent when it is compared. */
static void expire(hub_auth_t *auth, uint32_t now)
{
    for (size_t i = 0; i < AUTH_TRACKED_ADDRESSES; i++) {
        hub_auth_address_t *record = &auth->addresses[i];
        if (!record->used) {
            continue;
        }
        if (record->locked && elapsed(now, record->locked_since_ms, AUTH_LOCKOUT_MS)) {
            record->locked = false;
            record->failures = 0;
        }
        if (!record->locked && record->failures > 0 &&
            elapsed(now, record->window_start_ms, AUTH_FAILURE_WINDOW_MS)) {
            record->failures = 0;
        }
        if (!record->locked && record->failures == 0) {
            record->used = false;
        }
    }

    if (auth->global_locked && elapsed(now, auth->global_locked_since_ms, AUTH_LOCKOUT_MS)) {
        auth->global_locked = false;
        auth->global_failures = 0;
    }
    if (!auth->global_locked && auth->global_failures > 0 &&
        elapsed(now, auth->global_window_start_ms, AUTH_FAILURE_WINDOW_MS)) {
        auth->global_failures = 0;
    }

    for (size_t i = 0; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
        if (auth->saved_recently[i] &&
            elapsed(now, auth->saved_ms[i], AUTH_LAST_SEEN_SAVE_INTERVAL_MS)) {
            auth->saved_recently[i] = false;
        }
    }
}

/* The record for `address`: its own, else a free one, else the unlocked one
 * whose last failure is oldest, reused. NULL when every record holds a
 * lockout: a lockout is never given up to make room, or cycling through
 * more addresses than are tracked would lift it. The global limit still
 * counts such an address's failures. */
static hub_auth_address_t *address_record(hub_auth_t *auth, uint32_t address, uint32_t now)
{
    for (size_t i = 0; i < AUTH_TRACKED_ADDRESSES; i++) {
        if (auth->addresses[i].used && auth->addresses[i].address == address) {
            return &auth->addresses[i];
        }
    }
    hub_auth_address_t *choice = NULL;
    for (size_t i = 0; i < AUTH_TRACKED_ADDRESSES; i++) {
        hub_auth_address_t *record = &auth->addresses[i];
        if (!record->used) {
            choice = record;
            break;
        }
        if (!record->locked &&
            (choice == NULL || now - record->last_failure_ms > now - choice->last_failure_ms)) {
            choice = record;
        }
    }
    if (choice != NULL) {
        *choice = (hub_auth_address_t){.used = true, .address = address};
    }
    return choice;
}

/* True while `address` is locked out, or, when `include_global`, while
 * everyone is. The global lock guards the PIN alone: a paired client's
 * token is 32 random bytes nobody can guess, so refusing tokens too would
 * only let a stranger guessing PINs lock every paired app out (Link spec
 * 6.2). */
static bool locked_out(hub_auth_t *auth, uint32_t address, bool include_global)
{
    expire(auth, now_ms(auth));
    for (size_t i = 0; i < AUTH_TRACKED_ADDRESSES; i++) {
        const hub_auth_address_t *record = &auth->addresses[i];
        if (record->used && record->address == address && record->locked) {
            return true;
        }
    }
    return include_global && auth->global_locked;
}

/* Counts one failure from `address`, locking out at the limits. */
static void note_failure(hub_auth_t *auth, uint32_t address)
{
    const uint32_t now = now_ms(auth);
    expire(auth, now);
    hub_auth_address_t *record = address_record(auth, address, now);
    if (record != NULL) {
        if (record->failures == 0) {
            record->window_start_ms = now;
        }
        record->last_failure_ms = now;
        if (++record->failures >= AUTH_MAX_FAILURES_PER_ADDRESS) {
            record->locked = true;
            record->locked_since_ms = now;
        }
    }

    if (auth->global_failures == 0) {
        auth->global_window_start_ms = now;
    }
    if (++auth->global_failures >= AUTH_MAX_FAILURES_ALL_ADDRESSES) {
        auth->global_locked = true;
        auth->global_locked_since_ms = now;
    }
}

void hub_auth_tick(hub_auth_t *auth)
{
    expire(auth, now_ms(auth));
}

/* ---------------------------------------------------------------------------
 * Client names
 * ------------------------------------------------------------------------- */

/* The length of the well-formed UTF-8 character at `text` (RFC 3629: no
 * overlong forms, surrogates or values above U+10FFFF), or 0 if it is not
 * one. Reads no further than the first byte that fails. */
static size_t utf8_character_length(const unsigned char *text)
{
    const unsigned char lead = text[0];
    if (lead < 0x80) {
        return 1;
    }
    size_t length = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        low = lead == 0xE0 ? 0xA0 : low;
        high = lead == 0xED ? 0x9F : high;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        low = lead == 0xF0 ? 0x90 : low;
        high = lead == 0xF4 ? 0x8F : high;
    } else {
        return 0;
    }
    if (text[1] < low || text[1] > high) {
        return 0;
    }
    for (size_t k = 2; k < length; k++) {
        if ((text[k] & 0xC0) != 0x80) {
            return 0;
        }
    }
    return length;
}

void hub_auth_copy_name(char *out, size_t size, const char *name)
{
    if (size == 0) {
        return;
    }
    size_t written = 0;
    const unsigned char *text = (const unsigned char *)(name != NULL ? name : "");
    while (*text != '\0') {
        size_t length = utf8_character_length(text);
        const bool control = length == 1 && (*text < 0x20 || *text == 0x7F);
        const unsigned char *piece = text;
        size_t piece_length = length;
        if (length == 0 || control) {
            piece = (const unsigned char *)"?";
            piece_length = 1;
            length = 1;
        }
        if (written + piece_length >= size) {
            break; /* Cut between characters, never inside one */
        }
        memcpy(&out[written], piece, piece_length);
        written += piece_length;
        text += length;
    }
    out[written] = '\0';
}

/* ---------------------------------------------------------------------------
 * The PIN
 * ------------------------------------------------------------------------- */

void hub_auth_init(hub_auth_t *auth, const hub_auth_io_t *io, const hub_auth_store_t *saved)
{
    memset(auth, 0, sizeof(*auth));
    auth->io = *io;
    if (saved != NULL) {
        auth->store = *saved;
    }
    if (auth->store.next_cid == 0) {
        auth->store.next_cid = 1;
    }
    /* A name saved by older firmware may be cut inside a character, which
     * would break every auth.list reply that carries it. */
    for (size_t i = 0; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
        hub_auth_client_t *client = &auth->store.clients[i];
        char name[AUTH_CLIENT_NAME_BYTES];
        client->name[sizeof(client->name) - 1] = '\0';
        hub_auth_copy_name(name, sizeof(name), client->name);
        memcpy(client->name, name, sizeof(client->name));
    }
}

void hub_auth_mark_unreadable(hub_auth_t *auth)
{
    auth->unreadable = true;
}

bool hub_auth_unreadable(const hub_auth_t *auth)
{
    return auth->unreadable;
}

bool hub_auth_pin_enabled(const hub_auth_t *auth)
{
    return auth->store.pin_enabled || auth->unreadable;
}

bool hub_auth_pin_is_valid(const char *pin)
{
    if (pin == NULL || strlen(pin) != AUTH_PIN_DIGITS) {
        return false;
    }
    for (size_t i = 0; i < AUTH_PIN_DIGITS; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    }
    return true;
}

hub_auth_result_t hub_auth_set_pin(hub_auth_t *auth, const char *pin)
{
    if (pin != NULL && !hub_auth_pin_is_valid(pin)) {
        return HUB_AUTH_INVALID_PIN;
    }
    /* Made aside first, so a failed random source or hash leaves the old
     * PIN and every client in place. */
    uint8_t salt[AUTH_PIN_SALT_BYTES] = {0};
    uint8_t hash[HUB_AUTH_HASH_BYTES] = {0};
    if (pin != NULL && (!auth->io.random(auth->io.context, salt, sizeof(salt)) ||
                        !auth->io.pbkdf2(auth->io.context, pin, salt, hash))) {
        return HUB_AUTH_UNAVAILABLE;
    }

    const hub_auth_store_t previous = auth->store;
    hub_auth_store_t *store = &auth->store;
    memset(store->clients, 0, sizeof(store->clients)); /* Every token is revoked */
    memcpy(store->pin_salt, salt, sizeof(store->pin_salt));
    memcpy(store->pin_hash, hash, sizeof(store->pin_hash));
    store->pin_enabled = pin != NULL;
    if (!save(auth)) { auth->store = previous; return HUB_AUTH_UNAVAILABLE; }
    memset(auth->saved_recently, 0, sizeof(auth->saved_recently));
    auth->unreadable = false; /* Replaced by what was just chosen */
    return HUB_AUTH_OK;
}

hub_auth_result_t hub_auth_check_pin(hub_auth_t *auth, uint32_t address, const char *pin)
{
    if (auth->unreadable) {
        return HUB_AUTH_UNAVAILABLE; /* No PIN can be right; not the client's fault */
    }
    if (locked_out(auth, address, true)) {
        return HUB_AUTH_RATE_LIMITED;
    }
    uint8_t hash[HUB_AUTH_HASH_BYTES];
    const bool well_formed = hub_auth_pin_is_valid(pin);
    if (well_formed && !auth->io.pbkdf2(auth->io.context, pin, auth->store.pin_salt, hash)) {
        return HUB_AUTH_UNAVAILABLE; /* Not the client's fault, so not a failure */
    }
    if (!auth->store.pin_enabled || !well_formed || !same_hash(hash, auth->store.pin_hash)) {
        note_failure(auth, address);
        return HUB_AUTH_WRONG;
    }
    return HUB_AUTH_OK;
}

/* ---------------------------------------------------------------------------
 * Clients and tokens
 * ------------------------------------------------------------------------- */

/* A free entry, or the one seen least recently when all are used. */
static hub_auth_client_t *client_entry_for_new(hub_auth_t *auth)
{
    hub_auth_client_t *oldest = &auth->store.clients[0];
    for (size_t i = 0; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
        hub_auth_client_t *client = &auth->store.clients[i];
        if (!client->used) {
            return client;
        }
        if (client->last_seen < oldest->last_seen) {
            oldest = client;
        }
    }
    return oldest;
}

static uint16_t next_cid(hub_auth_t *auth)
{
    const uint16_t cid = auth->store.next_cid++;
    if (auth->store.next_cid == 0) {
        auth->store.next_cid = 1; /* 0 means "no client" */
    }
    return cid;
}

hub_auth_result_t hub_auth_issue(hub_auth_t *auth, const char *name,
                                 char token[HUB_AUTH_TOKEN_CHARS + 1], uint16_t *cid,
                                 uint16_t *replaced_cid)
{
    if (auth->unreadable) {
        return HUB_AUTH_UNAVAILABLE;
    }
    uint8_t secret[AUTH_TOKEN_BYTES];
    uint8_t token_hash[HUB_AUTH_HASH_BYTES];
    if (!auth->io.random(auth->io.context, secret, sizeof(secret))) {
        return HUB_AUTH_UNAVAILABLE;
    }
    encode_token(secret, token);
    if (!auth->io.sha256(auth->io.context, (const uint8_t *)token, HUB_AUTH_TOKEN_CHARS,
                         token_hash)) {
        return HUB_AUTH_UNAVAILABLE;
    }

    hub_auth_client_t *client = client_entry_for_new(auth);
    const hub_auth_client_t previous = *client;
    const uint16_t previous_next = auth->store.next_cid;
    if (replaced_cid != NULL) {
        *replaced_cid = client->used ? client->cid : 0;
    }
    const uint32_t now = auth->io.unix_time(auth->io.context);
    *client =
        (hub_auth_client_t){.used = true, .cid = next_cid(auth), .created = now, .last_seen = now};
    memcpy(client->token_hash, token_hash, sizeof(client->token_hash));
    hub_auth_copy_name(client->name, sizeof(client->name), name);
    *cid = client->cid;

    if (!save(auth)) {
        *client = previous; auth->store.next_cid = previous_next;
        token[0] = '\0'; *cid = 0;
        if (replaced_cid != NULL) *replaced_cid = 0;
        return HUB_AUTH_UNAVAILABLE;
    }
    /* Saved just now, with its last_seen. */
    const size_t index = (size_t)(client - auth->store.clients);
    auth->saved_recently[index] = true;
    auth->saved_ms[index] = now_ms(auth);
    return HUB_AUTH_OK;
}

hub_auth_result_t hub_auth_pair(hub_auth_t *auth, uint32_t address, const char *pin,
                                const char *name, char token[HUB_AUTH_TOKEN_CHARS + 1],
                                uint16_t *cid, uint16_t *replaced_cid)
{
    const hub_auth_result_t checked = hub_auth_check_pin(auth, address, pin);
    if (checked != HUB_AUTH_OK) {
        return checked;
    }
    return hub_auth_issue(auth, name, token, cid, replaced_cid);
}

hub_auth_result_t hub_auth_token(hub_auth_t *auth, uint32_t address, const char *token,
                                 uint16_t *cid)
{
    if (auth->unreadable) {
        return HUB_AUTH_UNAVAILABLE; /* Every token went with the unreadable store */
    }
    if (locked_out(auth, address, false)) {
        return HUB_AUTH_RATE_LIMITED;
    }
    if (token != NULL && strlen(token) == HUB_AUTH_TOKEN_CHARS) {
        uint8_t hash[HUB_AUTH_HASH_BYTES];
        if (!auth->io.sha256(auth->io.context, (const uint8_t *)token, HUB_AUTH_TOKEN_CHARS,
                             hash)) {
            return HUB_AUTH_UNAVAILABLE;
        }
        for (size_t i = 0; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
            const hub_auth_client_t *client = &auth->store.clients[i];
            if (client->used && same_hash(client->token_hash, hash)) {
                *cid = client->cid;
                hub_auth_touch(auth, client->cid);
                return HUB_AUTH_OK;
            }
        }
    }
    note_failure(auth, address);
    return HUB_AUTH_WRONG;
}

void hub_auth_touch(hub_auth_t *auth, uint16_t cid)
{
    const uint32_t now = now_ms(auth);
    expire(auth, now);
    for (size_t i = 0; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
        hub_auth_client_t *client = &auth->store.clients[i];
        if (!client->used || client->cid != cid) {
            continue;
        }
        const uint32_t unix_now = auth->io.unix_time(auth->io.context);
        if (unix_now != 0) {
            client->last_seen = unix_now;
        }
        if (!auth->saved_recently[i]) {
            if (save(auth)) {
                auth->saved_recently[i] = true;
                auth->saved_ms[i] = now;
            }
        }
        return;
    }
}

bool hub_auth_revoke(hub_auth_t *auth, uint16_t cid)
{
    for (size_t i = 0; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
        hub_auth_client_t *client = &auth->store.clients[i];
        if (client->used && client->cid == cid) {
            const hub_auth_client_t previous = *client;
            memset(client, 0, sizeof(*client));
            if (!save(auth)) { *client = previous; return false; }
            auth->saved_recently[i] = false;
            return true;
        }
    }
    return false;
}

const hub_auth_client_t *hub_auth_clients(const hub_auth_t *auth, size_t *count)
{
    *count = AUTH_MAX_PAIRED_CLIENTS;
    return auth->store.clients;
}
