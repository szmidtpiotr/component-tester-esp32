# Tester firmware (ATmega328P)

**ComponentTester 1.57m** by Markus Reschke, licensed under the **EUPL v1.2**.
Original source: <https://github.com/madires/Transistortester-Warehouse> →
`Firmware/m-firmware/ComponentTester-1.57m.tgz`.

This folder contains only our configuration files (`config.h`, `config_328.h`, `Makefile`
is unchanged) and `changes-vs-1.57m.diff` against the original. The ready-to-flash result is
`release/ComponentTester.hex` + `release/ComponentTester.eep`.

What we changed (all in the config files, no source code changes):

| Setting | Value | Why |
|---|---|---|
| Display | `LCD_VT100` (pseudo display), `SERIAL_BITBANG`, TX = **PD6**, 9600 8N1 | the whole user interface goes out as text to the ESP32; the physical display is not used |
| ST7565 display | disabled | replaced by VT100 |
| `UI_SERIAL_COPY` | off | VT100 already sends everything |
| `POWER_SWITCH_MANUAL` | on | the tester never switches itself off; PD6 (normally power control) becomes free |
| `BAT_NONE` | on | no battery; 5 V comes from the ESP32 |
| Fuses | unchanged (`lfuse F7`, `hfuse D9`, `efuse FD`) | 8 MHz crystal, as shipped |

## Rebuilding (optional)

Only needed if you want to change the configuration. Linux/macOS with `avr-gcc` and `make`:

```bash
tar xzf ComponentTester-1.57m.tgz
cp config.h config_328.h ComponentTester-1.57m/
cd ComponentTester-1.57m && make
```

Flashing: see [docs/02-tester-firmware.md](../docs/02-tester-firmware.md).
