# U1 Remote Screen for the Cheap Yellow Display

Firmware for the ESP32 "Cheap Yellow Display" (ESP32-2432S028R, 2.8" 320×240) that mirrors a **Snapmaker U1** touchscreen and lets you control the printer from it. It needs paxx12's Extended Firmware with **Remote Screen** turned on.

It uses the same endpoints as the printer's own `/screen/` web page:

| | |
|---|---|
| `GET http://<printer>/screen/snapshot` | PNG of the U1 framebuffer. It honours `If-None-Match`, so an unchanged screen costs a tiny 304. |
| `POST http://<printer>/screen/touch?a=down\|move\|up&x=&y=` | Touch input, in printer-screen pixels. |

## 1. Enable Remote Screen on the printer

Open `http://<printer-ip>/firmware-config`, go to the **Web** section, enable **Remote Screen**, then reboot the printer. Check that `http://<printer-ip>/screen/` works in a browser before you continue.

## 2. Build and flash

You need [PlatformIO](https://platformio.org/) (the VS Code extension is easiest).

```bash
# Original CYD (one micro-USB port):
pio run -e cyd -t upload
# Newer CYD with both micro-USB and USB-C ports:
pio run -e cyd2usb -t upload

pio device monitor     # optional: logs at 115200 baud
```

If the colours look inverted or wrong, flash the other environment.

## 3. First boot

1. **Touch calibration.** Press and hold each red cross in turn. You can then draw on the test screen to check it, and tap **Done**.
2. **Setup hotspot.** Join the Wi-Fi network `U1-Screen-Setup` from your phone and open `http://192.168.4.1`. Pick your Wi-Fi network and enter:
   - **Printer IP**, e.g. `192.168.1.50`
   - **Moonraker API key**: leave it blank unless you turned on "Require Login" in firmware-config
   - **Scaling**: `fit` keeps the aspect ratio and leaves thin black bars. `stretch` fills the whole screen.
3. The U1 screen appears. Tap and drag on it as you would on the printer.

**Changing settings later:** press the CYD's reset button (EN) and tap the screen while the splash shows. From there you can recalibrate or reopen the Wi-Fi / printer setup.

## Options (`include/config.h`)

| Setting | Default | |
|---|---|---|
| `DISPLAY_ROTATION` | `1` | Set to `3` if the CYD is mounted upside down. |
| `INVERT_COLOURS` | `true` | Inverts all display colours. Flip this if colours look like a photo negative. |
| `BACKLIGHT_LEVEL` | `255` | 0–255 |
| `SCREEN_SLEEP_MINUTES` | `0` | Turns the backlight off and stops polling after N idle minutes. The first touch only wakes the screen. |
| `FRAME_INTERVAL_MS` | `100` | Minimum time between frame requests. The printer's web UI uses the same value. |
| `TOUCH_MOVE_INTERVAL_MS` | `40` | How often drag updates are sent. |

## How it works

- **Streaming PNG decode.** A full 480×320 frame would not fit in the CYD's RAM, so each frame is decoded straight from the network socket one line at a time, using PNGdec with a small rewind buffer. Each line is area-averaged down to 320 px wide. This keeps the U1's text much more readable than simple pixel-dropping would.
- **Change detection.** Frames carry the printer's ETag. An unchanged screen costs a tiny 304 response and is not redrawn.
- **Touch on core 0.** One task samples the XPT2046 panel and debounces it. A second task POSTs the events over a keep-alive connection, merges any backlog of drag moves, and retries `down`/`up` so the printer never ends up with a "stuck finger".
- **Error screens.** You get a clear message when the printer can't be reached, when Remote Screen is off (HTTP 404/502), or when a login is required (HTTP 401/403).

## Troubleshooting

- **"Remote Screen is off":** enable it in firmware-config and reboot the printer (step 1).
- **"Login required":** create an API key in Fluidd/Mainsail (Settings → Authentication) and enter it in setup. It is sent as `X-Api-Key`, which nginx passes on to Moonraker's auth check.
- **Touches land in the wrong place:** recalibrate from the boot menu.
- **White or blank screen:** you probably flashed the wrong environment (`cyd` vs `cyd2usb`).
- **Laggy:** expect roughly 3–6 new frames per second while the screen is changing. Static screens cost almost nothing. Make sure the CYD has a decent Wi-Fi signal.

## Credits

Remote Screen server: [paxx12/screen-apps](https://github.com/paxx12/screen-apps) (`fb-http`). Libraries: TFT_eSPI, XPT2046_Touchscreen, PNGdec, WiFiManager.
# u1-cyd-screen
