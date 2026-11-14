#pragma once
#include <cstdint>
namespace SmbLibrary {
struct Scroll {unsigned row;int page;};
// Row zero always remains an encoder-accessible Back action.
inline Scroll scroll(unsigned row,unsigned entries,int direction,bool previous,bool next){
 if(row>entries)row=entries;
 if(direction>0){if(unsigned(direction)<=entries-row)return {row+unsigned(direction),0};if(next&&entries)return {1,1};return {entries,0};}
 if(direction<0){const auto steps=uint64_t(-int64_t(direction));if(steps<=row)return {row-unsigned(steps),0};if(previous)return {16,-1};return {0,0};}
 return {row,0};
}
}
