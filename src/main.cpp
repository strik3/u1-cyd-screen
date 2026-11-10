// U1 Remote Screen for the ESP32 Cheap Yellow Display (ESP32-2432S028R)
//
// Mirrors the Snapmaker U1 touchscreen exposed by paxx12's Extended Firmware
// ("Remote Screen" feature) and forwards touches back to the printer.
//
//   GET  http://<printer>/screen/snapshot          -> PNG of the U1 framebuffer
//                                                     (ETag / If-None-Match -> 304)
//   POST http://<printer>/screen/touch?a=down|move|up&x=&y=   (printer pixels)
//
// Core 1 (Arduino loop): fetch + stream-decode + draw frames.
// Core 0: one task samples the touch panel, another posts touch events.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFiManager.h>

#include "config.h"
#include "frame_decoder.h"

static const int SCREEN_W = 320;
static const int SCREEN_H = 240;
static const int BL_CHANNEL = 0;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static TFT_eSPI tft;
static SPIClass touchSpi(VSPI);
static XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
static Preferences prefs;
static FrameDecoder decoder;

struct Settings {
    char host[64];
    char apiKey[72];
    bool stretch;
} settings;

struct TouchCal {
    int16_t xLeft, xRight, yTop, yBottom;  // raw readings at the display edges
} cal;

// Geometry snapshot shared with the touch task.
static portMUX_TYPE geomMux = portMUX_INITIALIZER_UNLOCKED;
static FrameGeometry sharedGeom;

struct TouchEvent {
    char action;  // 'd' down, 'm' move, 'u' up
    int16_t x, y;
};
static QueueHandle_t touchQueue;

static volatile bool requestRefresh = false;
static volatile uint32_t lastTouchMs = 0;
static volatile bool screenAsleep = false;

static String frameEtag;
static String baseUrl;

// ---------------------------------------------------------------------------
// Backlight
// ---------------------------------------------------------------------------
static void backlight(uint8_t level) { ledcWrite(BL_CHANNEL, level); }

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------
static void loadSettings() {
    prefs.begin("u1screen", true);
    String h = prefs.getString("host", DEFAULT_PRINTER_HOST);
    String k = prefs.getString("apikey", DEFAULT_API_KEY);
    settings.stretch = prefs.getBool("stretch", DEFAULT_SCALE_STRETCH);
    cal.xLeft = prefs.getShort("cxl", TOUCH_RAW_X_LEFT);
    cal.xRight = prefs.getShort("cxr", TOUCH_RAW_X_RIGHT);
    cal.yTop = prefs.getShort("cyt", TOUCH_RAW_Y_TOP);
    cal.yBottom = prefs.getShort("cyb", TOUCH_RAW_Y_BOTTOM);
    prefs.end();
    strlcpy(settings.host, h.c_str(), sizeof(settings.host));
    strlcpy(settings.apiKey, k.c_str(), sizeof(settings.apiKey));
}

static bool hasCalibration() {
    prefs.begin("u1screen", true);
    bool ok = prefs.isKey("cxl");
    prefs.end();
    return ok;
}

static void saveSettings() {
    prefs.begin("u1screen", false);
    prefs.putString("host", settings.host);
    prefs.putString("apikey", settings.apiKey);
    prefs.putBool("stretch", settings.stretch);
    prefs.end();
}

static void saveCalibration() {
    prefs.begin("u1screen", false);
    prefs.putShort("cxl", cal.xLeft);
    prefs.putShort("cxr", cal.xRight);
    prefs.putShort("cyt", cal.yTop);
    prefs.putShort("cyb", cal.yBottom);
    prefs.end();
}

// ---------------------------------------------------------------------------
// Simple UI helpers (only used outside of streaming)
// ---------------------------------------------------------------------------
static void drawMessage(const char *title, const String &l1 = "", const String &l2 = "",
                        const String &l3 = "", uint16_t titleColor = TFT_WHITE) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(titleColor, TFT_BLACK);
    tft.drawString(title, SCREEN_W / 2, 70, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    if (l1.length()) tft.drawString(l1, SCREEN_W / 2, 120, 2);
    if (l2.length()) tft.drawString(l2, SCREEN_W / 2, 145, 2);
    if (l3.length()) tft.drawString(l3, SCREEN_W / 2, 170, 2);
}

