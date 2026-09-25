#ifndef SWAPP_GDIPLUS_MIN_H
#define SWAPP_GDIPLUS_MIN_H

/* The GDI+ headers shipped with the Windows SDK are C++ only, and this
 * project is C. Only four calls are needed to draw a PNG -- load, wrap a
 * DC, rotate, blit -- so they are declared here against gdiplus.dll's flat
 * C ABI and resolved with GetProcAddress at startup. That also means a
 * machine without GDI+ degrades to text instead of failing to start. */

#include <windows.h>

typedef void GpImage;
typedef void GpBitmap;
typedef void GpGraphics;
typedef int GpStatus;

typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

typedef GpStatus (WINAPI *GdiplusStartup_t)(ULONG_PTR *, const GdiplusStartupInput *, void *);
typedef GpStatus (WINAPI *GdiplusShutdown_t)(ULONG_PTR);
typedef GpStatus (WINAPI *GdipCreateBitmapFromFile_t)(const WCHAR *, GpBitmap **);
typedef GpStatus (WINAPI *GdipDisposeImage_t)(GpImage *);
typedef GpStatus (WINAPI *GdipCreateFromHDC_t)(HDC, GpGraphics **);
typedef GpStatus (WINAPI *GdipDeleteGraphics_t)(GpGraphics *);
typedef GpStatus (WINAPI *GdipTranslateWorldTransform_t)(GpGraphics *, float, float, int);
typedef GpStatus (WINAPI *GdipRotateWorldTransform_t)(GpGraphics *, float, int);
typedef GpStatus (WINAPI *GdipResetWorldTransform_t)(GpGraphics *);
typedef GpStatus (WINAPI *GdipDrawImageRectI_t)(GpGraphics *, GpImage *, int, int, int, int);
typedef GpStatus (WINAPI *GdipSetInterpolationMode_t)(GpGraphics *, int);

/* Tinting: the icons are single-colour glyphs, so a colour matrix that
 * replaces RGB and keeps alpha recolours them without separate art. */
typedef void GpImageAttributes;
typedef struct {
    float m[5][5];
} GpColorMatrix;
typedef GpStatus (WINAPI *GdipCreateImageAttributes_t)(GpImageAttributes **);
typedef GpStatus (WINAPI *GdipSetImageAttributesColorMatrix_t)(GpImageAttributes *, int, BOOL,
                                                                const GpColorMatrix *,
                                                                const GpColorMatrix *, int);
typedef GpStatus (WINAPI *GdipGetImageDimension_t)(GpImage *, UINT *);
typedef GpStatus (WINAPI *GdipDrawImageRectRectI_t)(GpGraphics *, GpImage *, int, int, int, int,
                                                     int, int, int, int, int,
                                                     GpImageAttributes *, void *, void *);

#define SWAPP_GDIP_MATRIX_ORDER_PREPEND 0
#define SWAPP_GDIP_INTERPOLATION_HIGH_QUALITY 2
#define SWAPP_GDIP_UNIT_PIXEL 2

#endif
