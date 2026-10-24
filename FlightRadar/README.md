# FlightRadar - a standalone ADS-B flight radar for the ESP32-S3

A desk-top radar that shows live aircraft around **your** position on a 3.5"
320x480 colour display. After you upload the firmware it runs on its own from
a USB power supply - no PC, phone or SD card needed.

* Live aircraft data combined from two independent free ADS-B networks (adsb.lol and adsb.fi)
* Global 7500 (hijack) and 7700 (emergency) squawk monitoring with red alert and audio beep
* Round radar scope, rotating sweep with fading trail, range rings, N/E/S/W
* Zoom 5 / 10 / 20 / 30 / 50 / 75 / 100 / 150 / 200 nautical miles with the knob
* Rotated aircraft icons (airliner, helicopter, light aircraft, military, unknown)
* Select an aircraft to see its callsign, ICAO, type, altitude, speed, heading,
  distance, departure/arrival airport codes and an aircraft photo when available
* Search for an aircraft by callsign/registration and have the radar lock on
  and keep it centred as soon as it appears, anywhere in range
* Airport/airfield point markers within range
* Green / amber / white themes, brightness, units (nm/km, ft/m, kt/km/h)
* Distinct audio alerts for military aircraft and for police/coastguard/air ambulance aircraft
* Optional live National Rail departure board for one station, swapped in for the radar screen
* Settings, WiFi and home position are saved in the ESP32's own flash, and survive firmware updates

> **Verification note:** the firmware compiles with Arduino ESP32 core 2.0.17
> for the ESP32-S3 N16R8 configuration. No physical board or display was
> available for hardware verification; run the DisplayTest sketch first and
> check the Troubleshooting section if the device behaves differently.

---

## 1. What you need

| Part | Notes |
|---|---|
| ESP32-S3-DevKitC-1 **N16R8** | 16 MB flash, 8 MB PSRAM. The PSRAM is **required** (the radar uses ~300 KB of picture buffers). |
| 3.5" ST7796S 320x480 SPI display | Touch not required. If your module happens to have a touch controller, it can be wired up and tested too - see WIRING.md section 1a and `TOUCH_ENABLED` in `config.h`. |
| Rotary encoder with push button | e.g. KY-040 / EC11 |
| 3 tactile push buttons | MENU, BACK, HOME |
| USB-C cable and 5 V USB power | one cable is all the power it needs |
| Jumper wires / breadboard | |
| 2.4 GHz WiFi with internet | The ESP32-S3 cannot use 5 GHz-only networks. |
| A computer (once) | To install the Arduino IDE and upload the firmware |

## 2. Install the Arduino IDE

1. Download **Arduino IDE 2.x** from <https://www.arduino.cc/en/software> and install it.
2. Start it once so it creates the `Arduino` folder in your Documents.

## 3. Install the ESP32 board package (version 2.0.17)

1. Open **File > Preferences** (macOS: *Arduino IDE > Settings*).
2. In *Additional boards manager URLs* add:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. **Tools > Board > Boards Manager...**, search `esp32`.
4. Find **esp32 by Espressif Systems**, use the version drop-down and select
   **2.0.17**, then click **Install**. (Not the newest 3.x - see `TFT_SETUP.md`
   for why.)

## 4. Install the libraries

Open **Sketch > Include Library > Manage Libraries...** (or the book icon).

1. Search **TFT_eSPI** - install **"TFT_eSPI" by Bodmer**, version **2.5.43**.
2. Search **ArduinoJson** - install **"ArduinoJson" by Benoit Blanchon**, version **7.x** (7.0 or newer; version 6 will not work).
3. Search **TJpg_Decoder** - install **"TJpg_Decoder" by Bodmer** to decode aircraft photos on the display.

Everything else (WiFi, HTTPClient, WebServer, DNSServer, Preferences, SPI) is
included with the ESP32 board package.

