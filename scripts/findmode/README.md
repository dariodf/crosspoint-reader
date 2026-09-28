# Testing find mode

Find mode lets the owner of a lost X4 Pro find it with a phone. While asleep,
the reader wakes every few minutes, listens for half a second for a secret
code the phone broadcasts, and when it hears it, shows "Find mode activated.
Press power to close." and broadcasts `CP-FIND` for a minute so a phone
scanner can follow the signal strength to it. The code lives in
`lib/FindMode/`, `src/FindModeRuntime.*` and
`src/activities/settings/FindModeCodeActivity.*`.

This page explains how it was checked and how to check it again. Almost all
of it runs with nobody touching the device.

## What was checked, and how

| Level | What it covers | Where | Needs a person |
| --- | --- | --- | --- |
| Host tests | Code matching (byte order, malformed packets), sleep state and checksum, crash switch-off, mute, found screen flag, journal ring | `test/find_code/`, `ctest --test-dir build/test` | No |
| Dev board suite | The real firmware on an ESP32-S3-DevKitC-1: timer wakes, listening, broadcasting, power button, mute, timeouts, random address, injected faults, found screen images | `run_suite.py` | One power press at the end |
| Reader | The panel, touch screens, battery gauge, a real phone, real battery drain | By hand | Yes |

The dev board runs the unchanged `x4pro` firmware: the X4 Pro is built for the
same board definition (`esp32-s3-devkitc1-n16r8`). Without a panel or SD card
the firmware still reaches its "SD card error" screen, and the find-mode fast
path runs before either is touched, so it behaves as on the reader.

## The test build

`CROSSPOINT_FIND_MODE_TEST_HOOKS` adds, and only in builds that set it:

- **A journal** (`lib/FindMode/FindJournal`): each wake records its steps in
  RTC memory with a timestamp from the RTC clock, which keeps counting through
  deep sleep. The fast path is over in under a second and USB serial drops at
  every sleep, so log lines rarely reach a computer; the journal does.
- **Serial commands**, read in `loop()` and, during the fast path, every time
  it checks the power button:

  | Command | Effect |
  | --- | --- |
  | `CMD:FIND_SLEEP` | Go to sleep, like a power-button hold |
  | `CMD:FIND_PRESS` | A virtual power press (fast path only) |
  | `CMD:FIND_JOURNAL`, `CMD:FIND_JOURNAL_CLEAR` | Print or empty the journal |
  | `CMD:FIND_STATE` | Print the find-mode state as `key=value` |
  | `CMD:FIND_INJECT crash\|hang\|radiofail\|battery <percent>` | A fault for the next timer wake |
  | `CMD:FIND_RETRY` | Clear a crash switch-off |
  | `CMD:FIND_PREVIEW <language> <orientation>` | Draw the found screen, for `CMD:SCREENSHOT` |

A dev board has no SD card for `settings.json`, so test builds for it also set
the find-mode settings as flags. The environment used for the runs below, in
`platformio.local.ini` (not committed), with `partitions.local.csv` for the
board's 8 MB of flash:

```ini
[env:x4pro-devkit]
extends = env:x4pro
board_build.flash_size = 8MB
board_upload.flash_size = 8MB
board_upload.maximum_size = 8388608
board_build.partitions = partitions.local.csv
build_flags =
  ${env:x4pro.build_flags}
  -DCROSSPOINT_FIND_MODE_TEST_CODE='"c0de0001-f1d0-4b1e-9a5e-000000000001"'
  -DCROSSPOINT_FIND_MODE_TEST_INTERVAL_MINUTES=1
  -DCROSSPOINT_FIND_MODE_TEST_MIN_BATTERY=15
  -DCROSSPOINT_FIND_MODE_TEST_HOOKS=1
```

```csv
# partitions.local.csv: one app slot, same size as upstream's
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x640000,
```

Flash it with `pio run -e x4pro-devkit -t upload`.

## Running the suite

