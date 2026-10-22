/*
 * config.h - ALL user-changeable settings for the FlightRadar project.
 *
 * Edit the values in this file, save, then compile and upload again.
 *
 * Runtime-safe options are copied into persistent Settings and can be edited
 * from the device menu or web portal. Changing a runtime default here changes
 * the settings signature, so the new firmware defaults replace saved values
 * once after upload. Hardware pins, display wiring, and memory/layout options
 * remain build-time-only.
 */
#pragma once

#if __has_include(<Arduino.h>)
  #include <Arduino.h>
#else
  // Fall back to minimal builtins so this file can still be parsed by an editor
  // or generic C++ toolchain when the ESP32 Arduino core headers are not on the
  // active include path.
  using uint8_t  = unsigned char;
  using int8_t   = signed char;
  using uint16_t = unsigned short;
  using int16_t  = signed short;
  using uint32_t = unsigned int;
  using int32_t  = signed int;
  using uint64_t = unsigned long long;
  using int64_t  = signed long long;
#endif

#define FW_NAME    "TRANSPORT TRACKER"
#define FW_VERSION "6.2.5"

// Saved-settings (flash/NVS) layout version. This is DELIBERATELY separate
// from FW_VERSION above. Only bump this when the Settings struct in
// settings.h itself changes shape (a field added, removed, or its type
// changed) - never just because you edited a value elsewhere in this file
// (WiFi creds, URLs, timeouts, etc.). Bumping it wipes saved WiFi/home
// location/every other on-device setting back to this file's defaults;
// leaving it alone (the normal case for an ordinary update) keeps everything
// the user has configured on the device, even though config.h itself changed.
#define SETTINGS_SCHEMA_VERSION 2

// ===========================================================================
//  WIFI
// ===========================================================================
// Name of your WiFi network (2.4 GHz only - the ESP32-S3 has no 5 GHz radio).
// Leave "" to configure WiFi later using the setup portal.
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#define WIFI_HOSTNAME "TransportTracker"


// How long to wait for one connection attempt before giving up (ms).
#define WIFI_CONNECT_TIMEOUT_MS   15000
// Delay before retrying after a failed attempt. It grows on repeated failures
// (doubling) up to WIFI_RETRY_MAX_MS. Retrying never blocks the display.
#define WIFI_RETRY_MIN_MS         3000
#define WIFI_RETRY_MAX_MS         30000

// Setup portal: the device creates its own WiFi network so you can type in
// new WiFi credentials / home position from any phone or computer browser.
// It starts automatically if no SSID is configured, or if the first
// WIFI_PORTAL_AFTER_FAILS connection attempts after power-up all fail.
// You can also start it from Menu > WiFi status, or by HOLDING the BACK button
// while powering up / pressing reset.
#define WIFI_PORTAL_SSID          "TransportTracker-Setup"
// Password for the setup network. "" = open network. If set it must be
// 8..63 characters.
#define WIFI_PORTAL_PASSWORD      ""
#define WIFI_PORTAL_AFTER_FAILS   5
// The portal switches itself off after this long (ms) so it is not left open.
#define WIFI_PORTAL_TIMEOUT_MS    600000UL

// Time zone used for the clock / "LAST UPDATE" display (POSIX TZ string).
// Examples:
//   "UTC0"                         Universal time
//   "GMT0BST,M3.5.0/1,M10.5.0"     United Kingdom / Ireland (with daylight saving)
//   "CET-1CEST,M3.5.0,M10.5.0/3"   Central Europe
//   "EST5EDT,M3.2.0,M11.1.0"       US Eastern
//   "PST8PDT,M3.2.0,M11.1.0"       US Pacific
//   "AEST-10AEDT,M10.1.0,M4.1.0/3" Australia East
// NOTE: "UTC0BST" (no rule) was fixed to the full UK string below - without
// the ",M3.5.0/1,M10.5.0" part the clock would not switch to BST in summer
// or back to GMT in winter.
#define TIMEZONE_POSIX            "GMT0BST,M3.5.0/1,M10.5.0"