Aircraft photos are looked up automatically when the selected aircraft
changes. The display shows a photo when the aircraft has one on
Planespotters.net; the downloaded JPEG is held temporarily in memory for
decoding, and the decoded image is released when the selection changes
or is cleared. No photo is written to flash or an SD card. Planespotters
requires a contact address in the photo request User-Agent; configure
`PLANESPOTTERS_CONTACT` in `config.h`. Because Planespotters JPEGs can be
progressive, the image URL is sent to images.weserv.nl to convert it to a
208x112 baseline JPEG at high JPEG quality. The dimensions align with JPEG's
16-pixel MCU blocks; the firmware decodes these photos in blocks through
TJpg_Decoder, swaps the RGB565 byte order required by TFT_eSPI sprites, and
crops the small border to fit the panel.
The original image is tried as a fallback only if
it is also baseline JPEG; progressive files are rejected rather than shown as
a low-resolution pixelated preview. The contact address is sent only to
Planespotters, not to the converter. Sharpness is still limited by the photo
the provider has available and by the display's native pixel resolution.

JetAPI is the primary photo source. The firmware queries it by aircraft
registration using `JETAPI_BASE_URL` in `config.h` (default:
`https://www.jetapi.dev`) and tries up to four listed JetPhotos images. If
JetAPI has no usable image, Planespotters is tried as a fallback. Both sources'
CDN images are sent to images.weserv.nl for baseline JPEG conversion; the
Planespotters contact address is not sent to JetAPI or JetPhotos. If neither
source lists a photo, the panel shows `NO PHOTO`; JetAPI requires an aircraft
registration.

The ESP32 advertises the Wi-Fi hostname `FlightRadar` to the router. A router
may still show its manufacturer as a generic IoT device based on the ESP32's
hardware address; firmware cannot change that manufacturer label. Rename the
device in the router app if it continues to display the cached generic label.

Departure and arrival airport codes are looked up automatically for the
selected aircraft using its callsign and the ADSBdb API. The route is shown
when a match is available; some aircraft or callsigns may not have route data.

## 5. Configure TFT_eSPI (required, one time)

Follow **`TFT_SETUP.md`**: copy `extras/User_Setup.h` over
`Arduino/libraries/TFT_eSPI/User_Setup.h` and restart the IDE.

## 6. Wire everything

Follow **`WIRING.md`** (matches the wiring diagram you supplied). Short
version: display on GPIO 8/9/10/11/12/13 (+5V, GND - check your module's VCC
voltage, see WIRING.md), encoder on GPIO 4/5/6, buttons on GPIO 15/16/17,
audio amp on GPIO 7/18/21 (+5V, GND). **Note:** your original plan used
GPIO23 for the display data line - that pin does not exist on the ESP32-S3,
so it was changed to **GPIO11**.

## 7. Arduino IDE settings (Tools menu)

Open the `FlightRadar` folder (`FlightRadar.ino`) and set:

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** |
| USB CDC On Boot | **Disabled** if you use the USB-C port marked **UART**; **Enabled** if you use the one marked **USB** |
| CPU Frequency | 240 MHz (WiFi) |
| Flash Mode | QIO 80 MHz |
| Flash Size | **16 MB (128 Mb)** |
| Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** |
| PSRAM | **OPI PSRAM** (not "QSPI" - that makes the N16R8 crash at start) |
| Upload Speed | 921600 (use 115200 if uploads fail) |
| Core Debug Level | None |
| Port | the COM / tty port that appears when you plug the board in |

## 8. Run the display test FIRST

1. Open `DisplayTest/DisplayTest.ino` (a separate sketch folder).
2. Same Tools settings, click **Upload**, then open **Tools > Serial Monitor**
   (115200 baud).
3. Watch the screen and Serial Monitor:
   * Serial shows PSRAM found and the resolution (must be 320 x 480).
   * Full-screen **RED, GREEN, BLUE, WHITE, BLACK** - the colour must match the word.
   * Shapes/text: the white border must reach all four edges, text must read normally.
   * The backlight fades down and up (tests GPIO13 PWM).
   * Finally a live view of the encoder and the three buttons: turn and press
     them; the boxes turn green and the edge counter changes.
4. Fix any problem now (see Troubleshooting) - do not continue until all steps pass.

## 9. Edit `config.h`

Open `FlightRadar/config.h` (the second tab in the IDE) and set at least:

```cpp
#define WIFI_SSID     "MyWiFiName"
#define WIFI_PASSWORD "MyWiFiPassword"
#define HOME_LAT      51.4700      // your latitude  (north +)
#define HOME_LON      -0.4543      // your longitude (east +, west -)
#define TIMEZONE_POSIX "UTC0"      // examples for other zones are in the file
```

