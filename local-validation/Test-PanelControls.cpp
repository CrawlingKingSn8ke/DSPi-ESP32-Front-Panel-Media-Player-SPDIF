#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/PanelControls.h"
using namespace PanelControls;
static unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }
static ChannelMap current() {
  return channelMap(selectProfile({Platform::Rp2350, 8, 9, true, true}));
}

// Catches old RP2350 numbering being accepted from a platform/version alone,
// output-local IDs being confused with EQ IDs, and the old 4-bit band packing.
static void mapping() {
  ChannelMap unknown = channelMap(Profile::Unknown);
  uint8_t id = 99, slot = 99;
  uint16_t selector = 0;
  CHECK(!outputEqId(unknown, 0, id));
  CHECK(selectProfile({Platform::Rp2350, 0, 9, true, true}) == Profile::Unknown);
  CHECK(selectProfile({Platform::Rp2350, 2, 9, true, true}) == Profile::Unknown);
  CHECK(selectProfile({Platform::Rp2350, 8, 9, false, true}) == Profile::Unknown);
  CHECK(selectProfile({Platform::Rp2350, 8, 9, true, false}) == Profile::Unknown);
  CHECK(selectProfile({Platform::Unknown, 8, 9, true, true}) == Profile::Unknown);
  const ChannelMap m = current();
  CHECK(m.writable && m.inputs == 8 && m.outputs == 9);
  CHECK(inputEqId(m, 7, id) && id == 7);
  CHECK(!inputEqId(m, 8, id));
  CHECK(outputEqId(m, 0, id) && id == 8);
  CHECK(outputEqId(m, 8, id) && id == 16);
  CHECK(!outputEqId(m, 9, id));
  CHECK(outputLocalId(m, 8, id) && id == 0);
  CHECK(outputLocalId(m, 16, id) && id == 8);
  CHECK(!outputLocalId(m, 7, id));
  CHECK(outputPinSlot(m, 0, slot) && slot == 0);
  CHECK(outputPinSlot(m, 1, slot) && slot == 0);
  CHECK(outputPinSlot(m, 7, slot) && slot == 3);
  CHECK(outputPinSlot(m, 8, slot) && slot == 4);
  CHECK(eqSelector(m, 16, 23, ScalarField::TargetQ, selector) && selector == 0x10bd);
  CHECK(eqSelector(m, 8, 9, ScalarField::Gain, selector) && selector == 0x084b);
  CHECK(!eqSelector(m, 0, 20, ScalarField::Frequency, selector));
  CHECK(!eqSelector(m, 8, 10, ScalarField::Type, selector));
  CHECK(!eqSelector(m, 8, 19, ScalarField::Type, selector));
  CHECK(!eqSelector(m, 8, 24, ScalarField::Type, selector));
  const ChannelMap small = channelMap(selectProfile({Platform::Rp2040, 2, 5, true, true}));
  CHECK(outputEqId(small, 4, id) && id == 6);
  CHECK(outputPinSlot(small, 4, slot) && slot == 2);
  const ChannelMap legacy = channelMap(Profile::LegacyRp2350StereoReadOnly);
  CHECK(!legacy.writable && legacy.inputs == 2 && legacy.outputs == 9);
  CHECK(outputEqId(legacy, 8, id) && id == 10);
  CHECK(!eqSelector(legacy, 2, 20, ScalarField::Type, selector));
}

