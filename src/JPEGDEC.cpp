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
// Optional 3-stage post-processing pipeline for low-bitrate images
// ----------------------------------------------------------------
// This code is completely separate from the decode path: it operates on a
// finished RGB8888 framebuffer (e.g. one filled by decode() together with
// setFramebuffer()) and never runs unless explicitly called. Each of the
// three stages can be toggled independently through the iFlags bit mask:
//   JPEG_PP_DEBLOCK - stage 1: block-boundary deblocking filter
//   JPEG_PP_CHROMA  - stage 2: chroma bilinear up-sampling (smooths the
//                     blocky nearest-neighbor 4:2:0 chroma)
//   JPEG_PP_SHARPEN - stage 3: mild luma sharpening (unsharp mask)
// The stages run in the order listed above when several are enabled.
// Returns JPEG_SUCCESS on success, JPEG_INVALID_PARAMETER for bad
// arguments, JPEG_UNSUPPORTED_FEATURE for an unsupported pixel type and
// JPEG_ERROR_MEMORY if a work buffer cannot be allocated.
//
#define JPEG_PP_DEBLOCK  0x1
#define JPEG_PP_CHROMA   0x2
#define JPEG_PP_SHARPEN  0x4
#define JPEG_PP_ALL      (JPEG_PP_DEBLOCK | JPEG_PP_CHROMA | JPEG_PP_SHARPEN)

// clamp an int to the 0..255 sample range
static uint8_t PP_Clamp8(int i)
{
    if (i < 0) return 0;
    if (i > 255) return 255;
    return (uint8_t)i;
} /* PP_Clamp8() */

//
// Stage 1: deblocking filter on the 8x8 DCT block grid.
// For each pair of pixels straddling a block boundary, if the step across
// the boundary is moderate and dominates the gradient on both sides (a
// blocking artifact, not a true edge), soften it with a 4-tap filter.
//
static void PP_Deblock(uint8_t *pFrame, int iWidth, int iHeight)
{
int x, y, c, iPitch = iWidth * 4;
uint8_t *p1, *p0, *q0, *q1;
int d0, d1, d2;

    for (x = 8; x + 1 < iWidth; x += 8) // vertical block boundaries
    {
        for (y = 0; y < iHeight; y++)
        {
            p1 = &pFrame[y * iPitch + (x-2)*4];
            p0 = p1 + 4; q0 = p0 + 4; q1 = q0 + 4;
            for (c = 0; c < 3; c++) // R,G,B (alpha is left alone)
            {
                d0 = abs(p0[c] - q0[c]); // step across the boundary
                d1 = abs(p1[c] - p0[c]); // gradient on the near side
                d2 = abs(q1[c] - q0[c]); // gradient on the far side
                if (d0 >= 4 && d0 < 40 && d0 > d1 + d2) // blocking artifact
                {
                    int n0 = (p1[c] + 2*p0[c] + q0[c] + 2) >> 2;
                    int n1 = (p0[c] + 2*q0[c] + q1[c] + 2) >> 2;
                    p0[c] = (uint8_t)n0; q0[c] = (uint8_t)n1;
                }
            }
        }
    }
    for (y = 8; y + 1 < iHeight; y += 8) // horizontal block boundaries
    {
        for (x = 0; x < iWidth; x++)
        {
            p1 = &pFrame[(y-2) * iPitch + x*4];
            p0 = p1 + iPitch; q0 = p0 + iPitch; q1 = q0 + iPitch;
            for (c = 0; c < 3; c++)
            {
                d0 = abs(p0[c] - q0[c]);
                d1 = abs(p1[c] - p0[c]);
                d2 = abs(q1[c] - q0[c]);
                if (d0 >= 4 && d0 < 40 && d0 > d1 + d2)
                {
                    int n0 = (p1[c] + 2*p0[c] + q0[c] + 2) >> 2;
                    int n1 = (p0[c] + 2*q0[c] + q1[c] + 2) >> 2;
                    p0[c] = (uint8_t)n0; q0[c] = (uint8_t)n1;
                }
            }
        }
    }
} /* PP_Deblock() */

