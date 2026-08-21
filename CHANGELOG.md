# Changelog

## 0.3.1 - 2026-08-21

- Fixed missing clock and spectrum updates when AirPlay consumed memory before the original UI tasks were created.
- Start and verify the ADC, NTP and FFT tasks before initializing the AirPlay receiver.
- Retry NTP every five seconds until the first successful synchronization.
- Report RAOP mDNS registration and RTSP task creation failures instead of reporting a false successful startup.

## 0.3.0 - 2026-08-21

- Added AirPlay 1 / RAOP audio receiving with ALAC decoding and mDNS discovery.
- Added automatic source switching: AirPlay pauses the radio and disconnecting resumes the selected station.
- Reused the existing ES8311, I2S output and FFT display path for AirPlay PCM.
- Added AirPlay enable and receiver-name settings to the Chinese WebUI.
- Added upstream attribution and GPLv3 notices for the vendored RAOP component.

## 0.2.4 - 2026-08-18

- Added the firmware version to the WebUI title and configuration page.

## 0.2.3 - 2026-08-17

- Fixed the audio watchdog callback override so the application implementation is linked instead of the library's empty weak default.
- Prevented false stream reconnects while decoded PCM data is still arriving normally.

## 0.2.2 - 2026-08-17

- Fixed periodic audio reconnects caused by an obsolete `audio_process_i2s` callback signature.
- Increased the audio-data watchdog timeout from 5 to 15 seconds to tolerate brief network stalls.

## 0.2.1 - 2026-08-17

- Fixed a reboot caused by transient SPI DMA allocation failure after the audio decoder allocated its buffers.
- Added a persistent 15 KB internal DMA transfer buffer while keeping the display framebuffer and lookup tables in PSRAM.
- Reduced the SPI transfer size and queue depth to match the reflective LCD framebuffer.
- Set the audio decoder to full digital volume and made the WebUI ES8311 volume range accurately use 0-100.
- Changed display transfer failures to drop and log the affected frame instead of aborting the device.

## 0.2.0 - 2026-08-17

- Added a 24 px embedded Simplified Chinese LVGL font and localized the device and web configuration interfaces.
- Replaced the default station list with five direct Chinese MP3 radio streams and added `name|URL` configuration support.
- Added Wi-Fi setup through the `RLCD-Radio-Setup` access point and Chinese setup page.
- Pinned PioArduino 55.03.311 with Arduino-ESP32 3.3.11 and C++20 for reproducible headless builds.
- Fixed display coordinate bounds, station switching bounds, NTP persistence and initialization order, and Arduino-ESP32 3.x BSP compatibility.
- Verified generation of both the application image and a single-file factory image for transfer to another flashing machine.

## 0.1.0 - 2026-08-15

- Established the PlatformIO/Arduino baseline for the ESP32-S3 reflective LCD radio.
- Recorded the planned Chinese localization scope: LVGL CJK font support, Chinese UI and configuration text, and a small set of Chinese radio presets.
- Documented the intended headless workflow: build firmware artifacts on the build host and transfer the generated binaries for flashing elsewhere.