// Catches dB edits corrupting LT fp, hidden fields being exposed, reserved type
// writes, unsafe initialization, and sidecars being dropped by linked edits.
static void recipes() {
  const uint8_t wantFields[] = {0,7,7,7,3,3,3,3,1,5,5,27,1,1};
  for (unsigned t = 0; t < sizeof(wantFields); ++t)
    CHECK(filterFields(static_cast<uint8_t>(t)) == wantFields[t]);
  CHECK(filterFields(14) == 0 && filterFields(31) == 0 && filterFields(64) == 0);
  CHECK(filterFields(32) == FieldFrequency);
  BandRecipe r{}, changed{};
  CHECK(makeRecipe(1, 1000, 48000, r) == Result::Ok);
  CHECK(r.type == 1 && r.bypass == 0 && near(r.q, 0.707f) && r.gainDb == 0);
  CHECK(makeRecipe(11, 55, 48000, r) == Result::Ok);
  CHECK(r.frequencyHz == 55 && r.gainDb == 55 && r.qpX512 == 362);
  r = {11, 1, 55, 1.1f, 25, 282};
  CHECK(editBand(r, BandField::Frequency, 60, 48000, changed) == Result::Ok);
  CHECK(changed.frequencyHz == 60 && changed.q == r.q && changed.gainDb == 25 && changed.qpX512 == 282 && changed.bypass == 1);
  CHECK(editBand(r, BandField::GainDb, -3, 48000, changed) == Result::InvalidField);
  CHECK(editBand(r, BandField::TargetFrequency, 30, 48000, changed) == Result::Ok && changed.gainDb == 30);
  CHECK(editBand(r, BandField::TargetQ, 0.5f, 48000, changed) == Result::Ok && changed.qpX512 == 256);
  CHECK(editBand(r, BandField::TargetQ, 0.1f, 48000, changed) == Result::Ok && changed.qpX512 == 51);
  CHECK(editBand(r, BandField::TargetQ, 20, 48000, changed) == Result::Ok && changed.qpX512 == 10240);
  CHECK(near(decodeTargetQ(0), 0.707f));
  CHECK(near(decodeTargetQ(282), 0.55078125f));
  CHECK(near(linkwitzBoostDb(r), 13.6969f));
  CHECK(editBand(r, BandField::Frequency, 7200, 48000, changed) == Result::Ok);
  CHECK(editBand(r, BandField::Frequency, 7201, 48000, changed) == Result::OutOfRange);
  CHECK(editBand(r, BandField::TargetFrequency, 9, 48000, changed) == Result::OutOfRange);
  CHECK(editBand(r, BandField::Q, 20, 48000, changed) == Result::Ok);
  CHECK(editBand(r, BandField::TargetQ, 20.01f, 48000, changed) == Result::OutOfRange);
  CHECK(editBand(r, BandField::Type, 1, 48000, changed) == Result::Ok && changed.gainDb == 0 && changed.qpX512 == 282);
  r = {1, 0, 900, 1.2f, 18, 973};
  CHECK(editBand(r, BandField::Frequency, 1200, 48000, changed) == Result::Ok);
  CHECK(changed.gainDb == 18 && changed.qpX512 == 973 && changed.q == 1.2f);
  CHECK(editBand(r, BandField::GainDb, 12, 48000, changed) == Result::Ok);
  CHECK(editBand(r, BandField::GainDb, 12.01f, 48000, changed) == Result::OutOfRange);
  CHECK(editBand(r, BandField::GainDb, -24, 48000, changed) == Result::Ok);
  CHECK(editBand(r, BandField::GainDb, -24.01f, 48000, changed) == Result::OutOfRange);
  CHECK(editBand(r, BandField::Frequency, 21600, 48000, changed) == Result::Ok);
  CHECK(editBand(r, BandField::Frequency, 21601, 48000, changed) == Result::OutOfRange);
  CHECK(editBand(r, BandField::Frequency, std::numeric_limits<float>::quiet_NaN(), 48000, changed) == Result::NonFinite);
  CHECK(editBand(r, BandField::Q, std::numeric_limits<float>::infinity(), 48000, changed) == Result::NonFinite);
  CHECK(editBand(r, BandField::Type, 1.5f, 48000, changed) == Result::OutOfRange);
  CHECK(editBand(r, BandField::Type, 14, 48000, changed) == Result::Unsupported);
  CHECK(editBand(r, BandField::Type, 11, 48000, changed) == Result::Ok && changed.gainDb == 900);
  r = {8, 0, 1000, 5, 4, 917};
  CHECK(editBand(r, BandField::Q, 1, 48000, changed) == Result::InvalidField);
  CHECK(editBand(r, BandField::GainDb, 1, 48000, changed) == Result::InvalidField);
  CHECK(editBand(r, BandField::Bypass, 1, 48000, changed) == Result::Ok && changed.q == 5 && changed.gainDb == 4 && changed.qpX512 == 917);
  CHECK(editBand(r, BandField::Bypass, 255, 48000, changed) == Result::OutOfRange);
  CHECK(makeRecipe(1, 1000, 0, changed) == Result::OutOfRange);
}

