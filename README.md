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

> **Hardware status (as of 2026-08-26):** the fork changes below are written and
> build in CI, but the first run on a real StickS3 is still open — the device
> still runs the factory firmware, and there is no ESP-IDF environment where the
> changes were authored. This line is a point in time, not a property of the
> repo; the live state lives in the `Todoteck: Audio Gerät` task, not here.

## What This Fork Changes

| Change | Why |
| --- | --- |
| German interface with Latin-1 fonts (`todoteck_font_10` … `todoteck_font_16`, generated from Montserrat with `lv_font_conv`) | LVGL's built-in Montserrat carries ASCII only, so umlauts and ß would render as gaps |
| Answer text on the display | The `ui_state` control event already carried a `text` field; the stock firmware used it only to pick fixed English hints |
| Acknowledgement tones (`audio_tone` component): one on record start, two rising for an answer, two falling for an error, one short low one for a discard | On the wrist you cannot see the display while speaking |
| Level meter during recording | Peak per frame, square-root compressed (raw speech sits near ten percent of full scale and would be invisible as a bar), with decay against flicker |
| Remaining-time bar up to the 30 s limit | The limit exists anyway; at the limit the take is **sent**, not discarded — a sentence that ran that far beats nothing |
| Side button aborts a running recording, screen shows `Abgebrochen` | Tearing the capture path down always emits a normal END audio frame, so the abort needs its own state event, `recording_discarded` |
| A second press of the **front** button aborts the running recording as well | In hold-to-talk a second press cannot start a turn — the button would have had to come up first, and then no recording would be running. So it is either a lost release or an explicit "stop this", and both end the same way. The way out sits on the button already under the thumb instead of the one on the edge |
| Hold really means hold: a press shorter than 500 ms is discarded on the device, screen shows `Zu kurz` | A tap started a recording exactly like a hold, so whoever started speaking afterwards spoke into a recording that had already ended. The bridge dropped those takes silently (`MIN_TURN_MILLIS`); the device now says so instead |
| A turn that nobody spoke into gets its own state (`no_speech`, screen `Nichts gehört`, the short low discard tone) instead of an error | Whisper cannot report silence — it must emit tokens, and on German silence it falls back to the subtitle credit from its training videos. A button that fires in a pocket therefore came back as an ordinary answer: the screen showed a failed wiki search for „Untertitelung des ZDF, 2020", a sentence nobody had said. The Todoteck server now detects the case and names it; the device says so, and the tone says „discarded" rather than „broken" |
| The recording never outlives the press: every 200 ms tick compares the GPIO against the recording state | The release arrives from an ISR callback through a twelve-slot queue. If it is lost, the take used to run to the 30 s limit with nobody speaking. The tick replays the missing event; two consecutive ticks are required so a single misread cannot cut a sentence |
| Long text pushes Tecki off the screen instead of running through him | An answer like "Aufgabe angelegt: AliExpress Bestellung · Fällig: Di., 25.08.2026 · Projekt: Home Lab" used to grow upwards from the bottom edge across the status line and straight through the figure. The decision is made on the measured text, not on the scene: if it fits into the column beside Tecki nothing changes, if it does not, Tecki steps aside and the text gets the whole width in the largest font that fits |
| Landscape instead of portrait: the panel is rotated in the controller (`swap_xy`), Tecki moves into a 64 px column on the left, status line and text into a 150 px column beside him. `LVGL_DRAW_BUF_LINES` drops from 24 to 10 with it | The same answer needs seven lines on 119 px of width and three on 224 — it therefore lands one or two font steps larger. On a wrist, text along the arm also reads more naturally. The rotation is free in CPU terms — the ST7789 does it in hardware — but not in RAM: a draw buffer holds `width * lines` pixels, so the wider screen doubled the two buffers from 12,960 to 23,040 bytes of DMA-capable internal memory, and that is exactly the pool the recording path takes its 32 KB task stack and I2S buffers from. The device answered every button press with `Aufnahme startet nicht: ESP_ERR_NO_MEM` until the line count came down |
| A clock on the pairing, ready and resting screens (`time` control event) | The stick is worn on a wrist. Between two sentences it showed a mascot and a hint — a watch face costs nothing there and answers the question a wrist device is asked most often. On `ready` and `resting` the status word is dropped and the clock is set in Montserrat 48: that the device is ready is visible from it saying nothing else. `pairing` keeps its word — that no bridge is there is visible nowhere else — and takes Montserrat 28 beside it. The board has no RTC and no Wi-Fi, so the time comes from a bridge; until one has sent it, the face reads `--:--`. Showing dashes rather than nothing is deliberate: without them a device whose bridge never sends the time looks exactly like one running older firmware, which is the confusion the first run on hardware produced |
| Header carries only the link dot and the battery percentage | The battery drawing said the same as the number beside it, only less precisely, and the device name repeated what the pairing screen already shows as its hint. On a watch face every pixel that is not needed is in the way. The percentage now carries the state as colour as well: red below 20 %, blue on power |
| Raising the wrist wakes the dimmed screen (BMI270, polled at 10 Hz while dimmed) | The screen dims after 30 s. Getting the time back meant pressing the side button — with the other hand, which is the movement a watch exists to avoid. The gesture is movement, then stillness, then a tilt of at least 20° against the pose before it; all three, because otherwise every step while walking wakes the screen. Deep sleep still needs the button: M5 documents the IMU interrupt as `G4 (PYG4_IMU_INT via M5PM1)`, and whether that reaches a wake-capable pin is a measurement nobody has taken |
| The answer is set in the largest of six sizes (<!-- doku-vertrag:schriftstufen -->10, 11, 12, 13, 14, 16 px<!-- /doku-vertrag -->) that still fits the free area, each with its own line spacing | With only 10 px and 16 px to choose from, anything longer than a line landed in the smallest one — a two-sentence answer filled two thirds of the screen and left the rest white while being barely readable. The steps in between turn that white space into type size |
| Characters the fonts lack are substituted or dropped, not drawn as boxes | Bridge answers carry emoji (check mark, calendar, folder) and typographic punctuation. The emoji showed as empty rectangles mid-sentence and are removed together with the gap around them; dashes, curly quotes and ellipses are replaced with their ASCII equivalents, because dropping them would turn "Aufgabe – heute" into "Aufgabe heute" |
| Side button while idle shows the last answer again | The screen dims after 30 s and sleeps after 5 minutes; whoever looks later never read the answer |
| Connection dot carries the real link state (filled green connected, empty ring disconnected) | It used to be derived from the scene, so a bridge dropping mid-screen was only noticed at the next attempt |
| `device_name` control event replaces the advertised `VS-XXXX` on the pairing screen | With two sticks, the Todoteck name is the only way to tell which one is in your hand. It sits where it is needed — on `pairing`, as its hint — and no longer in the header of every screen |
| Tecki instead of the upstream cats: the Todoteck mark itself, given eyes, drawn from LVGL primitives | The cats said nothing about Todoteck. Drawing the mascot from shapes instead of six 112×112 ARGB8888 blobs frees **294 KB of flash** and makes the figure editable without re-rendering and committing images |
| Firmware builds in GitHub Actions (`.github/workflows/firmware.yml`) | Nobody needs a local ESP-IDF toolchain to get a flashable image |

