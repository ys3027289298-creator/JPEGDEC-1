//
// JPEG Decoder
//
// written by Larry Bank
// bitbank@pobox.com
// Arduino port started 8/2/2020
// Original JPEG code written 26+ years ago :)
// The goal of this code is to decode baseline JPEG images
// using no more than 18K of RAM (if sent directly to an LCD display)
//
// Copyright 2020 BitBank Software, Inc. All Rights Reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//===========================================================================
//
#include "JPEGDEC.h"

// forward references
JPEG_STATIC int JPEGInit(JPEGIMAGE *pJPEG);
JPEG_STATIC int JPEGParseInfo(JPEGIMAGE *pPage, int bExtractThumb);
JPEG_STATIC void JPEGGetMoreData(JPEGIMAGE *pPage);
JPEG_STATIC int DecodeJPEG(JPEGIMAGE *pImage);
JPEG_STATIC void JPEG_setFramebuffer(JPEGIMAGE *pPage, void *pFramebuffer);
void JPEG_setCropArea(JPEGIMAGE *pJPEG, int x, int y, int w, int h);
void JPEG_getCropArea(JPEGIMAGE *pJPEG, int *x, int *y, int *w, int *h);
void JPEG_setFramebuffer(JPEGIMAGE *pJPEG, void *pFramebuffer);

// Include the C code which does the actual work
#include "jpeg.inl"

void JPEGDEC::setFramebuffer(void *pFramebuffer)
{
    JPEG_setFramebuffer(&_jpeg, pFramebuffer);
} /* setFramebuffer() */

int JPEGDEC::getPixelType()
{
    return (int)_jpeg.ucPixelType;
}
void JPEGDEC::setPixelType(int iType)
{
    if (iType >= 0 && iType < INVALID_PIXEL_TYPE)
        _jpeg.ucPixelType = (uint8_t)iType;
    else
        _jpeg.iError = JPEG_INVALID_PARAMETER;
} /* setPixelType() */

void JPEGDEC::setMaxOutputSize(int iMaxMCUs)
{
    if (iMaxMCUs < 1)
        iMaxMCUs = 1; // don't allow invalid value
    _jpeg.iMaxMCUs = iMaxMCUs;
} /* setMaxOutputSize() */
//
// Memory initialization
//
int JPEGDEC::openRAM(uint8_t *pData, int iDataSize, JPEG_DRAW_CALLBACK *pfnDraw)
{
    memset(&_jpeg, 0, sizeof(JPEGIMAGE));
    _jpeg.ucMemType = JPEG_MEM_RAM;
    _jpeg.pfnRead = readRAM;
    _jpeg.pfnSeek = seekMem;
    _jpeg.pfnDraw = pfnDraw;
    _jpeg.pfnOpen = NULL;
    _jpeg.pfnClose = NULL;
    _jpeg.JPEGFile.iSize = iDataSize;
    _jpeg.JPEGFile.pData = pData;
    _jpeg.iMaxMCUs = 1000; // set to an unnaturally high value to start
    return JPEGInit(&_jpeg);
} /* openRAM() */

int JPEGDEC::openFLASH(const uint8_t *pData, int iDataSize, JPEG_DRAW_CALLBACK *pfnDraw)
{
    memset(&_jpeg, 0, sizeof(JPEGIMAGE));
    _jpeg.ucMemType = JPEG_MEM_FLASH;
    _jpeg.pfnRead = readFLASH;
    _jpeg.pfnSeek = seekMem;
    _jpeg.pfnDraw = pfnDraw;
    _jpeg.pfnOpen = NULL;
    _jpeg.pfnClose = NULL;
    _jpeg.JPEGFile.iSize = iDataSize;
    _jpeg.JPEGFile.pData = (uint8_t *)pData;
    _jpeg.iMaxMCUs = 1000; // set to an unnaturally high value to start
    return JPEGInit(&_jpeg);
} /* openFLASH() */

int JPEGDEC::getOrientation()
{
    return (int)_jpeg.ucOrientation;
} /* getOrientation() */

int JPEGDEC::getLastError()
{
    return _jpeg.iError;
} /* getLastError() */

int JPEGDEC::getWidth()
{
    return _jpeg.iWidth;
} /* getWidth() */

int JPEGDEC::getHeight()
{
    return _jpeg.iHeight;
} /* getHeight() */

int JPEGDEC::hasThumb()
{
    return (int)_jpeg.ucHasThumb;
} /* hasThumb() */

int JPEGDEC::getThumbWidth()
{
    return _jpeg.iThumbWidth;
} /* getThumbWidth() */

int JPEGDEC::getThumbHeight()
{
    return _jpeg.iThumbHeight;
} /* getThumbHeight() */

