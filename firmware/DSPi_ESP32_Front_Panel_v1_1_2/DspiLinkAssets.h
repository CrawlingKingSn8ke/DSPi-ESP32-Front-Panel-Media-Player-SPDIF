#pragma once
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "src/dspi_link_vendor/web_assets.h"
static inline const web_asset_t* panel_link_asset(const char* path) {
  if(!path || (!strcmp(path,"/setup")) || (!strcmp(path,"/setup.html")))return NULL;
  return web_assets_find(!strcmp(path,"/")?"/index.html":path);
}
#ifdef __cplusplus
}
#endif
