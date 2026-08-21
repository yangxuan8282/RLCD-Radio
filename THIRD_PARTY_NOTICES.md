# Third-party notices

## ESP RAOP Receiver

AirPlay 1 / RAOP support is based on `jptrsn/esp-raop-receiver` at commit
`c17d3d619920d457ab25c3d6ee334aecd4b01d3d`.

- Upstream: https://github.com/jptrsn/esp-raop-receiver
- License: GNU General Public License v3.0
- Vendored code: `lib/esp-raop-receiver/`

The vendored component includes code derived from projects credited by its
upstream repository, including Squeezelite-ESP32, Shairport Sync and an ALAC
decoder. Source-file copyright and license headers have been retained. The
complete GPLv3 text is included at `lib/esp-raop-receiver/LICENSE`.

## AirPlay 2 reference

`rbouteiller/airplay-esp32` was reviewed as an architectural reference while
designing audio-source switching. No source code from that non-commercial
AirPlay 2 project is included in this release.

- Reference: https://github.com/rbouteiller/airplay-esp32