int JPEGDEC::getBpp()
{
    return (int)_jpeg.ucBpp;
} /* getBpp() */

int JPEGDEC::getSubSample()
{
    return (int)_jpeg.ucSubSample;
} /* getSubSample() */

void JPEGDEC::setCropArea(int x, int y, int w, int h)
{
    JPEG_setCropArea(&_jpeg, x, y, w, h);
} /* setCropArea() */

void JPEGDEC::getCropArea(int *x, int *y, int *w, int *h)
{
    JPEG_getCropArea(&_jpeg, x, y, w, h);
} /* getCropArea() */

int JPEGDEC::getJPEGType()
{
    return (_jpeg.ucMode == 0xc2) ? JPEG_MODE_PROGRESSIVE : JPEG_MODE_BASELINE;
} /* getJPEGType() */

#if defined( __LINUX__ ) || defined( __MACH__ )
int JPEGDEC::open(const char *szFilename, JPEG_DRAW_CALLBACK *pfnDraw)
{
    memset(&_jpeg, 0, sizeof(JPEGIMAGE));
    _jpeg.pfnRead = readFile;
    _jpeg.pfnClose = closeFile;
    _jpeg.pfnSeek = seekFile;
    _jpeg.pfnDraw = pfnDraw;
    _jpeg.iMaxMCUs = 1000;
    _jpeg.JPEGFile.fHandle = openFile(szFilename, &_jpeg.JPEGFile.iSize);
    if (_jpeg.JPEGFile.fHandle == NULL) return 0;
    return JPEGInit(&_jpeg);
} /* open() */
#endif // __LINUX__
//
// File (SD/MMC) based initialization
//
int JPEGDEC::open(const char *szFilename, JPEG_OPEN_CALLBACK *pfnOpen, JPEG_CLOSE_CALLBACK *pfnClose, JPEG_READ_CALLBACK *pfnRead, JPEG_SEEK_CALLBACK *pfnSeek, JPEG_DRAW_CALLBACK *pfnDraw)
{
    memset(&_jpeg, 0, sizeof(JPEGIMAGE));
    _jpeg.pfnRead = pfnRead;
    _jpeg.pfnSeek = pfnSeek;
    _jpeg.pfnDraw = pfnDraw;
    _jpeg.pfnOpen = pfnOpen;
    _jpeg.pfnClose = pfnClose;
    _jpeg.iMaxMCUs = 1000; // set to an unnaturally high value to start
    _jpeg.JPEGFile.fHandle = (*pfnOpen)(szFilename, &_jpeg.JPEGFile.iSize);
    if (_jpeg.JPEGFile.fHandle == NULL)
       return 0;
    return JPEGInit(&_jpeg);

} /* open() */

//
// data stream initialization
//
int JPEGDEC::open(void *fHandle, int iDataSize, JPEG_CLOSE_CALLBACK *pfnClose, JPEG_READ_CALLBACK *pfnRead, JPEG_SEEK_CALLBACK *pfnSeek, JPEG_DRAW_CALLBACK *pfnDraw)
{
    memset(&_jpeg, 0, sizeof(JPEGIMAGE));
    _jpeg.pfnRead = pfnRead;
    _jpeg.pfnSeek = pfnSeek;
    _jpeg.pfnDraw = pfnDraw;
    _jpeg.pfnClose = pfnClose;
    _jpeg.iMaxMCUs = 1000; // set to an unnaturally high value to start
    _jpeg.JPEGFile.iSize = iDataSize;
    _jpeg.JPEGFile.fHandle = fHandle;
    return JPEGInit(&_jpeg);
} /* open() */

#ifdef FS_H
static int32_t FileRead(JPEGFILE *handle, uint8_t *buffer, int32_t length)
{
    return ((File *)(handle->fHandle))->read(buffer, length);
}
static int32_t FileSeek(JPEGFILE *handle, int32_t position)
{
    return ((File *)(handle->fHandle))->seek(position);
}
static void FileClose(void *handle)
{
    ((File *)handle)->close();
}

int JPEGDEC::open(File &file, JPEG_DRAW_CALLBACK *pfnDraw)
{
    if (!file) return 0;
    memset(&_jpeg, 0, sizeof(JPEGIMAGE));
    _jpeg.pfnRead = FileRead;
    _jpeg.pfnSeek = FileSeek;
    _jpeg.pfnClose = FileClose;
    _jpeg.pfnDraw = pfnDraw;
    _jpeg.iMaxMCUs = 1000;
    _jpeg.JPEGFile.fHandle = &file;
    _jpeg.JPEGFile.iSize = file.size();
    return JPEGInit(&_jpeg);
}
#endif // FS_H

void JPEGDEC::close()
{
    if (_jpeg.pfnClose)
        (*_jpeg.pfnClose)(_jpeg.JPEGFile.fHandle);
} /* close() */