// ===========================================================================
//  RADAR CENTRE (YOUR POSITION)
// ===========================================================================
// Decimal degrees. North and East are positive; South and West negative.
#define HOME_LAT      0.0000000000000000      // your latitude  (north +)
#define HOME_LON      0.0000000000000000      // your longitude (east +, west -)

// ===========================================================================
//  DISPLAY (ST7796S 480x320 module, run in portrait)
// ===========================================================================
// The TFT's SPI pins (SCK, MOSI, CS, DC, RST) are NOT set here: TFT_eSPI can
// only read them from its own User_Setup.h - see TFT_SETUP.md. They are:
//   SCK=GPIO12  MOSI=GPIO11  CS=GPIO10  DC=GPIO9  RST=GPIO8
// Only the backlight pin is controlled by this firmware:
#define TFT_BLK_PIN               13

// Panel rotation: 0 = portrait 320x480 (normal), 2 = portrait rotated 180
// degrees (use if the display is mounted upside down).
#define DISPLAY_ROTATION          0

// Set to 1 if the test screen shows inverted colours (black looks white,
// red looks cyan...). Many IPS panels need this = 1.
#define DISPLAY_INVERT_COLOURS    0

// Backlight control. 1 = dim the backlight with PWM on TFT_BLK_PIN.
// 0 = backlight simply switched fully on (use this if the screen flickers or
// changes brightness strangely - the brightness menu then has no effect).
#define BACKLIGHT_PWM_ENABLED     1
#define BACKLIGHT_PWM_FREQ_HZ     5000
// 1 = pin HIGH turns the backlight on (normal). 0 = inverted module.
#define BACKLIGHT_ACTIVE_HIGH     1
// Default brightness in percent (5..100). Changeable in the menu.
#define DISPLAY_BRIGHTNESS        80

// Optional automatic night dimming (needs the clock, i.e. WiFi/NTP).
#define NIGHT_MODE_ENABLED        0
#define NIGHT_MODE_START_HOUR     22      // 0..23, local time
#define NIGHT_MODE_END_HOUR       7       // 0..23, local time
#define NIGHT_MODE_BRIGHTNESS     15      // percent while night mode is active

// ===========================================================================
//  CONTROLS (GPIO numbers) - change to suit your wiring
// ===========================================================================
// All inputs use INPUT_PULLUP and connect to GND when pressed/closed.
// Avoid GPIO 0/3/45/46 (strapping), 19/20 (USB), 22-37 (flash/PSRAM or not
// present on ESP32-S3), 43/44 (UART0). The compiler checks this for you.
#define ENCODER_PIN_A             4       // encoder CLK
#define ENCODER_PIN_B             5       // encoder DT
#define ENCODER_PIN_SW            6       // encoder push button
#define BUTTON_PIN_MENU           15
#define BUTTON_PIN_BACK           16
#define BUTTON_PIN_HOME           17

// 1 = swap encoder direction (if clockwise zooms OUT on your encoder).
#define ENCODER_REVERSE           0
// Number of quadrature edges per click (detent). 4 suits most encoders
// (KY-040, EC11). If one click only moves half a step (you must turn two
// clicks per zoom level) use 2; if one click moves several steps use 4.
#define ENCODER_STEPS_PER_DETENT  4
#define BUTTON_DEBOUNCE_MS        25
// Encoder button held this long = "long press" (ms).
#define LONG_PRESS_MS             1000

