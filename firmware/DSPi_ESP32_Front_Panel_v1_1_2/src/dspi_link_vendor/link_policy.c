/*
 * link_policy.c - looks commands up in the generated table. Implements the
 * authorization of Link spec 6.3 and 9.3: the hub asks it before sending
 * any CMD to the DSPi. See link_policy.h.
 *
 * Portable: no ESP-IDF headers.
 */

#include "link_policy.h"

#include "link_policy_table.h"

link_class_t link_policy_class(uint8_t request, bool is_get)
{
    return (link_class_t)LINK_POLICY_TABLE[request][is_get ? 1 : 0];
}

bool link_policy_role_allows(link_role_t role, link_class_t class_)
{
    switch (role) {
    case LINK_ROLE_ADMIN:
        return true;
    case LINK_ROLE_CONTROL:
        return class_ == LINK_CLASS_READ || class_ == LINK_CLASS_CONTROL;
    case LINK_ROLE_VIEWER:
        return class_ == LINK_CLASS_READ;
    }
    return false;
}

const char *link_policy_role_name(link_role_t role)
{
    switch (role) {
    case LINK_ROLE_ADMIN:
        return "admin";
    case LINK_ROLE_CONTROL:
        return "control";
    case LINK_ROLE_VIEWER:
        return "viewer";
    }
    return "unknown";
}