static void rawToScreen(const TS_Point &p, int *x, int *y) {
    int dx = cal.xRight - cal.xLeft;
    int dy = cal.yBottom - cal.yTop;
    if (dx == 0) dx = 1;
    if (dy == 0) dy = 1;
    *x = constrain((int)((long)(p.x - cal.xLeft) * SCREEN_W / dx), 0, SCREEN_W - 1);
    *y = constrain((int)((long)(p.y - cal.yTop) * SCREEN_H / dy), 0, SCREEN_H - 1);
}

// Blocking: wait for a press and release, return the press position.
static void waitTap(int *x, int *y) {
    while (ts.touched()) delay(10);
    int hits = 0;
    while (true) {
        if (ts.touched()) {
            if (++hits >= 3) break;
        } else {
            hits = 0;
        }
        delay(10);
    }
    rawToScreen(ts.getPoint(), x, y);
    while (ts.touched()) delay(10);
    delay(50);
}

static void drawButton(int y, const char *label, uint16_t color) {
    tft.fillRoundRect(20, y, SCREEN_W - 40, 44, 8, color);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, color);
    tft.drawString(label, SCREEN_W / 2, y + 22, 4);
}

// ---------------------------------------------------------------------------
// Touch calibration
// ---------------------------------------------------------------------------
static TS_Point sampleTarget(int x, int y) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Touch calibration", SCREEN_W / 2, SCREEN_H / 2 - 14, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Press and hold the red cross", SCREEN_W / 2, SCREEN_H / 2 + 16, 2);
    tft.drawLine(x - 10, y, x + 10, y, TFT_RED);
    tft.drawLine(x, y - 10, x, y + 10, TFT_RED);
    tft.drawCircle(x, y, 6, TFT_RED);

    while (ts.touched()) delay(10);
    long sx = 0, sy = 0;
    int n = 0, hits = 0;
    while (n < 24) {
        if (ts.touched()) {
            if (++hits > 4) {  // skip the first few noisy samples
                TS_Point p = ts.getPoint();
                sx += p.x; sy += p.y; n++;
            }
        } else {
            hits = 0;
            sx = sy = 0; n = 0;  // lifted early: start over
        }
        delay(10);
    }
    tft.fillCircle(x, y, 6, TFT_GREEN);
    while (ts.touched()) delay(10);
    delay(200);
    return TS_Point(sx / n, sy / n, 0);
}

static void runCalibration() {
    const int M = 24;
    TS_Point tl = sampleTarget(M, M);
    TS_Point tr = sampleTarget(SCREEN_W - 1 - M, M);
    TS_Point br = sampleTarget(SCREEN_W - 1 - M, SCREEN_H - 1 - M);
    TS_Point bl = sampleTarget(M, SCREEN_H - 1 - M);

    float xl = (tl.x + bl.x) / 2.0f, xr = (tr.x + br.x) / 2.0f;
    float yt = (tl.y + tr.y) / 2.0f, yb = (bl.y + br.y) / 2.0f;
    // Extrapolate from the target positions out to the screen edges.
    float xPerPx = (xr - xl) / (SCREEN_W - 1 - 2 * M);
    float yPerPx = (yb - yt) / (SCREEN_H - 1 - 2 * M);
    cal.xLeft = (int16_t)(xl - xPerPx * M);
    cal.xRight = (int16_t)(xr + xPerPx * (M + 1));
    cal.yTop = (int16_t)(yt - yPerPx * M);
    cal.yBottom = (int16_t)(yb + yPerPx * (M + 1));
    saveCalibration();
    Serial.printf("Calibration: x %d..%d  y %d..%d\n", cal.xLeft, cal.xRight, cal.yTop, cal.yBottom);

    // Quick test screen: draw dots where touched, tap "Done" to finish.
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Draw to test. Tap Done when happy.", SCREEN_W / 2, 20, 2);
    tft.fillRoundRect(SCREEN_W - 90, SCREEN_H - 50, 80, 40, 6, TFT_DARKGREEN);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.drawString("Done", SCREEN_W - 50, SCREEN_H - 30, 2);
    uint32_t idleSince = millis();
    while (millis() - idleSince < 30000) {
        if (ts.touched()) {
            int x, y;
            rawToScreen(ts.getPoint(), &x, &y);
            idleSince = millis();
            if (x > SCREEN_W - 90 && y > SCREEN_H - 50) break;
            tft.fillCircle(x, y, 2, TFT_YELLOW);
        }
        delay(5);
    }
    while (ts.touched()) delay(10);
}