// ===========================================================================
//  TOUCHSCREEN (optional - XPT2046-style resistive touch controller)
// ===========================================================================
// This panel wasn't ordered with a touch overlay, but if your board has a
// touch IC on it anyway (an XPT2046/similar chip tapped off the unused
// header pins), set TOUCH_ENABLED to 1 and wire it up to test. With it 0,
// none of this is compiled in and the encoder/buttons work exactly as
// before - touch is purely additive, never required.
//
// This runs on its OWN dedicated SPI bus (its own CLK/MOSI/MISO/CS), not
// shared with the TFT. TFT_eSPI's SPI bus is configured separately in
// User_Setup.h and is write-only (no MISO wired, since the TFT never needs
// to talk back) - the touch IC needs a MISO/T_DO line to report touches, so
// reusing the TFT's bus isn't an option here. Four extra wires, but no risk
// of the two SPI devices fighting over a bus neither of them fully owns.
#define TOUCH_ENABLED              0
#define TOUCH_CLK_PIN              1
#define TOUCH_MOSI_PIN             2       // -> touch IC T_DIN
#define TOUCH_MISO_PIN             42      // <- touch IC T_DO
#define TOUCH_CS_PIN               14
#define TOUCH_IRQ_PIN              -1     // -1 = no IRQ wired, poll continuously instead
// Right now this is wired up as a minimal TEST mode, not a full touch UI:
// any tap anywhere on the panel sends the same "short press" the encoder
// button sends (select/confirm/next), and a held tap sends a "long press"
// (opens the menu) - enough to prove the touch IC and wiring actually work
// without reworking every screen's input handling. Raw x/y readings print
// to the Serial Monitor while a touch is held, which is what you'd use to
// calibrate real coordinate mapping (TOUCH_RAW_X/Y_MIN/MAX below) if this
// gets built out into a proper touch UI later.
#define TOUCH_RAW_X_MIN            300
#define TOUCH_RAW_X_MAX            3800
#define TOUCH_RAW_Y_MIN            300
#define TOUCH_RAW_Y_MAX            3800
#define TOUCH_SWAP_XY              0
#define TOUCH_INVERT_X             0
#define TOUCH_INVERT_Y             0
// Minimum time between separate taps (ms) - prevents one touch registering
// as several rapid presses.
#define TOUCH_DEBOUNCE_MS          180

// ===========================================================================
//  ONBOARD RGB STATUS LED
// ===========================================================================
// Most ESP32-S3-DevKitC-1 (N16R8) boards have a single addressable WS2812
// "NeoPixel" LED built in, which lights up a default colour at boot until
// something explicitly turns it off - that's the stray light you're seeing,
// not a fault. It's driven over a single data pin (not a plain on/off GPIO),
// so turning it off needs the Adafruit NeoPixel library: install it via
// Tools > Manage Libraries > search "Adafruit NeoPixel" > Install.
// Pin is 48 on most current DevKitC-1 boards, 38 on older ones - if it's
// still lit after flashing, try changing this to 38 (or check your board's
// schematic/silkscreen for "RGB LED" or "WS2812").
#define ONBOARD_RGB_LED_PIN        48      // set to -1 if your board has none / you don't want this

// ===========================================================================
//  AUDIO ANNOUNCEMENTS (I2S amplifier, e.g. MAX98357A, + speaker)
// ===========================================================================
#define AUDIO_ENABLED              1
#define AUDIO_I2S_BCLK_PIN         18      // amp BCLK
#define AUDIO_I2S_LRC_PIN          21      // amp LRC / LRCLK / WSEL
#define AUDIO_I2S_DOUT_PIN         7       // amp DIN
// GAIN and SD/MODE on the amp are left unconnected (floating): this gives the
// MAX98357A its default ~9 dB gain and enables it in mono (L+R mixed), which
// is what you want for a single speaker.

// Instead of compiling the announcement clip in, fetch it from this URL the
// first time a clip is played, then keep it in PSRAM for the rest of the
// session. Emergency beeps remain immediate and do not wait for this download.
// The speaker test uses this WAV even when announcements are disabled. Leave
// as "" to use the built-in clip.
//
// Must be a DIRECT link to the raw .wav bytes with a normal Consstent-Length -
// a GitHub "raw." link, a plain web server, or an S3/R2/Cloudflare bucket all
// work. A Google Drive or Dropbox *share* page will NOT work - those serve a
// webpage, not the file itself (Dropbox can work if you change the link's
// "?dl=0" to "?dl=1"; Google Drive needs its direct-download link form).
// If the fetch fails for any reason (no WiFi yet, host unreachable, wrong
// format, too large) the firmware falls back to the built-in placeholder
// clip automatically - it never blocks startup or crashes over this.
#define MILITARY_WAV_URL                 "https://raw.githubusercontent.com/pixel-hosting/m/main/M1.wav"
// Safety cap so a misconfigured URL can't exhaust memory (bytes).
#define MILITARY_WAV_MAX_BYTES           (2UL * 1024UL * 1024UL)
// Timeout for each step of the one-time download (ms).
#define MILITARY_WAV_FETCH_TIMEOUT_MS    15000
// How long to wait for WiFi to come up before giving up on the fetch and
// using the placeholder clip instead (ms).
#define MILITARY_WAV_WIFI_WAIT_MS        30000

