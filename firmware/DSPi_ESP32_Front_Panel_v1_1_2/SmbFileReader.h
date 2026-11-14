#pragma once
#include <cstddef>
#include <cstdint>
namespace SmbLibrary {
class FileReader {
public:
 virtual ~FileReader()=default;
 virtual int readAt(uint64_t offset,uint8_t* bytes,size_t count)=0;
};
}
