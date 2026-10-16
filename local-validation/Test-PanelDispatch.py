"""Execute the actual sketch dispatch boundary against the real Runtime adapter.

The extracted prefix is the production code before any legacy handler. Removing
or moving its pending-work guard lets the fake legacy Mute/Play handlers run.
"""
import os
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / "firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino"


class PanelDispatchTests(unittest.TestCase):
    def test_actual_dispatch_rejects_shortcuts_during_guard_and_allows_back_home(self):
        source = SKETCH.read_text(encoding="utf-8")
        actions = re.search(r"enum UiAction : uint8_t \{.*?\};", source, re.S).group(0)
        start = source.index("void dispatchUiAction(UiAction action)\n{")
        end = source.index("  if (mediaVolumeOverlayVisible", start)
        prefix = source[start:end]
        fixture = (ROOT / "local-validation/Test-PanelRuntime.cpp").as_posix()
        code = r'''
#define main runtimeFixtureMain
#include "FIXTURE"
#undef main
ACTIONS
static unsigned legacyPlay=0,legacyMute=0;
bool recordUserActivity(){return false;}
UiAction contextualizeUiAction(UiAction action){return action;}
void transitionToHome(){panelCancel();uiView=VIEW_HOME;}
PREFIX
  if(action==ACT_MUTE){++legacyMute;nativeWire.muted=!nativeWire.muted;}
  if(action==ACT_MEDIA_PLAY_PAUSE)++legacyPlay;
}
int main(){
  enterPage(PAGE_HARDWARE_PAIR);
  PanelRuntime::hardwareGuard=4;
  PanelRuntime::hardwareGeneration=PanelRuntime::generation;
  PanelRuntime::hardwarePriorMute=false;
  nativeWire.muted=true;
  dispatchUiAction(ACT_MUTE);dispatchUiAction(ACT_MEDIA_PLAY_PAUSE);
  CHECK(nativeWire.muted&&legacyMute==0&&legacyPlay==0);
  CHECK(PanelRuntime::hardwareGuard==4);
  dispatchUiAction(ACT_BACK);
  CHECK(PanelRuntime::hardwareCancel);
  unsigned before=nativeWire.calls.size();passes(8);
  for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x7c);
  PanelRuntime::presetStage=1;PanelRuntime::saveAttempted=false;
  dispatchUiAction(ACT_HOME);
  CHECK(uiView==VIEW_HOME&&PanelRuntime::presetStage==0);
  dispatchUiAction(ACT_MUTE);dispatchUiAction(ACT_MEDIA_PLAY_PAUSE);
  CHECK(legacyMute==1&&legacyPlay==1);
  puts("Actual dispatch boundary: PASS");
}
'''.replace("FIXTURE", fixture).replace("ACTIONS", actions).replace("PREFIX", prefix)
        output = ROOT / "local-validation/build-work/speaker-control/Test-PanelDispatch.exe"
        zig = ROOT.parent / "local-validation/toolchains/zig-x86_64-windows-0.14.1/zig.exe"
        env = dict(os.environ)
        env["ZIG_LOCAL_CACHE_DIR"] = str(output.parent / "zig-cache")
        env["ZIG_GLOBAL_CACHE_DIR"] = str(output.parent / "zig-global")
        build = subprocess.run([str(zig), "c++", "-std=c++17", "-O0", "-UNDEBUG", "-x", "c++", "-", "-o", str(output)], input=code, text=True, capture_output=True, cwd=ROOT, env=env)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        result = subprocess.run([str(output)], text=True, capture_output=True, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Actual dispatch boundary: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
