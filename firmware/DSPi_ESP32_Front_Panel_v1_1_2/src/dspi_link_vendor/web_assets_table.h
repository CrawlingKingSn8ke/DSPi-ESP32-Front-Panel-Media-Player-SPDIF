/*
 * web_assets_table.h - the table of embedded files, private to web_assets.
 *
 * tools/embed_web.py writes the table into web_assets_data.c in the build
 * folder (bridge spec 10.5). Only web_assets.c reads it; everything else
 * looks files up through web_assets_find().
 */

#ifndef WEB_ASSETS_TABLE_H
#define WEB_ASSETS_TABLE_H

#include <stddef.h>

#include "web_assets.h"

extern const web_asset_t WEB_ASSETS[];
extern const size_t WEB_ASSET_COUNT;

#endif /* WEB_ASSETS_TABLE_H */
