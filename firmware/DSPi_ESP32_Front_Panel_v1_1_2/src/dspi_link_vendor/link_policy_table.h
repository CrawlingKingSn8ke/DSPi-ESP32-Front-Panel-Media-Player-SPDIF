/*
 * link_policy_table.h - the generated policy table, private to link_policy.
 *
 * link_policy_table.c (written by tools/gen_policy.py) defines these and
 * link_policy.c reads them. They are data rather than state: constant, made
 * at build time, never written. Everything else reaches them only through
 * link_policy.h.
 */

#ifndef LINK_POLICY_TABLE_H
#define LINK_POLICY_TABLE_H

#include <stdint.h>

/* The spec version of policy/commands.json the table was made from,
 * recorded as Link spec 9.3 asks. */
extern const char LINK_POLICY_SPEC_VERSION[];

/* [bRequest][0] is the SET class, [bRequest][1] the GET class, as
 * link_class_t values. Rows the JSON does not list are zero: config. */
extern const uint8_t LINK_POLICY_TABLE[256][2];

#endif /* LINK_POLICY_TABLE_H */
