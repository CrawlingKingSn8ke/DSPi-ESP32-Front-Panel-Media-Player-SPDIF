"""Execute unmodified legacy function bodies with the real new adapter hooks."""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text(encoding="utf-8")


def function(signature):
    start = SOURCE.index(signature + "\n{")
    opening = SOURCE.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


class PanelLegacyBoundaryTests(unittest.TestCase):
    def test_confirmed_source_and_legacy_save_destination_snapshot(self):
        fixture = (ROOT / "local-validation/Test-PanelRuntime.cpp").as_posix()
        code = r'''
#define main runtimeFixtureMain
#include "FIXTURE"
#undef main
enum {REQ_SET_INPUT_SOURCE=0xe0,REQ_GET_INPUT_SOURCE=0xe1,REQ_PRESET_SAVE=0x90};
bool mediaDspiSourcePrimed=false,presetSavePanelSettingsOk=false;
unsigned presetSaveCompletionAttempts=0,presetSaveCompletionSlot=0;
unsigned long presetSaveCompletionAt=0;
struct SerialStub{void println(const char*){}template<class...A>void printf(const char*,A...){}}Serial;
InputSource observedSource=SRC_USB;
bool dspiSetByte(uint8_t,uint8_t source){observedSource=InputSource(source);return true;}
bool getExactByte(uint8_t,uint8_t &source){source=observedSource;return true;}
void delay(unsigned ms){nowMs+=ms;}
void resetMetersForStateChange(const char*){}
void scheduleStateRefresh(unsigned,const char*){}
const char*dspiTxnResultText(DspiTxnResult){return "ok";}
DspiTxnResult dspiGetResult(uint8_t req,uint16_t slot,uint16_t length,uint8_t*out,uint16_t capacity,uint16_t&size,bool quiet){
  auto result=dspiTransactionResult(true,req,slot,2,nullptr,length,out,capacity,size,quiet,false);
  PanelRuntime::model.roles.left=4; // A later UI/live value must not replace captured roles.
  return result;
}
bool syncSuccess=true;
bool syncCurrentState(bool){dspi.activePreset=nativeWire.active;dspi.occupiedPresets=nativeWire.occupied;PanelRuntime::model.roles.left=8;return syncSuccess;}
void redrawCurrentView(){}
SOURCE_CHANGE
LEGACY_SAVE
LEGACY_COMPLETION
int main(){
  passes(2);CHECK(PanelRuntime::serialKnown);
  PanelRuntime::model.activeInputs=2;PanelRuntime::activeInputStep=1;
  PanelRuntime::groupJob=1;PanelRuntime::namesKnown[0]=true;
  unsigned before=nativeWire.calls.size();uint32_t generation=PanelRuntime::generation;
  CHECK(setInputSource(SRC_ADAT,true));
  CHECK(PanelRuntime::generation==generation+1&&PanelRuntime::activeInputStep==0);
  CHECK(PanelRuntime::model.activeInputs==0&&!PanelRuntime::namesKnown[0]&&!PanelRuntime::groupJob);
  uiView=VIEW_HOME;menuPage=PAGE_MAIN;passes(5);CHECK(nativeWire.calls.size()==before);
  PanelRuntime::model.roles.left=1;PanelRuntime::model.roles.right=2;PanelRuntime::savedKnown=true;
  CHECK(saveCurrentPreset(3));CHECK(!PanelRuntime::savedKnown);
  char key[16];PanelRuntime::localKey("r",3,key,sizeof(key));
  CHECK(preferences.getBytesLength(key)==0&&!PanelRuntime::roleDrafts[3].dirty);
  mediaPlayerPoc.playing=true;nowMs+=PRESET_SAVE_REFRESH_MS;servicePresetSaveCompletion();
  CHECK(PanelRuntime::roleDrafts[3].dirty&&PanelRuntime::roleDrafts[3].record.roles.left==1);
  CHECK(PanelRuntime::roleDrafts[3].record.roles.right==2&&PanelRuntime::model.roles.left==1);
  CHECK(preferences.getBytesLength(key)==0); // Completion adds no NVS write while Music is active.
  mediaPlayerPoc.playing=false;
  CHECK(panelPersistLocal());PanelRuntime::RoleRecord record{};
  CHECK(preferences.getBytes(key,&record,sizeof(record))==sizeof(record)&&record.roles.left==1);
  PanelRuntime::serialKnown=false;presetSaveCompletionPending=false;
  CHECK(saveCurrentPreset(4)); // Optional unknown identity must not break legacy saving.
  nowMs+=PRESET_SAVE_REFRESH_MS;servicePresetSaveCompletion();
  PanelRuntime::serialKnown=true;syncSuccess=false;CHECK(saveCurrentPreset(5));
  for(unsigned i=0;i<4;++i){nowMs+=PRESET_SAVE_REFRESH_MS;servicePresetSaveCompletion();}
  CHECK(!presetSaveCompletionPending&&!PanelRuntime::roleDrafts[5].dirty);
  syncSuccess=true;CHECK(saveCurrentPreset(6));PanelRuntime::serial[0]='B';
  nowMs+=PRESET_SAVE_REFRESH_MS;servicePresetSaveCompletion();
  CHECK(!PanelRuntime::roleDrafts[6].dirty&&!presetSavePanelSettingsOk);
  PanelRuntime::serial[0]='0';CHECK(saveCurrentPreset(7));PanelRuntime::serialKnown=false;
  nowMs+=PRESET_SAVE_REFRESH_MS;servicePresetSaveCompletion();
  CHECK(!PanelRuntime::roleDrafts[7].dirty&&!presetSavePanelSettingsOk);
  puts("Actual legacy boundaries: PASS");
}
'''.replace("FIXTURE", fixture).replace("SOURCE_CHANGE", function("bool setInputSource(InputSource source, bool userInitiated)")).replace("LEGACY_SAVE", function("bool saveCurrentPreset(uint8_t destinationSlot)")).replace("LEGACY_COMPLETION", function("void servicePresetSaveCompletion()"))
        output = ROOT / "local-validation/build-work/speaker-control/Test-PanelLegacyBoundary.exe"
        zig = ROOT.parent / "local-validation/toolchains/zig-x86_64-windows-0.14.1/zig.exe"
        env = dict(os.environ)
        env["ZIG_LOCAL_CACHE_DIR"] = str(output.parent / "zig-cache")
        env["ZIG_GLOBAL_CACHE_DIR"] = str(output.parent / "zig-global")
        build = subprocess.run([str(zig), "c++", "-std=c++17", "-O0", "-UNDEBUG", "-x", "c++", "-", "-o", str(output)], input=code, text=True, capture_output=True, cwd=ROOT, env=env)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        result = subprocess.run([str(output)], text=True, capture_output=True, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Actual legacy boundaries: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
