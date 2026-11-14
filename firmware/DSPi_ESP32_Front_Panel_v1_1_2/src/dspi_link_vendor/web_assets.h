/*
 * web_assets.h - the web pages embedded in the firmware (bridge spec 10.5).
 *
 * tools/embed_web.py turns js/apps/web/dist into a table of gzipped files at
 * build time. The HTTP server serves them as they are, with
 * Content-Encoding: gzip.
 */

#ifndef WEB_ASSETS_H
#define WEB_ASSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *path; /* "/index.html", "/assets/index-3f2a.js", ... */
    const char *content_type;
    const uint8_t *data; /* gzipped */
    size_t length;
    const char *etag; /* Quoted, as the header carries it */
    bool immutable;   /* Named by content hash, so safe to cache for ever */
} web_asset_t;

/* The asset at `path` ("/index.html"), or NULL. The table is constant,
 * so any task may call this. */
const web_asset_t *web_assets_find(const char *path);

#endif /* WEB_ASSETS_H */