// How long (ms) before the same aircraft can trigger the military
// announcement again (avoids repeats if it lingers or briefly drops out).
#define MILITARY_ANNOUNCE_COOLDOWN_MS  (4UL * 60UL * 1000UL)

// Set to 1 to print every aircraft's type/category/callsign/military-flag to
// the Serial Monitor as it's received - useful for finding the exact type
// code a real flyover reports, to add to HISTORIC_MIL_TYPES in aircraft.cpp
// if it isn't triggering the announcement. Leave at 0 normally (noisy).
#define DEBUG_PRINT_AIRCRAFT_TYPES      0

// ===========================================================================
//  FIRMWARE UPDATES (optional - over-the-air)
// ===========================================================================
// The device periodically fetches FW_UPDATE_VERSION_URL - a tiny text file
// that should contain nothing but the latest version number, e.g. "4.3.0".
// If it differs from FW_VERSION above, the device prompts on screen to
// install the update; confirming downloads FW_UPDATE_BIN_URL and flashes it
// over WiFi, then reboots into the new firmware. These URLs are never shown
// or editable on the device - only here.
//
// Already pointed at your jakew2011/flightradar GitHub repo below. Leave
// either one as "" to disable update checking entirely - nothing else in
// the firmware depends on this, so it is safe to leave blank.
//
// To publish a release:
//   1. Bump FW_VERSION above and commit.
//   2. Arduino IDE: Sketch > Export Compiled Binary. This writes a
//      FlightRadar.ino.bin (plus bootloader/partition files you don't need
//      for OTA - only the .ino.bin itself) into the sketch folder.
//   3. Push the new FW_VERSION's value as plain text to the VERSION file
//      below, and the matching FlightRadar.ino.bin to the bin file below,
//      at the exact URLs configured here (e.g. a "raw." GitHub link to a
//      fixed path in the repo - overwrite the same file each release so the
//      URL never changes).
#define FW_UPDATE_VERSION_URL     "https://raw.githubusercontent.com/jakew2011/flightradar/main/VERSION"   // e.g. ""
#define FW_UPDATE_BIN_URL         "https://raw.githubusercontent.com/jakew2011/flightradar/main/firmware.bin"   // e.g. ""
// How often to check automatically once WiFi is up (ms). The menu also has
// a manual "Check for update" button that checks immediately regardless.
#define FW_UPDATE_CHECK_INTERVAL_MS   (6UL * 60UL * 60UL * 1000UL)   // 6 hours
#define FW_UPDATE_HTTP_TIMEOUT_MS     15000

// ===========================================================================
//  AIRCRAFT DATA SOURCE
// ===========================================================================
// The default queries adsb.lol and adsb.fi as equal partners on every update;
// results are joined by ICAO and missing facts are supplemented across feeds.
// Set ADSB_MULTI_SOURCE_ENABLED to 0 to use one provider. All feeds return the same readsb-style JSON
//   {"ac":[{"hex":"406a7b","flight":"BAW123 ","lat":..,"lon":.., ...}], ...}
// See adsb_api.h for endpoint / limits / restrictions of each source.
#define ADSB_SOURCE_ADSB_LOL        1   // api.adsb.lol       (single-feed option)
#define ADSB_SOURCE_AIRPLANES_LIVE  2   // api.airplanes.live (may need access approval)
#define ADSB_SOURCE_ADSB_FI         3   // opendata.adsb.fi   (personal use)
#define ADSB_SOURCE_CUSTOM          4   // your own URL, see below
#define ADSB_SOURCE                 ADSB_SOURCE_ADSB_FI
// Query both free community feeds on each update and merge aircraft by ICAO.
// When enabled, the Advanced setting also allows selecting either feed alone.
// NOTE: adsb.lol has been flaky on this network (403/429/503 - looks like an
// overloaded/rate-limiting backend rather than a hard block, since it does
// respond sometimes). That's fine to leave on: each feed in doFetch() fails
// independently and silently falls back to "retain recent reports" without
// affecting adsb.fi, so merging just means "use adsb.lol's data whenever it
// happens to answer, ignore it otherwise" - more coverage with no downside.
#define ADSB_MULTI_SOURCE_ENABLED   1
// Only used when ADSB_SOURCE == ADSB_SOURCE_CUSTOM. printf-style format that
// receives (double latitude, double longitude, unsigned radius_nm).
#define ADSB_CUSTOM_URL_FORMAT      "https://example.com/v2/point/%.4f/%.4f/%u"