Find your coordinates by right-clicking your house in Google Maps (first line
is `latitude, longitude`). You may also leave WiFi empty and configure it on the
device (section 12). Every other option is explained by comments in the file.

## 10. Upload the radar

Open `FlightRadar/FlightRadar.ino`, check the Tools settings, and click
**Upload**. The first compile takes a few minutes. When it finishes the board
restarts.

## 11. Start-up and using the radar

Start-up sequence (about 5-20 seconds): **FLIGHT RADAR** -> **CONNECTING TO WIFI**
-> **CONNECTED** -> **Loading aircraft...** -> radar screen.

The screen is split into three parts: a header (range, mode, clock, status
dot), the radar, and an information panel. The dot is green = data OK, orange
= WiFi up but data problem, red = no WiFi.

### Controls

| Control | On the radar | In menus |
|---|---|---|
| Rotate encoder | **ZOOM mode:** clockwise = zoom **in**, counter-clockwise = zoom **out**. **SELECT mode:** cycle through aircraft (nearest first) | move highlight / change value |
| Short press encoder | ZOOM mode: **select the aircraft nearest the centre** (enter SELECT mode). SELECT mode: **confirm** (back to ZOOM mode, the aircraft stays highlighted) | open / edit / save |
| Long press encoder (0.7 s) | **open the menu** | close the menu, back to radar |
| MENU button | open the menu | close the menu |
| BACK button | clear the selection | cancel an edit / go up one level |
| HOME button | clear the selection, radar view | jump straight back to the radar |

**How the two modes work (so the knob is never ambiguous):** the radar starts
in **ZOOM** mode (turning = zoom). Press the knob once - the nearest aircraft is
selected and the header shows **SELECT**: now turning steps through the
aircraft. Press again to confirm; you are back in ZOOM mode with that aircraft's
details still on screen (press again to re-pick, BACK/HOME to clear). If the
selected aircraft leaves the range or disappears, the selection is cleared
automatically.

### Settings menu

**Range, Units** (distance / altitude / speed), **Theme** (green, amber, white),
**Brightness**, **Radar speed** (slow / normal / fast / off), **Home location**,
**WiFi status**, **About**. Press to edit a value, turn to change it (it applies
immediately), press to save or BACK to cancel.

**Home location** is edited digit by digit: turn to change a digit (or N/S,
E/W), press to move to the next one, and press on **SAVE** at the end.

Everything you change is stored in flash and survives power-off. (The range is
saved 3 seconds after you stop turning the knob, to spare the flash memory.)

## 12. WiFi set-up and recovery (no PC needed)

There are three ways to set or change WiFi / position:

1. **`config.h`** (before uploading) - if you edit WIFI_SSID, WIFI_PASSWORD, HOME_LAT or HOME_LON
   and upload again, the new values replace the ones saved on the device.
2. **Setup portal (any phone, tablet or laptop):** the device creates its own
   WiFi network called **FlightRadar-Setup**. Join it, open
   **http://192.168.4.1/** in a browser, enter WiFi name/password and your
   latitude/longitude, tap *Save and connect*. This AP starts by itself when
   no WiFi is configured or the first 5 connection attempts fail; you can
   also start it from **Menu > WiFi status > press**, or by **holding BACK
   while powering on**. Once the device connects to your home WiFi, this
   separate AP network switches itself off (so it's not permanently
   broadcasting its own network alongside yours) - except when started by
   holding BACK at power-on, which stays up deliberately since that's
   explicitly asking for setup mode. **The settings page itself doesn't go
   away when the AP does** - it keeps running the whole time the device is
   on, reachable at its normal IP on your home WiFi (shown on **Menu > WiFi
   status**) at `http://<that ip>/settings`, so you don't need to rejoin the
   setup AP just to change a setting later. Open **Advanced settings** there to configure the portal toggle and
   retry policy, time zone, ADS-B sources and polling, aircraft aging, audio,
   night dimming, radar labels/sweep, controls, and serial baud. Passwords are
   never shown back in the form; leave a password field empty to keep its saved
   value. **Restore defaults** restores firmware defaults and clears WiFi
   credentials and the home coordinates. Hardware pins, display wiring,
   firmware version, and memory-layout limits remain build-time-only.