// ---------------------------------------------------------------------------
// Wi-Fi + printer setup (captive portal)
// ---------------------------------------------------------------------------
static void showPortalInfo(WiFiManager *) {
    drawMessage("Setup mode",
                String("1. Join Wi-Fi \"") + SETUP_AP_NAME + "\"",
                "2. Open http://192.168.4.1",
                "3. Pick your Wi-Fi + enter the printer IP", TFT_YELLOW);
}

static void connectWifi(bool forcePortal) {
    WiFiManager wm;
    char scaleBuf[10];
    strlcpy(scaleBuf, settings.stretch ? "stretch" : "fit", sizeof(scaleBuf));

    WiFiManagerParameter pHost("host", "Printer IP address or hostname", settings.host, 63);
    WiFiManagerParameter pKey("apikey", "Moonraker API key (leave blank unless login is required)",
                              settings.apiKey, 70);
    WiFiManagerParameter pScale("scale", "Scaling: fit or stretch", scaleBuf, 9);
    wm.addParameter(&pHost);
    wm.addParameter(&pKey);
    wm.addParameter(&pScale);
    wm.setAPCallback(showPortalInfo);
    wm.setConfigPortalTimeout(SETUP_PORTAL_TIMEOUT_S);
    wm.setConnectTimeout(20);

    const char *apPass = strlen(SETUP_AP_PASSWORD) >= 8 ? SETUP_AP_PASSWORD : nullptr;
    if (!forcePortal) drawMessage("Connecting...", "Joining Wi-Fi");
    bool ok = forcePortal ? wm.startConfigPortal(SETUP_AP_NAME, apPass)
                          : wm.autoConnect(SETUP_AP_NAME, apPass);

    // Copy back anything the user entered (no-op if the portal never ran).
    Settings before = settings;
    String host = pHost.getValue();
    host.trim();
    host.replace("http://", "");
    host.replace("https://", "");
    while (host.endsWith("/")) host.remove(host.length() - 1);
    strlcpy(settings.host, host.c_str(), sizeof(settings.host));
    String key = pKey.getValue();
    key.trim();
    strlcpy(settings.apiKey, key.c_str(), sizeof(settings.apiKey));
    String scale = pScale.getValue();
    scale.trim();
    scale.toLowerCase();
    settings.stretch = scale.startsWith("s");
    if (memcmp(&before, &settings, sizeof(settings)) != 0) saveSettings();

    if (!ok) {
        drawMessage("Setup timed out", "Restarting...");
        delay(2000);
        ESP.restart();
    }
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);  // much lower latency for touch + frames
}

