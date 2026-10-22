# WIRING.md - FlightRadar wiring

Board: **ESP32-S3-DevKitC N16R8** (16 MB flash, 8 MB octal PSRAM).
Display: **4.0" SPI TFT, ST7796S, 480x320 module (non-touch)** - same
controller/resolution as before, just described in landscape (the firmware
still runs it in portrait via `setRotation(0)`, so no code change was needed
for the size itself).
Encoder: **KY-040**. Audio: **MAX98357A I2S amp + 4 ohm 3 W speaker**.

This file (and `config.h` / `extras/User_Setup.h` in this ZIP) matches the
wiring diagram you sent - I did **not** ask you to rewire anything; I updated
the firmware's pin numbers to match your diagram instead. All 15 required (plus 4 optional touchscreen) assigned
GPIOs were checked against the ESP32-S3's reserved pins (USB, PSRAM/flash,
UART0, strapping, non-existent) and there are no conflicts.

> **Everything is 3.3 V *logic*.** The two 5 V connections below (display VCC
> and amp VIN) are intentional - see the notes under each section.

## 1. Display (ST7796S 480x320 module)

| Display pin | ESP32-S3 pin | config / library name |
|---|---|---|
| VCC | **5V** | - (module has its own onboard 3.3 V regulator; see caution below) |
| GND | GND | |
| CS | GPIO10 | `TFT_CS` |
| RESET (RES) | GPIO8 | `TFT_RST` |
| DC (RS) | GPIO9 | `TFT_DC` |
| SDI (MOSI) | GPIO11 | `TFT_MOSI` |
| SCK | GPIO12 | `TFT_SCLK` |
| LED (BLK) | GPIO13 | `TFT_BLK_PIN` (backlight, PWM dimmed) |
| SDO (MISO) | not connected | display is write-only |
| T_CS, T_DIN, T_DO, T_CLK (touch) | see section 1a below if your module has a touch controller | leave unconnected on a non-touch module |
| SD_CS | not connected | on-board SD slot not used |

**Caution on VCC -> 5V:** this is correct *only if your module has an onboard
regulator* (common on these red-PCB 4.0" ST7796S boards - look for a small
3-pin regulator IC near the VCC pin). If your particular board does **not**
have one, its logic will be running above 3.3 V and could be damaged long
term - in that case connect VCC to **3V3** instead. I can't inspect the board
myself, so please double-check against its listing/datasheet before powering
it up.

## 1a. Touchscreen (XPT2046-style resistive touch, optional)

You ordered a non-touch panel, but the board you received has a touch
controller IC and breaks the T_CS/T_DIN/T_DO/T_CLK pins out to the header -
this wires that up so you can test it. It's on its **own, separate SPI bus**
(not shared with the display's bus above), and it's **off by default**:
nothing is read from these pins unless you set `TOUCH_ENABLED` to `1` in
`config.h`.

| Touch pin | ESP32-S3 pin | config.h name |
|---|---|---|
| T_CLK | GPIO1 | `TOUCH_CLK_PIN` |
| T_DIN (touch MOSI - ESP32 writes to it) | GPIO2 | `TOUCH_MOSI_PIN` |
| T_DO (touch MISO - ESP32 reads from it) | GPIO42 | `TOUCH_MISO_PIN` |
| T_CS | GPIO14 | `TOUCH_CS_PIN` |
| T_IRQ | not connected | `TOUCH_IRQ_PIN` is `-1` (not wired) - the firmware polls instead of waiting for an interrupt, which is fine at this screen size |

T_CLK/T_DIN/T_DO/T_CS are a bit-banged SPI link (no library), so there's no
hardware-SPI-bus conflict with the display even though both are technically
"SPI" - they just need four free GPIOs each, which they have.

After wiring, set `TOUCH_ENABLED 1` in `config.h` and re-upload. There's no
dedicated touch-test screen - this is wired as a minimal test mode on top of
the existing encoder/button UI, not a full touch UI: **tap anywhere on the
panel** and it acts like a short press of the encoder button (select/confirm/
next on whatever screen is showing); **hold a tap** and it acts like a long
press (opens the menu). That's enough to prove the touch IC and wiring
actually work without rewriting every screen's input handling. Open the
Serial Monitor at your configured baud (115200 by default) while you tap -
it prints `[touch] raw x=... y=...` the whole time a touch is held. If
nothing prints while you're pressing the glass, the wiring (most likely
T_DIN/T_DO swapped, or T_CS not on GPIO14) is the first thing to re-check; if
numbers print but taps don't do anything in the UI, check `TOUCH_DEBOUNCE_MS`
and that you're releasing the tap (it fires on release, not on touch-down).
The `TOUCH_RAW_X/Y_MIN/MAX`, `TOUCH_SWAP_XY` and `TOUCH_INVERT_X/Y` values in
`config.h` are reserved for a future coordinate-mapped touch UI and aren't
used by anything yet - ignore them for now, there's nothing to calibrate for
this tap/hold test mode.