3. **Serial Monitor commands** (configured baud; 115200 by default, "Both NL & CR"): `HELP`, `STATUS`,
   `SSID=name`, `PASS=password`, `LAT=51.47`, `LON=-0.45`, `PORTAL`, `RESTART`,
   `FACTORY` (erase saved settings).

The on-device **Menu > Advanced** tab provides quick toggles for the portal,
ADS-B source mode, night dimming, ground-aircraft visibility, audio, labels,
encoder direction/steps, and the automatic portal failure threshold. **Open
web settings** starts the setup AP (see section 12 above) so you can reach the
full configuration form from a phone/laptop even without home WiFi. **Switch
mode** (top of the Main menu) swaps the radar screen for the train departure
board - see section 13b - and back again. **Find aircraft** (also Main menu)
opens the callsign/registration search-and-lock screen - see section 13c.
**Restore defaults** asks for
confirmation, restores firmware defaults, and clears WiFi credentials and the
home coordinates. Serial baud changes and changes to the audio enable/URL take
effect after a reboot; both device and portal offer a reboot prompt and warn if
you defer it. **Test speaker** plays a clear test tone immediately, then the WAV
configured in `config.h` (or the saved Audio WAV URL), without changing the Audio
setting. It is also available from the web settings page and works when
announcements are disabled. On the device, select the outlined **TEST** button
and press the encoder.

If the WiFi is lost the radar keeps animating, shows **WIFI DISCONNECTED**, keeps
the last aircraft (they are removed after 60 s) and reconnects automatically
with increasing waits (3 s ... 30 s).

## 13. Military aircraft announcements

Any current or ex-military aircraft - a Typhoon, an RAF transport, a privately
owned Spitfire or Hurricane on the airshow circuit, etc. - plays a single
"military aircraft" sound through the amp/speaker the first time it comes
into range. It won't repeat for the same aircraft for
`MILITARY_ANNOUNCE_COOLDOWN_MS` (4 minutes by default), so a slow orbit
doesn't spam the speaker; after that, it will announce again if it's still
there.

**Where the clip comes from:** rather than compiling your recording into the
firmware, the sketch fetches it from a URL **once at startup** and keeps it
in PSRAM for the rest of the session - so changing the clip later is just
replacing the file at that link and rebooting the board (power-cycle it, or
type `RESTART` in the Serial Monitor), no re-flashing needed.

1. Record a short WAV clip (a spoken line like "Military aircraft" is
   plenty - a few seconds). Most phone voice recorders, Audacity, or an
   online text-to-speech site all work - export/save it as a **.wav** file
   named `military.wav` (or whatever you like - the name doesn't matter, only
   the URL does).
2. Host it somewhere that serves the **raw file directly** - a GitHub repo's
   "raw." link, a personal web server, an S3/R2/Cloudflare bucket. A Google
   Drive or Dropbox *share page* will NOT work (those serve a webpage, not
   the file) - Dropbox can work if you change the link's `?dl=0` to `?dl=1`;
   Google Drive needs its direct-download link form.
3. Put that link in `config.h`:
   ```cpp
   #define MILITARY_WAV_URL "https://raw.githubusercontent.com/you/repo/main/military.wav"
   ```
4. Upload the sketch once. From then on, every boot fetches whatever is
   currently at that URL.

If `MILITARY_WAV_URL` is left empty, or the fetch fails for any reason (no
WiFi yet, host unreachable, wrong format, file too large), the firmware
automatically falls back to a **synthesized two-tone alert siren** that's
compiled in as a safety net - not a voice: there's no text-to-speech engine or
internet access in the environment I build this project in, so an actual
spoken clip isn't something I can generate myself, but I can and did generate
real audio (a sine-sweep siren, via a short Python script). It's there so the
amp/speaker path always has something to play (DisplayTest step 7 also uses
it) even before you've set up hosting.

If you'd rather compile a clip in directly instead of hosting it (no WiFi
dependency at startup, works even if the host later goes offline), that's
still available: run `python3 extras/wav_to_header.py your_recording.wav` to
overwrite the placeholder in `audio_data.h`, and leave `MILITARY_WAV_URL`
empty so the firmware uses it.

