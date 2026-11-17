# DSPi ESP32 Front Panel — Media Player S/PDIF v1.5.1

DSPi Link now uses the standard `_dspi._tcp` discovery service searched by compatible DSPi Console applications. The advertised hub name follows renaming, and device details follow whether a DSPi is connected.

SMB network music, SD playback, Retro theme, Master/User volume selection, shared source names, effects, output/EQ controls, BLE remotes and local Wi-Fi transfer/update remain included. Audio and BLE behaviour are unchanged from v1.5.0.

## Downloads

- `DSPi-ESP32-Front-Panel-v1.5.1-OTA.bin` — update through **System → WiFi → Transfer/Update**.
- `DSPi-ESP32-Front-Panel-v1.5.1-Full.bin` — 16 MB USB installation/recovery image at `0x0`. Never upload through OTA.
- `DSPi-ESP32-Front-Panel-v1.5.1-Source.zip` — release source, build scripts and bundled dependency licences.
- `SHA256SUMS-v1.5.1.txt`.

Enable **System → WiFi → DSPi Link** on the panel and enable discovery in a Link-capable Console on the same accessible local network. Browser access still uses `http://ESP-IP:11915/`.

OTA preserves saved settings and BLE bonds. After the update restarts, disconnect all power for 10 seconds before using SD playback. The ESP update does not install or modify DSPi firmware.
