#ifndef DSPI_PANEL_CONTROLS_H
#define DSPI_PANEL_CONTROLS_H

// Portable control policy, not a transport or DSP implementation. All inputs
// used for read/modify/write must come from confirmed, fresh scalar readback.
// Nothing here enables an output, assigns speaker roles, polls, allocates,
// persists settings, or assumes that a linked multi-target edit is atomic.
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

namespace PanelControls {

enum class Result : uint8_t {
  Ok, Unsupported, InvalidTarget, InvalidField, OutOfRange, NonFinite,
  Stale, Conflict, Active, ProtectedLink, Unavailable, NeedsConfirmation,
  Partial, Empty, Occupied
};
enum class Platform : uint8_t { Unknown, Rp2040, Rp2350 };
enum class Profile : uint8_t {
  Unknown, Rp2040Current, Rp2350Current,
  LegacyRp2040StereoReadOnly, LegacyRp2350StereoReadOnly
};
struct CapabilityEvidence {
  Platform platform;
  uint8_t exposedInputs, exposedOutputs;
  bool countsConfirmed;        // Capacity, not current active USB/I2S channels.
  bool fiveBitEqConfirmed;     // Positive read-only evidence, never a SET probe.
};
struct ChannelMap {
  uint8_t inputs, outputs, stereoPairs;
  bool writable;
};

// GET_PLATFORM's output-count/version bytes alone are insufficient: old
// RP2350 firmware also reports nine outputs but uses a two-input EQ offset.
// Integration must leave this Unknown until independent capacity AND scalar
// encoding evidence is confirmed. Never synthesize exposedInputs from platform.
inline Profile selectProfile(const CapabilityEvidence &e) {
  if (!e.countsConfirmed || !e.fiveBitEqConfirmed) return Profile::Unknown;
  if (e.platform == Platform::Rp2350 && e.exposedInputs == 8 && e.exposedOutputs == 9)
    return Profile::Rp2350Current;
  if (e.platform == Platform::Rp2040 && e.exposedInputs == 2 && e.exposedOutputs == 5)
    return Profile::Rp2040Current;
  return Profile::Unknown;
}
inline ChannelMap channelMap(Profile profile) {
  switch (profile) {
    case Profile::Rp2040Current: return {2, 5, 2, true};
    case Profile::Rp2350Current: return {8, 9, 4, true};
    case Profile::LegacyRp2040StereoReadOnly: return {2, 5, 2, false};
    case Profile::LegacyRp2350StereoReadOnly: return {2, 9, 4, false};
    default: return {0, 0, 0, false};
  }
}
inline bool knownMap(const ChannelMap &m) {
  return (m.inputs == 8 && m.outputs == 9 && m.stereoPairs == 4) ||
         (m.inputs == 2 && m.outputs == 5 && m.stereoPairs == 2) ||
         (m.inputs == 2 && m.outputs == 9 && m.stereoPairs == 4 && !m.writable);
}
inline bool inputEqId(const ChannelMap &m, uint8_t input, uint8_t &id) {
  if (!knownMap(m) || input >= m.inputs) return false;
  id = input; return true;
}
inline bool outputEqId(const ChannelMap &m, uint8_t output, uint8_t &id) {
  if (!knownMap(m) || output >= m.outputs) return false;
  id = static_cast<uint8_t>(m.inputs + output); return true;
}
inline bool outputLocalId(const ChannelMap &m, uint8_t eqId, uint8_t &id) {
  if (!knownMap(m) || eqId < m.inputs || eqId >= m.inputs + m.outputs) return false;
  id = static_cast<uint8_t>(eqId - m.inputs); return true;
}
inline bool outputPinSlot(const ChannelMap &m, uint8_t output, uint8_t &slot) {
  if (!knownMap(m) || output >= m.outputs) return false;
  slot = output == m.outputs - 1 ? m.stereoPairs : static_cast<uint8_t>(output / 2);
  return true;
}
inline bool bandTarget(const ChannelMap &m, uint8_t eqId, uint8_t band) {
  if (!knownMap(m) || eqId >= m.inputs + m.outputs) return false;
  return band < 10 || (m.writable && eqId >= m.inputs && band >= 20 && band < 24);
}
enum class ScalarField : uint8_t { Type, Frequency, Q, Gain, Bypass, TargetQ };
inline bool eqSelector(const ChannelMap &m, uint8_t eqId, uint8_t band,
                       ScalarField field, uint16_t &selector) {
  // Even GET selector packing is not guessed for legacy firmware.
  if (!m.writable || !bandTarget(m, eqId, band) || static_cast<uint8_t>(field) > 5)
    return false;
  selector = static_cast<uint16_t>((uint16_t(eqId) << 8) | (uint16_t(band) << 3) |
                                  static_cast<uint8_t>(field));
  return true;
}

enum FilterField : uint8_t {
  FieldFrequency = 1, FieldQ = 2, FieldGain = 4,
  FieldTargetFrequency = 8, FieldTargetQ = 16
};
enum class BandField : uint8_t { Type, Frequency, Q, GainDb, Bypass, TargetFrequency, TargetQ };
struct BandRecipe {
  uint8_t type, bypass;
  float frequencyHz, q, gainDb; // LT: f0, Q0, fp in Hz (NOT dB).
  uint16_t qpX512;             // LT target Q; zero selects DSP default 0.707.
};
inline bool finite(float value) { return isfinite(value) != 0; }
inline bool peqType(uint8_t type) { return type <= 13; }
inline bool xoType(uint8_t type) { return type >= 32 && type <= 63; }
inline bool supportedType(uint8_t type) { return peqType(type) || xoType(type); }
inline uint8_t filterFields(uint8_t type) {
  switch (type) {
    case 1: case 2: case 3: return FieldFrequency | FieldQ | FieldGain;
    case 4: case 5: case 6: case 7: return FieldFrequency | FieldQ;
    case 8: case 12: case 13: return FieldFrequency;
    case 9: case 10: return FieldFrequency | FieldGain;
    case 11: return FieldFrequency | FieldQ | FieldTargetFrequency | FieldTargetQ;
    default: return xoType(type) ? FieldFrequency : 0;
  }
}
inline float decodeTargetQ(uint16_t encoded) {
  return encoded ? static_cast<float>(encoded) / 512.0f : 0.707f;
}
inline Result frequencyRange(float value, float sampleRate, bool lt) {
  if (!finite(value) || !finite(sampleRate)) return Result::NonFinite;
  if (sampleRate <= 0 || value < 10 || value > sampleRate * (lt ? 0.15f : 0.45f))
    return Result::OutOfRange;
  return Result::Ok;
}
inline Result qRange(float value) {
  if (!finite(value)) return Result::NonFinite;
  return value >= 0.1f && value <= 20 ? Result::Ok : Result::OutOfRange;
}
// Native bounds apply to active fields. Imported gain outside the panel's
// edit range is retained, not silently clamped when another field changes.
inline Result validateRecipe(const BandRecipe &r, float sampleRate) {
  if (!finite(r.frequencyHz) || !finite(r.q) || !finite(r.gainDb) || !finite(sampleRate))
    return Result::NonFinite;
  if (!supportedType(r.type)) return Result::Unsupported;
  if (sampleRate <= 0) return Result::OutOfRange;
  const uint8_t fields = filterFields(r.type);
  if (fields & FieldFrequency) {
    Result result = frequencyRange(r.frequencyHz, sampleRate, r.type == 11);
    if (result != Result::Ok) return result;
  }
  if (fields & FieldQ) {
    Result result = qRange(r.q); if (result != Result::Ok) return result;
  }
  if (r.type == 11) {
    Result result = frequencyRange(r.gainDb, sampleRate, true);
    if (result != Result::Ok) return result;
    // round(0.1 * 512) is 51, whose decoded value is 0.099609375;
    // the DSP clamps that quantized boundary to 0.1. Accept its native
    // representation rather than making the documented lower bound unusable.
    return r.qpX512 == 0 || (r.qpX512 >= 51 && r.qpX512 <= 10240)
             ? Result::Ok : Result::OutOfRange;
  }
  return Result::Ok;
}
inline Result makeRecipe(uint8_t type, float frequencyHz, float sampleRate, BandRecipe &out) {
  if (!supportedType(type)) return Result::Unsupported;
  BandRecipe recipe{type, 0, frequencyHz, 0.707f, type == 11 ? frequencyHz : 0.0f,
                    static_cast<uint16_t>(type == 11 ? 362 : 0)};
  Result result = validateRecipe(recipe, sampleRate);
  if (result == Result::Ok) out = recipe;
  return result;
}
inline Result editBand(const BandRecipe &fresh, BandField field, float value,
                       float sampleRate, BandRecipe &out) {
  if (!finite(value)) return Result::NonFinite;
  if (!supportedType(fresh.type)) return Result::Unsupported;
  BandRecipe next = fresh;
  const uint8_t fields = filterFields(fresh.type);
  switch (field) {
    case BandField::Type: {
      if (value < 0 || value > 63 || value != floorf(value)) return Result::OutOfRange;
      const uint8_t type = static_cast<uint8_t>(value);
      if (!supportedType(type)) return Result::Unsupported;
      if (type != fresh.type) {
        // Never reinterpret a dB value as an LT target frequency, or an LT
        // target frequency as gain. The sidecar remains untouched.
        if (type == 11) next.gainDb = fresh.frequencyHz;
        else if ((filterFields(type) & FieldGain) && !(fields & FieldGain)) next.gainDb = 0;
        next.type = type;
      }
      break;
    }
    case BandField::Bypass:
      if (value != 0 && value != 1) return Result::OutOfRange;
      next.bypass = static_cast<uint8_t>(value); break;
    case BandField::Frequency:
      if (!(fields & FieldFrequency)) return Result::InvalidField;
      next.frequencyHz = value; break;
    case BandField::Q:
      if (!(fields & FieldQ)) return Result::InvalidField;
      next.q = value; break;
    case BandField::GainDb:
      if (!(fields & FieldGain)) return Result::InvalidField;
      if (value < -24 || value > 12) return Result::OutOfRange;
      next.gainDb = value; break;
    case BandField::TargetFrequency:
      if (!(fields & FieldTargetFrequency)) return Result::InvalidField;
      next.gainDb = value; break;
    case BandField::TargetQ: {
      if (!(fields & FieldTargetQ)) return Result::InvalidField;
      Result result = qRange(value); if (result != Result::Ok) return result;
      next.qpX512 = static_cast<uint16_t>(floorf(value * 512.0f + 0.5f)); break;
    }
    default: return Result::InvalidField;
  }
  Result result = validateRecipe(next, sampleRate);
  if (result == Result::Ok) out = next;
  return result;
}
inline float linkwitzBoostDb(const BandRecipe &r) {
  if (r.type != 11 || !finite(r.frequencyHz) || !finite(r.gainDb) ||
      r.frequencyHz <= 0 || r.gainDb <= 0) return NAN;
  return 40.0f * log10f(r.frequencyHz / r.gainDb);
}

enum class XoFamily : uint8_t { LinkwitzRiley, Butterworth, Bessel };
inline bool crossoverType(XoFamily family, uint8_t slopeDbOct, bool highPass, uint8_t &type) {
  if (slopeDbOct < 6 || slopeDbOct > 48 || slopeDbOct % 6) return false;
  const uint8_t order = slopeDbOct / 6;
  uint8_t base = 0;
  switch (family) {
    case XoFamily::LinkwitzRiley:
      if (order % 2) return false;
      base = static_cast<uint8_t>(32 + (order / 2 - 1) * 2); break;
    case XoFamily::Butterworth:
      base = static_cast<uint8_t>(40 + (order - 1) * 2); break;
    case XoFamily::Bessel:
      if (order % 2) return false;
      base = static_cast<uint8_t>(56 + (order / 2 - 1) * 2); break;
    default: return false;
  }
  type = static_cast<uint8_t>(base + (highPass ? 1 : 0)); return true;
}
inline bool decodeCrossover(uint8_t type, XoFamily &family, uint8_t &order, bool &highPass) {
  if (!xoType(type)) return false;
  highPass = (type & 1) != 0;
  if (type < 40) { family = XoFamily::LinkwitzRiley; order = static_cast<uint8_t>(2 + (type - 32) / 2 * 2); }
  else if (type < 56) { family = XoFamily::Butterworth; order = static_cast<uint8_t>(1 + (type - 40) / 2); }
  else { family = XoFamily::Bessel; order = static_cast<uint8_t>(2 + (type - 56) / 2 * 2); }
  return true;
}

inline void putFloatLE(uint8_t *out, float value) {
  static_assert(sizeof(float) == 4, "DSP protocol requires float32");
  uint32_t bits = 0; memcpy(&bits, &value, 4);
  for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(bits >> (8 * i));
}
struct EqPacket { uint8_t bytes[18]; uint8_t size; };
inline Result packEq(const ChannelMap &m, uint8_t eqId, uint8_t band,
                     const BandRecipe &freshEdited, float sampleRate, EqPacket &out) {
  if (!m.writable) return Result::Unsupported;
  if (!bandTarget(m, eqId, band)) return Result::InvalidTarget;
  if ((band < 10 && !peqType(freshEdited.type)) ||
      (band >= 20 && freshEdited.type != 0 && !xoType(freshEdited.type))) return Result::Unsupported;
  Result result = validateRecipe(freshEdited, sampleRate);
  if (result != Result::Ok) return result;
  EqPacket packet{};
  packet.bytes[0] = eqId; packet.bytes[1] = band; packet.bytes[2] = freshEdited.type;
  packet.bytes[3] = freshEdited.bypass == 1 ? 1 : 0;
  putFloatLE(packet.bytes + 4, freshEdited.frequencyHz);
  putFloatLE(packet.bytes + 8, freshEdited.q);
  putFloatLE(packet.bytes + 12, freshEdited.gainDb);
  packet.size = freshEdited.type == 11 ? 18 : 16;
  packet.bytes[16] = static_cast<uint8_t>(freshEdited.qpX512);
  packet.bytes[17] = static_cast<uint8_t>(freshEdited.qpX512 >> 8);
  out = packet; return Result::Ok;
}

struct Route { uint8_t input, output; bool enabled, invert; float gainDb; };
enum class RouteField : uint8_t { Enabled, Invert, Gain };
struct RoutePacket { uint8_t bytes[8]; };
inline Result editRoute(const Route &fresh, RouteField field, float value, Route &out) {
  if (!finite(value) || !finite(fresh.gainDb)) return Result::NonFinite;
  Route next = fresh;
  switch (field) {
    case RouteField::Enabled:
      if (value != 0 && value != 1) return Result::OutOfRange;
      next.enabled = value != 0; break;
    case RouteField::Invert:
      if (value != 0 && value != 1) return Result::OutOfRange;
      next.invert = value != 0; break;
    case RouteField::Gain:
      if (value < -60 || value > 0) return Result::OutOfRange;
      next.gainDb = value; break;
    default: return Result::InvalidField;
  }
  out = next; return Result::Ok;
}
inline bool routeSelector(const ChannelMap &m, uint8_t input, uint8_t output, uint16_t &selector) {
  if (!m.writable || !knownMap(m) || input >= m.inputs || output >= m.outputs) return false;
  selector = static_cast<uint16_t>((uint16_t(input) << 8) | output); return true;
}
inline Result packRoute(const ChannelMap &m, const Route &freshEdited, RoutePacket &out) {
  uint16_t selector = 0;
  if (!m.writable) return Result::Unsupported;
  if (!routeSelector(m, freshEdited.input, freshEdited.output, selector)) return Result::InvalidTarget;
  if (!finite(freshEdited.gainDb)) return Result::NonFinite;
  RoutePacket packet{};
  packet.bytes[0] = freshEdited.input; packet.bytes[1] = freshEdited.output;
  packet.bytes[2] = freshEdited.enabled ? 1 : 0; packet.bytes[3] = freshEdited.invert ? 1 : 0;
  putFloatLE(packet.bytes + 4, freshEdited.gainDb);
  out = packet; return Result::Ok;
}
enum class SumMode : uint8_t { Average, Unattenuated };
inline unsigned bitCount(uint16_t mask) {
  unsigned count = 0;
  while (mask) { count += mask & 1; mask >>= 1; }
  return count;
}
// selectedRoutes indexes this bounded route array, not the input IDs. No route
// outside that explicit selection and no output enable is changed. Both modes
// require preview/confirmation; Unattenuated needs a headroom warning in the UI.
inline Result planSum(const Route *fresh, size_t count, uint16_t selectedRoutes,
                      SumMode mode, bool confirmed, Route *out) {
  if (!fresh || !out || count < 2 || count > 8 ||
      (selectedRoutes >> count) || bitCount(selectedRoutes) < 2) return Result::InvalidTarget;
  if (mode != SumMode::Average && mode != SumMode::Unattenuated) return Result::Unsupported;
  if (!confirmed) return Result::NeedsConfirmation;
  uint16_t seenInputs = 0;
  uint8_t output = 0;
  bool haveOutput = false;
  for (size_t i = 0; i < count; ++i) {
    if (!finite(fresh[i].gainDb)) return Result::NonFinite;
    if (!(selectedRoutes & (uint16_t(1) << i))) continue;
    if (fresh[i].input >= 8 || fresh[i].output >= 9 ||
        (seenInputs & (uint16_t(1) << fresh[i].input))) return Result::InvalidTarget;
    seenInputs |= uint16_t(1) << fresh[i].input;
    if (haveOutput && output != fresh[i].output) return Result::InvalidTarget;
    output = fresh[i].output;
    haveOutput = true;
  }
  const float gain = mode == SumMode::Average ? -20.0f * log10f(static_cast<float>(bitCount(selectedRoutes))) : 0;
  for (size_t i = 0; i < count; ++i) {
    out[i] = fresh[i];
    if (selectedRoutes & (uint16_t(1) << i)) { out[i].enabled = true; out[i].gainDb = gain; }
  }
  return Result::Ok;
}

struct GainBaseline {
  float original[9], expected[9];
  uint16_t mask;
  uint32_t generation;
  uint8_t count;
  bool valid;
};
inline Result captureBaseline(const float *fresh, size_t count, uint16_t explicitRoleMask,
                              uint32_t generation, GainBaseline &out) {
  if (!fresh || count == 0 || count > 9 || !explicitRoleMask || (explicitRoleMask >> count))
    return Result::InvalidTarget;
  GainBaseline next{}; next.mask = explicitRoleMask; next.generation = generation;
  next.count = static_cast<uint8_t>(count); next.valid = true;
  for (size_t i = 0; i < count; ++i) {
    if (!finite(fresh[i])) return Result::NonFinite;
    next.original[i] = next.expected[i] = fresh[i];
  }
  out = next; return Result::Ok;
}
inline Result checkBaseline(const GainBaseline &baseline, const float *fresh,
                            size_t count, uint32_t generation) {
  if (!baseline.valid || baseline.generation != generation || baseline.count != count) return Result::Stale;
  if (!fresh || count == 0 || count > 9) return Result::InvalidTarget;
  for (size_t i = 0; i < count; ++i) {
    if (!finite(fresh[i])) return Result::NonFinite;
    if ((baseline.mask & (uint16_t(1) << i)) && fabsf(fresh[i] - baseline.expected[i]) > 0.001f)
      return Result::Stale;
  }
  return Result::Ok;
}
inline Result outputGainRange(float value) {
  if (!finite(value)) return Result::NonFinite;
  return value >= -60 && value <= 0 ? Result::Ok : Result::OutOfRange;
}
inline Result planRelativeTrim(const GainBaseline &baseline, const float *fresh,
                               size_t count, uint32_t generation, float trimDb, float *out) {
  Result result = checkBaseline(baseline, fresh, count, generation);
  if (result != Result::Ok) return result;
  if (!out) return Result::InvalidTarget;
  if (!finite(trimDb)) return Result::NonFinite;
  for (size_t i = 0; i < count; ++i) {
    if (baseline.mask & (uint16_t(1) << i)) {
      result = outputGainRange(baseline.original[i] + trimDb); if (result != Result::Ok) return result;
    }
  }
  for (size_t i = 0; i < count; ++i)
    out[i] = baseline.mask & (uint16_t(1) << i) ? baseline.original[i] + trimDb : fresh[i];
  return Result::Ok;
}
// Positive balance attenuates left, negative attenuates right. Stored
// calibration remains the baseline: this is never a boost or gain equalizer.
inline Result planBalance(const GainBaseline &baseline, const float *fresh, size_t count,
                          uint32_t generation, uint16_t leftMask, uint16_t rightMask,
                          float balanceDb, float *out) {
  Result result = checkBaseline(baseline, fresh, count, generation);
  if (result != Result::Ok) return result;
  if (!out || !leftMask || !rightMask || (leftMask & rightMask) ||
      (leftMask | rightMask) != baseline.mask) return Result::InvalidTarget;
  if (!finite(balanceDb)) return Result::NonFinite;
  if (balanceDb < -24 || balanceDb > 24) return Result::OutOfRange;
  float planned[9];
  for (size_t i = 0; i < count; ++i) {
    const uint16_t bit = uint16_t(1) << i;
    planned[i] = fresh[i];
    if (!(baseline.mask & bit)) continue;
    planned[i] = baseline.original[i];
    if ((leftMask & bit) && balanceDb > 0) planned[i] -= balanceDb;
    if ((rightMask & bit) && balanceDb < 0) planned[i] += balanceDb;
    result = outputGainRange(planned[i]); if (result != Result::Ok) return result;
  }
  memcpy(out, planned, count * sizeof(float)); return Result::Ok;
}
inline Result planSubGain(const GainBaseline &baseline, const float *fresh, size_t count,
                          uint32_t generation, float valueDb, float *out) {
  if (bitCount(baseline.mask) != 1) return planRelativeTrim(baseline, fresh, count, generation, valueDb, out);
  Result result = checkBaseline(baseline, fresh, count, generation);
  if (result != Result::Ok) return result;
  if (!out) return Result::InvalidTarget;
  result = outputGainRange(valueDb); if (result != Result::Ok) return result;
  for (size_t i = 0; i < count; ++i)
    out[i] = baseline.mask & (uint16_t(1) << i) ? valueDb : fresh[i];
  return Result::Ok;
}
// Call only after EVERY targeted write has corresponding scalar readback.
// Any mismatched/missing target invalidates the baseline; never roll back
// implicitly or retry an ambiguous write using an old recipe.
inline Result confirmGains(GainBaseline &baseline, const float *planned, const float *readback,
                           size_t count, uint32_t generation) {
  if (!baseline.valid || count != baseline.count || generation != baseline.generation) {
    baseline.valid = false; return Result::Stale;
  }
  if (!planned || !readback || count == 0 || count > 9) {
    baseline.valid = false; return Result::InvalidTarget;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!(baseline.mask & (uint16_t(1) << i))) continue;
    if (!finite(planned[i]) || !finite(readback[i]) || fabsf(planned[i] - readback[i]) > 0.001f) {
      baseline.valid = false; return Result::Partial;
    }
  }
  for (size_t i = 0; i < count; ++i)
    if (baseline.mask & (uint16_t(1) << i)) baseline.expected[i] = readback[i];
  return Result::Ok;
}
enum class Completion : uint8_t { Pending, Unknown, Failed, Confirmed, Partial };
inline Completion linkedCompletion(const Completion *targets, size_t count) {
  if (!targets || count == 0 || count > 9) return Completion::Unknown;
  bool confirmed = false, failed = false, pending = false;
  for (size_t i = 0; i < count; ++i) {
    if (static_cast<uint8_t>(targets[i]) > static_cast<uint8_t>(Completion::Partial)) return Completion::Unknown;
    if (targets[i] == Completion::Unknown) return Completion::Unknown;
    if (targets[i] == Completion::Partial) return Completion::Partial;
    confirmed |= targets[i] == Completion::Confirmed;
    failed |= targets[i] == Completion::Failed;
    pending |= targets[i] == Completion::Pending;
  }
  if (failed) return confirmed ? Completion::Partial : Completion::Failed;
  return pending ? Completion::Pending : Completion::Confirmed;
}