// ---------------------------------------------------------------------------
// Boot menu
// ---------------------------------------------------------------------------
static void runMenu() {
    while (true) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("U1 Remote Screen", SCREEN_W / 2, 18, 4);
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.drawString(String("Printer: ") + (settings.host[0] ? settings.host : "(not set)"),
                       SCREEN_W / 2, 44, 2);
        drawButton(62, "Calibrate touch", TFT_NAVY);
        drawButton(116, "Wi-Fi / printer", TFT_NAVY);
        drawButton(170, "Start", TFT_DARKGREEN);

        int x, y;
        waitTap(&x, &y);
        if (y >= 62 && y < 106) {
            runCalibration();
        } else if (y >= 116 && y < 160) {
            connectWifi(true);
        } else if (y >= 170 && y < 214) {
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Frame streaming
// ---------------------------------------------------------------------------
// ByteSource over the HTTP response socket, with a timeout and length limit.
class TcpSource : public ByteSource {
public:
    TcpSource(WiFiClient *c, int32_t len) : _c(c), _remaining(len) {}
    int32_t read(uint8_t *buf, int32_t len) override {
        if (_remaining == 0) return 0;
        if (_remaining > 0 && len > _remaining) len = _remaining;
        uint32_t start = millis();
        while (true) {
            int avail = _c->available();
            if (avail > 0) {
                int n = _c->read(buf, len < avail ? len : avail);
                if (n > 0) {
                    if (_remaining > 0) _remaining -= n;
                    return n;
                }
            } else if (!_c->connected()) {
                return 0;
            }
            if (millis() - start > HTTP_TIMEOUT_MS) return 0;
            delay(1);
        }
    }
private:
    WiFiClient *_c;
    int32_t _remaining;  // -1 = unknown
};

static void drawRow(int y, const uint16_t *px, int w, void *) {
    if (y == 0 && decoder.geometryChanged()) {
        // New frame size: clear the letterbox bars and publish the mapping.
        tft.fillScreen(TFT_BLACK);
        FrameGeometry g = decoder.geometry();
        portENTER_CRITICAL(&geomMux);
        sharedGeom = g;
        portEXIT_CRITICAL(&geomMux);
        Serial.printf("Printer screen %dx%d -> %dx%d at %d,%d\n",
                      g.srcW, g.srcH, g.dstW, g.dstH, g.dstX, g.dstY);
    }
    FrameGeometry g = decoder.geometry();
    tft.pushImage(g.dstX, g.dstY + y, w, 1, (uint16_t *)px);
}

// Forget the current frame: next frame redraws everything (including the black
// bars) and touches are ignored until it arrives.
static void resetStream() {
    frameEtag = "";
    decoder.setTarget(SCREEN_W, SCREEN_H, settings.stretch ? SCALE_STRETCH : SCALE_FIT);
    portENTER_CRITICAL(&geomMux);
    sharedGeom = FrameGeometry();
    portEXIT_CRITICAL(&geomMux);
}

static void addCommonHeaders(HTTPClient &http) {
    if (settings.apiKey[0]) http.addHeader("X-Api-Key", settings.apiKey);
}

// Returns 200 (new frame drawn), 304 (unchanged), an HTTP/HTTPClient error code,
// or 1000 + PNGdec error.
static int fetchFrame() {
    WiFiClient client;
    HTTPClient http;
    http.useHTTP10(true);  // no chunked encoding: we read the raw socket
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, baseUrl + "/screen/snapshot")) return HTTPC_ERROR_CONNECTION_REFUSED;
    const char *keys[] = {"ETag"};
    http.collectHeaders(keys, 1);
    if (frameEtag.length()) http.addHeader("If-None-Match", frameEtag);
    addCommonHeaders(http);

    int code = http.GET();
    if (code != 200) {
        http.end();
        return code;
    }
    String etag = http.header("ETag");
    int32_t len = http.getSize();
    TcpSource src(http.getStreamPtr(), len);

    tft.startWrite();
    int rc = decoder.decode(&src, len, drawRow, nullptr);
    tft.endWrite();
    http.end();

    if (rc != 0) {
        frameEtag = "";  // partial frame on screen: force a full one next time
        return 1000 + rc;
    }
    frameEtag = etag;
    return 200;
}

static void showStreamError(int code) {
    String host = settings.host;
    if (code == 401 || code == 403) {
        drawMessage("Login required", "The printer requires a Moonraker login.",
                    "Reboot, tap the screen, choose Wi-Fi / printer", "and enter a Moonraker API key.", TFT_ORANGE);
    } else if (code == 404 || code == 502 || code == 503) {
        drawMessage("Remote Screen is off", "Enable \"Remote Screen\" at",
                    "http://" + host + "/firmware-config", "then reboot the printer.", TFT_ORANGE);
    } else if (code >= 1000) {
        drawMessage("Bad frame", pngErrorString(code - 1000), "Retrying...", "", TFT_ORANGE);
    } else if (code < 0) {
        drawMessage("Can't reach printer", host, HTTPClient::errorToString(code), "Retrying...", TFT_RED);
    } else {
        drawMessage("Printer error", host, "HTTP " + String(code), "Retrying...", TFT_RED);
    }
}

