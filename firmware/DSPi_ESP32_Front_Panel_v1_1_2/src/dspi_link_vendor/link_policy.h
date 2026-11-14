/*
 * link_policy.h - which role may send which command (Link spec 6.3 and 9.3).
 *
 * The classes come from the shared policy/commands.json, turned into C by
 * tools/gen_policy.py. A command the table does not list is config, so a
 * new firmware command is admin-only until classified (Link spec 10.7).
 *
 * Portable: no ESP-IDF headers. Every function only reads constant data, so
 * any task may call them; on the bridge it is the hub task.
 */

#ifndef LINK_POLICY_H
#define LINK_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* A command's class. CONFIG is zero so that a row missing from the
 * generated table is the strictest class. */
typedef enum {
    LINK_CLASS_CONFIG = 0,
    LINK_CLASS_CONTROL = 1,
    LINK_CLASS_READ = 2,
} link_class_t;

/* A session's role (Link spec 6.3). */
typedef enum {
    LINK_ROLE_VIEWER,
    LINK_ROLE_CONTROL,
    LINK_ROLE_ADMIN,
} link_role_t;

/* The class of `request` sent in one direction, from the generated table
 * (link_policy_table.h, private to this component). */
link_class_t link_policy_class(uint8_t request, bool is_get);

/* True when `role` may send a command of `class_`: a viewer reads, control
 * also controls, and admin may do anything. */
bool link_policy_role_allows(link_role_t role, link_class_t class_);

/* The role's name on the wire: "viewer", "control" or "admin", or
 * "unknown" for a value outside link_role_t. */
const char *link_policy_role_name(link_role_t role);

#endif /* LINK_POLICY_H */
