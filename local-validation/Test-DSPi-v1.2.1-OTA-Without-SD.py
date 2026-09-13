"""Source contracts: OTA-only entry, SD isolation, and safe exit."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1] / 'firmware/DSPi_ESP32_Front_Panel_v1_1_2'
INO = (ROOT / 'DSPi_ESP32_Front_Panel_v1_1_2.ino').read_text(encoding='utf-8')
CPP = (ROOT / 'WifiTransferMode.cpp').read_text(encoding='utf-8')
WEB = (ROOT / 'WifiTransferWeb.h').read_text(encoding='utf-8')


def body(source, signature):
    start = source.index('{', source.index(signature))
    depth = 0
    for i in range(start, len(source)):
        depth += (source[i] == '{') - (source[i] == '}')
        if not depth:
            return source[start:i+1]
    raise AssertionError('Unclosed function')


class OtaWithoutSd(unittest.TestCase):
    def test_explicit_no_mount_entry_does_not_probe_storage(self):
        case = body(INO, 'case WifiTransferPhase::ConfirmWriteAccess: {')
        self.assertIn('if (!mediaPlayerPoc.mounted())', case)
        self.assertIn('wifiTransferUpdateOnly = true;', case)
        self.assertIn('setWifiTransferPhase(WifiTransferPhase::StopBle,', case)
        self.assertIn('wifiTransferMode.start(mediaFs, "/", wifiTransferUpdateOnly)', INO)

    def test_update_only_has_no_storage_pointer(self):
        start = body(CPP, 'bool WifiTransferModeController::start(')
        self.assertIn('if (updateOnly)', start)
        self.assertIn('storage_ = nullptr;', start)
        self.assertIn('if (!updateOnly && !prepared &&', start)
        self.assertIn('preflightStorage(storage, transferRoot', start)

    def test_firmware_sync_optional_only_without_storage(self):
        handler = body(CPP, 'void WifiTransferModeController::handleFirmwareRaw(')
        self.assertIn('bool sdSynced = storage_ == nullptr;', handler)
        self.assertIn('sdSynced = storage_->syncTransferDevice();', handler)
        self.assertIn('if (!sdSynced || !Update.begin(', handler)

    def test_filesystem_endpoints_guard_null_storage(self):
        for signature in ['void WifiTransferModeController::handleList(',
                          'void WifiTransferModeController::handleMkdir(',
                          'void WifiTransferModeController::handleDeleteIncomplete(',
                          'void WifiTransferModeController::handleDeletePath(']:
            with self.subTest(signature=signature):
                handler = body(CPP, signature)
                self.assertIn('!storage_', handler)
                self.assertLess(handler.index('!storage_'), handler.index('storage_->'))

    def test_no_card_exit_skips_remount_failure(self):
        case = body(INO, 'case WifiTransferPhase::ValidateNormalAccess: {')
        self.assertIn('if (wifiTransferUpdateOnly)', case)
        self.assertLess(case.index('if (wifiTransferUpdateOnly)'), case.index('mediaPlayerPoc.mountCard()'))

    def test_browser_separates_storage_and_firmware(self):
        self.assertIn('storageAvailable=Boolean(data.storageAvailable)', WEB)
        self.assertIn('if(storageAvailable)await browse("",0)', WEB)
        self.assertIn('No SD card available. Firmware updates still work.', WEB)
        firmware_control = next(line for line in WEB.splitlines() if '$("installFirmware").disabled=' in line)
        self.assertNotIn('storageAvailable', firmware_control)


if __name__ == '__main__':
    unittest.main()