## 2. Rotary encoder (KY-040)

| Encoder pin | ESP32-S3 pin | config.h name |
|---|---|---|
| + | 3V3 | |
| GND | GND | |
| CLK | GPIO4 | `ENCODER_PIN_A` |
| DT | GPIO5 | `ENCODER_PIN_B` |
| SW | GPIO6 | `ENCODER_PIN_SW` |

KY-040 modules have their own pull-ups to "+"; powering "+" from 3V3 keeps the
signals at 3.3 V, so no extra resistors are needed.

## 3. Buttons (3x momentary, 6x6 mm)

All three use the ESP32's internal pull-ups - one leg to the GPIO, the other
leg of every button to **GND**.

| Button | ESP32-S3 pin | config.h name |
|---|---|---|
| MENU | GPIO15 | `BUTTON_PIN_MENU` |
| BACK | GPIO16 | `BUTTON_PIN_BACK` |
| HOME | GPIO17 | `BUTTON_PIN_HOME` |

## 4. Audio: MAX98357A I2S amplifier + speaker

| Amp pin | ESP32-S3 pin | config.h name |
|---|---|---|
| VIN | **5V** | class-D amps want 5 V for full volume |
| GND | GND | |
| BCLK | GPIO18 | `AUDIO_I2S_BCLK_PIN` |
| LRC (LRCLK / WSEL) | GPIO21 | `AUDIO_I2S_LRC_PIN` |
| DIN | **GPIO7** | `AUDIO_I2S_DOUT_PIN` |
| GAIN | leave unconnected (floating) | default ~9 dB gain |
| SD / MODE | leave unconnected (floating) | enables the amp, mono (L+R mixed) - right for one speaker |
| SPK+ / SPK- | speaker terminals | **never connect either speaker terminal to GND** - it must float, driven differentially by the amp |

Speaker: 4 ohm, 3 W, 40 mm - within the MAX98357A's supported range (4-8 ohm).

**Grounding:** with this many modules, wire all their GND pins together into one
junction (solder them together, heat-shrink over it) and run a single wire
from that junction to one of the ESP32-S3's GND pins - a "star" ground. That's
electrically the same as wiring each module straight to the board and avoids
hunting for six free GND pins.

> **On GPIO14:** an earlier wiring pass for this project used GPIO14 for DIN
> and a note came back saying the ESP32-S3 "doesn't expose GPIO14" - that's
> not correct, GPIO14 is a normal, usable pin on the DevKitC-1 (it's GPIO19,
> GPIO20 that are reserved, for USB D-/D+). Either pin works fine; **GPIO7**
> is what's documented here since that's what's now wired.

**Sound files:** there's no SD card in this design, so WAV clips will live in
the ESP32's own flash. The wiring above is ready for this; see `audio.h` /
`audio.cpp` and `extras/wav_to_header.py` for how the announcement clip gets
compiled in.

## 5. Power chain

Your chain - USB-A power supply (5 V) -> inline ON/OFF switch -> USB-A
extension (1 m) -> USB-A-to-USB-C cable (1 m) -> ESP32-S3 - is fine
electrically. One thing worth knowing: **two metres of cable plus a switch is
enough resistance to cause a voltage sag** once the display, backlight and amp
are all drawing current at once (a sound playing at volume while the screen
is fully lit is the worst case). If you ever see the screen glitch, dim
unexpectedly, or the board reset exactly when a sound plays, that's the
symptom - shorten the cable run, use a thicker (higher current-rated) USB
cable, or power the amp's VIN separately from a second 5 V source sharing GND
with the ESP32-S3.