**How a current vs. ex-military aircraft is recognised:** current military
aircraft are detected from the ADS-B feed's own military flag, its category
code, or a known fast-jet type; ex-military "warbirds" (Spitfire, Hurricane,
Lancaster, Mosquito, and similar) are recognised by aircraft type code; RAF
and a few other air forces are additionally recognised by callsign (ASCOT,
RRR, etc.). This list was written without being able to test it against live
traffic, so if a flyover you expect to trigger it doesn't: set
`DEBUG_PRINT_AIRCRAFT_TYPES` to `1` in `config.h`, re-upload, open the Serial
Monitor, note the `type=` code it prints for that aircraft, and add it to the
`HISTORIC_MIL_TYPES` list near the top of `aircraft.cpp`.

Right now there's only the one clip for every military aircraft, as asked -
if you later want different clips per operator (e.g. a distinct Spitfire
sound), that's a reasonably small change to `aircraft.cpp`/`audio.cpp` - just ask.

## 13a. Police / coastguard / air ambulance tones

Alongside the military announcement above, three UK emergency-service
categories get their own short, distinct beep pattern the first time they
come into range (same cooldown mechanism as military, so a slow orbit
doesn't repeat it): **police** (two-tone yelp), **coastguard** (descending
three-tone sweep), **air ambulance** (double-beep, twice). The header shows
`EMS n` (in place of `MIL n`) while any are visible, and the aircraft info
panel shows which category.

Detection is best-effort, by callsign/registration prefix (e.g. "POLICE" /
"NPAS", "RESCUE" / "COASTGUARD", "HELIMED" / "HEMS", plus common UK
registration prefixes like G-MCG for coastguard or G-HEMS/G-NAAC/etc. for air
ambulance) - written without being able to test it against live traffic, same
caveat as the military list. If a real unit near you doesn't trigger it, set
`DEBUG_PRINT_AIRCRAFT_TYPES` to `1` in `config.h`, watch the Serial Monitor
for the callsign/registration it actually reports, and add it to the
`POLICE_CALLSIGN` / `COASTGUARD_CALLSIGN` / `AIRAMB_CALLSIGN` (or `*_REG`)
lists near `aircraftClassifyService()` in `aircraft.cpp`.

## 13b. Train departure board

Press/hold whatever's mapped to "Switch mode" (Main menu) to flip the radar
screen over to a live National Rail departure board for one station, using
the Rail Data Marketplace's **Live Departure Board** API (REST/JSON - the
current replacement for the old OpenLDBWS SOAP service, which had its
self-service sign-up page retired). "Switch mode" again goes back to radar.

**Getting an API key (free, no usage limit):**

1. Register at <https://raildata.org.uk/registerPartner> - "Individual not
   affiliated with a company" is fine for personal use. Wait for the
   confirmation email (usually same-day).
2. Use the welcome email to set a password, log in, set up MFA, and accept
   the Terms of Use.
3. Open the Live Departure Board product page:
   <https://raildata.org.uk/dataProduct/P-d81d6eaf-8060-4467-a339-1c833e50cbbe/overview>,
   click **Subscribe**, accept the licence terms (charging model is Free, no
   usage limit), click **Subscribe for free**.
4. Click **View subscriptions**, open **Live Departure Board**, click the
   **Specification** tab, and scroll to **API access credentials**.
5. Copy the **Consumer key** shown there.
6. Paste it into the device's own web setup page (`http://<device-ip>/settings`
   -> "Train departures" section -> "Rail Data Marketplace API key" field) and
   save. **No source code access or reflashing needed** - this is a normal
   setting stored on the device, exactly like the WiFi password or home
   coordinates, so anyone using the finished device can set it themselves.
   It's also settable from the Serial Monitor (`TRAINKEY=your-consumer-key-here`)
   if you'd rather not use the web page. `OPENLDBWS_API_KEY` in `config.h` is
   only the one-time starting value used the very first time the firmware
   boots (or after a factory reset) - leave it blank if you're only ever
   going to set the key from the web page.
7. Set your station's 3-letter CRS code the same way - on-device (Advanced
   menu), the web setup portal's "Train departures" field, or `config.h`
   (`TRAIN_DEFAULT_STATION_CRS`, first-boot default only) - no reflash needed
   to change station.

