# BambuddyLights

An ESP8266 stack tower light that shows the state of every printer in a
[Bambuddy](https://github.com/maziggy/bambuddy) server at a glance.

Status: **firmware compiles for the Wemos D1 mini; not yet tested on hardware
or against a live Bambuddy server.**

## Build and flash

The project uses [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

```bash
pio run                 # compile
pio run -t upload       # flash over USB
pio device monitor      # serial log, 115200 baud
```

Later updates need no USB: open `http://bambulight.local/update`, sign in, and
upload `.pio/build/d1_mini/firmware.bin`.

If your lamps are on when they should be off (active-low relay module), add
`build_flags = -DLAMP_ON_LEVEL=LOW` to `platformio.ini`.

## Using it

- **First boot:** join the WiFi network `BambuLight-XXXX` and the setup page opens.
  Enter your WiFi, the Bambuddy IP, port (default 8000), API key and a
  **device password**.
- **Afterwards:** `http://bambulight.local` (or the device IP) shows the light
  status and a lamp test. **Settings**, **Change WiFi** and **Firmware** ask for
  user `admin` and the device password. Without a device password those pages
  stay locked; factory reset to set one.
- **Secrets:** the API key and device password are never shown again on any
  page. Leave the field blank to keep the saved value.
- **Factory reset:** hold the button for 5 seconds after the device has started.
  Do not hold it while plugging in power; that puts the ESP in flash mode.
- **Relay boards:** turn off the "green blip when every printer is busy"
  setting, or the relay clicks every 3 seconds.

## How a printer is classified

All printers are read in one request, `GET /api/v1/bulk/printer-statuses?ids=…`.
Older Bambuddy versions without that route are read one printer at a time from
`GET /api/v1/printers/{id}/status`. First match wins:

| Result          | Condition                                                                 |
|-----------------|---------------------------------------------------------------------------|
| Needs attention | Not connected (can be turned off in settings)                             |
| Error           | An HMS entry with severity 1 (this covers a failed print's `print_error`) |
| Needs attention | `state` is `PAUSE`, HMS severity 2, or `awaiting_plate_clear` is true     |
| Free            | `state` is `IDLE`, `FINISH`, or `FAILED` with no error (a cancelled print) |
| Busy (no lamp)  | Anything else (`RUNNING`, `PREPARE`, ...)                                 |
| Needs attention | No status came back for that printer                                      |

Printers marked inactive in Bambuddy are ignored. Up to 32 printers; any beyond
that are listed as a warning on the status page.

## Light logic

Each lamp works independently, so green, orange and red can be lit at the same time.

| Lamp   | Solid                                                        | Blinking  |
|--------|--------------------------------------------------------------|-----------|
| Green  | 1 printer free                                               | 2 or more |
| Orange | 1 needs attention (paused, filament runout, plate not cleared) | 2 or more |
| Red    | 1 has an error (failed print, HMS error)                     | 2 or more |

## Startup and connection status

| Stage                                   | Lights                                         |
|-----------------------------------------|------------------------------------------------|
| Power on                                | All three on for 1 s (lamp test)               |
| AP / setup mode                         | Chase: green, orange, red, repeating           |
| Connecting to WiFi                      | Green and orange alternating                   |
| WiFi OK, connecting to Bambuddy         | Orange and red alternating                     |
| Bambuddy rejects the API key            | Red double-flash, pause, repeat                |
| Bambuddy lost after it was working      | Orange and red alternating; never stale status |

## Hardware

- Wemos D1 mini (ESP8266)
- 12V 3-colour tower light, steady type, common-anode ("NPN") wiring; buzzer wire optional
- ULN2003 driver module (switches all three lamps plus the buzzer)
- Mini buck converter, 12V to 5V
- 12V 1A adapter
- One push button

```
12V adapter ──┬── tower common (+)
              └── buck 12V→5V ── D1 mini 5V/GND
D1 (GPIO5)  → ULN2003 IN1 → OUT1 → green wire
D2 (GPIO4)  → ULN2003 IN2 → OUT2 → orange wire
D5 (GPIO14) → ULN2003 IN3 → OUT3 → red wire
D6 (GPIO12) → ULN2003 IN4 → OUT4 → buzzer wire (optional)
D3 (GPIO0)  → button → GND (hold 5 s = factory reset)
All grounds common.
```

No-solder alternative: an ESP8266 4-channel relay board ("ESP-12F Relay X4",
7–28V input) wired straight to the tower light. The relays click on every blink.

## Setup flow

1. On first boot, or after a factory reset, the device starts an access point
   `BambuLight-XXXX` with a captive portal.
2. The portal asks for the WiFi network and password, the Bambuddy IP and port,
   and the API key. Optional: static IP and poll interval.
3. Settings are saved to flash and the device restarts.
4. It connects to WiFi, then to Bambuddy, showing the patterns above.
5. If WiFi fails for about 60 s, it reopens the AP for 3 minutes, then retries.

## Talking to Bambuddy

- REST API at `http://<ip>:<port>/api/v1`, `X-API-Key` header, read-only key
  (Settings > API Keys).
- Fetch the printer list (`GET /printers/`, refreshed every minute), then all
  statuses in one bulk request every 5 s (configurable). In per-printer
  fallback the interval stretches to stay under the 100 reads a minute limit.
- Field names were taken from the Bambuddy source, not from a live server.

## Included extras

- All printers busy: short green blip every 3 s so the device is visibly alive (optional)
- Printer offline counts as "needs attention" (optional)
- Status page at `bambulight.local` with a lamp test
- Password-protected settings, WiFi change and firmware update pages

## Later ideas

- Buzzer on a new error or finished print, button as mute/acknowledge
- Blink N times to show the count (2, 3, 4) instead of "more than one"
- Night mode (lamps off or buzzer muted during set hours)
- Printer filter (one tower per rack)
- WebSocket (`/api/v1/ws`) instead of polling