## 5b. Troubleshooting: screen flashing / flickering rapidly

This is almost always **power**, not wiring or code - the firmware's `displayInit()`
only runs once at boot and never re-initialises the screen in `loop()`, so a
repeating flash means something is resetting the *board itself*, not a display bug.
Check in this order:

1. **Power the ESP32-S3 directly first.** Unplug the inline switch and USB-A
   extension/adaptor chain and plug a short (under 50 cm), good-quality USB-C
   cable straight from a wall/phone charger (not a PC USB port, which often
   can't supply enough current) into the board. If the flashing stops, the
   cause was voltage sag in the cable/switch chain - see section 5 above.
2. **Disconnect the amplifier temporarily** (unplug its VIN wire) and power
   up with just the display. If the flashing stops, the display and amp are
   together pulling more current than the supply/cable can give - power the
   amp's VIN from a separate 5V source sharing GND with the board, instead of
   both riding the same USB feed.
3. **Check the display's GND and VCC are solid.** A loose VCC or GND (common
   on breadboards/jumper wires) makes the screen flash or show garbage
   instead of simply going blank - wiggle each wire gently while watching the
   screen; re-seat any that change the pattern.
4. **Confirm the module's VCC pin actually wants 5V.** If your board does
   *not* have the onboard 3.3V regulator mentioned in section 1, 5V on VCC
   can damage or destabilise the logic - move VCC to 3V3 instead and test
   again.
5. **Re-check the SPI data pins** (CS=GPIO10, DC=GPIO9, MOSI=GPIO11,
   SCK=GPIO12, RESET=GPIO8) are each in the *correct* header socket - a
   swapped DC/CS or RESET pin produces exactly this kind of rapid
   flash/garbage pattern rather than a dead screen.
6. **Open the Serial Monitor at 115200 baud while it flashes.** If you see
   the `=== FlightRadar display + controls test ===` banner printing over
   and over, the ESP32 itself is rebooting (brownout) - that confirms step 1.
   If it prints once and the screen keeps flashing with no new Serial output,
   it's a display wiring/power issue (steps 3-5), not a reboot.

## 6. Reserved GPIOs (do not use for anything else)

| GPIO | Why |
|---|---|
| 26-37 | flash/PSRAM |
| 19, 20 | USB D-/D+ |
| 43, 44 | UART0 (Serial Monitor) |
| 0, 3, 45, 46 | strapping pins (affect boot) |
| 22-25 | do not exist on the ESP32-S3 |

The firmware's `config.h` refuses to compile if any assigned pin above lands
on this list - all 15 required pins (and the 4 optional touchscreen pins in section 1a) in this diagram already passed that check.

## 7. Quick wiring summary (copy/paste)

```
TFT VCC  -> 5V (see caution)   TFT CS   -> GPIO10     TFT SDI  -> GPIO11
TFT GND  -> GND                TFT RES  -> GPIO8      TFT SCK  -> GPIO12
TFT LED  -> GPIO13             TFT DC   -> GPIO9      TFT SDO  -> (nothing)

TOUCH T_CLK -> GPIO1   TOUCH T_DIN -> GPIO2   TOUCH T_DO -> GPIO42
TOUCH T_CS  -> GPIO14  TOUCH T_IRQ -> (n/c)   (set TOUCH_ENABLED 1 in config.h)

ENC CLK  -> GPIO4      ENC DT   -> GPIO5      ENC SW   -> GPIO6
ENC +    -> 3V3        ENC GND  -> GND

MENU     -> GPIO15 / GND
BACK     -> GPIO16 / GND
HOME     -> GPIO17 / GND

AMP VIN  -> 5V          AMP GND  -> GND
AMP BCLK -> GPIO18       AMP LRC  -> GPIO21     AMP DIN -> GPIO7
AMP GAIN -> (n/c)         AMP SD   -> (n/c)      SPEAKER -> SPK+/SPK-
```