```sh
python3 -m venv .venv && .venv/bin/pip install pyserial bleak bless pillow
.venv/bin/python scripts/findmode/run_suite.py --port '/dev/cu.usbmodem*' --out findmode-suite --ble-python .venv/bin/python
```

Give the terminal Bluetooth permission (macOS: System Settings > Privacy &
Security > Bluetooth). Start with the device awake, for example right after
flashing. The computer plays the owner's phone with `emit.py` and watches for
`CP-FIND` with `monitor.py`. The suite prints a PASS or FAIL line per check
with the value it measured, and writes `suite.log`, `report.txt`, `monitor.log`
and the screenshots into `--out`. `--only a,b` runs single scenarios (names
below), `--skip-button` leaves out the last one.

| Scenario | Checks |
| --- | --- |
| `found_screens` | The found screen in English and Spanish, in all four orientations, saved as PNG |
| `quiet_and_heard` | Two quiet wakes finish within 900 ms of boot; timer wakes 60 ± 4 s apart; the code is heard within 900 ms; the screen is drawn once |
| `press_and_mute` | A press in found mode boots normally and mutes; a muted wake hears the code and stays silent; a quiet wake re-arms |
| `timeout_and_address` | A broadcast ends by itself at 60 ± 1.5 s; every found session uses a new address |
| `radio_failure_keeps_mute` | A radio that fails to start leaves the mute in place |
| `crashes_switch_off` | Each injected crash is counted; three switch the mode off; `FIND_RETRY` re-arms |
| `hang_guard` | A hang inside the fast path is aborted within 7 s and counted as a crash |
| `low_battery_and_button` | Below the minimum the device stops waking on the timer; only the power button wakes it (the one step that needs a person: on the dev board, touch GPIO3 to GND for about a second when asked) |

A full run takes about 25 minutes, most of it waiting for timer wakes.

## Results

Dev board, ESP32-S3-DevKitC-1 (WROOM-1 N8R8), `x4pro-devkit` build,
2026-09-27. Full run 4 (45 checks) plus run 5 (timeout and crash scenarios
again) and run 6 (screens after the wrapping fix): every check passed in at
least one clean run. Run 4's four failures were a suite bug fixed before run 5
(the sleep step while a broadcast was running) and an accidental power press
during the crash scenario.

| Check | Measured |
| --- | --- |
| Quiet wake, listening done | 823-837 ms after boot (limit 900) |
| Code heard | 382-506 ms after boot (limit 900) |
| Timer wakes, 60 s interval | 60.9 s apart |
| Broadcast timeout | 60,002-60,012 ms |
| Addresses | a new one for every found session (4 of 4, 2 of 2) |
| Mute | heard and silent, then a quiet wake re-arms: listens `[0, 1, 0]` |
| Radio failure | keeps the mute: listens `[4, 0, 1, 0]` |
| Crashes | counted 1, 2, 3; the third switches the mode off; retry re-arms |
| Hang guard | aborted after 6.7 s (limit 7), counted as a crash |
| Low battery | sleeps until the button; no timer wake; the press wakes it |
| Found screen | 8 of 8 images (English and Spanish, four orientations) |

Journal codes: listen results are `0` heard, `1` nothing heard, `4` radio
failed (`RadioResult` in `lib/FindMode/FindRadio.h`).

## What still needs a person

- **The panel.** The dev board has no display: the suite checks the drawn
  framebuffer, not how the e-ink panel shows it.
- **Touch screens.** The Find mode code screen and the settings rows run only
  on the reader.
- **The battery gauge.** A dev board has none; the fast path treats an
  unreadable gauge as "keep listening", and `FIND_INJECT battery` stands in
  for real readings.
- **A real phone.** The computer's advertisement matches what nRF Connect
  sends on a phone (verified once with a Pixel: `02011A 1107 <UUID,
  byte-reversed>`), but phones differ.
- **Battery drain.** Not measured. The journal's awake times and the wake
  counters are the stand-in until someone measures current.
- **A power-button wake from deep sleep.** A sleeping chip cannot receive a
  serial command, so the suite asks for one press at the end.