If no API key is set, "Switch mode" just stays on the radar screen and the
train screen (if you do reach it) shows "NO RAIL DATA API KEY CONFIGURED". A
wrong or expired key shows as an HTTP 401/403 error on that screen -
re-check the Consumer key on the Specification tab above.

Below the departure list, the bottom panel cycles through the visible
departures a few seconds at a time with a proper station-announcer-style
line - time and destination, the stops it's calling at, and which operator
runs it (e.g. "10:15 TO BRIGHTON" / "CALLING AT: TBD, GTW, HOR, HOVE" /
"OPERATED BY SOUTHERN") - text on screen only, no speech (the hardware has
no text-to-speech capability). This uses the **GetDepBoardWithDetails**
endpoint rather than the plain GetDepartureBoard one; same key, same setup,
just a bit more data per service.

## 13c. Find aircraft (search-and-lock)

**Main menu > Find aircraft** opens an 8-character entry screen (turn to pick
each character, press to move on - same idea as the train station editor).
Enter a callsign (e.g. `BAW123`), a registration (e.g. `G-ABCD`, hyphen
optional - it's ignored), or the ICAO hex address shown for an aircraft that
isn't broadcasting either of those (e.g. `4CA1B2` - whatever's actually on
screen as that aircraft's label is always what to search for), and press
**LOCK**. From then on, the radar centres
on and auto-selects that aircraft the moment it's seen - even if it isn't in
range yet, or temporarily drops out and comes back - until you clear it.
An active 7500/7700 emergency still always takes priority for the radar's
centre. To clear a lock: open **Find aircraft** again, clear all 8
characters and press **LOCK**, or just manually select a different aircraft
(turning the knob to pick one, or pressing BACK/HOME on the radar screen)
- either clears the lock automatically. The header shows `LOCK <query>`
while searching/locked.

## 14. Where the data comes from

Default: the firmware queries **adsb.lol** and **adsb.fi** independently on
every update; neither is a fallback for the other. It joins aircraft with the
same ICAO address, takes the newer position report, and fills facts missing
from one response using the other. Each feed's last successful snapshot is
kept briefly if that feed temporarily fails. This combines independent
volunteer-receiver coverage without duplicating aircraft on the radar. Both
requests run in the background; the display does not wait for either network
call. `ADSB_MULTI_SOURCE_ENABLED` in `config.h` enables this partnership mode.
The Advanced setting can select both sources (default) or one source alone.
Separately, the firmware alternates free adsb.lol global squawk searches for
**7500** and **7700**. A reported target is highlighted in red, triggers an
urgent double beep, recenters the radar on its latest reported position, and
switches to a 5 NM follow view. This depends on adsb.lol's receiver coverage,
feed freshness, API availability and rate limits; it cannot guarantee worldwide
detection or identify a real hijack/emergency. Treat the transponder code as an
alert only and contact the appropriate authorities for real incidents.

| `ADSB_SOURCE` | Provider | Notes |
|---|---|---|
| `ADSB_SOURCE_ADSB_LOL` | api.adsb.lol | open community feed; no key currently, but this may change |
| `ADSB_SOURCE_AIRPLANES_LIVE` | api.airplanes.live | free but their guide asks users to request access for non-commercial use |
| `ADSB_SOURCE_ADSB_FI` | opendata.adsb.fi | open data for personal, non-commercial use |
| `ADSB_SOURCE_CUSTOM` | your own URL | same JSON format, `ADSB_CUSTOM_URL_FORMAT` |

The radar asks each selected provider once every 5 s by default
(`AIRCRAFT_UPDATE_INTERVAL`, minimum 2 s), within the documented limits. These
are free sources; no paid API is used. No FlightRadar24 scraping is used. The
endpoint documentation, fields used and how to add another source are in
`adsb_api.h`. Please respect each provider's terms; the project is for
personal use. Serial output logs each provider's successful aircraft count or
its specific failure reason, making it clear whether a successful response
simply added no new aircraft or the provider request failed.

Data is only as good as volunteer coverage: aircraft without ADS-B, or outside
any receiver's range, will not appear. **Not for navigation or safety use.**

### TLS (HTTPS) note
By default the connection is encrypted with HTTPS but the server certificate
is **not verified** (`ADSB_VALIDATE_TLS 0`), because certificates and their root
authorities change and a wrong built-in certificate would silently break the
radar. Nothing secret is sent (only your rounded position in the URL). To verify the server define
`ADSB_ROOT_CA_PEM` (PEM text of the provider's root certificate) in `config.h`
and set `ADSB_VALIDATE_TLS 1`.

## 15. How it behaves (technical summary)

* Distance and bearing use the great-circle (haversine) formula with correct
  longitude wrap-around; an aircraft due north appears at the top, east on the
  right, etc.
* Between updates an aircraft moves along its track at its ground speed;
  when new data arrives the drawn position **glides** to the new one over 1.5 s,
  never more than 20 s of extrapolation, and aircraft are removed after 60 s
  without a report.
* Missing fields never crash anything - they show as `N/A`.
* Downloading runs in a background task on the second CPU core and the radar
  keeps sweeping during slow or failed requests. Large replies are parsed as a
  stream, one aircraft at a time, and only the nearest 250 aircraft are kept.
* The picture is drawn into off-screen buffers in PSRAM and sent to the
  display (~20 frames per second at 40 MHz SPI).
* The encoder and buttons are read by their own task every millisecond, so no
  click is lost while the screen redraws.

## 16. Files

| File | Purpose |
|---|---|
| `FlightRadar.ino` | `setup()` / `loop()` only |
| `config.h` | **all user settings** |
| `settings.h/.cpp` | flash storage, units formatting, serial commands |
| `display.h/.cpp` | TFT init, sprites, backlight PWM, themes |
| `radar.h/.cpp` | radar scope drawing |
| `aircraft.h/.cpp` | aircraft list, smoothing, classification, selection |
| `adsb_api.h/.cpp` | live data download (endpoint documentation is at the top of `adsb_api.h`) |
| `wifi_manager.h/.cpp` | WiFi, retries, setup portal, clock |
| `input.h/.cpp` | encoder and buttons |
| `icons.h/.cpp` | aircraft icons |
| `audio.h/.cpp` | I2S playback of the military-aircraft announcement |
| `audio_data.h` | the compiled-in WAV clip (regenerate with `extras/wav_to_header.py`) |
| `ui.h/.cpp` | screens, menus, input handling |
| `geo.h/.cpp`, `util.h` | maths and small helpers |
| `extras/User_Setup.h` | TFT_eSPI configuration to copy into the library |
| `extras/wav_to_header.py` | converts your recorded WAV into `audio_data.h` |
| `../DisplayTest/DisplayTest.ino` | hardware test sketch (now also checks the amp/speaker) |
| `WIRING.md`, `TFT_SETUP.md`, `README.md` | documentation |

## 17. Troubleshooting

**Compile problems**

* *"TFT_eSPI is not configured for the ST7796S"* - `User_Setup.h` not installed (`TFT_SETUP.md`), restart the IDE.
* *"ArduinoJson version 7 is required"* - install ArduinoJson 7.x in the Library Manager.
* *"A GPIO in config.h is not usable"* - pick another pin (see `WIRING.md`).
* *"text section exceeds available space"* - Tools > Partition Scheme = **16M Flash (3MB APP/9.9MB FATFS)**.
* Errors mentioning `ledc...`, `SPI_PORT`, or TFT_eSPI internals - you probably have board package 3.x; install **2.0.17**.
* *"Sketch too big"/"not enough memory"* - check Flash Size = 16 MB.

**Upload problems**

* Port missing: try the other USB-C port, another cable (many are charge-only). To force boot mode hold **BOOT**, tap **RESET**, release BOOT.
* Upload fails half-way: set Upload Speed 115200.

**Screen problems**

* Blank/white: wiring, 3.3 V, BLK pin; run DisplayTest.
* Board restarts endlessly when the display starts: board package 2.0.17, toggle `USE_HSPI_PORT` (`TFT_SETUP.md`), PSRAM = OPI.
* Serial says `PSRAM: NOT FOUND` or the screen shows "NOT ENOUGH MEMORY": Tools > PSRAM = **OPI PSRAM**.
* Colours negative: `DISPLAY_INVERT_COLOURS 1`. Red/blue swapped: `TFT_RGB_ORDER` in `User_Setup.h`.
* Upside down: `DISPLAY_ROTATION 2`.
* Backlight flickers or brightness menu does nothing: `BACKLIGHT_PWM_ENABLED 0`. Backlight off: `BACKLIGHT_ACTIVE_HIGH 0` if the module is inverted.

**Control problems**

* Knob turns the wrong way: `ENCODER_REVERSE 1`. Needs two clicks per step (or one click jumps): `ENCODER_STEPS_PER_DETENT` 2 or 4.

**Audio problems**

* Serial prints `i2s_driver_install failed` or `i2s_set_pin failed`: double-check `AUDIO_I2S_BCLK_PIN` / `AUDIO_I2S_LRC_PIN` / `AUDIO_I2S_DOUT_PIN` in `config.h` match your wiring, and that none of them clash with another pin (the compiler checks this for you).
* No sound, but Serial shows `[audio] ready`: run DisplayTest step 7 to isolate whether it's the amp/speaker wiring or the firmware's triggering logic. Check VIN is 5 V, GND is common, and the speaker is on SPK+/SPK- (never GND).
* Serial prints `MILITARY_WAV ... is not a playable 16-bit PCM WAV`: re-run `extras/wav_to_header.py` on your file - some recording apps save 24-bit or 32-bit float WAV, which isn't supported, re-export as 16-bit.
* Serial prints `download failed: HTTP ...`, `download size is missing...`, or `downloaded file is not a playable WAV`: check the link opens the raw file directly in a browser (not a webpage around it), that it's 16-bit PCM WAV, and that it's under `MILITARY_WAV_MAX_BYTES` (2 MB by default). The firmware falls back to the built-in siren automatically either way, so this won't stop the rest of the radar working.
* Changed the file at your `MILITARY_WAV_URL` but still hear the old clip: it's only fetched once at boot - reboot the board (power-cycle, or `RESTART` in the Serial Monitor).
* A military aircraft flies over and nothing plays: see the debug-print tip in section 13 - the type code list is best-effort and may need one adding.
* Crackling/distorted sound, or the board resets when it plays: see the power-chain note in `WIRING.md` - likely a voltage sag under combined display+amp load.
* Nothing reacts: run DisplayTest step 6 to see which pin does not change.

**WiFi / data problems**

* Stuck on "CONNECTING TO WIFI": wrong name/password (case matters), 5 GHz-only network, weak signal. The setup portal appears after 5 failed attempts when enabled.
* "API ERROR: HTTP 403/401" - the provider now requires a key; change `ADSB_SOURCE`. "HTTP 429" - too many requests (raise `AIRCRAFT_UPDATE_INTERVAL`).
* "API ERROR: TIMEOUT" - slow internet or a very large area; use a smaller range.
* "API ERROR: BAD JSON" - provider changed format or an error page was returned; open the URL from `adsb_api.h` in a browser to test it.
* "NO AIRCRAFT IN RANGE" with WiFi/data OK - genuinely nothing there (try 100 nm), or `HOME_LAT/LON` is wrong (Menu > Home location).
* Clock shows `--:--:--` - NTP not reached yet; wait, or check `TIMEZONE_POSIX`.

Serial Monitor (115200) prints `[wifi]`, `[adsb]`, `[display]` lines - include them when asking for help.

## 18. Limitations

* Aircraft counts beyond `MAX_AIRCRAFT` (250) are trimmed to the nearest ones; at 200 nm over busy areas the outer edge can therefore be incomplete.
* SD card and GPS are deliberately not used. Touch is optional/experimental (off by default, see section 6) - the UI isn't designed around it, it's just available for boards that happen to have a touch controller.
* Airport/airfield markers are simple labelled points (a small list in `radar.cpp`, `SHOW_AIRPORTS` in `config.h`), not real coastline/lake vector outlines - embedding genuine geographic shape data isn't practical on this hardware.
* The night-dimming timer needs the clock (WiFi + NTP).
* Public feeds can lag by several seconds; positions are estimates between updates.

## 19. Licences and data

Firmware: use and modify freely for personal projects. TFT_eSPI (Bodmer,
BSD-style) and ArduinoJson (MIT) are installed separately by you. Aircraft
data: see each provider's terms (adsb.lol data is published under ODbL).
