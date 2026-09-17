#include <cassert>
#include <cmath>
#include <cstdio>
#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/SubSynth.h"
int main() {
  SubSynthState state;
  assert(state.value[SUB_LINK] == 1 && state.value[SUB_ENABLE] == 0);
  assert(state.value[SUB_TOP] == -30 && state.value[SUB_HOLD] == 150);
  assert(state.value[SUB_SELECT] == 0 && state.value[SUB_DEPTH] == 100);
  assert(subSynthParameter(0, 1) == SUB_LOW);
  assert(subSynthParameter(0, 8) == SUB_LINK);
  assert(subSynthParameter(0, 4) == SUB_INVALID);
  assert(subSynthParameter(1, 0) == SUB_SELECT);
  assert(subSynthParameter(1, 2) == SUB_HOLD);
  assert(subSynthOutputAt(0x105, 0) == 0);
  assert(subSynthOutputAt(0x105, 1) == 2);
  assert(subSynthOutputAt(0x105, 2) == 8);
  assert(subSynthOutputAt(0x105, 3) == -1);
  assert(subSynthOutputCount(0) == 0 && subSynthOutputCount(0x105) == 3);
  assert(subSynthToggleOutput(0x101, 2) == 0x105);
  assert(subSynthToggleOutput(0x105, 2) == 0x101);
  assert(subSynthValid(SUB_LOW, 12) && !subSynthValid(SUB_LOW, 12.5f));
  assert(!subSynthValid(SUB_LOW, NAN));
  assert(!subSynthValid(SUB_SELECT, 1.5f));
  assert(subSynthValid(SUB_HOLD, 400) && !subSynthValid(SUB_HOLD, 401));
  assert(subSynthSpecs[SUB_MASK].get == 0x19 && subSynthSpecs[SUB_MASK].bytes == 2);
  assert(subSynthSpecs[SUB_LINK].set == 0x2e);
  std::puts("Sub Synth policy tests passed");
}