// Catches incorrect LP/HP/family/order mapping, dangerous reserved bands and
// non-little-endian packets. Literal bytes are independent of the encoder.
static void crossoversAndPackets() {
  uint8_t type = 0, order = 0;
  bool hp = false;
  XoFamily family = XoFamily::LinkwitzRiley;
  CHECK(crossoverType(XoFamily::LinkwitzRiley, 24, true, type) && type == 35);
  CHECK(crossoverType(XoFamily::Butterworth, 6, false, type) && type == 40);
  CHECK(crossoverType(XoFamily::Butterworth, 48, true, type) && type == 55);
  CHECK(crossoverType(XoFamily::Bessel, 48, false, type) && type == 62);
  CHECK(!crossoverType(XoFamily::LinkwitzRiley, 18, false, type));
  CHECK(!crossoverType(XoFamily::Bessel, 6, false, type));
  CHECK(!crossoverType(XoFamily::Butterworth, 0, false, type));
  for (unsigned t = 32; t <= 63; ++t) {
    CHECK(decodeCrossover(static_cast<uint8_t>(t), family, order, hp));
    CHECK(crossoverType(family, static_cast<uint8_t>(order * 6), hp, type) && type == t);
  }
  CHECK(!decodeCrossover(31, family, order, hp));
  BandRecipe lt{11, 1, 55, 1.1f, 25, 282};
  EqPacket packet{};
  CHECK(packEq(current(), 16, 9, lt, 48000, packet) == Result::Ok && packet.size == 18);
  const uint8_t expected[] = {16,9,11,1,0,0,92,66,205,204,140,63,0,0,200,65,26,1};
  CHECK(std::memcmp(packet.bytes, expected, sizeof(expected)) == 0);
  BandRecipe peq{1, 255, 1000, 0.707f, 18, 573};
  CHECK(packEq(current(), 8, 0, peq, 48000, packet) == Result::Ok && packet.size == 16 && packet.bytes[3] == 0);
  CHECK(packEq(current(), 0, 20, peq, 48000, packet) == Result::InvalidTarget);
  CHECK(packEq(current(), 8, 10, peq, 48000, packet) == Result::InvalidTarget);
  CHECK(packEq(current(), 8, 20, peq, 48000, packet) == Result::Unsupported);
  CHECK(packEq(channelMap(Profile::LegacyRp2350StereoReadOnly), 2, 0, peq, 48000, packet) == Result::Unsupported);
  BandRecipe xo{35, 0, 80, 0, 0, 0};
  CHECK(packEq(current(), 8, 20, xo, 48000, packet) == Result::Ok);
  CHECK(packEq(current(), 8, 0, xo, 48000, packet) == Result::Unsupported);
  peq.frequencyHz = std::numeric_limits<float>::infinity();
  CHECK(packEq(current(), 8, 0, peq, 48000, packet) == Result::NonFinite);
  // A linked edit is two independent fresh read/modify/writes, never cloning
  // the first member's unrelated parameters onto the second member.
  BandRecipe second{11, 0, 65, 0.9f, 35, 461}, firstEdit{}, secondEdit{};
  CHECK(editBand(lt, BandField::Frequency, 70, 48000, firstEdit) == Result::Ok);
  CHECK(editBand(second, BandField::Frequency, 70, 48000, secondEdit) == Result::Ok);
  CHECK(firstEdit.gainDb == 25 && firstEdit.qpX512 == 282 && firstEdit.bypass == 1);
  CHECK(secondEdit.gainDb == 35 && secondEdit.qpX512 == 461 && secondEdit.bypass == 0 && secondEdit.q == 0.9f);
}

