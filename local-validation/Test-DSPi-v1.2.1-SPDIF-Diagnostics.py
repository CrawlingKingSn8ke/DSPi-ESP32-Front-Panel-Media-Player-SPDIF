import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2"
DIAGNOSTICS = (FIRMWARE / "SpdifDiagnostics.cpp").read_text()
DIAGNOSTICS_HEADER = (FIRMWARE / "SpdifDiagnostics.h").read_text()
PLAYER = (FIRMWARE / "MediaPlayerPoC.cpp").read_text()
INO = (FIRMWARE / "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()
WIFI = (FIRMWARE / "WifiTransferMode.cpp").read_text()
WEB = (FIRMWARE / "WifiTransferWeb.h").read_text()


class SpdifDiagnosticContracts(unittest.TestCase):
    def test_log_is_fixed_size_ram_only(self):
        self.assertIn("constexpr size_t kEntryCapacity = 128", DIAGNOSTICS)
        self.assertIn("SpdifDiagnosticEntry entries[kEntryCapacity]", DIAGNOSTICS)
        forbidden = ("SD.", "SdFat", "Preferences", "WiFi", "Serial", "fopen")
        for token in forbidden:
            self.assertNotIn(token, DIAGNOSTICS)

    def test_log_is_cross_core_safe(self):
        self.assertIn("portMUX_TYPE diagnosticsMux", DIAGNOSTICS)
        self.assertGreaterEqual(DIAGNOSTICS.count("portENTER_CRITICAL"), 3)
        self.assertGreaterEqual(
            DIAGNOSTICS.count("portEXIT_CRITICAL"),
            DIAGNOSTICS.count("portENTER_CRITICAL"),
        )

    def test_entries_have_sequence_uptime_and_bounded_message(self):
        self.assertIn("uint32_t sequence", DIAGNOSTICS_HEADER)
        self.assertIn("uint32_t uptimeMs", DIAGNOSTICS_HEADER)
        self.assertIn("char message[112]", DIAGNOSTICS_HEADER)
        self.assertIn("vsnprintf(message, sizeof(message)", DIAGNOSTICS)

    def test_audio_path_records_only_bounded_events_and_ten_second_heartbeat(self):
        self.assertIn("completedUs - heartbeatStartedUs >= 10000000LL", PLAYER)
        self.assertIn("producer gap=%lu us", PLAYER)
        self.assertIn("PCM underrun started", PLAYER)
        self.assertIn("PCM underrun recovered", PLAYER)
        self.assertIn("DMA write timeout", PLAYER)
        self.assertNotIn("spdifDiagnosticLog", PLAYER[PLAYER.index("static bool IRAM_ATTR"):PLAYER.index("static bool IRAM_ATTR") + 500] if "static bool IRAM_ATTR" in PLAYER else "")

    def test_heartbeat_captures_writer_and_decoder_discriminators(self):
        for field in (
            "write_max=%lu us",
            "gap_max=%lu us",
            "underruns=%lu",
            "timeouts=%lu",
            "errors=%lu",
        ):
            self.assertIn(field, PLAYER)

    def test_dspi_lock_and_rate_changes_are_recorded(self):
        self.assertIn("DSPi RX changed", INO)
        self.assertIn("DSPi RX status read failed", INO)
        self.assertIn("DSPi RX status read recovered", INO)

    def test_web_endpoint_streams_log_without_storage_access(self):
        self.assertIn("kRouteSpdifDiagnostics", WIFI)
        self.assertIn("handleSpdifDiagnostics", WIFI)
        self.assertIn("text/plain; charset=utf-8", WIFI)
        self.assertIn("spdifDiagnosticRead(index, entry)", WIFI)

    def test_browser_exposes_refreshable_copyable_log(self):
        self.assertIn('spdifDiagnostics:"/api/spdif-diagnostics"', WEB)
        self.assertIn('id="refreshSpdifDiagnostics"', WEB)
        self.assertIn('id="copySpdifDiagnostics"', WEB)
        self.assertIn("navigator.clipboard.writeText", WEB)
        self.assertIn("await refreshSpdifDiagnostics()", WEB)


if __name__ == "__main__":
    unittest.main(verbosity=2)
