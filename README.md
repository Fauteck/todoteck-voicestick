# Voice Stick — Todoteck Fork

Fork of [78/voicestick](https://github.com/78/voicestick) with the `upstream`
remote kept, so upstream work on audio, display and power management can still
be merged in.

Upstream turns an M5Stack StickS3 into a Bluetooth push-to-talk input device for
a desktop app that pastes recognized text into the focused input field. This
fork keeps that firmware and protocol, and changes what happens **on the
device**: the screen speaks German, the speaker acknowledges what the stick is
doing, and the answer sent back by the host is rendered instead of discarded.

The host side for this fork is **not** the desktop app in this repository. It is
the Todoteck bridge (`scripts/voice-bridge/` for the PC, plus the Android
foreground service) in the [Todoteck repository](https://github.com/Fauteck/todo),
which uploads the audio to `POST /api/voice/turn` and writes the answer back into
the `ui_state` control event. The macOS, Windows and Linux apps in `desktop/` are
upstream code and untouched by this fork — see
[Upstream Desktop Apps](#upstream-desktop-apps).

> **Hardware status:** the fork changes below are written and build in CI, but
> the first run on a real StickS3 is still open. There is no ESP-IDF environment
> where they were authored.

## What This Fork Changes

| Change | Why |
| --- | --- |
| German interface with Latin-1 fonts (`todoteck_font_10/16`, generated from Montserrat with `lv_font_conv`) | LVGL's built-in Montserrat carries ASCII only, so umlauts and ß would render as gaps |
| Answer text on the display | The `ui_state` control event already carried a `text` field; the stock firmware used it only to pick fixed English hints |
| Acknowledgement tones (`audio_tone` component): one on record start, two rising for an answer, two falling for an error, one short low one for a discard | On the wrist you cannot see the display while speaking |
| Level meter during recording | Peak per frame, square-root compressed (raw speech sits near ten percent of full scale and would be invisible as a bar), with decay against flicker |
| Remaining-time bar up to the 30 s limit | The limit exists anyway; at the limit the take is **sent**, not discarded — a sentence that ran that far beats nothing |
| Side button aborts a running recording, screen shows `Abgebrochen` | Tearing the capture path down always emits a normal END audio frame, so the abort needs its own state event, `recording_discarded` |
| Side button while idle shows the last answer again | The screen dims after 30 s and sleeps after 5 minutes; whoever looks later never read the answer |
| Connection dot carries the real link state (filled green connected, empty ring disconnected) | It used to be derived from the scene, so a bridge dropping mid-screen was only noticed at the next attempt |
| `device_name` control event overrides the advertised `VS-XXXX` in the header | With two sticks, the Todoteck name is the only way to tell which one is in your hand |
| Firmware builds in GitHub Actions (`.github/workflows/firmware.yml`) | Nobody needs a local ESP-IDF toolchain to get a flashable image |

Both protocol additions are optional in both directions: a bridge that does not
know `recording_discarded` simply uploads the aborted take, and firmware that
does not know `device_name` ignores it.

## Project Layout

- `firmware/`: ESP-IDF firmware for M5Stack StickS3 / ESP32-S3.
- `firmware/components/ui_status/`: display, fonts, level and time bars, link dot.
- `firmware/components/audio_tone/`: acknowledgement tones over the ES8311.
- `desktop/macos/`: Swift Package for the native macOS menu bar app (upstream).
- `desktop/windows/`, `desktop/linux/`: upstream desktop workspaces.
- `docs/protocol.md`: BLE protocol between StickS3 and host, including the fork additions.
- `docs/volcengine-asr.md`: trimmed Volcengine ASR notes used by the upstream desktop client.
- `docs/release.md`: upstream release process (not used by this fork).
- `scripts/`: sprite slicing, palette tuning, and LVGL ARGB binary conversion helpers.

## Firmware Features

- StickS3 advertises as `VS-XXXX`, where `XXXX` is derived from the last two bytes of the eFuse MAC. A `device_name` control event replaces that name in the header.
- The front button maps to the protocol `primary` role; it starts a recording session on press and ends it on release once the host has put the device in `ready`.
- The firmware reads 16 kHz mono PCM from the ES8311 microphone, encodes it as Opus, and sends it over BLE notifications.
- Recording is capped at 30 seconds. A bar counts the remaining time down; at zero the firmware sends the take like a normal button release.
- The side button aborts a running recording, emits `recording_discarded`, shows `Abgebrochen` and plays the discard tone.
- The screen shows pairing, ready, listening (`Hört zu`), thinking (`Denkt nach`), pending confirmation, error and battery states from host-sent `ui_state` updates. Text arriving with `ready` is rendered as the answer and kept for recall (up to 192 bytes; the display wraps and clips it).
- The screen dims after 30 seconds of inactivity. On battery power it enters deep sleep after 5 minutes; while charging or USB powered it stays at the dimmed-screen stage. The front button wakes it from deep sleep.
- BLE OTA updates over the two 3 MB app slots of the OTA partition table.

Tones share the I2S lines with the microphone: `audio_tone_play()` stays silent
while the capture path is up. That is why the start tone is played *before*
`audio_pipeline_start()` — which makes it the "speak now" cue at the same time.

## Hardware Target

- Board: M5Stack StickS3 / ESP32-S3-PICO-1-N8R8
- Front button: GPIO11, protocol `primary`, push-to-talk and deep-sleep wake
- Side button: GPIO12, protocol `secondary`, abort, cancel, or recall the last answer
- PMIC IRQ: GPIO13
- Audio codec: ES8311 over I2S, 16 kHz / 16 bit / mono, speaker on the same lines
- Display: 135 x 240 ST7789P3 portrait screen
- LCD backlight: GPIO38 PWM

Main pin definitions live in `firmware/components/stick_s3_board/include/stick_s3_board.h`.

## Interaction Model

| State | Front button | Side button |
| --- | --- | --- |
| Unpaired / disconnected | No recording; screen shows `VS-XXXX` | No effective action |
| Connected idle | Hold to record | Show the last answer again on the device; the click still reaches the host, where the upstream app restores the last input confirmation |
| Recording | Release to finish recording | Abort the recording, nothing is sent |
| Thinking / finalizing | New recording is ignored | Cancel the in-progress recognition |
| Pending confirmation countdown | Pause auto-paste and keep pending confirmation | Cancel pending text |
| Manual pending confirmation | Confirm paste | Cancel pending text |

The firmware reports raw button facts (`button_down` / `button_up` /
`button_click` with `primary` or `secondary`). The host owns the interaction
state machine and sends `ui_state` updates back to the firmware for the screen.
The pending-confirmation rows apply to the upstream desktop apps, which paste
into the focused input field; the Todoteck bridge does not paste and does not
use them.

## Audio Path

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> host -> Ogg Opus -> ASR -> answer -> ui_state text -> display
```

Neither host decodes Opus back to PCM. The frames are wrapped into Ogg Opus and
forwarded as they are.

## BLE Protocol Summary

GATT service:

```text
8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
```

Characteristics:

| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `audio_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101` | StickS3 -> host | notify |
| `state_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102` | StickS3 -> host | notify |
| `control_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103` | host -> StickS3 | write without response |

Fork additions, both documented in `docs/protocol.md`:

```json
{"event":"recording_discarded","session_id":1234}
{"event":"device_name","name":"Handgelenk"}
```

The negotiated MTU (measured: 247) caps a control event at 244 bytes, which
leaves roughly 195 bytes of answer text after the JSON frame — about 180
characters in UTF-8, fewer with umlauts. Longer answers need a continuation
frame, which does not exist yet.

## Firmware Build in CI

`.github/workflows/firmware.yml` (`Firmware bauen`) builds the firmware with
ESP-IDF v5.5.1 on every push touching `firmware/**`, and on manual dispatch. It
fails if the image does not fit the 3 MB OTA partition, because otherwise you end
up holding a finished file the device refuses. Two artifacts:

| File | Use |
| --- | --- |
| `voice_stick.bin` | Plain app image; this is what the Todoteck bridge pushes over BLE OTA |
| `merged.bin` | Bootloader, partition table and app in one — the USB rescue path when a build breaks the radio link |

Rescue flashing needs esptool only, no toolchain:

```sh
python -m esptool --chip esp32s3 write_flash 0x0 merged.bin
```

## Firmware Build Locally

Optional — CI covers the normal case. Prepare ESP-IDF; the commands below use
the local path `~/esp/v5.5.1/esp-idf`, replace it if your checkout lives
elsewhere.

```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
```

If `export.sh` reports that the ESP-IDF Python virtual environment is missing, run the matching installer once:

```sh
"$HOME/esp/v5.5.1/esp-idf/install.sh" esp32s3
```

Flash and monitor:

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

The firmware uses an OTA partition table with two 3 MB app slots plus a reserved 1984 KB `storage` partition. Devices flashed with the old single-app table need one USB flash to install the new partition table before BLE OTA updates can be used:

```sh
idf.py -p /dev/cu.usbmodemXXXX erase-flash flash monitor
```

Firmware dependencies are declared through the ESP-IDF component manager:

- `espressif/button`
- `espressif/esp_codec_dev`
- `78/esp-opus`
- `lvgl/lvgl`

## Upstream Desktop Apps

The apps under `desktop/` are upstream code. They are kept so the fork stays
mergeable, and they still work against upstream ASR — they know nothing about
Todoteck.

The macOS app is a Swift Package targeting macOS 12 or newer:

```sh
cd desktop/macos
swift build
swift run VoiceStickApp
```

It is a menu bar accessory app and requests Bluetooth permission. Text insertion
uses simulated `Command-V` plus optional Return. If macOS blocks the keyboard
events, grant Accessibility permission to the running terminal or app in System
Settings.

Its config lives at `~/Library/Application Support/VoiceStick/config.toml`;
create it from `desktop/macos/Config/config.example.toml`, which documents every
field (ASR provider and keys, `paired_device_ids`, `auto_enter`,
`interaction_mode`, per-device overlay and translation settings). Do not commit
API keys.

Pairing: flash and boot the StickS3, start the app, open `Pair Device...`,
select the matching `VS-XXXX` and click `Pair`. Repeat for further devices; with
multiple IDs saved, the app ignores nearby unpaired VoiceStick devices.

The upstream release path — Sparkle/WinSparkle appcast, Aliyun OSS firmware
manifests, GitHub Pages deploy — belongs to the upstream repository and is not
used by this fork. It is described in `docs/release.md`.

## Interaction with the Todoteck Bridge

- The bridge connects to the stick over BLE, wraps the Opus frames into Ogg Opus and posts one turn to `POST /api/voice/turn` with a device token.
- The answer comes back as plain text and is sent to the stick as `ui_state` with `state: "ready"` and the text in `text` — that is what triggers the answer screen and the done tone.
- Since the firmware renders umlauts, bridges send the full text from the JSON `text` field rather than the ASCII short form from the `X-Todoteck-Text` header. That header was a workaround for firmware that could not display umlauts and stays in place for other clients.
- Concept, phases and the server side: `docs/sprach-eingabegeraet-konzept.md` in the Todoteck repository.

## License

This project is licensed under the [MIT License](LICENSE).