//
// Stage 2: chroma bilinear up-sampling.
// Recovers the half-resolution (4:2:0) Cb/Cr planes by averaging each 2x2
// block, up-samples them back to full resolution with bilinear
// interpolation and re-applies only the chroma difference to the pixels,
// so the luma channel passes through untouched.
//
static int PP_ChromaBilinear(uint8_t *pFrame, int iWidth, int iHeight)
{
int i, j, x, y, dx, dy;
int iHW = iWidth >> 1, iHH = iHeight >> 1;
uint8_t *pCb, *pCr, *p;
int cb, cr, R, G, B;

    pCb = (uint8_t *)malloc(iHW * iHH * 2); // both half-res chroma planes
    if (pCb == NULL)
        return JPEG_ERROR_MEMORY;
    pCr = pCb + iHW * iHH;
    // recover the half-res chroma; coefficients are 512x the JPEG RGB->YCbCr
    // values, so the sums below are 2048x the average (Cb-128)/(Cr-128)
    for (j = 0; j < iHH; j++)
    {
        for (i = 0; i < iHW; i++)
        {
            cb = cr = 0;
            for (dy = 0; dy < 2; dy++)
            {
                for (dx = 0; dx < 2; dx++)
                {
                    p = &pFrame[((j*2+dy) * iWidth + i*2+dx) * 4];
                    R = p[0]; G = p[1]; B = p[2];
                    cb += -87*R - 169*G + 256*B;
                    cr +=  256*R - 214*G - 42*B;
                }
            }
            pCb[j*iHW+i] = PP_Clamp8(128 + ((cb + 1024) >> 11));
            pCr[j*iHW+i] = PP_Clamp8(128 + ((cr + 1024) >> 11));
        }
    }
    // Half-res sample i is centered at full-res position 2i+0.5; interpolate
    // bilinearly between the 4 nearest half-res samples (weights sum to 16).
    for (y = 0; y < iHeight; y++)
    {
        int y0 = (2*y - 1) >> 2, fy = (2*y - 1) & 3, y1;
        if (y0 < 0) { y0 = 0; fy = 0; }
        y1 = (y0 + 1 < iHH) ? y0 + 1 : iHH - 1;
        for (x = 0; x < iWidth; x++)
        {
            int x0 = (2*x - 1) >> 2, fx = (2*x - 1) & 3, x1;
            int cbNew, crNew, cbOld, crOld, dCb, dCr;
            if (x0 < 0) { x0 = 0; fx = 0; }
            x1 = (x0 + 1 < iHW) ? x0 + 1 : iHW - 1;
            cbNew = ((4-fy)*((4-fx)*pCb[y0*iHW+x0] + fx*pCb[y0*iHW+x1])
                        + fy*((4-fx)*pCb[y1*iHW+x0] + fx*pCb[y1*iHW+x1]) + 8) >> 4;
            crNew = ((4-fy)*((4-fx)*pCr[y0*iHW+x0] + fx*pCr[y0*iHW+x1])
                        + fy*((4-fx)*pCr[y1*iHW+x0] + fx*pCr[y1*iHW+x1]) + 8) >> 4;
            p = &pFrame[(y * iWidth + x) * 4];
            R = p[0]; G = p[1]; B = p[2];
            cbOld = 128 + ((-87*R - 169*G + 256*B + 256) >> 9);
            crOld = 128 + ((256*R - 214*G - 42*B + 256) >> 9);
            dCb = cbNew - cbOld; dCr = crNew - crOld;
            // apply only the chroma delta (256x the YCbCr->RGB coefficients)
            p[0] = PP_Clamp8(R + ((359*dCr + 128) >> 8));
            p[1] = PP_Clamp8(G + ((-88*dCb - 183*dCr + 128) >> 8));
            p[2] = PP_Clamp8(B + ((454*dCb + 128) >> 8));
        }
    }
    free(pCb);
    return JPEG_SUCCESS;
} /* PP_ChromaBilinear() */

//
// Stage 3: mild luma sharpening (unsharp mask).
// Adds a fraction of (luma - 3x3 binomial blur of luma) back to each pixel;
// the same delta is added to R, G and B so the chroma is not affected.
//
static int PP_SharpenLuma(uint8_t *pFrame, int iWidth, int iHeight)
{
int x, y, i, iBlur, iDelta;
uint8_t *pY, *p;

    pY = (uint8_t *)malloc(iWidth * iHeight);
    if (pY == NULL)
        return JPEG_ERROR_MEMORY;
    for (i = 0; i < iWidth * iHeight; i++) // 256x the JPEG luma coefficients
    {
        p = &pFrame[i*4];
        pY[i] = (uint8_t)((77*p[0] + 150*p[1] + 29*p[2] + 128) >> 8);
    }
    for (y = 1; y < iHeight-1; y++) // border pixels are left unchanged
    {
        for (x = 1; x < iWidth-1; x++)
        {
            i = y * iWidth + x;
            iBlur = (pY[i-iWidth-1] + 2*pY[i-iWidth] + pY[i-iWidth+1]
                   + 2*pY[i-1]        + 4*pY[i]        + 2*pY[i+1]
                   + pY[i+iWidth-1] + 2*pY[i+iWidth] + pY[i+iWidth+1] + 8) >> 4;
            iDelta = (pY[i] - iBlur) / 4; // mild: 25% unsharp mask
            if (iDelta > 24) iDelta = 24; // limit overshoot on strong edges
            else if (iDelta < -24) iDelta = -24;
            if (iDelta)
            {
                p = &pFrame[i*4];
                p[0] = PP_Clamp8(p[0] + iDelta);
                p[1] = PP_Clamp8(p[1] + iDelta);
                p[2] = PP_Clamp8(p[2] + iDelta);
            }
        }
    }
    free(pY);
    return JPEG_SUCCESS;
} /* PP_SharpenLuma() */

//
// Public entry point: run the enabled post-processing stages in place on an
// RGB8888 framebuffer. All parameters are validated before the image is
// touched; an invalid parameter leaves the buffer unchanged.
//
int JPEG_PostProcess(uint8_t *pFrame, int iWidth, int iHeight, int iPixelType, int iFlags)
{
int iErr = JPEG_SUCCESS;

    if (pFrame == NULL || iWidth <= 0 || iHeight <= 0 || (iFlags & ~JPEG_PP_ALL) != 0)
        return JPEG_INVALID_PARAMETER;
    if (iPixelType != RGB8888)
        return JPEG_UNSUPPORTED_FEATURE;
    if ((iFlags & JPEG_PP_CHROMA) && ((iWidth & 1) || (iHeight & 1)))
        return JPEG_INVALID_PARAMETER; // the 4:2:0 chroma stage needs even dimensions
    if (iFlags & JPEG_PP_DEBLOCK)
        PP_Deblock(pFrame, iWidth, iHeight);
    if ((iFlags & JPEG_PP_CHROMA) && iErr == JPEG_SUCCESS)
        iErr = PP_ChromaBilinear(pFrame, iWidth, iHeight);
    if ((iFlags & JPEG_PP_SHARPEN) && iErr == JPEG_SUCCESS)
        iErr = PP_SharpenLuma(pFrame, iWidth, iHeight);
    return iErr;
} /* JPEG_PostProcess() */
