// ===========================================================================
//  TFT_eSPI User_Setup.h for the FlightRadar project
//  ESP32-S3-DevKitC (N16R8) + 3.5" IPS ST7796S 320x480, 4-wire SPI
//
//  HOW TO USE: copy this file over
//     Documents/Arduino/libraries/TFT_eSPI/User_Setup.h
//  (replace the whole file), then restart the Arduino IDE. See TFT_SETUP.md.
// ===========================================================================
#define USER_SETUP_INFO "FlightRadar ST7796S ESP32-S3"

// ---- Driver ---------------------------------------------------------------
#define ST7796_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// Colour order. Leave both lines commented out first. If red and blue are
// swapped on the display test, enable the line below; if that makes it worse,
// disable it again and try TFT_BGR instead.
// #define TFT_RGB_ORDER TFT_RGB
// #define TFT_RGB_ORDER TFT_BGR

// ---- ESP32-S3 SPI pins (matches WIRING.md / the wiring diagram) -----------
#define TFT_MISO -1     // display SDO is not connected (write-only)
#define TFT_MOSI 11     // display SDI / MOSI
#define TFT_SCLK 12     // display SCK
#define TFT_CS   10     // display CS
#define TFT_DC    9     // display DC / RS
#define TFT_RST   8     // display RESET / RES
// TFT_BL is deliberately NOT defined: the firmware drives the backlight
// (GPIO13, with PWM dimming) itself.

// ---- ESP32-S3 with Arduino-ESP32 2.0.x: use the second SPI peripheral ------
// Reported to prevent a crash/boot loop in tft.init() on ESP32-S3 boards.
// If the board does not boot or the screen stays blank, try commenting this
// line out (and re-check TFT_SETUP.md, troubleshooting).
#define USE_HSPI_PORT

// ---- Fonts (only the ones the firmware uses, to keep the program small) ---
#define LOAD_GLCD    // Font 1: 6x8 pixel font (aircraft labels)
#define LOAD_FONT2   // Font 2: 16 pixel font
#define LOAD_FONT4   // Font 4: 26 pixel font
// #define LOAD_FONT6
// #define LOAD_FONT7
// #define LOAD_FONT8
// #define LOAD_GFXFF
// #define SMOOTH_FONT

// ---- SPI speed -------------------------------------------------------------
#define SPI_FREQUENCY       40000000   // 40 MHz: safe with short jumper wires
#define SPI_READ_FREQUENCY  20000000
