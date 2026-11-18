//
// JpegStripDecoder - strip (band) decoding on top of JPEGDEC with a single
// reused row buffer.
//
// Instead of buffering the whole image (width*height*bpp bytes), the decoder
// assembles one strip of `stripRows` image rows at a time into a small
// reusable buffer and hands each completed strip to a consumer callback.
// Peak heap usage drops from O(width*height) to O(width*stripRows)
// (stripRows is one MCU row: 16 pixels for 4:2:0/4:1:1, 8 otherwise).
//
// The JPEG file is read through POSIX fd callbacks, so the compressed data
// is never fully resident in memory either (the library streams it through
// its internal 2KB buffer).
//
// Only the library's public API is used; no JPEGDEC internals are touched.
//
#ifndef JPEG_STRIP_DECODE_H
#define JPEG_STRIP_DECODE_H

#include <JPEGDEC.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

class JpegStripDecoder {
  public:
    // Called once per completed strip, top to bottom.
    // y/height locate the strip in the image; pixels are tightly packed
    // (width * height pixels of the decoder's current pixel type).
    typedef void (STRIP_CALLBACK)(int y, int height, const uint16_t *pixels,
                                  int width, void *user);

    JpegStripDecoder() { memset(this, 0, sizeof(*this)); }

    // Open the file and allocate the reusable strip buffer.
    // Returns true on success. Call close() when done.
    bool open(const char *path, STRIP_CALLBACK *consume, void *user) {
        _consume = consume;
        _user = user;
        if (!_dec.open(path, _fileOpen, _fileClose, _fileRead, _fileSeek, _drawCb))
            return false;
        _width = _dec.getWidth();
        _height = _dec.getHeight();
        // MCU row height in pixels: 16 for vertically subsampled images
        int ss = _dec.getSubSample();
        _stripRows = (ss == 0x22 || ss == 0x12) ? 16 : 8;
        _bpp = (_dec.getPixelType() == RGB8888) ? 4 :
               (_dec.getPixelType() == EIGHT_BIT_GRAYSCALE) ? 1 : 2;
        _strip = (uint8_t *)malloc((size_t)_width * _stripRows * _bpp);
        if (!_strip) { _dec.close(); return false; }
        _dec.setUserPointer(this);
        _curStripY = -1;
        return true;
    }

    // Decode the whole image, emitting strips. Returns true on success.
    bool decode(int options = 0) {
        if (!_dec.decode(0, 0, options)) return false;
        _flush(); // final strip
        return true;
    }

    void close() {
        _dec.close();
        free(_strip);
        _strip = NULL;
    }

    int width() const { return _width; }
    int height() const { return _height; }
    int stripRows() const { return _stripRows; }
    JPEGDEC *decoder() { return &_dec; }

  private:
    JPEGDEC _dec;
    uint8_t *_strip;
    int _width, _height, _stripRows, _bpp;
    int _curStripY;
    STRIP_CALLBACK *_consume;
    void *_user;

    void _flush() {
        if (_curStripY >= 0) {
            int h = _height - _curStripY;
            if (h > _stripRows) h = _stripRows;
            _consume(_curStripY, h, (const uint16_t *)_strip, _width, _user);
            _curStripY = -1;
        }
    }

    static int _drawCb(JPEGDRAW *p) {
        JpegStripDecoder *s = (JpegStripDecoder *)p->pUser;
        if (p->y != s->_curStripY) { // crossed into the next strip
            s->_flush();
            s->_curStripY = p->y;
        }
        int rows = p->iHeight;
        for (int r = 0; r < rows; r++) {
            uint8_t *dst = s->_strip +
                ((size_t)(p->y - s->_curStripY + r) * s->_width + p->x) * s->_bpp;
            const uint8_t *src = (const uint8_t *)p->pPixels +
                (size_t)r * p->iWidth * s->_bpp;
            memcpy(dst, src, (size_t)p->iWidthUsed * s->_bpp);
        }
        return 1; // continue decoding
    }

    // POSIX file callbacks (no stdio buffers, nothing extra on the heap)
    static void *_fileOpen(const char *name, int32_t *size) {
        int fd = ::open(name, O_RDONLY);
        if (fd < 0) return NULL;
        *size = (int32_t)lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        return (void *)(intptr_t)fd;
    }
    static void _fileClose(void *h) { ::close((int)(intptr_t)h); }
    static int32_t _fileRead(JPEGFILE *f, uint8_t *buf, int32_t len) {
        return (int32_t)::read((int)(intptr_t)f->fHandle, buf, len);
    }
    static int32_t _fileSeek(JPEGFILE *f, int32_t pos) {
        return (int32_t)lseek((int)(intptr_t)f->fHandle, pos, SEEK_SET);
    }
};

#endif // JPEG_STRIP_DECODE_H