Both protocol additions are optional in both directions: a bridge that does not
know `recording_discarded` simply uploads the aborted take, and firmware that
does not know `device_name` ignores it.

### The Mascot

Tecki is not a second drawing — he is the Todoteck mark plus a face layer
computed from it. The geometry is normative and lives in the
[Todoteck repository](https://github.com/Fauteck/todo/blob/main/docs/maskottchen.md);
`ui_status_icons.c` derives everything from a single constant, `MARK_WIDTH`.

Two things about him are decided here rather than there:

- **Two greens.** `ui_status.c` paints a cream ground (`0xfff7ed`) in every
  scene but `Ruht`, which goes dark (`0x1b2430`). The brand green `0x2ecc71`
  reaches only 1.98:1 on cream — below the 3:1 WCAG asks for graphical
  objects — so the light scenes use `0x1e9e56` (3.25:1) and only the resting
  scene keeps `0x2ecc71` (7.45:1).
- **101.5, not 112.** Three scenes tilt the figure around its bend, and
  rotating about a point that is not the centre moves the bounding box. 101.5
  is the widest the mark can be while all six poses stay inside the 112 px
  square; wider and the tip in `Fehler` leaves the frame on the right.

What he deliberately does not do: no blinking (the battery is small), no sound waves while
recording (the level meter and the time bar are already there), no warning
sign on error (the body turns red and the status line says so). A third signal
for the same thing is noise.

There is no speaking pose, because this firmware has no speaking state — the
answer is shown as `Bereit` with text. That pose exists in the Todoteck
repository and is waiting for text-to-speech.

The three helper scripts in `scripts/` that built the cat sprites
(`slice_cat_sprites.py`, `tune_cat_sprites.py`, `png_to_lvgl_argb_bin.py`)
have no consumer any more. They are left in place on purpose: they are
upstream tooling, and deleting them would only add conflicts the next time
`upstream` is merged.

## Project Layout

- `firmware/`: ESP-IDF firmware for M5Stack StickS3 / ESP32-S3.
- `firmware/components/ui_status/`: display, fonts, level and time bars, link dot.
- `firmware/components/audio_tone/`: acknowledgement tones over the ES8311.
- `desktop/macos/`: Swift Package for the native macOS menu bar app (upstream).
- `desktop/windows/`, `desktop/linux/`: upstream desktop workspaces.
- `docs/protocol.md`: BLE protocol between StickS3 and host, including the fork additions.
- `docs/volcengine-asr.md`: trimmed Volcengine ASR notes used by the upstream desktop client.
- `docs/release.md`: upstream release process (not used by this fork).
- `scripts/`: upstream packaging and release helpers, the three cat-sprite tools that lost their consumer with Tecki, and `check-docs.py`, which checks this documentation against the code.

## Documentation Contracts

Parts of this README and of `docs/protocol.md` restate sets that live in the
code — UUIDs, the states the firmware announces, dependency names. Those parts
are marked:

```text
<!-- doku-vertrag:name --> ... <!-- /doku-vertrag -->
```

`scripts/check-docs.py` compares every marked region against the source and
fails if they have drifted apart; `.github/workflows/doku.yml` runs it on each
push and pull request touching the docs or the firmware. Run it yourself with
`python3 scripts/check-docs.py` — it needs nothing but Python, and the script
itself is the list of what is under contract.

Wording around a marked region can be rewritten freely: the contract holds the
claim, not the sentence. A missing marker is a failure too, so a contract
cannot be dropped by deleting a comment. Everything outside the markers is
prose, examples and reasoning, and is deliberately not checked — those cannot
go stale without someone reading them.

## Firmware Features

- StickS3 advertises as `VS-XXXX`, where `XXXX` is derived from the last two bytes of the eFuse MAC. A `device_name` control event replaces that name in the header.
- The front button maps to the protocol `primary` role; it starts a recording session on press and ends it on release once the host has put the device in `ready`. A press shorter than 500 ms is discarded (`Zu kurz`), and a press while a recording is already running aborts it — both emit `recording_discarded`.
- The firmware reads 16 kHz mono PCM from the ES8311 microphone, encodes it as Opus, and sends it over BLE notifications.
- Recording is capped at 30 seconds. A bar counts the remaining time down; at zero the firmware sends the take like a normal button release.
- The side button aborts a running recording, emits `recording_discarded`, shows `Abgebrochen` and plays the discard tone.
- The screen shows pairing, ready, listening (`Hört zu`), thinking (`Denkt nach`), pending confirmation, error and battery states from host-sent `ui_state` updates. Text arriving with `ready` is rendered as the answer and kept for recall (up to 192 bytes).
- Text that does not fit below Tecki takes the whole screen and Tecki is hidden for as long as it is shown; the font is the largest size that fits, and anything beyond even the smallest ends in an ellipsis. Characters outside the fonts' Latin-1 range are replaced with ASCII where an equivalent exists and dropped otherwise.
- The screen dims after 30 seconds of inactivity. On battery power it enters deep sleep after 5 minutes; while charging or USB powered it stays at the dimmed-screen stage. The front button wakes it from deep sleep.
- BLE OTA updates over the two 3 MB app slots of the OTA partition table.

Tones share the I2S lines with the microphone: `audio_tone_play()` stays silent
while the capture path is up. That is why the start tone is played *before*
`audio_pipeline_start()` — which makes it the "speak now" cue at the same time.

## Hardware Target

- Board: M5Stack StickS3 / ESP32-S3-PICO-1-N8R8
- Front button: GPIO11, protocol `primary`, push-to-talk, abort on a second press, and deep-sleep wake
- Side button: GPIO12, protocol `secondary`, abort, cancel, or recall the last answer
- IMU: BMI270 on the shared I2C bus (0x68), used for the wrist-raise gesture; its interrupt line (documented as G4) is unverified and unused
- PMIC IRQ: GPIO13
- Audio codec: ES8311 over I2S, 16 kHz / 16 bit / mono, speaker on the same lines
- Display: 135 x 240 ST7789P3 panel, driven in landscape (240 x 135) via the controller's axis swap
- LCD backlight: GPIO38 PWM

Main pin definitions live in `firmware/components/stick_s3_board/include/stick_s3_board.h`.

## Interaction Model

| State | Front button | Side button |
| --- | --- | --- |
| Unpaired / disconnected | No recording; screen shows `VS-XXXX` | No effective action |
| Connected idle | Hold to record — a press under 500 ms is discarded with `Zu kurz` | Show the last answer again on the device; the click still reaches the host, where the upstream app restores the last input confirmation |
| Recording | Release to finish recording; press again to abort it, nothing is sent | Abort the recording, nothing is sent |
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

One GATT service with five characteristics — audio and state as notifications
from the stick, control writes and the two OTA channels in the other direction.
UUIDs, frame layouts and the full event list live in
[docs/protocol.md](docs/protocol.md) and are not repeated here.

The fork adds one event in each direction, both optional: `recording_discarded`
(stick to host, a take was thrown away on the device) and `device_name` (host to
stick, the name shown in the header). A counterpart that does not know them
keeps working.

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

### Getting the image without a GitHub login

Runs on `main` additionally attach both images, plus a small `firmware.json`
manifest (commit, build time, sizes, SHA-256), to the rolling pre-release
[`firmware-latest`](https://github.com/Fauteck/todoteck-voicestick/releases/tag/firmware-latest).
Artifacts stay for per-commit debugging, but they are a poor pickup path: an
artifact is always a ZIP, expires after 90 days, and needs a token even on a
public repository. A release asset is a single file at a fixed URL:

```sh
curl -LO https://github.com/Fauteck/todoteck-voicestick/releases/download/firmware-latest/voice_stick.bin
```

That is what `scripts/fetch-firmware.ps1` in the
[Todoteck repository](https://github.com/Fauteck/todo) uses to drop both images
into the shared Drive folder the phone flashes from — no credentials involved.

The tag is deliberately not a `v*` one: `release.yml` triggers on `push: tags:
["v*"]` and would check the tag against `VERSION`, upload to Aliyun OSS and
kick off a website deploy in the **upstream** repository.

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

<!-- doku-vertrag:firmware-abhaengigkeiten -->
- `espressif/bmi270`
- `espressif/button`
- `espressif/esp_codec_dev`
- `78/esp-opus`
- `lvgl/lvgl`
<!-- /doku-vertrag -->

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