//
// Decode the image
// returns:
// 1 = good result
// 0 = error
//
int JPEGDEC::decode(int x, int y, int iOptions)
{
    _jpeg.iXOffset = x;
    _jpeg.iYOffset = y;
    _jpeg.iOptions = iOptions;
    return DecodeJPEG(&_jpeg);
} /* decode() */
//
// set draw callback user pointer variable
//
void JPEGDEC::setUserPointer(void *p)
{
    _jpeg.pUser = p;
}

int JPEGDEC::decodeDither(int x, int y, uint8_t *pDither, int iOptions)
{
    _jpeg.iXOffset = x;
    _jpeg.iYOffset = y;
    _jpeg.iOptions = iOptions;
    _jpeg.pDitherBuffer = pDither;
    return DecodeJPEG(&_jpeg);
}

int JPEGDEC::decodeDither(uint8_t *pDither, int iOptions)
{
    _jpeg.iOptions = iOptions;
    _jpeg.pDitherBuffer = pDither;
    return DecodeJPEG(&_jpeg);
}

//===========================================================================
// Adaptive 8x8 block boundary deblocking filter (optional post-process)
//
// Highly compressed JPEG images show visible 8x8 block boundaries when
// displayed on large screens. This filter smooths ONLY the pixels straddling
// 8x8 block boundaries (it is not a spatial blur). The filter strength is
// chosen per 8x8 block pair: blocks whose interior is flat (mean absolute
// gradient <= iThreshold) are filtered at full strength; textured blocks are
// filtered proportionally weaker so that real detail is preserved.
//
// pPixels        = pointer to the decoded image (modified in place)
// iWidth/iHeight = image size in pixels
// iPitch         = bytes per row of the image buffer
// iBytesPerPixel = bytes per pixel (1, 3 or 4)
// iStrength      = filter strength 0 (off) to 100 (maximum)
// iThreshold     = flat/texture threshold, mean abs gradient 0..255
//
// returns JPEG_SUCCESS (0) or JPEG_INVALID_PARAMETER (1) if a parameter
// (including an out-of-range strength) is invalid
//
#define JPEG_DEBLOCK_MAX_STRENGTH 100
#define JPEG_DEBLOCK_MAX_THRESHOLD 255
#define JPEG_DEBLOCK_MAX_BLKS 128 // block-row cache covers widths up to 1024

// Mean absolute gradient (all channels) inside one 8x8 block
static int JPEGBlockGradient(uint8_t *pPixels, int iPitch, int iBpp, int x, int y)
{
int i, j, c, iSum = 0;
uint8_t *p0, *p1;

    for (j = 0; j < 8; j++) { // horizontal gradients
        p0 = &pPixels[(y + j) * iPitch + x * iBpp];
        for (i = 0; i < 7; i++) {
            for (c = 0; c < iBpp; c++)
                iSum += abs(p0[c + iBpp] - p0[c]);
            p0 += iBpp;
        }
    }
    for (j = 0; j < 7; j++) { // vertical gradients
        p0 = &pPixels[(y + j) * iPitch + x * iBpp];
        p1 = p0 + iPitch;
        for (i = 0; i < 8 * iBpp; i++)
            iSum += abs(p1[i] - p0[i]);
    }
    return iSum / (7 * 8 * 2 * iBpp);
} /* JPEGBlockGradient() */

// Per-block filter weight (0..100): full strength on flat blocks,
// decaying strength as the interior gradient exceeds the threshold
static int JPEGBlockWeight(int iGradient, int iStrength, int iThreshold)
{
    if (iGradient <= iThreshold)
        return iStrength;
    return (iStrength * iThreshold) / (iGradient + 1);
} /* JPEGBlockWeight() */