// ---------------------------------------------------------------------------
// Touch tasks (core 0)
// ---------------------------------------------------------------------------
static bool sendTouch(HTTPClient &http, WiFiClient &client, const TouchEvent &e) {
    const char *a = e.action == 'd' ? "down" : e.action == 'm' ? "move" : "up";
    String url = baseUrl + "/screen/touch?a=" + a + "&x=" + e.x + "&y=" + e.y;
    if (!http.begin(client, url)) return false;
    http.setTimeout(1500);
    http.addHeader("Content-Length", "0");
    addCommonHeaders(http);
    int code = http.POST((uint8_t *)nullptr, 0);
    http.end();
    if (code != 200) Serial.printf("touch %s -> %d\n", a, code);
    return code == 200;
}

static void touchSendTask(void *) {
    WiFiClient client;
    HTTPClient http;
    http.setReuse(true);  // keep-alive to nginx: much faster drags
    TouchEvent e, next;
    for (;;) {
        if (xQueueReceive(touchQueue, &e, portMAX_DELAY) != pdTRUE) continue;
        // Collapse a backlog of moves into the most recent one.
        while (e.action == 'm' && xQueuePeek(touchQueue, &next, 0) == pdTRUE && next.action == 'm') {
            xQueueReceive(touchQueue, &e, 0);
        }
        // Downs and ups must arrive or the printer thinks a finger is stuck.
        int attempts = (e.action == 'm') ? 1 : 3;
        for (int i = 0; i < attempts; i++) {
            if (sendTouch(http, client, e)) break;
            client.stop();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (e.action == 'u') requestRefresh = true;
    }
}

static void queueTouch(char action, int x, int y) {
    TouchEvent e = {action, (int16_t)x, (int16_t)y};
    if (action == 'm' && uxQueueSpacesAvailable(touchQueue) < 8) return;  // drop moves under load
    xQueueSend(touchQueue, &e, action == 'm' ? 0 : pdMS_TO_TICKS(100));
}

static void touchReadTask(void *) {
    bool down = false;     // finger down and forwarded to printer
    bool swallow = false;  // current press is ignored (woke screen / outside image)
    int hits = 0, misses = 0;
    int lastX = 0, lastY = 0;
    uint32_t lastMoveMs = 0;

    for (;;) {
        bool touched = ts.touched();
        if (touched) {
            misses = 0;
            TS_Point p = ts.getPoint();
            int dx, dy;
            rawToScreen(p, &dx, &dy);
            FrameGeometry g;
            portENTER_CRITICAL(&geomMux);
            g = sharedGeom;
            portEXIT_CRITICAL(&geomMux);

            if (!down && !swallow) {
                if (++hits >= 2) {  // debounce
                    lastTouchMs = millis();
                    int sx, sy;
                    if (screenAsleep) {
                        screenAsleep = false;
                        backlight(BACKLIGHT_LEVEL);
                        requestRefresh = true;
                        swallow = true;
                    } else if (mapDisplayToSource(g, dx, dy, &sx, &sy, false)) {
                        down = true;
                        lastX = sx; lastY = sy;
                        lastMoveMs = millis();
                        queueTouch('d', sx, sy);
                    } else {
                        swallow = true;  // started in the black bars
                    }
                }
            } else if (down) {
                int sx, sy;
                lastTouchMs = millis();
                if (mapDisplayToSource(g, dx, dy, &sx, &sy, true) &&
                    millis() - lastMoveMs >= TOUCH_MOVE_INTERVAL_MS &&
                    (abs(sx - lastX) >= TOUCH_MOVE_MIN_PX || abs(sy - lastY) >= TOUCH_MOVE_MIN_PX)) {
                    lastX = sx; lastY = sy;
                    lastMoveMs = millis();
                    queueTouch('m', sx, sy);
                }
            }
        } else {
            hits = 0;
            // Require a few consecutive "not touched" samples: the XPT2046
            // briefly drops out during drags.
            if (++misses >= 4) {
                if (down) queueTouch('u', lastX, lastY);
                down = false;
                swallow = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("\nU1 Remote Screen starting");

    tft.init();
    tft.setRotation(DISPLAY_ROTATION);
    tft.setSwapBytes(true);
    tft.invertDisplay(INVERT_COLOURS);
    tft.fillScreen(TFT_BLACK);

    ledcSetup(BL_CHANNEL, 5000, 8);
    ledcAttachPin(BACKLIGHT_PIN, BL_CHANNEL);
    backlight(BACKLIGHT_LEVEL);

    touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    ts.begin(touchSpi);
    ts.setRotation(DISPLAY_ROTATION);

    loadSettings();

    if (!hasCalibration()) {
        drawMessage("Welcome!", "First, let's calibrate the touchscreen.", "Tap anywhere to begin.");
        int x, y;
        waitTap(&x, &y);
        runCalibration();
    }

    // Splash with a short window to enter the menu.
    drawMessage("U1 Remote Screen", "Tap now for setup / calibration",
                String(settings.host[0] ? "Printer: " : "Printer not set") + settings.host);
    bool menu = false;
    for (uint32_t t0 = millis(); millis() - t0 < 2500;) {
        if (ts.touched()) { menu = true; break; }
        delay(20);
    }
    if (menu) {
        while (ts.touched()) delay(10);
        runMenu();
    }

    connectWifi(false);
    while (!settings.host[0]) {
        drawMessage("Printer IP needed", "Opening setup...");
        delay(1500);
        connectWifi(true);
    }
    baseUrl = String("http://") + settings.host;
    Serial.printf("Wi-Fi OK, IP %s, printer %s\n", WiFi.localIP().toString().c_str(), settings.host);
    drawMessage("Connected", "Printer: " + String(settings.host), "Waiting for screen...");

    resetStream();

    touchQueue = xQueueCreate(32, sizeof(TouchEvent));
    lastTouchMs = millis();
    xTaskCreatePinnedToCore(touchReadTask, "touchRead", 3072, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(touchSendTask, "touchSend", 8192, nullptr, 2, nullptr, 0);
}

void loop() {
    static uint32_t lastFetch = 0;
    static int failures = 0;
    static int shownError = 0;
    static uint32_t fpsT0 = 0;
    static int fpsFrames = 0;

    uint32_t now = millis();

    if (SCREEN_SLEEP_MINUTES > 0 && !screenAsleep &&
        now - lastTouchMs > (uint32_t)SCREEN_SLEEP_MINUTES * 60000UL) {
        screenAsleep = true;
        backlight(0);
    }
    if (screenAsleep) {  // don't load the printer while nobody is looking
        delay(50);
        return;
    }

    uint32_t interval = failures >= 3 ? ERROR_RETRY_MS : FRAME_INTERVAL_MS;
    if (!requestRefresh && now - lastFetch < interval) {
        delay(2);
        return;
    }
    requestRefresh = false;
    lastFetch = now;

    if (WiFi.status() != WL_CONNECTED) {
        if (shownError != -9999) {
            drawMessage("Wi-Fi lost", "Reconnecting...", "", "", TFT_RED);
            shownError = -9999;
            resetStream();
        }
        failures = 3;
        return;
    }

    int code = fetchFrame();
    if (code == 200 || code == 304) {
        failures = 0;
        shownError = 0;
        if (code == 200) fpsFrames++;
    } else {
        Serial.printf("Frame error %d\n", code);
        if (++failures >= 3 && shownError != code) {
            showStreamError(code);
            shownError = code;
            resetStream();  // error screen replaced the image
        }
    }

    if (now - fpsT0 >= 10000) {
        if (fpsFrames) Serial.printf("%.1f new frames/s, heap %u\n",
                                     fpsFrames * 1000.0f / (now - fpsT0), ESP.getFreeHeap());
        fpsT0 = now;
        fpsFrames = 0;
    }
}
