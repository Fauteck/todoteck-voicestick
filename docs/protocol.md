# Voice Stick Protocol

This document describes the protocol implemented by the firmware. It has two
hosts: the upstream desktop apps under `desktop/`, and the Todoteck bridge in
the [Todoteck repository](https://github.com/Fauteck/todo). Where a section
speaks only for one of them, it says so; the goals below are upstream's.

## Goals

- Low-latency push-to-talk audio from StickS3 to macOS.
- Opus over BLE to keep wireless bandwidth low.
- Ogg Opus forwarding from macOS to either Volcengine ASR or the VoiceStick Cloud relay.
- Final ASR text insertion into the focused macOS input field after release and confirmation.

## BLE GATT

Device name: `VS-XXXX`, where `XXXX` is derived from the last two bytes of the device eFuse MAC.

Service UUID:

<!-- doku-vertrag:gatt-dienst -->
```text
8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
```
<!-- /doku-vertrag -->

Characteristics:

<!-- doku-vertrag:gatt-merkmale -->
| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `audio_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101` | StickS3 -> host | notify |
| `state_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102` | StickS3 -> host | notify |
| `control_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103` | host -> StickS3 | write without response |
| `ota_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104` | host -> StickS3 | write, write without response |
| `ota_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105` | StickS3 -> host | notify |
<!-- /doku-vertrag -->

The desktop app scans for this service and only connects to devices whose `VS-XXXX` ID is present in the local paired-device list. Multiple paired devices may be connected at the same time; audio, state, control, and OTA handling are scoped by CoreBluetooth peripheral identity.

## Audio Frame

All multibyte fields are little-endian.

```text
struct AudioBleFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x01 audio
  uint16_t header_len;    // 16
  uint32_t session_id;
  uint32_t seq;
  uint8_t  flags;         // bit0=start, bit1=end
  uint8_t  reserved;      // currently 0
  uint16_t payload_len;
  uint8_t  payload[payload_len];
}
```

The payload contains one raw Opus packet when `payload_len > 0`. The firmware currently encodes 60 ms of 16 kHz mono audio per packet. When recording stops, the firmware also sends an end frame with `flags & 0x02` and an empty payload.

The macOS app wraps incoming Opus packets into an Ogg Opus stream before sending them to ASR. It does not decode Opus to PCM.

## State Event

All multibyte fields are little-endian.

```text
struct StateBleFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x10 state
  uint16_t payload_len;
  uint8_t  json[payload_len];
}
```

State events report device facts from the firmware to the app. They do not carry
business actions such as "cancel" or "confirm"; the app owns that interpretation.

Currently emitted state events:

<!-- doku-vertrag:device-info -->
```json
{"event":"device_info","hardware":"stick_s3","firmware_version":"0.3.2","buttons":["primary","secondary"],"interaction_modes":["hold_to_talk","click_to_talk"],"ui_states":["ready","recording","thinking","pending_confirmation","error","no_speech"]}
```
<!-- /doku-vertrag -->

```json
{"event":"button_down","button":"primary","session_id":1234}
{"event":"button_up","button":"primary","duration_ms":620,"session_id":1234}
{"event":"button_down","button":"secondary"}
{"event":"button_up","button":"secondary","duration_ms":90}
```

Buttons are named by role instead of physical placement, and the Todoteck fork
is the reason that matters: it swapped the two pins in 08/2026 without changing
a byte on the wire. `session_id` is included when a `primary` press starts or
stops a local audio recording.

Which pin carries which role on StickS3:

<!-- doku-vertrag:tasten-rollen -->
| Role | Button | GPIO |
| --- | --- | --- |
| `primary` | side (bottom edge in landscape) | 12 |
| `secondary` | front (the large blue one) | 11 |
<!-- /doku-vertrag -->

Upstream firmware has it the other way round. A host cannot tell and does not
need to: it sees roles, never pins.

### Fork addition: `recording_discarded`

The Todoteck fork emits one further state event when a recording is thrown away
on the device:

```json
{"event":"recording_discarded","session_id":1234}
```

It is a device fact, not an app action: tearing the capture path down always
produces a normal END audio frame, so without this event a discarded take is
indistinguishable from a completed utterance and the bridge would upload it.
The event is sent before the END frame. A bridge that does not know it keeps
working — it simply uploads the discarded take, exactly as before.

Three situations produce it, and the app does not need to tell them apart:

| Situation | Screen |
| --- | --- |
| The `primary` button is pressed *again* while recording | `Abgebrochen` |
| The `primary` button was held for less than 500 ms | `Zu kurz` |

A `secondary` press while recording used to be a third situation. It is not any
more: since that role moved to the large front button, it is the one that fires
by accident on a wrist, and an accidental press must not be able to destroy a
sentence in progress. The abort now lives only on the button that is already
under the finger.

The 500 ms match `MIN_TURN_MILLIS` in the Todoteck bridge, which dropped such
takes anyway — silently, and only after they had been transferred.

This is deliberately *not* called `cancel`; see the deprecation table below.

Deprecated firmware-to-app events:

| Event | Replacement | Reason |
| --- | --- | --- |
| `press_start` | `button_down` with `button:"primary"` | The old name assumed the front button and implied recording semantics. |
| `press_end` | `button_up` with `button:"primary"` | The old name implied recording semantics and did not include a button role. |
| `cancel` | `button_down` / `button_up` with `button:"secondary"` | The old event encoded app meaning; the same button can cancel, restore, or be ignored depending on app state. |

## Control Event

The Mac writes compact JSON to `control_rx`. Control events are authoritative UI
state from the app to the firmware display.

Current desktop events:

```json
{"event":"ui_state","state":"ready","text":""}
{"event":"ui_state","state":"recording","text":""}
{"event":"ui_state","state":"thinking","text":"partial text"}
{"event":"ui_state","state":"pending_confirmation","text":"final text"}
{"event":"ui_state","state":"error","text":"ASR timeout"}
{"event":"interaction_mode","mode":"hold_to_talk"}
{"event":"interaction_mode","mode":"click_to_talk"}
```

The desktop helper always includes a `text` field, even for states without text
content. Firmware may immediately render local physical feedback, such as
switching Tecki to the recording pose when the primary button starts audio, but
the app's `ui_state` is the authoritative display state.

### Fork addition: `question` on `ui_state`

The Todoteck bridge adds one optional field to the `ready` state:

```json
{"event":"ui_state","state":"ready","text":"Aufgabe angelegt: Pool rückspülen","question":"Leg eine Aufgabe an, Pool rückspülen"}
```

`question` is what the host understood — the transcript, not the answer. The
fork shows it as the headline above the answer and keeps both in a five-entry
ring that the `secondary` button steps through. It rides in the same write as
the answer so the two cannot drift apart, and it is budgeted at 64 bytes:
inside one 244-byte write the answer is the content and the question is its
label.

Optional in both directions. A host that does not send it leaves the screen as
it was (the status word `Bereit` above the answer); firmware that does not know
it ignores an unknown JSON field, as it always has.

The fork renders the `text` field: it arrives with `ready` and is shown as the
answer, in the largest of six font sizes that fits, with Tecki stepping aside
for text that needs the whole screen. Upstream firmware ignores it for anything
but picking a fixed English hint, because its LVGL font set is ASCII-only. Both
behaviours are legal for a host — a bridge cannot tell from the protocol which
one it is talking to, and the fork's fonts stop at Latin-1 either way.

### Fork addition: `ui_state:no_speech`

The Todoteck fork accepts one further UI state:

```json
{"event":"ui_state","state":"no_speech","text":"Ich habe nichts gehört."}
```

It means the turn ran but nobody spoke: the recording was silent, or the
recognizer invented a sentence out of that silence. Whisper has no way to say
"there was nothing" — it must emit tokens, and on German silence it falls back
to the subtitle credit it saw at the end of thousands of training videos
("Untertitelung des ZDF, 2020"). The Todoteck server detects that case and
answers the bridge with the reason `no_speech` instead of acting on the
invented sentence.

The device shows `Nichts gehört` and plays the short low discard tone, not the
two falling error tones: nothing is broken, the button simply fired in a
pocket. Without the state, a false trigger arrived as an ordinary answer — the
screen showed a failed wiki search for a phrase nobody had said.

A bridge learns whether the firmware knows the state from `ui_states` in
`device_info`; the Todoteck bridges fall back to `error` when it is missing,
because an unknown state is ignored and would leave the display sitting on
`thinking`.

### Fork addition: `device_name`

The Todoteck fork accepts one further control event, which replaces the
advertised `VS-XXXX` name in the header with the name the device carries in
Todoteck:

```json
{"event":"device_name","name":"Handgelenk"}
```

The name survives a disconnect, so the header keeps saying which of two sticks
is in your hand. Firmware that does not know the event ignores it.

### Fork addition: `time`

The Todoteck fork accepts a second control event, because the StickS3 has
neither an RTC chip nor Wi-Fi and therefore no clock of its own:

```json
{"event":"time","epoch":1756400000,"tz_offset_min":120}
```

`epoch` is UTC seconds since 1970, `tz_offset_min` the offset of local time in
minutes (120 for CEST). The firmware sets its system clock from it and renders
`HH:MM` on the pairing, ready and resting screens — the device is worn on a
wrist, and a watch that shows no time is a watch nobody looks at.

Sending an offset instead of a timezone name keeps a timezone database out of
the firmware. A host that never sends the event leaves the device without a
clock; the screens then look exactly as they did before, so this event is
optional like the others. Bridges send it right after connecting, which also
handles DST changes and the drift of the module's oscillator — the system clock
survives deep sleep but is not accurate over days, and the display therefore
shows no seconds.

`interaction_mode` controls the front-button behavior and idle screen hint.
`hold_to_talk` starts audio on primary down and stops on primary up.
`click_to_talk` starts audio on the first primary click and stops on the next
primary click.

Deprecated app-to-firmware events:

| Event | Replacement | Reason |
| --- | --- | --- |
| `connected` | `ui_state:ready` | Connection is not a display state after pairing. |
| `partial` | `ui_state:thinking` with `text` | Partial text is display content for the thinking state. |
| `final` | `ui_state:pending_confirmation` with `text` | Final text is still cancellable until pasted. |
| `paste_done` | `ui_state:ready` | Once pasted, the device returns to ready. |
| `paste_cancelled` | `ui_state:ready` | Once cancelled, the device returns to ready. |
| `error` | `ui_state:error` with `text` | Errors are another UI state. |

## BLE OTA

The firmware uses a custom OTA channel over the same Voice Stick service. The macOS app writes OTA `begin` and `end` frames with BLE write-with-response, and streams OTA `data` frames with write-without-response using CoreBluetooth flow control.
The device sends progress notifications roughly every 32 KB of accepted firmware data.

The macOS app starts OTA for one connected device at a time. It discovers updates from the latest firmware manifest, downloads the manifest `ota_url`, verifies byte size and SHA-256, then sends the verified app-slot image over BLE. The browser flasher uses the manifest `merged_url` instead because USB flashing writes a merged image at offset `0x0`.

The 8 MB flash layout uses two 3 MB OTA app slots and keeps the remaining flash as a reserved SPIFFS data partition:

| Name | Offset | Size |
| --- | ---: | ---: |
| `ota_0` | `0x10000` | 3 MB |
| `ota_1` | `0x310000` | 3 MB |
| `storage` | `0x610000` | 1984 KB |

All multibyte fields are little-endian.

```text
struct OtaBeginFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x20 begin
  uint16_t header_len;    // 12
  uint32_t image_size;
  uint32_t transfer_id;
}

struct OtaDataFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x21 data
  uint16_t header_len;    // 12
  uint32_t transfer_id;
  uint32_t offset;
  uint8_t  payload[];
}

struct OtaEndFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x22 end
  uint16_t header_len;    // 12
  uint32_t transfer_id;
  uint32_t image_size;
}

struct OtaAbortFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x23 abort
  uint16_t header_len;    // 8
  uint32_t transfer_id;
}
```

`ota_tx` sends a state frame:

```text
struct OtaStateFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x30 OTA state
  uint16_t payload_len;
  uint8_t  json[payload_len];
}
```

OTA state events include:

```json
{"event":"ready","transfer_id":1,"size":1385760,"partition":"ota_1"}
{"event":"progress","transfer_id":1,"written":32768,"size":1385760}
{"event":"done","transfer_id":1,"reboot_ms":500}
{"event":"error","code":"bad_offset","esp_err":258}
{"event":"aborted"}
```

On the device display, OTA switches the normal idle/recording UI into an update state:

- `Updating` with percentage while the image is being written.
- `Rebooting` after the new boot partition is selected.

While OTA is active, the device ignores push-to-talk input and pauses display dimming/deep sleep timers. After a successful transfer, the firmware waits about 500 ms after sending the `done` event and then calls `esp_restart()`.
The desktop updater can cancel an in-progress transfer by sending `OtaAbortFrame`; the device aborts the OTA handle and keeps booting the current firmware.

## Runtime State Machine

StickS3:

```text
boot -> advertising -> connected -> idle -> recording -> idle
```

The firmware also dims the display after 30 seconds of idle time. On battery power it enters deep sleep after 5 minutes; while charging or USB powered it stays at the dimmed-screen stage. The front button wakes the device from deep sleep.

macOS:

```text
needs_pairing -> scanning -> ready -> recording -> thinking -> pending_confirmation -> ready
```

During recognition and confirmation, the firmware keeps showing the thinking
pose until the app sends `ui_state:ready`. During pending confirmation, `primary`
confirms or pauses according to the app's internal countdown mode, and
`secondary` cancels. When idle, `secondary` restores the last recoverable input
confirmation. These meanings are app state-machine behavior, not firmware
protocol events.

Recordings shorter than 0.5 seconds are discarded locally and are not sent to ASR.

## ASR Transport

The desktop app can connect either directly to Volcengine or to VoiceStick Cloud. Both providers use the same WebSocket binary framing in the client, so request, audio, response, and error handling are shared.

Volcengine endpoint:

```text
wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async
```

VoiceStick Cloud default endpoint:

```text
wss://api.xiaozhi.me/voicestick/asr/
```

The first request payload currently sent by the desktop app is:

```json
{
  "user": {"uid": "voice-stick-local"},
  "audio": {
    "format": "ogg",
    "codec": "opus",
    "rate": 16000,
    "bits": 16,
    "channel": 1
  },
  "request": {
    "model_name": "bigmodel",
    "enable_nonstream": true,
    "show_utterances": false,
    "enable_ddc": true
  }
}
```

The desktop app buffers Ogg chunks until the recording reaches 0.5 seconds, then starts ASR and flushes the buffered chunks. On button release, it sends the final Ogg chunk with the WebSocket last-packet flag and waits for the final response.

VoiceStick Cloud business errors should use the same error frame shape as Volcengine: message type `0x0f`, a four-byte big-endian error code, a four-byte big-endian message size, and a UTF-8 message. For quota or billing errors, the message should be JSON so the desktop app can surface an upgrade action:

```json
{
  "error": "quota_exceeded",
  "message": "Daily free quota has been used up.",
  "upgrade_url": "https://voicestick.app/account/billing"
}
```

See `docs/volcengine-asr.md` for the trimmed Volcengine API notes used by the desktop app.
