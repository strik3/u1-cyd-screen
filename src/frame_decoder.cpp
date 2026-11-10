#include "frame_decoder.h"
#include <PNGdec.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// StreamWindow
// ---------------------------------------------------------------------------
void StreamWindow::reset(ByteSource *src) {
    _src = src;
    _pulled = 0;
    _pos = 0;
}

int32_t StreamWindow::pullFromSource(uint8_t *dst, int32_t len) {
    int32_t got = _src ? _src->read(dst, len) : 0;
    if (got <= 0) return 0;
    // Append to the history ring.
    for (int32_t i = 0; i < got; i++) {
        _hist[(_pulled + i) % HISTORY] = dst[i];
    }
    _pulled += got;
    return got;
}

int32_t StreamWindow::read(uint8_t *dst, int32_t len) {
    int32_t done = 0;
    // 1) Serve whatever we can from history (after a backwards seek).
    while (done < len && _pos < _pulled) {
        dst[done++] = _hist[_pos % HISTORY];
        _pos++;
    }
    // 2) Then pull fresh bytes from the source until satisfied or EOF.
    while (done < len) {
        int32_t got = pullFromSource(dst + done, len - done);
        if (got <= 0) break;
        done += got;
        _pos += got;
    }
    return done;
}

int32_t StreamWindow::seek(int32_t pos) {
    if (pos < 0) return -1;
    if (pos <= _pulled) {
        if (_pulled - pos > HISTORY) return -1;  // fell out of the window
        _pos = pos;
        return _pos;
    }
    // Forward seek past what we've pulled: read and discard.
    _pos = _pulled;
    uint8_t scratch[256];
    while (_pulled < pos) {
        int32_t want = pos - _pulled;
        if (want > (int32_t)sizeof(scratch)) want = sizeof(scratch);
        int32_t got = pullFromSource(scratch, want);
        if (got <= 0) break;
    }
    _pos = _pulled;
    return _pos;
}

// ---------------------------------------------------------------------------
// PNGdec glue. PNGdec's open callback has no user pointer, so we pass the
// active window through a static (decoding is single-threaded).
// ---------------------------------------------------------------------------
static PNG s_png;                 // ~49 KB, lives in .bss
static StreamWindow s_window;
static int32_t s_openSize = 0;

static void *cbOpen(const char *, int32_t *size) {
    *size = s_openSize;
    return &s_window;
}
static void cbClose(void *) {}
static int32_t cbRead(PNGFILE *f, uint8_t *buf, int32_t len) {
    StreamWindow *w = (StreamWindow *)f->fHandle;
    int32_t n = w->read(buf, len);
    f->iPos = w->pos();
    return n;
}
static int32_t cbSeek(PNGFILE *f, int32_t pos) {
    StreamWindow *w = (StreamWindow *)f->fHandle;
    int32_t r = w->seek(pos);
    f->iPos = w->pos();
    return r;
}
static int cbDraw(PNGDRAW *d) {
    FrameDecoder *self = (FrameDecoder *)d->pUser;
    return self->onLine(d->pPixels, d->iPixelType, d->iWidth, d->y);
}

// ---------------------------------------------------------------------------
// FrameDecoder
// ---------------------------------------------------------------------------
FrameDecoder::FrameDecoder() {}

FrameDecoder::~FrameDecoder() {
    free(_hIdx); free(_hW0); free(_hRow); free(_acc); free(_out);
}

void FrameDecoder::setTarget(int dispW, int dispH, ScaleMode mode) {
    _dispW = dispW;
    _dispH = dispH;
    _mode = mode;
    _srcW = _srcH = 0;  // force geometry recompute on next frame
}

