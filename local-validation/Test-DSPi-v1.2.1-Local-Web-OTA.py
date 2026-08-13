import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2"
MODE = (FIRMWARE / "WifiTransferMode.cpp").read_text()
HEADER = (FIRMWARE / "WifiTransferMode.h").read_text()
WEB = (FIRMWARE / "WifiTransferWeb.h").read_text()
BUILD = (ROOT / "Build-and-Flash-DSPi-Front-Panel-v1.2.0.ps1").read_text()


class LocalWebOtaContracts(unittest.TestCase):
    def test_existing_partition_scheme_has_two_three_megabyte_ota_slots(self):
        self.assertIn("app3M_fat9M_16MB", BUILD)
        self.assertIn("0x10000", BUILD)

    def test_update_targets_inactive_application_partition_only(self):
        self.assertIn("esp_ota_get_next_update_partition(nullptr)", MODE)
        self.assertIn("Update.begin(static_cast<size_t>(declared), U_FLASH)", MODE)
        self.assertNotIn("U_SPIFFS", MODE)

    def test_full_flash_image_is_rejected_by_ota_slot_size(self):
        self.assertIn("declared > next->size", MODE)
        self.assertIn("application-only firmware .bin", MODE)
        self.assertIn("Do not select a 16 MB Full image", WEB)

    def test_declared_and_http_body_lengths_must_match(self):
        self.assertIn("declared != static_cast<uint64_t>(contentLength)", MODE)
        self.assertIn("firmwareReceivedBytes_ != firmwareDeclaredBytes_", MODE)

    def test_image_prefix_validates_s3_and_app_descriptor(self):
        self.assertIn("imageHeader.magic != ESP_IMAGE_HEADER_MAGIC", MODE)
        self.assertIn("imageHeader.chip_id != ESP_CHIP_ID_ESP32S3", MODE)
        self.assertIn("app.magic_word != ESP_APP_DESC_MAGIC_WORD", MODE)
        self.assertIn("kFirmwareHeaderCapacity = 288U", HEADER)

    def test_sd_is_synchronised_before_internal_flash_write(self):
        sync = MODE.index("sdSynced = storage_->syncTransferDevice()")
        begin = MODE.index("Update.begin(static_cast<size_t>(declared), U_FLASH)")
        self.assertLess(sync, begin)

    def test_other_portal_writes_are_blocked_during_ota(self):
        self.assertGreaterEqual(MODE.count("firmwareUpdateActive_"), 20)
        self.assertIn("accepting_ = false", MODE)
        self.assertIn("const locked=finishingClient||firmwareRunning", WEB)

    def test_failed_update_aborts_and_restores_service(self):
        self.assertIn("if (Update.isRunning()) Update.abort()", MODE)
        self.assertIn("firmwareUpdateSucceeded_ = false", MODE)
        self.assertIn("Update failed; the current firmware remains bootable", WEB)

    def test_success_response_is_sent_before_delayed_restart(self):
        response = MODE.index("endChunkedJson();", MODE.index("handleFirmwareFinished"))
        restart_arm = MODE.index("firmwareRebootAt_ = millis()", response)
        self.assertLess(response, restart_arm)
        self.assertIn("kFirmwareResponseDrainMs = 1800U", MODE)

    def test_browser_upload_is_manual_raw_local_put(self):
        self.assertIn('firmware:"/api/firmware"', WEB)
        self.assertIn('xhr.open("PUT",API.firmware,true)', WEB)
        self.assertIn('xhr.send(selected)', WEB)
        self.assertIn('type="file" accept=".bin,application/octet-stream"', WEB)

    def test_no_internet_updater_or_remote_firmware_url_was_added(self):
        joined = MODE + HEADER + WEB
        self.assertNotIn("HTTPClient", joined)
        self.assertNotIn("github.com", joined.lower())
        self.assertNotIn("https://", joined.lower())

    def test_browser_warns_during_navigation_and_requires_confirmation(self):
        self.assertIn("window.confirm(warning)", WEB)
        self.assertIn("if(firmwareRunning||running||serverWriterActive)", WEB)


if __name__ == "__main__":
    unittest.main()
