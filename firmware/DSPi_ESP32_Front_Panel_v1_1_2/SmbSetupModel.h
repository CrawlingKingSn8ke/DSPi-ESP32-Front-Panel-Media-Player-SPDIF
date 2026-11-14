#pragma once
#include "SmbSettings.h"
namespace SmbLibrary {
struct SetupEdit {Settings settings;unsigned slot=0;bool remove=false;};
bool parseSetupEdit(const char* json,const Settings&,SetupEdit&);
bool setupList(const Settings&,const char* status,char*,size_t);
}
