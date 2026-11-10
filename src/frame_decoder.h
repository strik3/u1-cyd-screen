// Portable (no Arduino deps) streaming PNG -> scaled RGB565 row decoder.
//
// The U1's fb-http server sends each frame as an unfiltered 24-bit PNG
// (typically 480x320). A full decoded frame would not fit in the CYD's RAM,
// so we decode straight off the network socket, one source line at a time,
// area-average it down to the target rectangle and emit finished RGB565 rows.
//
// PNGdec reads mostly sequentially but occasionally seeks *backwards* by up to
// one file buffer (2 KB). StreamWindow keeps a small history ring so those
// seeks can be satisfied without buffering the whole file.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Anything that can hand us bytes (a TCP socket on the ESP32, a file in tests).
class ByteSource {
public:
    virtual ~ByteSource() {}
    // Read up to len bytes. Return bytes read (>0), or <=0 on EOF/error/timeout.
    virtual int32_t read(uint8_t *buf, int32_t len) = 0;
};

class StreamWindow {
public:
    static const int32_t HISTORY = 8192;  // must exceed PNG_FILE_BUF_SIZE (2048)

    void reset(ByteSource *src);
    int32_t read(uint8_t *dst, int32_t len);
    // Returns new position, or -1 if the target is no longer in the window.
    int32_t seek(int32_t pos);
    int32_t pos() const { return _pos; }
    int32_t pulled() const { return _pulled; }

private:
    int32_t pullFromSource(uint8_t *dst, int32_t len);
    ByteSource *_src = nullptr;
    uint8_t _hist[HISTORY];
    int32_t _pulled = 0;  // total bytes taken from the source so far
    int32_t _pos = 0;     // logical read position
};

// Called once per finished output row (RGB565, native endianness).
typedef void (*RowSink)(int dstY, const uint16_t *pixels, int width, void *user);

enum ScaleMode { SCALE_FIT = 0, SCALE_STRETCH = 1 };

// Where the printer's screen lands on the display. Plain struct so it can be
// copied to the touch task safely.
struct FrameGeometry {
    int srcW = 0, srcH = 0;              // printer screen size
    int dstX = 0, dstY = 0, dstW = 0, dstH = 0;  // rectangle on the display
};

// Map a display coordinate to printer-screen coordinates. Returns false if
// outside the image (unless clamp=true, which pins to the nearest edge).
bool mapDisplayToSource(const FrameGeometry &g, int dx, int dy, int *sx, int *sy, bool clamp);

class FrameDecoder {
public:
    FrameDecoder();
    ~FrameDecoder();

    // Target area on the display.
    void setTarget(int dispW, int dispH, ScaleMode mode);

    // Decode one PNG from src. contentLength may be <0 if unknown.
    // Returns 0 on success, otherwise a PNGdec error code (or -1 for setup errors).
    int decode(ByteSource *src, int32_t contentLength, RowSink sink, void *user);

    // Geometry of the current frame (valid once the PNG header has been read,
    // i.e. from the first RowSink call onwards).
    FrameGeometry geometry() const;
    // True if this frame's size differs from the previous one (letterbox
    // bars need clearing).
    bool geometryChanged() const { return _geomChanged; }

    // Internal (called from the PNGdec draw callback).
    int onLine(const uint8_t *pixels, int pixelType, int width, int y);

private:
    bool prepareGeometry(int w, int h);
    void flushRow();

    int _dispW = 320, _dispH = 240;
    ScaleMode _mode = SCALE_FIT;

    int _srcW = 0, _srcH = 0;
    int _dstX = 0, _dstY = 0, _dstW = 0, _dstH = 0;
    bool _geomChanged = false;

    // Horizontal mapping, per source column: first dest column + weight into it.
    uint16_t *_hIdx = nullptr;
    uint16_t *_hW0 = nullptr;
    int _hCap = 0;

    uint32_t *_hRow = nullptr;   // one source line reduced horizontally (dstW*3)
    uint32_t *_acc = nullptr;    // vertical accumulator (dstW*3)
    uint16_t *_out = nullptr;    // RGB565 output row
    int _rowCap = 0;
    int _accRow = 0;             // dest row currently being accumulated

    RowSink _sink = nullptr;
    void *_user = nullptr;
};

const char *pngErrorString(int err);