bool FrameDecoder::prepareGeometry(int w, int h) {
    _geomChanged = false;
    if (w == _srcW && h == _srcH && _hIdx) return true;
    _geomChanged = true;
    _srcW = w;
    _srcH = h;

    if (_mode == SCALE_STRETCH) {
        _dstW = _dispW;
        _dstH = _dispH;
    } else {
        // Fit, preserving aspect ratio. Never upscale.
        if ((int64_t)w * _dispH > (int64_t)h * _dispW) {  // source is wider
            _dstW = _dispW;
            _dstH = (int)((int64_t)h * _dispW / w);
        } else {
            _dstH = _dispH;
            _dstW = (int)((int64_t)w * _dispH / h);
        }
    }
    if (_dstW > w) _dstW = w;
    if (_dstH > h) _dstH = h;
    if (_dstW < 1) _dstW = 1;
    if (_dstH < 1) _dstH = 1;
    _dstX = (_dispW - _dstW) / 2;
    _dstY = (_dispH - _dstH) / 2;

    if (w > _hCap) {
        free(_hIdx); free(_hW0);
        _hIdx = (uint16_t *)malloc(w * sizeof(uint16_t));
        _hW0 = (uint16_t *)malloc(w * sizeof(uint16_t));
        if (!_hIdx || !_hW0) { _hCap = 0; _srcW = 0; return false; }
        _hCap = w;
    }
    if (_dstW > _rowCap) {
        free(_hRow); free(_acc); free(_out);
        _hRow = (uint32_t *)malloc(_dstW * 3 * sizeof(uint32_t));
        _acc = (uint32_t *)malloc(_dstW * 3 * sizeof(uint32_t));
        _out = (uint16_t *)malloc(_dstW * sizeof(uint16_t));
        if (!_hRow || !_acc || !_out) { _rowCap = 0; _srcW = 0; return false; }
        _rowCap = _dstW;
    }

    // Area-average weights. In units where one dest pixel == srcW units,
    // source column x covers [x*dstW, (x+1)*dstW). Because we only downscale,
    // a source column overlaps at most two dest columns.
    for (int x = 0; x < w; x++) {
        int32_t start = x * _dstW;
        int32_t end = start + _dstW;
        int d0 = start / w;
        int32_t boundary = (d0 + 1) * w;
        _hIdx[x] = (uint16_t)d0;
        _hW0[x] = (uint16_t)((end <= boundary) ? _dstW : (boundary - start));
    }
    return true;
}