enum class PinKind : uint8_t { OutputData, PdmData, I2sRxData, SpdifRxData, ClockPair, MasterClock, AdatData };
struct PinRules {
  Platform platform;
  bool snapshotComplete;
  uint32_t otherClaimedPins; // ALL other owners; exclude only this edited owner.
  uint32_t liveUartPins;     // Separate, never excluded, even for a no-op.
  uint32_t clockPins;       // Both pairs in split mode; reserve conservatively.
  bool targetActive;        // Source/clock/PDM in use, incl pending transitions.
  bool transitionPending;
};
inline bool gpioValid(Platform platform, uint8_t pin) {
  if ((platform != Platform::Rp2040 && platform != Platform::Rp2350) || (pin >= 23 && pin <= 25)) return false;
  return pin <= (platform == Platform::Rp2350 ? 29 : 28);
}
// All GPIOs here belong to DSPi, never the ESP32. 0xFF is deliberately rejected:
// resolve a reset-to-default through verified platform data, then revalidate.
// Include stored output pins even when disabled, optional RX only when claimed,
// all configured I2S RX pair pins for RX edits, DAC mute, MCK, ADAT, I2C and
// control-surface owners. An incomplete ownership snapshot disables the editor.
inline Result checkPin(const PinRules &rules, PinKind kind, uint8_t pin, uint8_t currentPin) {
  if (!rules.snapshotComplete || (rules.platform != Platform::Rp2040 && rules.platform != Platform::Rp2350)) return Result::Unavailable;
  if (static_cast<uint8_t>(kind) > static_cast<uint8_t>(PinKind::AdatData)) return Result::Unsupported;
  if (!gpioValid(rules.platform, pin)) return Result::OutOfRange;
  uint32_t pins = uint32_t(1) << pin;
  if (kind == PinKind::ClockPair) {
    if (!gpioValid(rules.platform, static_cast<uint8_t>(pin + 1))) return Result::OutOfRange;
    pins |= uint32_t(1) << (pin + 1);
  }
  if (kind == PinKind::MasterClock && pin != 21 &&
      !(rules.platform == Platform::Rp2350 && (pin == 13 || pin == 15))) return Result::OutOfRange;
  if (pins & rules.liveUartPins) return Result::ProtectedLink;
  if (pins & rules.otherClaimedPins) return Result::Conflict;
  if ((kind != PinKind::ClockPair || pin != currentPin) && (pins & rules.clockPins)) return Result::Conflict;
  if (rules.transitionPending) return Result::Active;
  if (kind == PinKind::MasterClock && rules.targetActive) return Result::Active;
  if (pin != currentPin && rules.targetActive) return Result::Active;
  return Result::Ok;
}
inline Result checkPairFormat(const ChannelMap &m, uint8_t pair, uint8_t format, bool clocksConflictFree) {
  if (!m.writable || !knownMap(m)) return Result::Unsupported;
  if (pair >= m.stereoPairs) return Result::InvalidTarget; // PDM has no format.
  if (format > 1) return Result::OutOfRange;
  if (format == 1 && !clocksConflictFree) return Result::Conflict;
  return Result::Ok;
}
inline Result checkOutputEnable(const ChannelMap &m, uint8_t output, uint16_t enabledOutputs) {
  if (!m.writable || !knownMap(m)) return Result::Unsupported;
  if (output >= m.outputs || (enabledOutputs >> m.outputs)) return Result::InvalidTarget;
  const uint16_t pdmBit = uint16_t(1) << (m.outputs - 1);
  const uint16_t workerBits = static_cast<uint16_t>(((uint16_t(1) << (m.outputs - 1)) - 1) & ~uint16_t(3));
  if ((output == m.outputs - 1 && (enabledOutputs & workerBits)) ||
      (output >= 2 && output < m.outputs - 1 && (enabledOutputs & pdmBit))) return Result::Conflict;
  return Result::Ok;
}