int JPEGDeblock(uint8_t *pPixels, int iWidth, int iHeight, int iPitch, int iBytesPerPixel, int iStrength, int iThreshold)
{
int bx, by, i, j, c, x, y, w, iStep, iDelta;
int iNBX, iNBY, iGate;
uint8_t *p;
uint8_t ucFlat[JPEG_DEBLOCK_MAX_BLKS];  // current block row
uint8_t ucFlat2[JPEG_DEBLOCK_MAX_BLKS]; // next block row (horizontal pass)

    if (pPixels == NULL || iWidth < 16 || iHeight < 16 ||
        iPitch < iWidth * iBytesPerPixel ||
        (iBytesPerPixel != 1 && iBytesPerPixel != 3 && iBytesPerPixel != 4))
        return JPEG_INVALID_PARAMETER;
    if (iStrength < 0 || iStrength > JPEG_DEBLOCK_MAX_STRENGTH ||
        iThreshold < 0 || iThreshold > JPEG_DEBLOCK_MAX_THRESHOLD)
        return JPEG_INVALID_PARAMETER; // strength/threshold out of range
    if (iStrength == 0 || iThreshold == 0)
        return JPEG_SUCCESS; // nothing to do

    iNBX = iWidth / 8;  // work on full 8x8 blocks only
    iNBY = iHeight / 8;
    iGate = 4 * iThreshold; // larger steps are real edges: leave them alone

    // Pass 1: vertical block boundaries
    for (by = 0; by < iNBY; by++) {
        y = by * 8;
        if (iNBX <= JPEG_DEBLOCK_MAX_BLKS) { // cache this row of blocks
            for (bx = 0; bx < iNBX; bx++)
                ucFlat[bx] = (uint8_t)JPEGBlockGradient(pPixels, iPitch, iBytesPerPixel, bx * 8, y);
        }
        for (bx = 1; bx < iNBX; bx++) {
            if (iNBX <= JPEG_DEBLOCK_MAX_BLKS)
                w = (JPEGBlockWeight(ucFlat[bx - 1], iStrength, iThreshold) +
                     JPEGBlockWeight(ucFlat[bx], iStrength, iThreshold)) / 2;
            else // too wide for the cache; measure on the fly
                w = (JPEGBlockWeight(JPEGBlockGradient(pPixels, iPitch, iBytesPerPixel, (bx - 1) * 8, y), iStrength, iThreshold) +
                     JPEGBlockWeight(JPEGBlockGradient(pPixels, iPitch, iBytesPerPixel, bx * 8, y), iStrength, iThreshold)) / 2;
            if (w == 0)
                continue;
            x = bx * 8;
            for (j = 0; j < 8; j++) {
                p = &pPixels[(y + j) * iPitch + x * iBytesPerPixel];
                for (c = 0; c < iBytesPerPixel; c++) {
                    iStep = (int)p[c] - (int)p[c - iBytesPerPixel];
                    if (iStep > -iGate && iStep < iGate) {
                        // split the seam step across the 2 pixels on each side
                        iDelta = (iStep * w) / 200;
                        p[c - 2 * iBytesPerPixel] += (uint8_t)(iDelta / 2);
                        p[c - iBytesPerPixel] += (uint8_t)iDelta;
                        p[c] -= (uint8_t)iDelta;
                        p[c + iBytesPerPixel] -= (uint8_t)(iDelta / 2);
                    }
                }
            }
        }
    }
    // Pass 2: horizontal block boundaries
    if (iNBX <= JPEG_DEBLOCK_MAX_BLKS) { // prime the row-above cache
        for (bx = 0; bx < iNBX; bx++)
            ucFlat2[bx] = (uint8_t)JPEGBlockGradient(pPixels, iPitch, iBytesPerPixel, bx * 8, 0);
    }
    for (by = 1; by < iNBY; by++) {
        y = by * 8;
        if (iNBX <= JPEG_DEBLOCK_MAX_BLKS) {
            memcpy(ucFlat, ucFlat2, iNBX);
            for (bx = 0; bx < iNBX; bx++)
                ucFlat2[bx] = (uint8_t)JPEGBlockGradient(pPixels, iPitch, iBytesPerPixel, bx * 8, y);
        }
        for (bx = 0; bx < iNBX; bx++) {
            if (iNBX <= JPEG_DEBLOCK_MAX_BLKS)
                w = (JPEGBlockWeight(ucFlat[bx], iStrength, iThreshold) +
                     JPEGBlockWeight(ucFlat2[bx], iStrength, iThreshold)) / 2;
            else
                w = (JPEGBlockWeight(JPEGBlockGradient(pPixels, iPitch, iBytesPerPixel, bx * 8, y - 8), iStrength, iThreshold) +
                     JPEGBlockWeight(JPEGBlockGradient(pPixels, iPitch, iBytesPerPixel, bx * 8, y), iStrength, iThreshold)) / 2;
            if (w == 0)
                continue;
            x = bx * 8;
            p = &pPixels[y * iPitch + x * iBytesPerPixel];
            for (i = 0; i < 8; i++) {
                for (c = 0; c < iBytesPerPixel; c++) {
                    iStep = (int)p[c] - (int)p[c - iPitch];
                    if (iStep > -iGate && iStep < iGate) {
                        // split the seam step across the 2 pixels on each side
                        iDelta = (iStep * w) / 200;
                        p[c - 2 * iPitch] += (uint8_t)(iDelta / 2);
                        p[c - iPitch] += (uint8_t)iDelta;
                        p[c] -= (uint8_t)iDelta;
                        p[c + iPitch] -= (uint8_t)(iDelta / 2);
                    }
                }
                p += iBytesPerPixel;
            }
        }
    }
    return JPEG_SUCCESS;
} /* JPEGDeblock() */
