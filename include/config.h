// Compile-time defaults. Printer address, Wi-Fi and API key are normally set
// from the on-device setup portal and stored in flash; these are fallbacks.
#pragma once

// ---- Defaults for the setup portal -----------------------------------------
#define DEFAULT_PRINTER_HOST   ""          // e.g. "192.168.1.50" or "u1.local"
#define DEFAULT_API_KEY        ""          // Moonraker API key, only if login is required
#define DEFAULT_SCALE_STRETCH  false       // false = keep aspect (thin black bars), true = fill screen

#define SETUP_AP_NAME          "U1-Screen-Setup"
#define SETUP_AP_PASSWORD      ""          // set 8+ chars to password-protect the setup hotspot
#define SETUP_PORTAL_TIMEOUT_S 300         // reboot if setup isn't finished in this time

// ---- Display ---------------------------------------------------------------
#define DISPLAY_ROTATION       1           // 1 = landscape, 3 = landscape flipped 180°
#define INVERT_COLOURS         true        // flip all display colours (set false if they look wrong)
#define BACKLIGHT_PIN          21
#define BACKLIGHT_LEVEL        255         // 0-255
#define SCREEN_SLEEP_MINUTES   0           // backlight off after N min without touch (0 = never)

// ---- Streaming -------------------------------------------------------------
#define FRAME_INTERVAL_MS      100         // min time between frame requests (web UI uses 100)
#define ERROR_RETRY_MS         1500        // retry interval while the printer is unreachable
#define HTTP_TIMEOUT_MS        3000

// ---- Touch (XPT2046 on its own SPI bus) ------------------------------------
#define TOUCH_CS    33
#define TOUCH_IRQ   36
#define TOUCH_MOSI  32
#define TOUCH_MISO  39
#define TOUCH_CLK   25

// Raw readings at the screen edges, used until you run calibration.
#define TOUCH_RAW_X_LEFT    200
#define TOUCH_RAW_X_RIGHT   3700
#define TOUCH_RAW_Y_TOP     240
#define TOUCH_RAW_Y_BOTTOM  3800

#define TOUCH_MOVE_INTERVAL_MS  40         // throttle drag updates sent to the printer
#define TOUCH_MOVE_MIN_PX       3          // ignore jitter smaller than this (printer pixels)