// Display cleaning never modifies the DSP's stored channel/source name. The
// common panel fonts contain letters/digits/space/.-/ and a synthetic '+',
// but no '?': unsupported bytes use the visible '-' glyph, never zero-width
// punctuation. Always terminate and bound reads even for non-NUL wire names.
inline bool displayName(const uint8_t *source, size_t sourceBytes, char *out,
                        size_t capacity, size_t maxCharacters) {
  if (!source || !out || capacity == 0) return false;
  size_t n = 0;
  while (n < sourceBytes && n < maxCharacters && n + 1 < capacity && source[n]) {
    const uint8_t ch = source[n];
    const bool supported = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                           (ch >= '0' && ch <= '9') || ch == ' ' || ch == '.' ||
                           ch == '-' || ch == '/' || ch == '+';
    out[n] = ch < 32 || ch == 127 ? ' ' : (supported ? static_cast<char>(ch) : '-');
    ++n;
  }
  out[n] = 0; return true;
}
typedef uint16_t (*MeasureText)(const char *text, void *context);
inline bool fitName(const char *source, size_t sourceBytes, char *out, size_t capacity,
                    uint16_t maxPixels, MeasureText measure, void *context) {
  if (!measure || !displayName(reinterpret_cast<const uint8_t *>(source), sourceBytes,
                               out, capacity, 31)) return false;
  if (measure(out, context) <= maxPixels) return true;
  const size_t fullLength = strlen(out);
  size_t dots = capacity > 3 ? 3 : capacity - 1;
  while (dots) {
    char suffix[4] = {'.', '.', '.', 0}; suffix[dots] = 0;
    if (measure(suffix, context) <= maxPixels) break;
    --dots;
  }
  size_t keep = fullLength;
  if (keep + dots >= capacity) keep = capacity - dots - 1;
  for (;;) {
    for (size_t i = 0; i < dots; ++i) out[keep + i] = '.';
    out[keep + dots] = 0;
    if (measure(out, context) <= maxPixels) return true;
    if (keep == 0) { out[0] = 0; return true; }
    --keep;
  }
}
inline bool validStoredName(const char *name, size_t bytes) {
  if (!name || bytes == 0 || bytes > 31) return false;
  bool nonSpace = false;
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t ch = static_cast<uint8_t>(name[i]);
    if (ch < 32 || ch > 126) return false;
    nonSpace |= ch != ' ';
  }
  return nonSpace;
}

