/*
 * web_assets.c - looks embedded web files up by path (bridge spec 10.5).
 * See web_assets.h. The HTTP server and the setup API serve what it finds;
 * the table itself is generated at build time (web_assets_table.h).
 */

#include "web_assets.h"

#include <string.h>

#include "web_assets_table.h"

const web_asset_t *web_assets_find(const char *path)
{
    for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
        if (strcmp(WEB_ASSETS[i].path, path) == 0) {
            return &WEB_ASSETS[i];
        }
    }
    return NULL;
}
