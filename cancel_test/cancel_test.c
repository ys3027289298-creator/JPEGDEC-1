//
// cancel_test.c - verifies cancelable JPEG decoding (feature lives in src/jpeg.inl)
//
// Scenarios:
//   S0 reference   - plain decode, captures the reference pixels
//   S1 no cancel   - cancelable decode configured, flag never set
//   S2 cancel      - decode canceled mid-way, progress reported
//   S3 resume      - next decode() call continues from the breakpoint
//   S4 discard     - canceled, state discarded, image re-decoded from scratch
//   S5 bad flag    - invalid cancel flag / invalid interval handling
//
// Build (from the repo root, no build scripts touched):
//   gcc -O2 -Wall -D__LINUX__ cancel_test/cancel_test.c -lm -o /tmp/cancel_test
//
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../src/JPEGDEC.h"
#include "../src/jpeg.inl"
#include "../test_images/tulips.h" // 640x480 color JPEG

static JPEGIMAGE jpg;
#define FB_W 640
#define FB_H 480
static uint16_t usFB[FB_W * FB_H];   // stitched output of the current scenario
static uint16_t usRef[FB_W * FB_H];  // reference pixels from an uninterrupted decode
static volatile int iCancelFlag = 0; // the "UI cancel button"
static int iCancelAtY = -1;          // when a drawn block reaches this y, set the flag
static int iDrawCalls = 0;
static int iMaxYDrawn = 0;

static long micros(void)
{
struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
    return 1000000L * res.tv_sec + res.tv_nsec / 1000;
} /* micros() */

static int Draw(JPEGDRAW *pDraw)
{
int r;
    for (r = 0; r < pDraw->iHeight; r++)
        memcpy(&usFB[(pDraw->y + r) * FB_W + pDraw->x],
               &pDraw->pPixels[r * pDraw->iWidth], pDraw->iWidthUsed * sizeof(uint16_t));
    iDrawCalls++;
    if (pDraw->y + pDraw->iHeight > iMaxYDrawn)
        iMaxYDrawn = pDraw->y + pDraw->iHeight;
    if (iCancelAtY >= 0 && pDraw->y >= iCancelAtY)
        iCancelFlag = 1; // simulate the UI asking to cancel
    return 1; // continue decoding
} /* Draw() */

static int Open(void)
{
    return JPEG_openRAM(&jpg, (uint8_t *)tulips, sizeof(tulips), Draw);
} /* Open() */

static void ResetFB(void)
{
    memset(usFB, 0xAB, sizeof(usFB)); // poison: any pixel never drawn is detectable
    iDrawCalls = 0;
    iMaxYDrawn = 0;
} /* ResetFB() */

static int CompareFull(const uint16_t *pRef)
{
    return memcmp(usFB, pRef, sizeof(usFB)) == 0;
} /* CompareFull() */

static int CompareRows(const uint16_t *pRef, int iRows)
{
    return memcmp(usFB, pRef, (size_t)iRows * FB_W * sizeof(uint16_t)) == 0;
} /* CompareRows() */