// Planespotters requires a contact address in the User-Agent for photo lookups.
#define PLANESPOTTERS_CONTACT       "jakewakeham2011@gmail.com"
// JetAPI photo fallback. Queries use the aircraft registration when Planespotters
// has no usable photo for the selected ICAO address.
#define JETAPI_BASE_URL             "https://www.jetapi.dev"

// Milliseconds between aircraft requests (approximately 5 s). Minimum 2000.
// Public APIs allow at most 1 request per second; please stay well below.
#define AIRCRAFT_UPDATE_INTERVAL    5000
// Never send two requests closer together than this (ms), even when you zoom.
#define AIRCRAFT_MIN_REQUEST_GAP    2000
// HTTP timeouts (ms).
#define ADSB_CONNECT_TIMEOUT_MS     6000
#define ADSB_HTTP_TIMEOUT_MS        8000
// 0 = HTTPS encrypted but the server certificate is NOT verified (works
// everywhere, nothing secret is sent). 1 = also verify the server: you must
// then define ADSB_ROOT_CA_PEM (a PEM string) - see README, "TLS".
#define ADSB_VALIDATE_TLS           0

// Aircraft whose last position report is older than this are removed (ms).
#define AIRCRAFT_MAX_AGE            60000
// Positions are extrapolated along the track for at most this long (ms)
// after the last report; after that the aircraft simply stays where it was
// until it is removed at AIRCRAFT_MAX_AGE.
#define AIRCRAFT_MAX_EXTRAPOLATION  20000
// After each update the display position glides to the new position over
// this time (ms) instead of jumping.
#define AIRCRAFT_BLEND_MS           1500
// Maximum number of aircraft kept (nearest ones win). Each uses ~180 bytes
// of PSRAM (x3 buffers).
#define MAX_AIRCRAFT                250
// 1 = also show aircraft reported as being on the ground.
#define SHOW_GROUND_AIRCRAFT        0

// ===========================================================================
//  RADAR APPEARANCE
// ===========================================================================
// Default range in nautical miles: 5, 10, 20, 30, 50, 75, 100, 150 or 200.
#define DEFAULT_RANGE               20
// Default theme: 0 = green, 1 = amber, 2 = white.
#define DEFAULT_THEME               0
// Time for one sweep revolution at "NORMAL" radar speed (ms).
#define RADAR_SWEEP_PERIOD          4000
// Upper limit of animation frames per second.
#define RADAR_MAX_FPS               25
// Fading trail behind the sweep line: number of wedges and total angle.
#define SWEEP_TRAIL_SEGMENTS        20
#define SWEEP_TRAIL_DEGREES         55.0f
// Aircraft icon size multiplier (1.0 = about 18 pixels across).
#define AIRCRAFT_ICON_SCALE         1.0f
// 1 = draw callsign labels beside aircraft (when there is room).
#define LABELS_ENABLED              1
// Maximum number of labels drawn per frame.
#define LABEL_MAX                   24
// 1 = draw small square markers + code for nearby airports/airfields (see
// the AIRPORTS list in radar.cpp - a scoped-down version of real coastline/
// lake outlines, which aren't practical to embed as vector data here).
#define SHOW_AIRPORTS               1

// ===========================================================================
//  DEFAULT UNITS (changeable in the menu)
// ===========================================================================
#define DEFAULT_DISTANCE_UNIT       0   // 0 = nautical miles, 1 = kilometres
#define DEFAULT_ALTITUDE_UNIT       0   // 0 = feet,           1 = metres
#define DEFAULT_SPEED_UNIT          0   // 0 = knots,          1 = km/h