void FrameDecoder::flushRow() {
    // Each dest pixel accumulated a total weight of srcW * srcH.
    const uint32_t div = (uint32_t)_srcW * (uint32_t)_srcH;
    const uint32_t half = div / 2;
    for (int i = 0; i < _dstW; i++) {
        uint32_t r = (_acc[i * 3 + 0] + half) / div;
        uint32_t g = (_acc[i * 3 + 1] + half) / div;
        uint32_t b = (_acc[i * 3 + 2] + half) / div;
        _out[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    if (_sink) _sink(_accRow, _out, _dstW, _user);
    memset(_acc, 0, _dstW * 3 * sizeof(uint32_t));
    _accRow++;
}

int FrameDecoder::onLine(const uint8_t *px, int pixelType, int width, int y) {
    const int bpp = (pixelType == PNG_PIXEL_TRUECOLOR_ALPHA) ? 4 : 3;

    // 1) Horizontal reduction of this source line into _hRow.
    memset(_hRow, 0, _dstW * 3 * sizeof(uint32_t));
    for (int x = 0; x < width; x++) {
        const uint8_t *p = px + x * bpp;
        uint32_t r = p[0], g = p[1], b = p[2];
        int d0 = _hIdx[x];
        uint32_t w0 = _hW0[x];
        uint32_t *h = &_hRow[d0 * 3];
        h[0] += r * w0; h[1] += g * w0; h[2] += b * w0;
        uint32_t w1 = (uint32_t)_dstW - w0;
        if (w1 && d0 + 1 < _dstW) {
            h += 3;
            h[0] += r * w1; h[1] += g * w1; h[2] += b * w1;
        }
    }

    // 2) Vertical: source row y covers [y*dstH, (y+1)*dstH) in units where one
    //    dest row == srcH units.
    int32_t start = y * _dstH;
    int32_t end = start + _dstH;
    int d0 = start / _srcH;
    int32_t boundary = (d0 + 1) * _srcH;
    uint32_t w0 = (end <= boundary) ? (uint32_t)_dstH : (uint32_t)(boundary - start);
    uint32_t w1 = (uint32_t)_dstH - w0;

    const int n = _dstW * 3;
    for (int i = 0; i < n; i++) _acc[i] += _hRow[i] * w0;
    if (end >= boundary && _accRow < _dstH) flushRow();
    if (w1 && _accRow < _dstH) {
        for (int i = 0; i < n; i++) _acc[i] += _hRow[i] * w1;
    }
    return 1;  // keep decoding
}

int FrameDecoder::decode(ByteSource *src, int32_t contentLength, RowSink sink, void *user) {
    s_window.reset(src);
    // PNGdec sanity-checks chunk lengths against the file size; if the server
    // didn't send Content-Length, give it a generous upper bound.
    s_openSize = contentLength > 0 ? contentLength : (16 * 1024 * 1024);

    int rc = s_png.open("u1", cbOpen, cbClose, cbRead, cbSeek, cbDraw);
    if (rc != PNG_SUCCESS) return rc ? rc : PNG_INVALID_FILE;

    int w = s_png.getWidth(), h = s_png.getHeight();
    int type = s_png.getPixelType();
    if (s_png.getBpp() != 8 ||
        (type != PNG_PIXEL_TRUECOLOR && type != PNG_PIXEL_TRUECOLOR_ALPHA) ||
        s_png.isInterlaced()) {
        s_png.close();
        return PNG_UNSUPPORTED_FEATURE;
    }
    if ((w * (type == PNG_PIXEL_TRUECOLOR_ALPHA ? 4 : 3) + 1) * 2 > PNG_MAX_BUFFERED_PIXELS) {
        s_png.close();
        return PNG_TOO_BIG;
    }
    if (!prepareGeometry(w, h)) {
        s_png.close();
        return PNG_MEM_ERROR;
    }

    _sink = sink;
    _user = user;
    _accRow = 0;
    memset(_acc, 0, _dstW * 3 * sizeof(uint32_t));

    rc = s_png.decode(this, 0);
    s_png.close();
    // Rounding can leave the final row un-flushed; emit it.
    if (rc == PNG_SUCCESS && _accRow < _dstH) flushRow();
    return rc;
}

FrameGeometry FrameDecoder::geometry() const {
    FrameGeometry g;
    g.srcW = _srcW; g.srcH = _srcH;
    g.dstX = _dstX; g.dstY = _dstY; g.dstW = _dstW; g.dstH = _dstH;
    return g;
}

bool mapDisplayToSource(const FrameGeometry &g, int dx, int dy, int *sx, int *sy, bool clamp) {
    if (g.srcW == 0 || g.dstW == 0) return false;
    int rx = dx - g.dstX, ry = dy - g.dstY;
    bool inside = rx >= 0 && ry >= 0 && rx < g.dstW && ry < g.dstH;
    if (!inside && !clamp) return false;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx >= g.dstW) rx = g.dstW - 1;
    if (ry >= g.dstH) ry = g.dstH - 1;
    // Map the centre of the display pixel into the source.
    *sx = (int)(((int64_t)(2 * rx + 1) * g.srcW) / (2 * g.dstW));
    *sy = (int)(((int64_t)(2 * ry + 1) * g.srcH) / (2 * g.dstH));
    return true;
}

const char *pngErrorString(int err) {
    switch (err) {
        case PNG_SUCCESS: return "ok";
        case PNG_INVALID_PARAMETER: return "invalid parameter";
        case PNG_DECODE_ERROR: return "decode error (truncated frame?)";
        case PNG_MEM_ERROR: return "out of memory";
        case PNG_NO_BUFFER: return "no buffer";
        case PNG_UNSUPPORTED_FEATURE: return "unsupported PNG format";
        case PNG_INVALID_FILE: return "not a PNG";
        case PNG_TOO_BIG: return "frame too wide";
        case PNG_QUIT_EARLY: return "aborted";
        default: return "unknown error";
    }
}
