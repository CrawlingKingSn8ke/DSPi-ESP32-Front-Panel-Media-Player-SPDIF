#pragma once
#include <cstdint>
namespace SmbWorkers {
enum class Kind:uint8_t {Audio,Browse,Artwork,ReadAhead};
using Function=void(*)(void*);
// One outstanding lease per role, held until the owner acknowledges release.
// Completion means the callback has returned, not just published its own flag.
bool submit(Kind,Function,void* job);
bool finished(Kind,const void* job);
bool release(Kind,const void* job);
}
