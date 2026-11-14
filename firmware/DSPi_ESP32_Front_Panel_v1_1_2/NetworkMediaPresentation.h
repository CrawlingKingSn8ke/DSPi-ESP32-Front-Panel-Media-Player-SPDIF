#pragma once
#include <cstdint>
// Main-loop snapshot only. Rendering remains in the existing SD player view.
struct NetworkMediaPresentation {
 const char* path="";const char* album="";const char* error="";
 uint16_t* artwork=nullptr;uint64_t positionFrames=0;
 bool loading=false,seeking=false;
};