// Catches whole-route overwrites, selector reversal, implicit sum changes, and
// clamping an imported gain while editing only enable/invert.
static void routes() {
  Route fresh{7, 8, true, true, 6}, changed{};
  RoutePacket packet{};
  CHECK(editRoute(fresh, RouteField::Enabled, 0, changed) == Result::Ok);
  CHECK(!changed.enabled && changed.invert && changed.gainDb == 6);
  CHECK(editRoute(fresh, RouteField::Gain, 0, changed) == Result::Ok && changed.invert);
  CHECK(editRoute(fresh, RouteField::Gain, 0.1f, changed) == Result::OutOfRange);
  CHECK(editRoute(fresh, RouteField::Gain, -60, changed) == Result::Ok);
  CHECK(editRoute(fresh, RouteField::Gain, -60.1f, changed) == Result::OutOfRange);
  CHECK(packRoute(current(), fresh, packet) == Result::Ok);
  const uint8_t expected[] = {7,8,1,1,0,0,192,64};
  CHECK(std::memcmp(packet.bytes, expected, 8) == 0);
  uint16_t selector = 0;
  CHECK(routeSelector(current(), 7, 8, selector) && selector == 0x0708);
  CHECK(!routeSelector(current(), 8, 8, selector));
  Route sum[3] = {{0,1,false,true,-3},{1,1,true,false,-9},{2,1,false,true,-12}}, out[3]{};
  CHECK(planSum(sum, 3, 3, SumMode::Average, false, out) == Result::NeedsConfirmation);
  CHECK(planSum(sum, 3, 3, SumMode::Average, true, out) == Result::Ok);
  CHECK(out[0].enabled && out[1].enabled && near(out[0].gainDb, -6.0206f) && near(out[1].gainDb, -6.0206f));
  CHECK(out[0].invert && !out[1].invert && out[2].gainDb == -12 && !out[2].enabled);
  CHECK(planSum(sum, 3, 3, SumMode::Unattenuated, true, out) == Result::Ok && out[0].gainDb == 0 && out[1].gainDb == 0);
  CHECK(planSum(sum, 3, 1, SumMode::Average, true, out) == Result::InvalidTarget);
}

// Catches repeated trim accumulating, balance boosting the louder side,
// external/preset edits being overwritten, and partial results called success.
static void speakerAdjustments() {
  float fresh[9] = {-4,-6,-12,-18,0,0,0,0,-9}, planned[9]{};
  GainBaseline baseline{};
  CHECK(captureBaseline(fresh, 9, 0x000c, 41, baseline) == Result::Ok);
  CHECK(planRelativeTrim(baseline, fresh, 9, 41, 3, planned) == Result::Ok && planned[2] == -9 && planned[3] == -15 && planned[8] == -9);
  float confirmed[9]; std::memcpy(confirmed, planned, sizeof(planned));
  CHECK(confirmGains(baseline, planned, confirmed, 9, 41) == Result::Ok);
  CHECK(planRelativeTrim(baseline, confirmed, 9, 41, 2, planned) == Result::Ok && planned[2] == -10 && planned[3] == -16);
  CHECK(planRelativeTrim(baseline, confirmed, 9, 42, 2, planned) == Result::Stale);
  confirmed[2] = -8;
  CHECK(planRelativeTrim(baseline, confirmed, 9, 41, 2, planned) == Result::Stale);
  CHECK(captureBaseline(fresh, 9, 3, 50, baseline) == Result::Ok);
  CHECK(planBalance(baseline, fresh, 9, 50, 1, 2, 8, planned) == Result::Ok && planned[0] == -12 && planned[1] == -6);
  CHECK(planBalance(baseline, fresh, 9, 50, 1, 2, -8, planned) == Result::Ok && planned[0] == -4 && planned[1] == -14);
  CHECK(planBalance(baseline, fresh, 9, 50, 1, 1, 8, planned) == Result::InvalidTarget);
  CHECK(planBalance(baseline, fresh, 9, 50, 1, 2, 25, planned) == Result::OutOfRange);
  CHECK(planSubGain(baseline, fresh, 9, 50, -15, planned) == Result::Ok && planned[0] == -19 && planned[1] == -21);
  CHECK(captureBaseline(fresh, 9, 0x0100, 50, baseline) == Result::Ok);
  CHECK(planSubGain(baseline, fresh, 9, 50, -15, planned) == Result::Ok && planned[8] == -15);
  CHECK(planSubGain(baseline, fresh, 9, 50, 1, planned) == Result::OutOfRange);
  CHECK(captureBaseline(fresh, 9, 0, 50, baseline) == Result::InvalidTarget);
  CHECK(captureBaseline(fresh, 9, 0x0200, 50, baseline) == Result::InvalidTarget);
  CHECK(captureBaseline(fresh, 9, 3, 50, baseline) == Result::Ok);
  CHECK(planRelativeTrim(baseline, fresh, 9, 50, 0, planned) == Result::Ok);
  std::memcpy(confirmed, planned, sizeof(planned)); confirmed[1] = -7;
  CHECK(confirmGains(baseline, planned, confirmed, 9, 50) == Result::Partial && !baseline.valid);
  Completion both[] = {Completion::Confirmed, Completion::Pending};
  CHECK(linkedCompletion(both, 2) == Completion::Pending);
  both[1] = Completion::Failed; CHECK(linkedCompletion(both, 2) == Completion::Partial);
  both[1] = Completion::Unknown; CHECK(linkedCompletion(both, 2) == Completion::Unknown);
  both[1] = Completion::Confirmed; CHECK(linkedCompletion(both, 2) == Completion::Confirmed);
  both[1] = static_cast<Completion>(255); CHECK(linkedCompletion(both, 2) == Completion::Unknown);
}

