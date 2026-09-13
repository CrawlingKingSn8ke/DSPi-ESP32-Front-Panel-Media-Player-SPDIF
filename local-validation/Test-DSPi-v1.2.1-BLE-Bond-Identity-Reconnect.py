import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INO = (
    ROOT
    / "firmware"
    / "DSPi_ESP32_Front_Panel_v1_1_2"
    / "DSPi_ESP32_Front_Panel_v1_1_2.ino"
).read_text(encoding="utf-8")


def function_body(signature: str) -> str:
    start = INO.index(signature)
    brace = INO.index("{", start)
    depth = 0
    for offset in range(brace, len(INO)):
        if INO[offset] == "{":
            depth += 1
        elif INO[offset] == "}":
            depth -= 1
            if depth == 0:
                return INO[brace : offset + 1]
    raise AssertionError(f"unterminated function: {signature}")


class BleBondIdentityReconnectContracts(unittest.TestCase):
    def test_normal_reconnect_matches_identity_address_and_type_family(self):
        body = function_body("bool bleDeviceMatchesSaved(")
        self.assertIn("equalsIgnoreCase(bleSavedAddress)", body)
        self.assertIn("device->getAddressType() & 0x01U", body)
        self.assertIn("bleSavedAddressType & 0x01U", body)
        self.assertNotIn("device->getAddressType() == bleSavedAddressType", body)
        self.assertNotIn("getName", body)

    def test_identity_is_captured_from_bonded_connection(self):
        self.assertIn(
            "if (connInfo.isBonded()) captureBleResolvedIdentity(connInfo.getIdAddress());",
            INO,
        )
        identity_callback = function_body("void onIdentity(NimBLEConnInfo &connInfo)")
        self.assertIn("captureBleResolvedIdentity(connInfo.getIdAddress())", identity_callback)

    def test_new_profile_prefers_resolved_identity(self):
        worker = function_body("void bleConnectWorker(")
        self.assertIn("copyBleResolvedIdentity(savedDevice)", worker)
        self.assertIn("saveRemoteDevice(savedDevice)", worker)
        self.assertLess(
            worker.index("copyBleResolvedIdentity(savedDevice)"),
            worker.index("saveRemoteDevice(savedDevice)"),
        )

    def test_existing_profile_migration_requires_exactly_one_bond(self):
        body = function_body("void migrateSavedRemoteToBondIdentity(")
        self.assertIn("NimBLEDevice::getNumBonds()", body)
        self.assertIn("if (bondCount != 1)", body)
        self.assertIn("NimBLEDevice::getBondedAddress(0)", body)
        self.assertNotIn("bleSavedName", body)

    def test_migration_persists_identity_without_erasing_mappings(self):
        body = function_body("void migrateSavedRemoteToBondIdentity(")
        self.assertIn("strlcpy(bleSavedAddress, identityText.c_str()", body)
        self.assertIn("bleSavedAddressType = identity.getType()", body)
        self.assertIn("markDeferredPreference(PREF_DIRTY_BLE_DEVICE, 500)", body)
        self.assertNotIn("saveRemoteDevice", body)
        self.assertNotIn("remoteMap", body)

    def test_boot_normalises_identity_before_reconnect_scan(self):
        body = function_body("void beginBleRemote(")
        self.assertLess(
            body.index("migrateSavedRemoteToBondIdentity()"),
            body.index("lastBleScanAt = millis() - BLE_RESCAN_INTERVAL_MS"),
        )

    def test_audio_protection_for_background_reconnect_is_unchanged(self):
        body = function_body("void pollBleRemote()\n{")
        self.assertIn("audioCritical && bleScanActive", body)
        self.assertIn("bleScanPurpose == BLE_SCAN_RECONNECT", body)
        self.assertIn('Serial.println("BLE RECONNECT: deferred during audio playback")', body)

    def test_reconnect_scan_is_continuous_and_passive(self):
        self.assertIn("#define BLE_RECONNECT_SCAN_MS 0", INO)
        body = function_body("bool startBleScan(BleScanPurpose purpose, uint32_t duration)")
        self.assertIn("scan->setActiveScan(purpose == BLE_SCAN_USER)", body)
        self.assertIn("scan->setWindow(purpose == BLE_SCAN_RECONNECT ? 100 : 80)", body)
        self.assertIn("scan->start(duration, false, true)", body)

    def test_continuous_reconnect_cannot_fill_a_bounded_result_table(self):
        body = function_body("bool startBleScan(BleScanPurpose purpose, uint32_t duration)")
        self.assertIn(
            "scan->setMaxResults(purpose == BLE_SCAN_RECONNECT ? 0 : BLE_MAX_DEVICES)",
            body,
        )

    def test_reconnect_accepts_bonded_directed_advertisements(self):
        body = function_body("bool startBleScan(BleScanPurpose purpose, uint32_t duration)")
        self.assertIn("BLE_HCI_SCAN_FILT_NO_WL_INITA", body)
        self.assertIn("BLE_HCI_SCAN_FILT_NO_WL", body)
        self.assertIn("scan->setFilterPolicy", body)

    def test_boot_and_reconnect_scan_have_bounded_diagnostics(self):
        begin = function_body("void beginBleRemote(")
        scan = function_body("bool startBleScan(BleScanPurpose purpose, uint32_t duration)")
        callback = function_body("void onResult(const NimBLEAdvertisedDevice *device)")
        self.assertIn("BLE BOOT: stack=ready", begin)
        self.assertIn("BLE RECONNECT: scan start", scan)
        self.assertIn("BLE RECONNECT: matched", callback)

    def test_final_release_omits_verbose_advertisement_capture(self):
        callback = function_body("void onResult(const NimBLEAdvertisedDevice *device)")
        self.assertIn("bleDeviceMatchesSaved(device)", callback)
        self.assertNotIn("BLE RECONNECT: advert", callback)

    def test_final_release_omits_verbose_scan_end_capture(self):
        callback = function_body("void onScanEnd(const NimBLEScanResults &results, int reason)")
        self.assertNotIn("BLE SCAN: end", callback)
        self.assertIn("bleIgnoreNextScanEnd", callback)

    def test_zero_address_directed_wake_requires_one_saved_bond(self):
        self.assertIn("bool bleIsSingleBondDirectedWake(", INO)
        body = function_body("bool bleIsSingleBondDirectedWake(")
        self.assertIn("bleProfileValid", body)
        self.assertIn("device->getAddress().isNull()", body)
        self.assertIn("device->getAdvType() == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND", body)
        self.assertIn("!device->isScannable()", body)
        self.assertIn("NimBLEDevice::getNumBonds() == 1", body)
        self.assertIn("NimBLEDevice::isBonded(savedIdentity)", body)

    def test_directed_wake_connects_with_saved_identity_not_zero_address(self):
        callback = function_body("void onResult(const NimBLEAdvertisedDevice *device)")
        self.assertIn("bleIsSingleBondDirectedWake(device)", callback)
        self.assertIn("copySavedBleDevice(blePendingDevice, device->getRSSI())", callback)
        self.assertIn("BLE RECONNECT: directed wake; using saved identity", callback)

        self.assertIn("void copySavedBleDevice(", INO)
        copier = function_body("void copySavedBleDevice(")
        self.assertIn("bleSavedAddress", copier)
        self.assertIn("bleSavedAddressType", copier)
        self.assertIn("bleSavedName", copier)

    def test_new_ble_printf_diagnostics_use_crlf(self):
        for marker in (
            "BLE BOOT: stack=ready",
            "BLE RECONNECT: scan start",
            "BLE RECONNECT: matched",
            "BLE RECONNECT: directed wake",
        ):
            line_start = INO.index('Serial.printf("' + marker)
            statement_end = INO.index(");", line_start)
            statement = INO[line_start : statement_end + 2]
            self.assertIn("\\r\\n", statement, marker)

    def test_reconnect_recovers_if_controller_scan_stops_silently(self):
        body = function_body("void pollBleRemote()\n{")
        self.assertIn("!NimBLEDevice::getScan()->isScanning()", body)
        self.assertIn("!bleConnectRequested", body)
        self.assertIn(
            "bleScanPurpose == BLE_SCAN_RECONNECT && !bleConnectRequested &&\n"
            "      !bleConnectTaskRunning &&",
            body,
        )
        self.assertIn("resetBleScanState(false)", body)
        self.assertIn("scheduleSavedReconnect()", body)
        self.assertIn("BLE RECONNECT: scan stopped unexpectedly; retrying", body)

    def test_profile_requires_live_and_persisted_bond(self):
        body = function_body("bool connectBleRemote(const BleDeviceInfo &device, bool saveOnSuccess)\n{")
        self.assertIn("connectionInfo.isBonded()", body)
        self.assertIn("NimBLEDevice::isBonded(identity)", body)
        self.assertIn("if (!storedBonded)", body)
        self.assertIn("bleClient->disconnect()", body)
        self.assertLess(body.index("if (!storedBonded)"), body.index("stage=service-discovery"))

    def test_unstored_bond_is_not_reported_ready(self):
        worker = function_body("void bleConnectWorker(")
        self.assertLess(worker.index("connectBleRemote"), worker.index("saveRemoteDevice"))
        poll = function_body("void pollBleRemote()\n{")
        self.assertIn('bleConnectBondFailed ? "Bond failed - reset remote"', poll)


if __name__ == "__main__":
    unittest.main(verbosity=2)
