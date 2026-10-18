# TFT_SETUP.md - configuring TFT_eSPI for the ST7796S

TFT_eSPI reads its pin and driver settings from a file **inside the library
folder**. The sketch cannot override them, so you must install the ready-made
file below once.

## Which versions to install (important)

| Software | Use |
|---|---|
| Arduino IDE | 2.x (or 1.8.19) |
| **esp32 by Espressif Systems** (Boards Manager) | **2.0.17** (choose this exact version, *not* the newest 3.x) |
| TFT_eSPI (Bodmer) | 2.5.43 (latest release) |
| ArduinoJson (Benoit Blanchon) | 7.x |

Why not the newest board package? TFT_eSPI has not been updated for Arduino-ESP32 3.x.
Public reports (TFT_eSPI issue tracker and discussions) describe boot loops and
crashes inside `tft.init()` on ESP32-S3 boards with an ST7796 when using
core 3.x, and describe `#define USE_HSPI_PORT` as the fix for ESP32-S3 with
core 2.0.x. The firmware code itself also compiles for 3.x, but the TFT_eSPI
library is the weak point, so 2.0.17 is the safe choice.
(I could not test on real hardware - see the troubleshooting section if your
board behaves differently.)

## Install the setup file

1. Find your Arduino libraries folder:
   * Windows: `Documents\Arduino\libraries\TFT_eSPI\`
   * macOS: `~/Documents/Arduino/libraries/TFT_eSPI/`
   * Linux: `~/Arduino/libraries/TFT_eSPI/`
2. Open the file `User_Setup.h` in that folder.
3. **Delete its entire contents** and paste in the contents of
   `FlightRadar/extras/User_Setup.h` (or simply copy that file over it).
4. Save, then **restart the Arduino IDE**.

(Updating the TFT_eSPI library later overwrites this file - just repeat.)

If the sketch stops with the message *"TFT_eSPI is not configured for the
ST7796S"* the file was not installed correctly.

## The exact file

```cpp
#define USER_SETUP_INFO "FlightRadar ST7796S ESP32-S3"

#define ST7796_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// #define TFT_RGB_ORDER TFT_RGB     // enable only if red/blue are swapped
// #define TFT_RGB_ORDER TFT_BGR     // ... or try this one instead

#define TFT_MISO -1
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8
// TFT_BL is NOT defined - the firmware controls the backlight (GPIO13)

#define USE_HSPI_PORT

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
```

* **Colour depth / colour mode:** 16-bit RGB565 (TFT_eSPI default for this driver).
* **Resolution:** 320 x 480 native; the firmware uses it in portrait
  (`setRotation(0)`).
* **Colours look like a negative** (black is white): set
  `DISPLAY_INVERT_COLOURS 1` in the sketch's `config.h` (and `TEST_INVERT 1` in
  the test sketch). This is done at run time - no library change needed.
* **Red and blue swapped:** enable `#define TFT_RGB_ORDER TFT_RGB` in
  `User_Setup.h`; if that does not fix it, use `TFT_BGR` instead. Restart the IDE.

## Troubleshooting the display

| Symptom | Try |
|---|---|
| White or blank screen, nothing else | Check wiring (CS/DC/RES/SDA/SCK), 3.3 V power, that BLK is connected. Run **DisplayTest**. |
| Board reboots in a loop when `tft.init()` runs | Use board package **2.0.17**; toggle the `#define USE_HSPI_PORT` line (comment it out if it is on, or enable it if it is off); restart the IDE. |
| Garbage / shifted picture | Lower `SPI_FREQUENCY` to `27000000`, shorten wires. |
| Mirrored or upside-down | `DISPLAY_ROTATION` in `config.h` (0 or 2). |
| Only a 240x320 area is used | Confirm `TFT_WIDTH 320` / `TFT_HEIGHT 480` and that `User_Setup.h` really is the one in use (only one `User_Setup_Select.h` should be active). |
| Compile error about `ST7796_DRIVER` | The setup file was not installed - see above. |

If you ever need an alternative library: LovyanGFX supports ESP32-S3 with both
board-package generations, but this project is written for TFT_eSPI.