// Catches ESP pin confusion, adjacent clock-pair overlaps, reset escapes being
// treated as pins, live-UART loss and PDM/Core1 mutually-exclusive enabling.
static void hardware() {
  PinRules rules{Platform::Rp2350, true, uint32_t(1) << 5, (uint32_t(1)<<16)|(uint32_t(1)<<17), (uint32_t(1)<<14)|(uint32_t(1)<<15), false, false};
  CHECK(checkPin(rules, PinKind::OutputData, 0, 6) == Result::Ok);
  CHECK(checkPin(rules, PinKind::OutputData, 29, 6) == Result::Ok);
  CHECK(checkPin(rules, PinKind::OutputData, 30, 6) == Result::OutOfRange);
  CHECK(checkPin(rules, PinKind::OutputData, 23, 6) == Result::OutOfRange);
  CHECK(checkPin(rules, PinKind::OutputData, 255, 6) == Result::OutOfRange);
  CHECK(checkPin(rules, PinKind::OutputData, 5, 6) == Result::Conflict);
  CHECK(checkPin(rules, PinKind::OutputData, 16, 16) == Result::ProtectedLink);
  CHECK(checkPin(rules, PinKind::OutputData, 14, 6) == Result::Conflict);
  CHECK(checkPin(rules, PinKind::ClockPair, 22, 14) == Result::OutOfRange);
  CHECK(checkPin(rules, PinKind::ClockPair, 13, 26) == Result::Conflict);
  CHECK(checkPin(rules, PinKind::ClockPair, 15, 26) == Result::ProtectedLink);
  CHECK(checkPin(rules, PinKind::MasterClock, 12, 13) == Result::OutOfRange);
  CHECK(checkPin(rules, PinKind::MasterClock, 21, 13) == Result::Ok);
  rules.targetActive = true;
  CHECK(checkPin(rules, PinKind::PdmData, 11, 10) == Result::Active);
  CHECK(checkPin(rules, PinKind::I2sRxData, 11, 20) == Result::Active);
  CHECK(checkPin(rules, PinKind::ClockPair, 26, 14) == Result::Active);
  CHECK(checkPin(rules, PinKind::MasterClock, 13, 13) == Result::Active);
  rules.targetActive = false; rules.snapshotComplete = false;
  CHECK(checkPin(rules, PinKind::OutputData, 11, 6) == Result::Unavailable);
  rules.snapshotComplete = true; rules.platform = Platform::Rp2040;
  CHECK(checkPin(rules, PinKind::OutputData, 29, 6) == Result::OutOfRange);
  CHECK(checkPin(rules, PinKind::MasterClock, 13, 21) == Result::OutOfRange);
  CHECK(checkPin(rules, static_cast<PinKind>(255), 11, 6) == Result::Unsupported);
  rules.platform = static_cast<Platform>(255);
  CHECK(checkPin(rules, PinKind::OutputData, 11, 6) == Result::Unavailable);
  CHECK(checkPairFormat(current(), 3, 1, false) == Result::Conflict);
  CHECK(checkPairFormat(current(), 3, 1, true) == Result::Ok);
  CHECK(checkPairFormat(current(), 4, 1, true) == Result::InvalidTarget);
  CHECK(checkPairFormat(current(), 3, 2, true) == Result::OutOfRange);
  CHECK(checkOutputEnable(current(), 8, uint16_t(1)<<2) == Result::Conflict);
  CHECK(checkOutputEnable(current(), 2, uint16_t(1)<<8) == Result::Conflict);
  CHECK(checkOutputEnable(current(), 0, uint16_t(1)<<8) == Result::Ok);
  CHECK(checkOutputEnable(current(), 8, 3) == Result::Ok);
}

