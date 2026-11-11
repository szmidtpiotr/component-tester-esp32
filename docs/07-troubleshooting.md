# 7. Troubleshooting

## Flashing the tester (chapter 2)

| Symptom | Cause and fix |
|---|---|
| `avrdude: error: program enable: target doesn't answer` | Programmer wiring. Check the six wires against the table in 2.3; RES and GND are the usual suspects. Is the USBasp's power jumper set to 5 V? |
| Signature is not `0x1e950f` | Not a genuine ATmega328P (e.g. APT32F172K8T6 with a fake Atmel marking, or LGT8F328). ComponentTester will not run on it. |
| `cannot set sck period` | Old USBasp firmware. Harmless. |
| `verification mismatch` | Run the same command again. If it repeats: shorter wires, stable USB port. |
| Windows does not see the USBasp | Install the driver with Zadig (libusbK). |

## The tester itself

| Symptom | Cause and fix |
|---|---|
| Tester display stays blank | Expected — the display is not used any more; see the screen in the browser (**Tester panel**). |
| With the factory firmware or other firmware: `Bye!` right after start, or the screen flashes | Battery check: ComponentTester switches off below 6.4 V. Our firmware has no battery check; power it from the ESP32. |
| Strange result for a part (`->\|- 4`, "JFET" with `x` pins) | It is an integrated circuit (regulator, op-amp…) or a damaged part. The tester only identifies single components. |
| Small capacitance shown with an empty socket (e.g. 30 pF) | Not adjusted yet — run the self-adjustment (4.5). |

## ESP32 and Wi-Fi

| Symptom | Cause and fix |
|---|---|
| No port in esptool-js | Charge-only cable, or the board needs bootloader mode: hold **BOOT**, plug in, release. |
| **Tester-Setup** does not appear | Wait 20 s after power-up. The ESP32 opens it only when it has no saved network or cannot reach it. |
| Phone: "IP configuration failure" on Tester-Setup | Forget the network on the phone and join again. (Firmware older than 1.0 kept scanning in the background and missed the phone's requests.) |
| Phone joined, no sign-in page | Open **http://192.168.4.1** in the browser. |
| `http://tester.local/` does not open | Android often does not resolve `.local` — use the IP address from the router's device list. On Windows, `.local` needs Bonjour/mDNS (present on most current systems). |
| Tester went back to **Tester-Setup** | It lost the saved network for 60 s. It keeps retrying every 5 minutes (when no phone is connected) and closes **Tester-Setup** one minute after it is back. |

## Measuring through the API

| Symptom | Cause and fix |
|---|---|
| `/measure` answers `"error":"no fresh result screen"` (HTTP 504) | No text from the tester: check PD6 → 10 kΩ → GPIO4 and the 20 kΩ to GND; check the tester is powered (5V/GND). |
| `/measure` takes 10 s or more and `reset_banner` is `false` | The restart does not reach the tester: RES diode reversed or wire on the wrong header pin. Measuring still works (it waits for the automatic cycle). |
| **Menu** does nothing | Pressed during `Probing...`. Wait until a result is on the screen. If it never works: PD7 diode reversed or wrong pad. |
| History is empty after an update | Should not happen — the history lives in its own flash area that no update touches. |
| After flashing over USB the tester is back in **Tester-Setup** | Expected: the `-full.bin` image also overwrites the area with the Wi-Fi settings. Set Wi-Fi again (4.2); the history is kept. Updates over Wi-Fi keep everything. |

Next: [8. How it works](08-how-it-works.md)