// ===========================================================================
//  TRAIN DEPARTURE BOARD (National Rail / Rail Data Marketplace, via the "Switch mode"
//  button/menu item - toggles the radar screen with a live departure board)
// ===========================================================================
// The old OpenLDBWS self-service registration page is dead; this now goes
// through the Rail Data Marketplace (raildata.org.uk), a different, plain
// REST/JSON API (not the old SOAP/XML one) with its own "Consumer key":
//   1. Register at https://raildata.org.uk/registerPartner (an individual
//      account is fine) and verify your email.
//   2. Log in, open the "Live Departure Board" data product:
//      https://raildata.org.uk/dataProduct/P-d81d6eaf-8060-4467-a339-1c833e50cbbe/overview
//   3. Click Subscribe (it's free, no usage limit), accept the licence terms.
//   4. Go to your subscriptions, open "Live Departure Board", click the
//      "Specification" tab, and scroll to "API access credentials".
//   5. Copy the "Consumer key" shown there into OPENLDBWS_API_KEY below.
// Leave "" to disable the train board; "Switch mode" then just stays on the
// radar screen.
#define OPENLDBWS_API_KEY           ""
#define OPENLDBWS_ENDPOINT          "https://api1.raildata.org.uk/1010-live-departure-board-dep1_2/LDBWS/api/20220120/GetDepartureBoard"
// 3-letter CRS station code, e.g. "LIT" = Littlehampton. Changeable on the
// device (Advanced menu) and in the web setup portal without reflashing.
#define TRAIN_DEFAULT_STATION_CRS   "LKX"
// How often the board is refreshed while it's the active screen (ms).
#define TRAIN_UPDATE_INTERVAL_MS    30000UL
// Rows requested from the API / shown on screen.
#define TRAIN_MAX_ROWS              8
#define TRAIN_CONNECT_TIMEOUT_MS    6000UL
#define TRAIN_HTTP_TIMEOUT_MS       10000UL

// ===========================================================================
//  MISC
// ===========================================================================
#define SERIAL_BAUD                 115200

// ---------------------------------------------------------------------------
//  Compile-time sanity checks (do not edit)
// ---------------------------------------------------------------------------
#if AIRCRAFT_UPDATE_INTERVAL < 2000
  #error "AIRCRAFT_UPDATE_INTERVAL must be at least 2000 ms (public APIs are rate limited)."
#endif
#if MAX_AIRCRAFT < 20 || MAX_AIRCRAFT > 1000
  #error "MAX_AIRCRAFT must be between 20 and 1000."
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  #define FR_PIN_BAD(p) (((p) >= 22 && (p) <= 37) || (p) == 19 || (p) == 20 || (p) == 43 || (p) == 44 || (p) < 0 || (p) > 48)
  #if FR_PIN_BAD(TFT_BLK_PIN) || FR_PIN_BAD(ENCODER_PIN_A) || FR_PIN_BAD(ENCODER_PIN_B) || \
      FR_PIN_BAD(ENCODER_PIN_SW) || FR_PIN_BAD(BUTTON_PIN_MENU) || FR_PIN_BAD(BUTTON_PIN_BACK) || \
      FR_PIN_BAD(BUTTON_PIN_HOME) || FR_PIN_BAD(AUDIO_I2S_BCLK_PIN) || \
      FR_PIN_BAD(AUDIO_I2S_LRC_PIN) || FR_PIN_BAD(AUDIO_I2S_DOUT_PIN)
    #error "A GPIO in config.h is not usable on the ESP32-S3 (22-25 do not exist, 26-37 are flash/PSRAM, 19/20 USB, 43/44 UART0). See WIRING.md."
  #endif
  #if TOUCH_ENABLED && (FR_PIN_BAD(TOUCH_CLK_PIN) || FR_PIN_BAD(TOUCH_MOSI_PIN) || \
      FR_PIN_BAD(TOUCH_MISO_PIN) || FR_PIN_BAD(TOUCH_CS_PIN) || \
      (TOUCH_IRQ_PIN >= 0 && FR_PIN_BAD(TOUCH_IRQ_PIN)))
    #error "A TOUCH_*_PIN in config.h is not usable on the ESP32-S3. See WIRING.md."
  #endif
#endif