int main(void)
{
long t0, tRef = 0, tNoCancel = 0, tCancel = 0, tResume = 0, tDiscard = 0;
int rc, iErr, iProg, iProgCancel = 0, iCancelRows = 0, iConsistentPart = 0;
int iOK = 1;

    // ---------- S0: reference decode (feature not configured) ----------
    ResetFB();
    if (!Open()) { printf("S0: open failed\n"); return 1; }
    t0 = micros();
    rc = JPEG_decode(&jpg, 0, 0, 0);
    tRef = micros() - t0;
    if (rc != 1) { printf("S0: reference decode failed\n"); return 1; }
    memcpy(usRef, usFB, sizeof(usRef));
    printf("S0 reference: %dx%d, subsample=0x%02x, decode ok in %ld us (%d draw calls)\n",
           JPEG_getWidth(&jpg), JPEG_getHeight(&jpg), JPEG_getSubSample(&jpg), tRef, iDrawCalls);

    // ---------- S1: cancelable decode configured, never canceled ----------
    ResetFB();
    Open();
    JPEG_cancelableDecodeConfig(&jpg, &iCancelFlag, 8 /*check every 8 MCUs*/, 1 /*resumable*/);
    t0 = micros();
    rc = JPEG_decode(&jpg, 0, 0, 0);
    tNoCancel = micros() - t0;
    iProg = JPEG_decodeProgress(&jpg);
    {
        int iSame = CompareFull(usRef);
        printf("S1 no cancel: rc=%d canceled=%d progress=%d.%d%% pixels=%s time=%ld us\n",
               rc, JPEG_decodeWasCanceled(&jpg), iProg / 10, iProg % 10,
               iSame ? "IDENTICAL" : "DIFFER", tNoCancel);
        iOK &= (rc == 1) && !JPEG_decodeWasCanceled(&jpg) && (iProg == 1000) && iSame;
    }

    // ---------- S2: cancel mid-way (resumable) ----------
    ResetFB();
    Open();
    JPEG_cancelableDecodeConfig(&jpg, &iCancelFlag, 16 /*check every 16 MCUs*/, 1 /*resumable*/);
    iCancelFlag = 0;
    iCancelAtY = FB_H / 2; // "user quits" once the output passes the middle
    t0 = micros();
    rc = JPEG_decode(&jpg, 0, 0, 0);
    tCancel = micros() - t0;
    iCancelAtY = -1;
    iProgCancel = JPEG_decodeProgress(&jpg);
    iErr = JPEG_getLastError(&jpg);
    iCancelRows = iMaxYDrawn - 16; // the last 16-row MCU band may be only partially drawn
    iConsistentPart = CompareRows(usRef, iCancelRows);
    printf("S2 cancel: rc=%d canceled=%d lastError=%d progress=%d.%d%% drawn-rows=%d partial-pixels=%s time=%ld us\n",
           rc, JPEG_decodeWasCanceled(&jpg), iErr, iProgCancel / 10, iProgCancel % 10,
           iCancelRows, iConsistentPart ? "IDENTICAL" : "DIFFER", tCancel);
    iOK &= (rc == 0) && (JPEG_decodeWasCanceled(&jpg) == 1) && (iErr == JPEG_SUCCESS)
           && (iProgCancel > 0) && (iProgCancel < 1000) && iConsistentPart;

    // ---------- S3: continue the canceled decode from the breakpoint ----------
    iCancelFlag = 0;
    t0 = micros();
    rc = JPEG_decode(&jpg, 0, 0, 0); // same open session: resumes at the checkpoint
    tResume = micros() - t0;
    iProg = JPEG_decodeProgress(&jpg);
    {
        int iSame = CompareFull(usRef); // S2's blocks + S3's blocks must equal the reference
        printf("S3 resume: rc=%d canceled=%d progress=%d.%d%% stitched-pixels=%s resume-time=%ld us (cancel+resume=%ld us vs one-shot=%ld us)\n",
               rc, JPEG_decodeWasCanceled(&jpg), iProg / 10, iProg % 10,
               iSame ? "IDENTICAL" : "DIFFER", tResume, tCancel + tResume, tNoCancel);
        iOK &= (rc == 1) && (iProg == 1000) && iSame;
    }

    // ---------- S4: cancel, then discard the state and start over ----------
    ResetFB();
    Open();
    JPEG_cancelableDecodeConfig(&jpg, &iCancelFlag, 16, 0 /*not resumable*/);
    iCancelFlag = 0;
    iCancelAtY = FB_H / 2;
    rc = JPEG_decode(&jpg, 0, 0, 0);
    iCancelAtY = -1;
    iProg = JPEG_decodeProgress(&jpg);
    printf("S4 discard: cancel pass rc=%d canceled=%d progress=%d.%d%%; ",
           rc, JPEG_decodeWasCanceled(&jpg), iProg / 10, iProg % 10);
    iOK &= (rc == 0) && (JPEG_decodeWasCanceled(&jpg) == 1);
    JPEG_cancelableDecodeReset(&jpg); // user discards the partial decode
    JPEG_cancelableDecodeConfig(&jpg, &iCancelFlag, 16, 0); // re-arm for a fresh decode
    iCancelFlag = 0;
    ResetFB();
    t0 = micros();
    rc = JPEG_decode(&jpg, 0, 0, 0); // no checkpoint kept: decodes the whole image again
    tDiscard = micros() - t0;
    iProg = JPEG_decodeProgress(&jpg);
    {
        int iSame = CompareFull(usRef);
        printf("fresh decode rc=%d progress=%d.%d%% pixels=%s time=%ld us\n",
               rc, iProg / 10, iProg % 10, iSame ? "IDENTICAL" : "DIFFER", tDiscard);
        iOK &= (rc == 1) && (iProg == 1000) && iSame;
    }

    // ---------- S5: invalid cancel flag / invalid interval ----------
    ResetFB();
    Open();
    rc = JPEG_cancelableDecodeConfig(&jpg, NULL, 16, 1); // NULL flag pointer
    iErr = JPEG_getLastError(&jpg);
    printf("S5 invalid flag: config rc=%d (expect 0) lastError=%d (expect %d=JPEG_INVALID_PARAMETER); ",
           rc, iErr, JPEG_INVALID_PARAMETER);
    iOK &= (rc == 0) && (iErr == JPEG_INVALID_PARAMETER);
    {
        int r2 = JPEG_decode(&jpg, 0, 0, 0); // bad config -> feature stays off -> normal decode
        int iSame = CompareFull(usRef);
        printf("decode rc=%d pixels=%s\n", r2, iSame ? "IDENTICAL" : "DIFFER");
        iOK &= (r2 == 1) && iSame;
    }
    Open();
    rc = JPEG_cancelableDecodeConfig(&jpg, &iCancelFlag, 0 /*invalid interval*/, 1);
    iCancelFlag = 0;
    iCancelAtY = FB_H / 2;
    ResetFB();
    {
        int r2 = JPEG_decode(&jpg, 0, 0, 0); // interval clamped to 1: cancel still works
        iCancelAtY = -1;
        iProg = JPEG_decodeProgress(&jpg);
        printf("S5 invalid interval: config rc=%d (clamped to 1) decode rc=%d canceled=%d progress=%d.%d%%\n",
               rc, r2, JPEG_decodeWasCanceled(&jpg), iProg / 10, iProg % 10);
        iOK &= (rc == 1) && (r2 == 0) && (JPEG_decodeWasCanceled(&jpg) == 1)
               && (iProg > 0) && (iProg < 1000);
    }

    printf("\nSummary: reference=%ld us | no-cancel=%ld us | cancel-pass=%ld us | resume-pass=%ld us | cancel+resume=%ld us | discard+full=%ld us\n",
           tRef, tNoCancel, tCancel, tResume, tCancel + tResume, tDiscard);
    printf("%s\n", iOK ? "ALL SCENARIOS PASSED" : "FAILURES DETECTED");
    return iOK ? 0 : 1;
} /* main() */