struct PresetDirectory { uint16_t occupiedMask; uint32_t generation; bool fresh; };
enum class PresetAction : uint8_t { Load, SaveChanges, SaveAsNew, Rename, Revert };
struct PresetPlan {
  uint8_t request, slot;
  bool capturesLive, loadsDestination;
};
// Plans exactly ONE action request. SaveAsNew captures the LIVE unsaved state
// directly: there is never a destination load/defaults step. ESP-local per-slot
// state must be persisted only after DSP success has been confirmed. Timeouts
// stay unknown; caller must never blindly retry save/load/rename actions.
inline Result planPreset(PresetAction action, uint8_t target, uint8_t active,
                         const PresetDirectory &directory, uint32_t generation,
                         bool confirmed, PresetPlan &out) {
  if (!directory.fresh) return Result::Unavailable;
  if (directory.generation != generation) return Result::Stale;
  if (target >= 10 || (directory.occupiedMask & ~uint16_t(0x03ff))) return Result::InvalidTarget;
  const bool occupied = (directory.occupiedMask & (uint16_t(1) << target)) != 0;
  PresetPlan next{0, target, false, false};
  switch (action) {
    case PresetAction::SaveAsNew:
      if (occupied) return Result::Occupied;
      next.request = 0x90; next.capturesLive = true; break;
    case PresetAction::SaveChanges:
      if (target != active) return Result::InvalidTarget;
      if (!occupied) return Result::Empty;
      next.request = 0x90; next.capturesLive = true; break;
    case PresetAction::Load:
      if (!occupied) return Result::Empty;
      next.request = 0x91; next.loadsDestination = true; break;
    case PresetAction::Revert:
      if (target != active) return Result::InvalidTarget;
      if (!occupied) return Result::Empty;
      next.request = 0x91; next.loadsDestination = true; break;
    case PresetAction::Rename:
      if (!occupied) return Result::Empty;
      next.request = 0x94; break;
    default: return Result::Unsupported;
  }
  if (!confirmed) return Result::NeedsConfirmation;
  out = next; return Result::Ok;
}

} // namespace PanelControls
#endif
