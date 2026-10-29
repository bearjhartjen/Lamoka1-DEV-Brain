## What is the Portal Interface Engine?

PIE is the operating system for Lamoka models. It gives you simple "portals" to
whatever you want to do with your unit — lights, games, settings, sleep, USB
bootloader entry — through one button and a strip of addressable LEDs. Tap to
move, double-tap to go back, hold to select.

## Compatibility

PIE **V1.8** runs on the **Lamoka1-DEV-Brain** (ten LEDs, analog entropy). It does
**not** run on the main Lamoka1/2 board unmodified:

| V1.8 uses | Main board |
| --- | --- |
| GPIO 0 — button | button (matches) |
| GPIO 11 — LED data | LED data (matches) |
| GPIO 5 — digital entropy input | not connected |
| GPIO 26 — ADC entropy input | `TEMP_CTRL`, a TMP235 temperature sensor output |
| GPIO 27 — ADC entropy input | `TEMP_INV`, a TMP235 temperature sensor output |
| GPIO 28, GPIO 29 — ADC entropy inputs | not connected |
| `NUM_LEDS` = 10 | 5 WS2812B LEDs fitted (D1–D5) |

Reading GPIO26 and GPIO27 as entropy would instead read the two temperature sensors, and
driving ten LEDs would address five that are not fitted. The menu, the brightness and
speed bars, the binary score display, and the Fireball game all index across the full
array, so the interface would be truncated rather than merely dimmer.