static uint16_t monospace(const char *s, void *) { return static_cast<uint16_t>(std::strlen(s)*8); }
// Catches unterminated/unbounded names, raw control characters on screen,
// byte-count Home truncation instead of measured width, and an empty-slot load.
static void namesAndPresets() {
  char out[32]{};
  const uint8_t name[] = {'L','o','n','g','\n','N','a','m','e',0,'X'};
  CHECK(displayName(name, sizeof(name), out, sizeof(out), 31) && std::strcmp(out, "Long Name") == 0);
  uint8_t longName[40]; std::memset(longName, 'x', sizeof(longName));
  CHECK(displayName(longName, 40, out, sizeof(out), 31) && std::strlen(out) == 31);
  char small[4] = {};
  CHECK(displayName(longName, 40, small, sizeof(small), 31) && std::strcmp(small, "xxx") == 0);
  CHECK(!displayName(name, sizeof(name), out, 0, 31));
  CHECK(fitName("Long Speaker Name", 17, out, sizeof(out), 64, monospace, nullptr) && std::strcmp(out, "Long ...") == 0);
  CHECK(fitName("Hi", 2, out, sizeof(out), 16, monospace, nullptr) && std::strcmp(out, "Hi") == 0);
  CHECK(fitName("Long Speaker Name", 17, out, sizeof(out), 8, monospace, nullptr) && std::strcmp(out, ".") == 0);
  CHECK(validStoredName("Room", 4));
  CHECK(!validStoredName("bad\n", 4));
  CHECK(!validStoredName("", 0));
  CHECK(!validStoredName(reinterpret_cast<const char *>(longName), 32));
  PresetDirectory dir{0x0003, 9, true}; PresetPlan plan{};
  CHECK(planPreset(PresetAction::SaveAsNew, 2, 0, dir, 9, true, plan) == Result::Ok);
  CHECK(plan.request == 0x90 && plan.slot == 2 && plan.capturesLive && !plan.loadsDestination);
  CHECK(planPreset(PresetAction::SaveAsNew, 1, 0, dir, 9, true, plan) == Result::Occupied);
  CHECK(planPreset(PresetAction::Load, 2, 0, dir, 9, true, plan) == Result::Empty);
  CHECK(planPreset(PresetAction::Load, 1, 0, dir, 9, true, plan) == Result::Ok && plan.request == 0x91 && plan.loadsDestination);
  CHECK(planPreset(PresetAction::SaveChanges, 1, 0, dir, 9, true, plan) == Result::InvalidTarget);
  CHECK(planPreset(PresetAction::SaveChanges, 0, 0, dir, 9, false, plan) == Result::NeedsConfirmation);
  CHECK(planPreset(PresetAction::Revert, 0, 0, dir, 9, true, plan) == Result::Ok && plan.request == 0x91);
  CHECK(planPreset(PresetAction::Rename, 0, 0, dir, 9, true, plan) == Result::Ok && plan.request == 0x94 && !plan.loadsDestination);
  CHECK(planPreset(PresetAction::SaveAsNew, 10, 0, dir, 9, true, plan) == Result::InvalidTarget);
  CHECK(planPreset(PresetAction::SaveAsNew, 2, 0, dir, 8, true, plan) == Result::Stale);
  dir.fresh = false;
  CHECK(planPreset(PresetAction::SaveAsNew, 2, 0, dir, 9, true, plan) == Result::Unavailable);
}

int main() {
  mapping(); recipes(); crossoversAndPackets(); routes(); speakerAdjustments(); hardware(); namesAndPresets();
  std::printf("PanelControls: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
