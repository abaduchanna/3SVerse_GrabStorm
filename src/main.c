/* 3SVerse GrabStorm v1.1.0 — multi-segment download accelerator for Windows
 * MIT License — Copyright (c) 2026 Abad Umair Channa / 3S Verse
 *
 * Clean-room statement: GrabStorm is an independent, original implementation
 * written from scratch. No code, assets or resources from any other product.
 *
 * Brand system = the 3SVerse xfer design tokens (canvas #07060b, warm text,
 * wordmark header, hero spiral top-right + glossy orb bottom-left, chip pill,
 * cards radius 14, cyan->magenta CTAs, standard 3SVerse footer), dark + light.
 *
 * Engine: WinHTTP, dynamic file segmentation (up to 32 connections), resume
 * via .gstmeta, retry with backoff, queue, clipboard monitor, grabstorm://
 * protocol. Browser integration: loopback HTTP bridge (silent capture,
 * token-authed /add, presence probe) + policy-installed extension.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "util.h"
#include "engine.h"
#include "bridge.h"

/* GDI+ opaque types (flat API) */
typedef struct GpGraphics GpGraphics;
typedef struct GpImage GpImage;
typedef struct GpImageAttributes GpImageAttributes;

#define APP_TITLEW  L"3SVerse GrabStorm v1.1.0"
#define APP_CLASSW  L"3SVerseGrabStorm"
#define BTN_CLASSW  L"3SVBtn"
#define ADD_CLASSW  L"GSAddDlg"
#define OPT_CLASSW  L"GSOptDlg"
#define APP_VER_A   L"v1.1.0"

/* embedded brand art (RCDATA in src/res.rc) */
#define RES_LOGO    101
#define RES_SPIRAL  102
#define RES_ORB     103

/* main window control ids */
#define IDC_THEME    110
#define IDC_ADD      111
#define IDC_START    112
#define IDC_STARTALL 113
#define IDC_STOP     114
#define IDC_STOPALL  115
#define IDC_DEL      116
#define IDC_OPT      117
#define IDC_LIST     120

/* add-url dialog ids */
#define IDC_AURL     201
#define IDC_ANAME    202
#define IDC_ADIR     203
#define IDC_ABROWSE  204
#define IDC_ADOWN    205
#define IDC_AQUEUE   206
#define IDC_ACANCEL  207

/* options dialog ids */
#define IDC_OSEG     221
#define IDC_OMAP     222
#define IDC_ODIR     223
#define IDC_OBROWSE  224
#define IDC_OCLIP    225
#define IDC_OSAVE    226
#define IDC_OCANCEL  227
#define IDC_OPORT    228
#define IDC_OTOKEN   229
#define IDC_OTGEN    230
#define IDC_OBRIDGE  231

/* list context menu ids */
#define MNU_OPENFILE  9001
#define MNU_OPENFOLD  9002
#define MNU_PAUSE     9003
#define MNU_RESUME    9004
#define MNU_STOP      9005
#define MNU_RETRY     9006
#define MNU_DELETE    9007
#define MNU_COPYURL   9008
#define MNU_SHOW      9010
#define MNU_STARTALL  9011
#define MNU_STOPALL   9012
#define MNU_EXIT      9013

#define BSTYLE_PRIMARY 0
#define BSTYLE_GHOST   1
#define BSTYLE_TOGGLE  2

#define CLIENT_W  780
#define CLIENT_H  620
#define FOOTER_H  32

/* ------------------------------------------------------------------ */
/* palette — DARK values are the exact xfer / 3sverse.com brand tokens */
/* ------------------------------------------------------------------ */
typedef struct {
    COLORREF bg, card, border, fieldBg;
    COLORREF txt, txtDim, txtMute;
    COLORREF cyan, mag, lime;
    COLORREF footTxt, footVer;
    COLORREF primTxt;
    double   artAlpha;
} PALETTE;

static PALETTE g_pal;
static BOOL    g_dark = TRUE;

static void SetDarkPalette(PALETTE *p)
{
    p->bg      = RGB(0x07, 0x06, 0x0b);
    p->card    = RGB(0x0d, 0x0c, 0x14);
    p->border  = RGB(0x23, 0x21, 0x30);
    p->fieldBg = RGB(0x12, 0x10, 0x1a);
    p->txt     = RGB(0xf5, 0xf1, 0xe6);
    p->txtDim  = RGB(0xb9, 0xb5, 0xcc);
    p->txtMute = RGB(0x9b, 0x97, 0xb3);
    p->cyan    = RGB(0x6e, 0xe7, 0xef);
    p->mag     = RGB(0xe4, 0x4b, 0xd7);
    p->lime    = RGB(0xc7, 0xef, 0x70);
    p->footTxt = RGB(0xc7, 0xcb, 0xe0);
    p->footVer = RGB(0x8b, 0x87, 0xa0);
    p->primTxt = RGB(0x07, 0x06, 0x0b);
    p->artAlpha = 0.90;
}

static void SetLightPalette(PALETTE *p)
{
    p->bg      = RGB(0xf4, 0xf2, 0xec);
    p->card    = RGB(0xff, 0xff, 0xff);
    p->border  = RGB(0xe4, 0xdf, 0xd3);
    p->fieldBg = RGB(0xfa, 0xf8, 0xf3);
    p->txt     = RGB(0x2a, 0x27, 0x32);
    p->txtDim  = RGB(0x5c, 0x58, 0x68);
    p->txtMute = RGB(0x8b, 0x87, 0x98);
    p->cyan    = RGB(0x0e, 0x9f, 0xb6);
    p->mag     = RGB(0xc0, 0x2b, 0xa6);
    p->lime    = RGB(0x6f, 0x9c, 0x12);
    p->footTxt = RGB(0x4c, 0x48, 0x60);
    p->footVer = RGB(0x92, 0x8e, 0x9e);
    p->primTxt = RGB(0x07, 0x06, 0x0b);
    p->artAlpha = 0.25;
}

static void ApplyPalette(void)
{
    if (g_dark) SetDarkPalette(&g_pal); else SetLightPalette(&g_pal);
}

/* ------------------------------------------------------------------ */
/* globals                                                             */
/* ------------------------------------------------------------------ */
static HWND g_hMain, g_hList;
static HWND g_hTheme, g_hAdd, g_hStart, g_hStartAll, g_hStop, g_hStopAll, g_hDel, g_hOpt;
static HFONT g_hFont, g_hFontBtn, g_hMono, g_hH1, g_hSub, g_hLbl, g_hChip, g_hFoot;
static HBRUSH g_brBg, g_brField;
static RECT g_rcCardList, g_rcChip, g_rcFooter, g_rcStatus;
static double g_phase = 0.0;
static int    g_tick  = 0;
static wchar_t g_clipExts[512];
static wchar_t g_defDir[MAX_PATH];

/* add dialog prefill/state */
static wchar_t g_addUrl[2048];
static wchar_t g_addName[160];
static wchar_t g_addDirOut[MAX_PATH];
static int     g_addResult = 0;
static HWND    g_hAddDlg;
static HWND    g_aUrl, g_aName, g_aDir, g_aBrowse, g_aDown, g_aQueue, g_aCancel;

/* options dialog state */
static HWND g_hOptDlg;
static HWND g_oSeg, g_oMap, g_oDir, g_oBrowse, g_oClip, g_oSave, g_oCancel;
static HWND g_oPort, g_oToken, g_oTgen, g_oBridge;
static BOOL g_clipOn = TRUE;
static BOOL g_bridgeOn = TRUE;

/* GDI+ state */
static ULONG_PTR         g_gdipToken = 0;
static GpImage          *g_imgLogo = NULL, *g_imgSpiral = NULL, *g_imgOrb = NULL;
static GpImageAttributes *g_attrDark = NULL, *g_attrLite = NULL;
static IStream          *g_streams[3] = { NULL, NULL, NULL };

static NOTIFYICONDATAW g_nid;
static HICON  g_hIcon = NULL;
static HINSTANCE g_hInst;

/* ------------------------------------------------------------------ */
/* GDI+ flat API (C-safe; MinGW gdiplus headers are C++-oriented)      */
/* ------------------------------------------------------------------ */
typedef float REAL;
typedef int   GpStatus;      /* 0 = Ok */

typedef struct {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} GpStartupInput;

#define WINGDIPAPI __stdcall

#define GPUNIT_PIXEL        2
#define MXORDER_APPEND      1
#define ITPL_HQ_BICUBIC     7
#define COLORTYPE_DEFAULT   0
#define CMFLAGS_DEFAULT     0

typedef struct { REAL m[5][5]; } GpColorMatrix;

static GpStatus (WINGDIPAPI *p_GdiplusStartup)(ULONG_PTR *, const GpStartupInput *, void *);
static VOID      (WINGDIPAPI *p_GdiplusShutdown)(ULONG_PTR);
static GpStatus  (WINGDIPAPI *p_GdipLoadImageFromStream)(IStream *, GpImage **);
static GpStatus  (WINGDIPAPI *p_GdipDisposeImage)(GpImage *);
static GpStatus  (WINGDIPAPI *p_GdipGetImageWidth)(GpImage *, UINT *);
static GpStatus  (WINGDIPAPI *p_GdipGetImageHeight)(GpImage *, UINT *);
static GpStatus  (WINGDIPAPI *p_GdipCreateFromHDC)(HDC, GpGraphics **);
static GpStatus  (WINGDIPAPI *p_GdipDeleteGraphics)(GpGraphics *);
static GpStatus  (WINGDIPAPI *p_GdipSetInterpolationMode)(GpGraphics *, int);
static GpStatus  (WINGDIPAPI *p_GdipRotateWorldTransform)(GpGraphics *, REAL, int);
static GpStatus  (WINGDIPAPI *p_GdipTranslateWorldTransform)(GpGraphics *, REAL, REAL, int);
static GpStatus  (WINGDIPAPI *p_GdipResetWorldTransform)(GpGraphics *);
static GpStatus  (WINGDIPAPI *p_GdipDrawImageRectRectI)(GpGraphics *, GpImage *,
                     INT, INT, INT, INT, INT, INT, INT, INT, INT,
                     const GpImageAttributes *, void *, void *);
static GpStatus  (WINGDIPAPI *p_GdipCreateImageAttributes)(GpImageAttributes **);
static GpStatus  (WINGDIPAPI *p_GdipDisposeImageAttributes)(GpImageAttributes *);
static GpStatus  (WINGDIPAPI *p_GdipSetImageAttributesColorMatrix)(GpImageAttributes *,
                     INT, BOOL, const GpColorMatrix *, const GpColorMatrix *, INT);

static BOOL LoadGdiPlus(void)
{
    HMODULE m = LoadLibraryW(L"gdiplus.dll");
    if (!m) return FALSE;
#define GPGET(f) p_##f = (void *)GetProcAddress(m, #f); if (!p_##f) return FALSE
    GPGET(GdiplusStartup);
    GPGET(GdiplusShutdown);
    GPGET(GdipLoadImageFromStream);
    GPGET(GdipDisposeImage);
    GPGET(GdipGetImageWidth);
    GPGET(GdipGetImageHeight);
    GPGET(GdipCreateFromHDC);
    GPGET(GdipDeleteGraphics);
    GPGET(GdipSetInterpolationMode);
    GPGET(GdipRotateWorldTransform);
    GPGET(GdipTranslateWorldTransform);
    GPGET(GdipResetWorldTransform);
    GPGET(GdipDrawImageRectRectI);
    GPGET(GdipCreateImageAttributes);
    GPGET(GdipDisposeImageAttributes);
    GPGET(GdipSetImageAttributesColorMatrix);
#undef GPGET
    return TRUE;
}

static GpImage *ImageFromResource(int resId, IStream **keep)
{
    HRSRC hr = FindResourceW(NULL, MAKEINTRESOURCEW(resId), (LPCWSTR)RT_RCDATA);
    HGLOBAL res;
    const void *p;
    DWORD sz = 0;
    IStream *stm = NULL;
    GpImage *img = NULL;
    *keep = NULL;
    if (!hr) return NULL;
    res = LoadResource(NULL, hr);
    if (!res) return NULL;
    p = LockResource(res);
    sz = SizeofResource(NULL, hr);
    if (!p || !sz) return NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stm) != S_OK || !stm) return NULL;
    {
        ULONG written = 0;
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (stm->lpVtbl->Write(stm, p, sz, &written) != S_OK || written != sz) {
            stm->lpVtbl->Release(stm);
            return NULL;
        }
        stm->lpVtbl->Seek(stm, zero, STREAM_SEEK_SET, NULL);
        if (p_GdipLoadImageFromStream(stm, &img) != 0) {
            stm->lpVtbl->Release(stm);
            return NULL;
        }
    }
    *keep = stm;
    return img;
}

static GpImageAttributes *MakeAlphaAttr(double alpha)
{
    GpImageAttributes *attr = NULL;
    GpColorMatrix cm;
    int i, j;
    for (i = 0; i < 5; i++)
        for (j = 0; j < 5; j++)
            cm.m[i][j] = (i == j) ? 1.0f : 0.0f;
    cm.m[3][3] = (REAL)alpha;
    if (p_GdipCreateImageAttributes(&attr) != 0 || !attr) return NULL;
    p_GdipSetImageAttributesColorMatrix(attr, COLORTYPE_DEFAULT, TRUE,
                                        &cm, NULL, CMFLAGS_DEFAULT);
    return attr;
}

static void DrawBrandImage(HDC hdc, GpImage *img, int cx, int cy,
                           int w, int h, double angleDeg,
                           GpImageAttributes *attr)
{
    GpGraphics *g = NULL;
    UINT iw = 0, ih = 0;
    if (!img || p_GdipCreateFromHDC(hdc, &g) != 0 || !g) { if (g) p_GdipDeleteGraphics(g); return; }
    p_GdipGetImageWidth(img, &iw);
    p_GdipGetImageHeight(img, &ih);
    if (iw && ih) {
        p_GdipSetInterpolationMode(g, ITPL_HQ_BICUBIC);
        p_GdipTranslateWorldTransform(g, (REAL)cx, (REAL)cy, MXORDER_APPEND);
        p_GdipRotateWorldTransform(g, (REAL)angleDeg, MXORDER_APPEND);
        p_GdipTranslateWorldTransform(g, (REAL)(-w / 2), (REAL)(-h / 2), MXORDER_APPEND);
        p_GdipDrawImageRectRectI(g, img, 0, 0, w, h, 0, 0, (INT)iw, (INT)ih,
                                 GPUNIT_PIXEL, attr, NULL, NULL);
        p_GdipResetWorldTransform(g);
    }
    p_GdipDeleteGraphics(g);
}

/* ------------------------------------------------------------------ */
/* small drawing helpers                                               */
/* ------------------------------------------------------------------ */
static COLORREF Lerp(COLORREF a, COLORREF b, double t)
{
    int r, g, bl;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    r  = GetRValue(a) + (int)((GetRValue(b) - GetRValue(a)) * t);
    g  = GetGValue(a) + (int)((GetGValue(b) - GetGValue(a)) * t);
    bl = GetBValue(a) + (int)((GetBValue(b) - GetBValue(a)) * t);
    if (r < 0) r = 0;  if (r > 255) r = 255;
    if (g < 0) g = 0;  if (g > 255) g = 255;
    if (bl < 0) bl = 0; if (bl > 255) bl = 255;
    return RGB(r, g, bl);
}

static COLORREF Shade(COLORREF c, double f)
{
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    if (f <= 1.0) {
        r = (int)(r * f); g = (int)(g * f); b = (int)(b * f);
    } else {
        double t = f - 1.0;
        r = (int)(r + (255 - r) * t);
        g = (int)(g + (255 - g) * t);
        b = (int)(b + (255 - b) * t);
    }
    if (r < 0) r = 0;  if (r > 255) r = 255;
    if (g < 0) g = 0;  if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return RGB(r, g, b);
}

static void FillSolid(HDC dc, RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void FillRound(HDC dc, RECT r, int rad, COLORREF fill, COLORREF outline)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void FillHGrad(HDC dc, RECT r, COLORREF c1, COLORREF c2, int bands)
{
    int i;
    int w = r.right - r.left;
    if (w <= 0 || bands < 1) return;
    for (i = 0; i < bands; i++) {
        RECT s = r;
        s.left  = r.left + (int)((long long)w * i / bands);
        s.right = r.left + (int)((long long)w * (i + 1) / bands);
        if (s.right <= s.left) s.right = s.left + 1;
        FillSolid(dc, s, Lerp(c1, c2, (double)(i + 0.5) / bands));
    }
}

static void PaintTextMono(HDC dc, const WCHAR *s, RECT r, HFONT f,
                          COLORREF c, int extra, UINT align)
{
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetTextCharacterExtra(dc, extra);
    DrawTextW(dc, s, -1, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SetTextCharacterExtra(dc, 0);
    SelectObject(dc, of);
}

/* ------------------------------------------------------------------ */
/* theme persistence (HKCU\Software\3SVerse\GrabStorm)                 */
/* ------------------------------------------------------------------ */
static void LoadTheme(void)
{
    HKEY k;
    g_dark = TRUE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY,
                      0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        WCHAR v[16] = L"";
        DWORD cb = sizeof(v), type = 0;
        if (RegQueryValueExW(k, L"Theme", NULL, &type, (BYTE *)v, &cb) == ERROR_SUCCESS
            && type == REG_SZ)
            g_dark = lstrcmpiW(v, L"light") != 0;
        RegCloseKey(k);
    }
}

static void SaveTheme(void)
{
    SettingsSetStr(L"Theme", g_dark ? L"dark" : L"light");
}

typedef HRESULT (WINAPI *FN_DwmSetAttr)(HWND, DWORD, const void *, DWORD);

static void ApplyDwm(HWND w)
{
    HMODULE m = GetModuleHandleW(L"dwmapi.dll");
    FN_DwmSetAttr f;
    BOOL dark = g_dark;
    DWORD pref = 2;
    COLORREF cap;
    if (!m || !w) return;
    f = (FN_DwmSetAttr)(void *)GetProcAddress(m, "DwmSetWindowAttribute");
    if (!f) return;
    f(w, 20, &dark, sizeof(dark));
    f(w, 19, &dark, sizeof(dark));
    f(w, 33, &pref, sizeof(pref));
    cap = g_pal.bg;
    f(w, 35, &cap, sizeof(cap));
}

static void RebuildBrushes(void)
{
    if (g_brBg)     DeleteObject(g_brBg);
    if (g_brField)  DeleteObject(g_brField);
    g_brBg    = CreateSolidBrush(g_pal.bg);
    g_brField = CreateSolidBrush(g_pal.fieldBg);
}

static void ToggleTheme(HWND hwnd)
{
    g_dark = !g_dark;
    SaveTheme();
    ApplyPalette();
    RebuildBrushes();
    ApplyDwm(hwnd);
    if (g_hTheme) SetWindowTextW(g_hTheme, g_dark ? L"Light mode" : L"Dark mode");
    InvalidateRect(hwnd, NULL, TRUE);
}

/* ------------------------------------------------------------------ */
/* 3SVBtn — custom self-painting button class (no BS_OWNERDRAW —      */
/* blank-out impossible, T190 standing pattern)                        */
/* ------------------------------------------------------------------ */
typedef struct {
    int  style;
    BOOL hover;
    BOOL down;
    HFONT font;
} BTNSTATE;

static void BtnPaint(HWND h, BTNSTATE *st)
{
    PAINTSTRUCT ps;
    RECT r, rt;
    HDC dc;
    WCHAR t[64];
    COLORREF txtc, fill, out;
    int bands = 24;

    dc = BeginPaint(h, &ps);
    GetClientRect(h, &r);
    rt = r;
    GetWindowTextW(h, t, 64);
    txtc = g_pal.txtDim;
    fill = g_pal.card;
    out  = g_pal.border;

    if (!IsWindowEnabled(h)) {
        FillRound(dc, r, 11, (st->style == BSTYLE_TOGGLE) ? g_pal.bg : g_pal.card, g_pal.border);
        txtc = g_pal.txtMute;
        bands = 1;
    } else if (st->style == BSTYLE_PRIMARY) {
        COLORREF a = st->down ? Shade(g_pal.cyan, 0.86) :
                     (st->hover ? Shade(g_pal.cyan, 1.12) : g_pal.cyan);
        COLORREF b = st->down ? Shade(g_pal.mag, 0.86) :
                     (st->hover ? Shade(g_pal.mag, 1.12) : g_pal.mag);
        {
            HRGN rg = CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, 22, 22);
            SelectClipRgn(dc, rg);
            FillHGrad(dc, r, a, b, bands);
            SelectClipRgn(dc, NULL);
            DeleteObject(rg);
        }
        {
            HPEN p = CreatePen(PS_SOLID, 1, Shade(b, 0.72));
            HGDIOBJ op = SelectObject(dc, p);
            HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
            HGDIOBJ ob = SelectObject(dc, nb);
            RoundRect(dc, r.left, r.top, r.right, r.bottom, 22, 22);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(p);
        }
        txtc = g_pal.primTxt;
    } else {
        COLORREF fillc = (st->style == BSTYLE_TOGGLE) ? g_pal.bg : g_pal.card;
        if (st->down)      { fill = g_pal.fieldBg; }
        else if (st->hover){ fill = fillc; out = g_pal.cyan; txtc = g_pal.cyan; }
        else               { fill = fillc; }
        FillRound(dc, r, 11, fill, out);
    }

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, st->font ? st->font : g_hFont);
    SetTextColor(dc, txtc);
    DrawTextW(dc, t, -1, &rt, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    BTNSTATE *st = (BTNSTATE *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)l;
        st = (BTNSTATE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(BTNSTATE));
        if (!st) return -1;
        st->style = (int)(INT_PTR)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        return 0;
    }
    case WM_SETFONT:
        if (st) st->font = (HFONT)w;
        InvalidateRect(h, NULL, TRUE);
        return 0;
    case WM_MOUSEMOVE:
        if (st && !st->hover) {
            TRACKMOUSEEVENT t;
            st->hover = 1;
            ZeroMemory(&t, sizeof(t));
            t.cbSize = sizeof(t);
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            TrackMouseEvent(&t);
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (st) { st->hover = 0; InvalidateRect(h, NULL, TRUE); }
        return 0;
    case WM_LBUTTONDOWN:
        if (st) { st->down = 1; SetCapture(h); InvalidateRect(h, NULL, TRUE); }
        return 0;
    case WM_LBUTTONUP:
        if (st && st->down) {
            POINT pt;
            RECT r;
            st->down = 0;
            if (GetCapture() == h) ReleaseCapture();
            GetClientRect(h, &r);
            pt.x = (short)LOWORD(l); pt.y = (short)HIWORD(l);
            if (PtInRect(&r, pt))
                SendMessageW(GetParent(h), WM_COMMAND,
                             MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (st) { st->down = 0; InvalidateRect(h, NULL, TRUE); }
        return 0;
    case WM_ENABLE:
        InvalidateRect(h, NULL, TRUE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (st) BtnPaint(h, st);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_BUTTON;
    case WM_NCDESTROY:
        if (st) { HeapFree(GetProcessHeap(), 0, st); SetWindowLongPtrW(h, GWLP_USERDATA, 0); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* edit subclass used by the popups: dark fill + Enter/Esc -> WM_COMMAND */
static LRESULT CALLBACK FieldDProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC old = (WNDPROC)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_ERASEBKGND) {
        HDC dc = (HDC)w;
        RECT r;
        HBRUSH b;
        GetClientRect(h, &r);
        b = CreateSolidBrush(g_pal.fieldBg);
        FillRect(dc, &r, b);
        DeleteObject(b);
        return 1;
    }
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        HWND p = GetParent(h);
        int id = (h == g_aUrl) ? IDC_ADOWN : (h == g_oSeg || h == g_oMap) ? IDC_OSAVE : IDC_ADOWN;
        SendMessageW(p, WM_COMMAND, MAKEWPARAM(id, 0), (LPARAM)h);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(IDC_ACANCEL, 0), (LPARAM)h);
        return 0;
    }
    return CallWindowProcW(old, h, m, w, l);
}

static void SubField(HWND h)
{
    WNDPROC old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)FieldDProc);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)old);
}

/* ------------------------------------------------------------------ */
/* tray                                                                */
/* ------------------------------------------------------------------ */
static void TrayAdd(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hMain;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_hIcon;
    lstrcpyW(g_nid.szTip, L"GrabStorm");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void TrayBalloon(const WCHAR *title, const WCHAR *msg, DWORD flags)
{
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_INFO;
    lstrcpynW(g_nid.szInfoTitle, title, 64);
    lstrcpynW(g_nid.szInfo, msg, 256);
    g_nid.dwInfoFlags = flags;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ShowMain(void)
{
    ShowWindow(g_hMain, SW_RESTORE);
    SetForegroundWindow(g_hMain);
}

/* ------------------------------------------------------------------ */
/* shared dialog helpers                                               */
/* ------------------------------------------------------------------ */
static void BrowseFolder(HWND owner, HWND target)
{
    BROWSEINFOW bi;
    WCHAR path[MAX_PATH];
    LPITEMIDLIST pidl;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Choose the folder to save downloads into:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(target, path);
        CoTaskMemFree(pidl);
    }
}

static void RunModal(HWND dlg)
{
    MSG m;
    BOOL run = TRUE;
    EnableWindow(g_hMain, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    while (run && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.hwnd == dlg && m.message == WM_APP_MODAL) { run = FALSE; continue; }
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(g_hMain, TRUE);
    SetForegroundWindow(g_hMain);
}

static void ModalQuit(HWND dlg)
{
    DestroyWindow(dlg);
    PostMessageW(dlg, WM_APP_MODAL, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Add URL dialog                                                      */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK AddDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE h = ((LPCREATESTRUCTW)lp)->hInstance;
        RECT wa;
        GetWindowRect(g_hMain, &wa);
        g_aUrl   = CreateWindowW(L"EDIT", g_addUrl, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 28, 64, 404, 30, hwnd, (HMENU)IDC_AURL, h, NULL);
        g_aName  = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 28, 122, 270, 30, hwnd, (HMENU)IDC_ANAME, h, NULL);
        g_aDir   = CreateWindowW(L"EDIT", g_defDir, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 28, 176, 270, 30, hwnd, (HMENU)IDC_ADIR, h, NULL);
        g_aBrowse = CreateWindowW(BTN_CLASSW, L"Browse...",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  304, 176, 120, 30, hwnd, (HMENU)IDC_ABROWSE, h,
                                  (LPVOID)(INT_PTR)BSTYLE_GHOST);
        g_aDown  = CreateWindowW(BTN_CLASSW, L"Download now",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 16, 232, 208, 40, hwnd, (HMENU)IDC_ADOWN, h,
                                 (LPVOID)(INT_PTR)BSTYLE_PRIMARY);
        g_aQueue = CreateWindowW(BTN_CLASSW, L"Add to list",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 232, 232, 140, 40, hwnd, (HMENU)IDC_AQUEUE, h,
                                 (LPVOID)(INT_PTR)BSTYLE_GHOST);
        g_aCancel = CreateWindowW(BTN_CLASSW, L"Cancel",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  380, 232, 64, 40, hwnd, (HMENU)IDC_ACANCEL, h,
                                  (LPVOID)(INT_PTR)BSTYLE_GHOST);
        SendMessageW(g_aUrl,  WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_aName, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_aDir,  WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_aBrowse, WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
        SendMessageW(g_aDown,   WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
        SendMessageW(g_aQueue,  WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
        SendMessageW(g_aCancel, WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
        SubField(g_aUrl);
        SubField(g_aName);
        SubField(g_aDir);
        SendMessageW(g_aUrl, EM_SETLIMITTEXT, 2000, 0);
        if (g_addUrl[0]) SetFocus(g_aName); else SetFocus(g_aUrl);
        /* center over main window */
        {
            int w = 460 + 2 * GetSystemMetrics(SM_CXFIXEDFRAME);
            int hgt = 284 + 2 * GetSystemMetrics(SM_CYFIXEDFRAME) + GetSystemMetrics(SM_CYCAPTION);
            SetWindowPos(hwnd, NULL,
                         wa.left + ((wa.right - wa.left) - w) / 2,
                         wa.top + ((wa.bottom - wa.top) - hgt) / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r, card;
        GetClientRect(hwnd, &r);
        {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
            HGDIOBJ ob = SelectObject(mem, bmp);
            FillSolid(mem, r, g_pal.bg);
            card.left = 16; card.top = 36; card.right = 444; card.bottom = 216;
            FillRound(mem, card, 14, g_pal.card, g_pal.border);
            SetBkMode(mem, TRANSPARENT);
            PaintTextMono(mem, L"DOWNLOAD URL", (RECT){ 28, 46, 300, 62 }, g_hLbl, g_pal.txtMute, 1, DT_LEFT);
            PaintTextMono(mem, L"SAVE AS (OPTIONAL)", (RECT){ 28, 104, 300, 120 }, g_hLbl, g_pal.txtMute, 1, DT_LEFT);
            PaintTextMono(mem, L"FOLDER", (RECT){ 28, 158, 300, 174 }, g_hLbl, g_pal.txtMute, 1, DT_LEFT);
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, g_pal.txt);
        SetBkColor((HDC)wp, g_pal.fieldBg);
        return (LRESULT)g_brField;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, g_pal.txt);
        SetBkColor((HDC)wp, g_pal.bg);
        return (LRESULT)g_brBg;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ADOWN:  g_addResult = 1; goto finish;
        case IDC_AQUEUE: g_addResult = 2; goto finish;
        case IDC_ACANCEL: g_addResult = 0; goto finish;
        case IDC_ABROWSE:
            BrowseFolder(hwnd, g_aDir);
            return 0;
        finish: {
            GetWindowTextW(g_aUrl, g_addUrl, 2048);
            GetWindowTextW(g_aName, g_addName, 160);
            GetWindowTextW(g_aDir, g_addDirOut, MAX_PATH);
            ModalQuit(hwnd);
            return 0;
        }
        }
        return 0;
    case WM_CLOSE:
        g_addResult = 0;
        ModalQuit(hwnd);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
/* ------------------------------------------------------------------ */
/* Options dialog                                                      */
/* ------------------------------------------------------------------ */
static void ClipToggleText(void)
{
    if (g_oClip) SetWindowTextW(g_oClip, g_clipOn ? L"Clipboard monitor: On"
                                                  : L"Clipboard monitor: Off");
}

static void BridgeToggleText(void)
{
    if (g_oBridge) SetWindowTextW(g_oBridge, g_bridgeOn ? L"Browser bridge: On"
                                                        : L"Browser bridge: Off");
}

static LRESULT CALLBACK OptDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE h = ((LPCREATESTRUCTW)lp)->hInstance;
        RECT wa;
        wchar_t buf[32];
        GetWindowRect(g_hMain, &wa);
        g_oSeg = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                               28, 64, 120, 30, hwnd, (HMENU)IDC_OSEG, h, NULL);
        g_oMap = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                               28, 122, 120, 30, hwnd, (HMENU)IDC_OMAP, h, NULL);
        g_oDir = CreateWindowW(L"EDIT", g_defDir, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               28, 180, 270, 30, hwnd, (HMENU)IDC_ODIR, h, NULL);
        g_oBrowse = CreateWindowW(BTN_CLASSW, L"Browse...",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  304, 180, 120, 30, hwnd, (HMENU)IDC_OBROWSE, h,
                                  (LPVOID)(INT_PTR)BSTYLE_GHOST);
        g_oClip = CreateWindowW(BTN_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                28, 226, 240, 36, hwnd, (HMENU)IDC_OCLIP, h,
                                (LPVOID)(INT_PTR)BSTYLE_TOGGLE);
        g_oBridge = CreateWindowW(BTN_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                28, 332, 240, 36, hwnd, (HMENU)IDC_OBRIDGE, h,
                                (LPVOID)(INT_PTR)BSTYLE_TOGGLE);
        g_oPort = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                28, 396, 90, 30, hwnd, (HMENU)IDC_OPORT, h, NULL);
        g_oToken = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                126, 396, 196, 30, hwnd, (HMENU)IDC_OTOKEN, h, NULL);
        g_oTgen = CreateWindowW(BTN_CLASSW, L"New token",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                330, 396, 94, 30, hwnd, (HMENU)IDC_OTGEN, h,
                                (LPVOID)(INT_PTR)BSTYLE_GHOST);
        g_oSave = CreateWindowW(BTN_CLASSW, L"Save",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                16, 458, 208, 40, hwnd, (HMENU)IDC_OSAVE, h,
                                (LPVOID)(INT_PTR)BSTYLE_PRIMARY);
        g_oCancel = CreateWindowW(BTN_CLASSW, L"Cancel",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  232, 458, 120, 40, hwnd, (HMENU)IDC_OCANCEL, h,
                                  (LPVOID)(INT_PTR)BSTYLE_GHOST);
        _snwprintf(buf, 32, L"%d", EngineGetSegments());
        SetWindowTextW(g_oSeg, buf);
        _snwprintf(buf, 32, L"%d", EngineGetMaxActive());
        SetWindowTextW(g_oMap, buf);
        g_clipOn = SettingsGetInt(L"ClipMonitor", 1) != 0;
        ClipToggleText();
        g_bridgeOn = SettingsGetInt(L"BridgeOn", 1) != 0;
        BridgeToggleText();
        {
            wchar_t pb[16], tb[128];
            _snwprintf(pb, 16, L"%d", BridgePort());
            SetWindowTextW(g_oPort, pb);
            SettingsGetStr(L"BridgeToken", tb, 128, L"");
            SetWindowTextW(g_oToken, tb);
        }
        SendMessageW(g_oSeg, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_oMap, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_oDir, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_oBrowse, WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
        SendMessageW(g_oClip,   WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
        SendMessageW(g_oBridge, WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
        SendMessageW(g_oPort,   WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_oToken,  WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_oTgen,   WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
        SendMessageW(g_oSave,   WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
        SendMessageW(g_oCancel, WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
        SubField(g_oSeg);
        SubField(g_oMap);
        SubField(g_oDir);
        SubField(g_oPort);
        SubField(g_oToken);
        SetFocus(g_oSeg);
        {
            int w = 460 + 2 * GetSystemMetrics(SM_CXFIXEDFRAME);
            int hgt = 514 + 2 * GetSystemMetrics(SM_CYFIXEDFRAME) + GetSystemMetrics(SM_CYCAPTION);
            SetWindowPos(hwnd, NULL,
                         wa.left + ((wa.right - wa.left) - w) / 2,
                         wa.top + ((wa.bottom - wa.top) - hgt) / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r, card;
        GetClientRect(hwnd, &r);
        {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
            HGDIOBJ ob = SelectObject(mem, bmp);
            FillSolid(mem, r, g_pal.bg);
            card.left = 16; card.top = 36; card.right = 444; card.bottom = 446;
            FillRound(mem, card, 14, g_pal.card, g_pal.border);
            SetBkMode(mem, TRANSPARENT);
            PaintTextMono(mem, L"SEGMENTS PER DOWNLOAD (1-32)",
                          (RECT){ 28, 46, 400, 62 }, g_hLbl, g_pal.txtMute, 1, DT_LEFT);
            PaintTextMono(mem, L"MAX ACTIVE DOWNLOADS (1-10)",
                          (RECT){ 28, 104, 400, 120 }, g_hLbl, g_pal.txtMute, 1, DT_LEFT);
            PaintTextMono(mem, L"DOWNLOAD FOLDER",
                          (RECT){ 28, 162, 400, 178 }, g_hLbl, g_pal.txtMute, 1, DT_LEFT);
            PaintTextMono(mem, L"BROWSER BRIDGE — SILENT EXTENSION CAPTURE",
                          (RECT){ 28, 314, 420, 330 }, g_hLbl, g_pal.txtMute, 1, DT_LEFT);
            PaintTextMono(mem, L"PORT",
                          (RECT){ 28, 380, 90, 394 }, g_hLbl, g_pal.txtMute, 1, DT_LEFT);
            PaintTextMono(mem, L"TOKEN (EXTENSION MUST MATCH)",
                          (RECT){ 126, 380, 420, 394 }, g_hLbl, g_pal.txtMute, 1, DT_LEFT);
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, g_pal.txt);
        SetBkColor((HDC)wp, g_pal.fieldBg);
        return (LRESULT)g_brField;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, g_pal.txt);
        SetBkColor((HDC)wp, g_pal.bg);
        return (LRESULT)g_brBg;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_OSAVE: {
            wchar_t b[32], tb[128];
            int segs, act, port;
            GetWindowTextW(g_oSeg, b, 32);
            segs = (int)wcstoul(b, NULL, 10);
            if (segs < 1) segs = 1;
            if (segs > 32) segs = 32;
            GetWindowTextW(g_oMap, b, 32);
            act = (int)wcstoul(b, NULL, 10);
            if (act < 1) act = 1;
            if (act > 10) act = 10;
            GetWindowTextW(g_oDir, g_defDir, MAX_PATH);
            GetWindowTextW(g_oPort, b, 32);
            port = (int)wcstoul(b, NULL, 10);
            if (port < 1024 || port > 65535) port = BridgePort();
            GetWindowTextW(g_oToken, tb, 128);
            EngineSetSegments(segs);
            EngineSetMaxActive(act);
            SettingsSetStr(L"Dir", g_defDir);
            SettingsSetInt(L"ClipMonitor", g_clipOn ? 1 : 0);
            SettingsSetInt(L"BridgeOn", g_bridgeOn ? 1 : 0);
            SettingsSetInt(L"BridgePort", port);
            if (tb[0]) SettingsSetStr(L"BridgeToken", tb);
            BridgeRestart(g_hMain);
            ModalQuit(hwnd);
            return 0;
        }
        case IDC_OCANCEL:
            ModalQuit(hwnd);
            return 0;
        case IDC_OBROWSE:
            BrowseFolder(hwnd, g_oDir);
            return 0;
        case IDC_OCLIP:
            g_clipOn = !g_clipOn;
            ClipToggleText();
            return 0;
        case IDC_OBRIDGE:
            g_bridgeOn = !g_bridgeOn;
            BridgeToggleText();
            return 0;
        case IDC_OTGEN: {
            wchar_t tb[128];
            BridgeRenewToken(hwnd);
            SettingsGetStr(L"BridgeToken", tb, 128, L"");
            SetWindowTextW(g_oToken, tb);
            return 0;
        }
        }
        return 0;
    case WM_CLOSE:
        ModalQuit(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* dialog launchers                                                    */
/* ------------------------------------------------------------------ */
static void OpenAddDlg(const wchar_t *url)
{
    if (g_hAddDlg) { SetForegroundWindow(g_hAddDlg); return; }
    g_addUrl[0] = 0;
    g_addName[0] = 0;
    if (url) lstrcpynW(g_addUrl, url, 2048);
    EngineDefaultDir(g_defDir, MAX_PATH);
    g_addResult = 0;
    g_hAddDlg = CreateWindowW(ADD_CLASSW, L"Add download",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 284,
        g_hMain, NULL, g_hInst, NULL);
    ApplyDwm(g_hAddDlg);
    RunModal(g_hAddDlg);
    g_hAddDlg = NULL;
    if (g_addResult && g_addUrl[0]) {
        wchar_t dir[MAX_PATH], nm[160];
        lstrcpynW(dir, g_addDirOut[0] ? g_addDirOut : g_defDir, MAX_PATH);
        lstrcpynW(nm, g_addName, 160);
        EngineAdd(g_addUrl, dir, nm, g_addResult == 1);
        EngineTick();
    }
}

static void OpenOptsDlg(void)
{
    if (g_hOptDlg) { SetForegroundWindow(g_hOptDlg); return; }
    g_hOptDlg = CreateWindowW(OPT_CLASSW, L"GrabStorm options",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 514,
        g_hMain, NULL, g_hInst, NULL);
    ApplyDwm(g_hOptDlg);
    RunModal(g_hOptDlg);
    g_hOptDlg = NULL;
}

/* ------------------------------------------------------------------ */
/* list helpers                                                        */
/* ------------------------------------------------------------------ */
static const wchar_t *JobDisplayName(Job *j)
{
    const wchar_t *p;
    if (j->file[0]) {
        p = wcsrchr(j->file, L'\\');
        if (p && p[1]) return p + 1;
        return j->file;
    }
    p = wcsrchr(j->url, L'/');
    return (p && p[1]) ? p + 1 : j->url;
}

static const wchar_t *JobStatusText(Job *j)
{
    switch (j->state) {
    case ST_QUEUED:     return L"Queued";
    case ST_CONNECTING: return L"Connecting";
    case ST_DOWNLOADING:return L"Downloading";
    case ST_PAUSED:     return L"Paused";
    case ST_ERROR:      return L"Error";
    case ST_DONE:       return L"Done";
    }
    return L"";
}

static Job *SelJob(void)
{
    int idx = (int)SendMessageW(g_hList, LVM_GETNEXTITEM, (WPARAM)-1,
                                MAKELPARAM(LVNI_SELECTED, 0));
    if (idx < 0) return NULL;
    return EngineAt(idx);
}

static void RefreshList(int invalidateAll)
{
    int n = EngineCount();
    SendMessageW(g_hList, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemCountEx(g_hList, n, LVSICF_NOINVALIDATEALL);
    SendMessageW(g_hList, WM_SETREDRAW, TRUE, 0);
    if (invalidateAll) InvalidateRect(g_hList, NULL, FALSE);
}

static void OpenJobFile(Job *j, int folderOnly)
{
    if (!j || !j->file[0]) return;
    if (folderOnly) {
        wchar_t dir[MAX_PATH];
        wchar_t *p;
        lstrcpynW(dir, j->file, MAX_PATH);
        p = wcsrchr(dir, L'\\');
        if (p) *p = 0;
        ShellExecuteW(g_hMain, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(g_hMain, L"open", j->file, NULL, NULL, SW_SHOWNORMAL);
    }
}

static void CopyJobUrl(Job *j)
{
    if (!j) return;
    if (OpenClipboard(g_hMain)) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (wcslen(j->url) + 1) * sizeof(WCHAR));
        if (h) {
            void *p = GlobalLock(h);
            if (p) {
                memcpy(p, j->url, (wcslen(j->url) + 1) * sizeof(WCHAR));
                GlobalUnlock(h);
                EmptyClipboard();
                SetClipboardData(CF_UNICODETEXT, h);
            } else {
                GlobalFree(h);
            }
        }
        CloseClipboard();
    }
}

/* grabstorm://add?u=...&r=... */
static BOOL ParseProtoUrl(const wchar_t *link, wchar_t *u, int ucap)
{
    const wchar_t *q = wcschr(link, L'?');
    if (!q) return FALSE;
    q++;
    while (*q) {
        const wchar_t *eq = wcschr(q, L'=');
        const wchar_t *amp = wcschr(q, L'&');
        size_t klen = eq ? (size_t)(eq - q) : 0;
        if (eq && (!amp || eq < amp)) {
            size_t vlen = amp ? (size_t)(amp - eq - 1) : wcslen(eq + 1);
            wchar_t val[2048];
            if (vlen >= 2048) vlen = 2047;
            memcpy(val, eq + 1, vlen * sizeof(WCHAR));
            val[vlen] = 0;
            if (klen == 1 && q[0] == L'u') {
                PercentDecodeW(val, u, ucap);
                return u[0] != 0;
            }
        }
        if (!amp) break;
        q = amp + 1;
    }
    return FALSE;
}

/* clipboard grab */
static void OnClipboardUpdate(void)
{
    static ULONGLONG lastTick = 0;
    static wchar_t lastUrl[2048];
    ULONGLONG now;
    HANDLE h;
    wchar_t *txt;
    if (!SettingsGetInt(L"ClipMonitor", 1)) return;
    if (!OpenClipboard(g_hMain)) return;
    h = GetClipboardData(CF_UNICODETEXT);
    txt = h ? (wchar_t *)GlobalLock(h) : NULL;
    if (txt) {
        wchar_t url[2048];
        lstrcpynW(url, txt, 2048);
        GlobalUnlock(h);
        if ((_wcsnicmp(url, L"http://", 7) == 0 || _wcsnicmp(url, L"https://", 8) == 0)
            && ExtInListW(url, g_clipExts)) {
            now = GetTickCount64();
            if (!(lstrcmpW(url, lastUrl) == 0 && now - lastTick < 12000)) {
                lstrcpynW(lastUrl, url, 2048);
                lastTick = now;
                if (!g_hAddDlg) OpenAddDlg(url);
            }
        }
    }
    CloseClipboard();
}
/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */
static void PaintHeader(HDC dc)
{
    UINT lw = 0, lh = 0;
    int logoH = 46, logoW = 152;
    SetBkMode(dc, TRANSPARENT);
    if (g_imgLogo) {
        p_GdipGetImageWidth(g_imgLogo, &lw);
        p_GdipGetImageHeight(g_imgLogo, &lh);
        if (lw && lh) logoW = (int)(logoH * lw / lh);
        DrawBrandImage(dc, g_imgLogo, CLIENT_W / 2, 32, logoW, logoH, 0.0, NULL);
    } else {
        PaintTextMono(dc, L"3SVERSE", (RECT){ 0, 20, CLIENT_W, 44 }, g_hH1, g_pal.txt, 3,
                      DT_CENTER);
    }
    {
        const WCHAR *p1 = L"3SVERSE ";
        const WCHAR *p2 = L"GRABSTORM";
        SIZE s1, s2;
        int x0, extra = 3;
        HGDIOBJ of = SelectObject(dc, g_hH1);
        GetTextExtentPoint32W(dc, p1, (int)wcslen(p1), &s1);
        GetTextExtentPoint32W(dc, p2, (int)wcslen(p2), &s2);
        SelectObject(dc, of);
        s1.cx += extra * (int)wcslen(p1);
        s2.cx += extra * (int)wcslen(p2);
        x0 = (CLIENT_W - (s1.cx + s2.cx)) / 2;
        PaintTextMono(dc, p1, (RECT){ x0, 62, x0 + s1.cx + 4, 80 }, g_hH1, g_pal.txt, extra, DT_LEFT);
        PaintTextMono(dc, p2, (RECT){ x0 + s1.cx, 62, x0 + s1.cx + s2.cx + 8, 80 },
                      g_hH1, g_pal.cyan, extra, DT_LEFT);
    }
    PaintTextMono(dc, L"MULTI-SEGMENT ACCELERATOR \x2022 PAUSE/RESUME \x2022 100% LOCAL \x2022 NO CLOUD",
                  (RECT){ 0, 84, CLIENT_W, 100 }, g_hSub, g_pal.cyan, 3, DT_CENTER);
}

static void PaintChip(HDC dc)
{
    RECT r = g_rcChip;
    int cy = (r.top + r.bottom) / 2;
    double pulse = 0.5 + 0.5 * sin(g_phase * 2.6);
    COLORREF dotc;
    int act = EngineActiveCount(), qd = EngineQueuedCount();
    wchar_t t[160];

    FillRound(dc, r, 13, g_pal.card, g_pal.border);
    if (act > 0) {
        wchar_t sp[48];
        FormatSpeedBits(EngineAggSpeed(), sp, 48);
        _snwprintf(t, 160, L"DOWNLOADING \x2022 %d ACTIVE \x2022 %s", act, sp);
        dotc = Lerp(Shade(g_pal.cyan, 0.45), g_pal.cyan, pulse);
    } else if (qd > 0) {
        _snwprintf(t, 160, L"READY \x2022 %d QUEUED", qd);
        dotc = Lerp(Shade(g_pal.lime, 0.7), g_pal.lime, pulse);
    } else {
        lstrcpynW(t, L"READY \x2022 COPY A LINK TO GRAB", 160);
        dotc = g_pal.lime;
    }
    PaintTextMono(dc, t, (RECT){ r.left + 30, r.top, r.right - 14, r.bottom },
                  g_hChip, g_pal.txtDim, 1, DT_LEFT);
    {
        HBRUSH b = CreateSolidBrush(dotc);
        HGDIOBJ ob = SelectObject(dc, b);
        HPEN np = (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ op = SelectObject(dc, np);
        Ellipse(dc, r.left + 18, cy - 4, r.left + 26, cy + 4);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
    }
}

static void PaintFooter(HDC dc)
{
    FillSolid(dc, g_rcFooter, g_pal.card);
    {
        RECT b = { 0, g_rcFooter.top, CLIENT_W, g_rcFooter.top + 1 };
        FillSolid(dc, b, g_pal.border);
    }
    PaintTextMono(dc, L"DEVELOPED BY WWW.3SVERSE.COM",
                  (RECT){ 0, g_rcFooter.top, CLIENT_W, g_rcFooter.bottom },
                  g_hFoot, g_pal.footTxt, 1, DT_CENTER);
    PaintTextMono(dc, APP_VER_A,
                  (RECT){ CLIENT_W - 90, g_rcFooter.top, CLIENT_W - 10, g_rcFooter.bottom },
                  g_hFoot, g_pal.footVer, 0, DT_RIGHT);
}

static void PaintStatusBar(HDC dc)
{
    wchar_t L[160], R[96], sp[48];
    FormatSpeedBits(EngineAggSpeed(), sp, 48);
    _snwprintf(L, 160, L"Active: %d \x2022 Queued: %d \x2022 Done: %d",
               EngineActiveCount(), EngineQueuedCount(), EngineDoneCount());
    _snwprintf(R, 96, L"Speed: %s", sp);
    SetBkMode(dc, TRANSPARENT);
    PaintTextMono(dc, L, g_rcStatus, g_hLbl, g_pal.txtMute, 0, DT_LEFT);
    PaintTextMono(dc, R, g_rcStatus, g_hChip, g_pal.txtDim, 0, DT_RIGHT);
}

static void OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    {
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);

        FillSolid(mem, rc, g_pal.bg);
        if (g_imgSpiral) {
            double ang = g_tick * 0.135;
            double dy = 16.0 * sin(g_tick * (2.0 * 3.14159265358979) / 266.7);
            DrawBrandImage(mem, g_imgSpiral, CLIENT_W - 14, 42 + (int)dy,
                           300, 312, ang, g_dark ? g_attrDark : g_attrLite);
        }
        if (g_imgOrb) {
            double ang2 = g_tick * 0.1157;
            double dy2 = 12.0 * sin(g_tick * (2.0 * 3.14159265358979) / 288.9);
            DrawBrandImage(mem, g_imgOrb, 26, CLIENT_H - 44 + (int)dy2,
                           260, 260, ang2, g_dark ? g_attrDark : g_attrLite);
        }
        PaintHeader(mem);
        PaintChip(mem);
        FillRound(mem, g_rcCardList, 14, g_pal.card, g_pal.border);
        PaintStatusBar(mem);
        PaintFooter(mem);

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    EndPaint(hwnd, &ps);
}

/* progress bar inside the Progress sub-item (column 2) */
static void PaintListRow(NMLVCUSTOMDRAW *ncd)
{
    HWND lv = ncd->nmcd.hdr.hwndFrom;
    int item = (int)ncd->nmcd.dwItemSpec;
    Job *j = EngineAt(item);
    RECT rc;
    ListView_GetSubItemRect(lv, item, 2, LVIR_BOUNDS, &rc);
    FillSolid(ncd->nmcd.hdc, rc, g_pal.card);
    if (j && j->total > 0 && j->state != ST_DONE) {
        int pct = (int)((unsigned __int64)j->got * 100 / j->total);
        RECT bar = { rc.left + 2, rc.top + 5, rc.right - 2, rc.bottom - 5 };
        int w = (bar.right - bar.left - 4) * pct / 100;
        FillRound(ncd->nmcd.hdc, bar, 6, g_pal.fieldBg, g_pal.border);
        if (w > 0) {
            RECT f = { bar.left + 2, bar.top + 2, bar.left + 2 + w, bar.bottom - 2 };
            HRGN rg = CreateRoundRectRgn(f.left, f.top, f.right, f.bottom, 8, 8);
            SelectClipRgn(ncd->nmcd.hdc, rg);
            FillHGrad(ncd->nmcd.hdc, f, g_pal.cyan, g_pal.mag, 24);
            SelectClipRgn(ncd->nmcd.hdc, NULL);
            DeleteObject(rg);
        }
        {
            wchar_t t[16];
            _snwprintf(t, 16, L"%d%%", pct);
            SetBkMode(ncd->nmcd.hdc, TRANSPARENT);
            SelectObject(ncd->nmcd.hdc, g_hLbl);
            SetTextColor(ncd->nmcd.hdc, pct > 0 ? g_pal.txt : g_pal.txtMute);
            DrawTextW(ncd->nmcd.hdc, t, -1, &bar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else if (j && j->state == ST_DONE) {
        RECT bar = { rc.left + 2, rc.top + 5, rc.right - 2, rc.bottom - 5 };
        HRGN rg = CreateRoundRectRgn(bar.left, bar.top, bar.right, bar.bottom, 8, 8);
        SelectClipRgn(ncd->nmcd.hdc, rg);
        FillHGrad(ncd->nmcd.hdc, bar, g_pal.cyan, g_pal.mag, 24);
        SelectClipRgn(ncd->nmcd.hdc, NULL);
        DeleteObject(rg);
        SetBkMode(ncd->nmcd.hdc, TRANSPARENT);
        SelectObject(ncd->nmcd.hdc, g_hLbl);
        SetTextColor(ncd->nmcd.hdc, g_pal.primTxt);
        DrawTextW(ncd->nmcd.hdc, L"100%", -1, &bar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        SetBkMode(ncd->nmcd.hdc, TRANSPARENT);
        SelectObject(ncd->nmcd.hdc, g_hLbl);
        SetTextColor(ncd->nmcd.hdc, g_pal.txtMute);
        DrawTextW(ncd->nmcd.hdc, j && j->total ? L"0%" : L"--", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void AddColumns(HWND lv)
{
    LVCOLUMNW defs[] = {
        { LVCF_TEXT | LVCF_WIDTH, 0, 230, (LPWSTR)L"File", 0, 0, 0, 0 },
        { LVCF_TEXT | LVCF_WIDTH, 0, 80,  (LPWSTR)L"Size", 0, 0, 0, 0 },
        { LVCF_TEXT | LVCF_WIDTH, 0, 130, (LPWSTR)L"Progress", 0, 0, 0, 0 },
        { LVCF_TEXT | LVCF_WIDTH, 0, 95,  (LPWSTR)L"Speed", 0, 0, 0, 0 },
        { LVCF_TEXT | LVCF_WIDTH, 0, 83,  (LPWSTR)L"Time left", 0, 0, 0, 0 },
        { LVCF_TEXT | LVCF_WIDTH, 0, 90,  (LPWSTR)L"Status", 0, 0, 0, 0 }
    };
    int i;
    for (i = 0; i < 6; i++) {
        defs[i].mask |= LVCF_FMT;
        defs[i].iSubItem = i;
        SendMessageW(lv, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&defs[i]);
    }
}

/* ------------------------------------------------------------------ */
/* main wndproc                                                        */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE h = ((LPCREATESTRUCTW)lp)->hInstance;

        g_rcCardList.left = 24; g_rcCardList.top = 210; g_rcCardList.right = 756; g_rcCardList.bottom = 548;
        g_rcChip.left = 170; g_rcChip.top = 120; g_rcChip.right = 610; g_rcChip.bottom = 146;
        g_rcStatus.left = 24; g_rcStatus.top = 556; g_rcStatus.right = 756; g_rcStatus.bottom = 584;
        g_rcFooter.left = 0; g_rcFooter.top = CLIENT_H - FOOTER_H;
        g_rcFooter.right = CLIENT_W; g_rcFooter.bottom = CLIENT_H;

        g_hFont    = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_hFontBtn = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_hMono    = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");
        g_hH1      = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");
        g_hSub     = CreateFontW(-10, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");
        g_hLbl     = CreateFontW(-9, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");
        g_hChip    = CreateFontW(-10, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");
        g_hFoot    = CreateFontW(-9, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");

        /* toolbar (custom class — self-painting) */
        g_hAdd      = CreateWindowW(BTN_CLASSW, L"Add URL",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        24, 158, 150, 40, hwnd, (HMENU)IDC_ADD, h, (LPVOID)(INT_PTR)BSTYLE_PRIMARY);
        g_hStart    = CreateWindowW(BTN_CLASSW, L"Start",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        182, 158, 84, 40, hwnd, (HMENU)IDC_START, h, (LPVOID)(INT_PTR)BSTYLE_GHOST);
        g_hStartAll = CreateWindowW(BTN_CLASSW, L"Start all",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        274, 158, 84, 40, hwnd, (HMENU)IDC_STARTALL, h, (LPVOID)(INT_PTR)BSTYLE_GHOST);
        g_hStop     = CreateWindowW(BTN_CLASSW, L"Stop",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        366, 158, 74, 40, hwnd, (HMENU)IDC_STOP, h, (LPVOID)(INT_PTR)BSTYLE_GHOST);
        g_hStopAll  = CreateWindowW(BTN_CLASSW, L"Stop all",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        448, 158, 84, 40, hwnd, (HMENU)IDC_STOPALL, h, (LPVOID)(INT_PTR)BSTYLE_GHOST);
        g_hDel      = CreateWindowW(BTN_CLASSW, L"Delete",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        540, 158, 84, 40, hwnd, (HMENU)IDC_DEL, h, (LPVOID)(INT_PTR)BSTYLE_GHOST);
        g_hOpt      = CreateWindowW(BTN_CLASSW, L"Options",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        632, 158, 84, 40, hwnd, (HMENU)IDC_OPT, h, (LPVOID)(INT_PTR)BSTYLE_GHOST);
        g_hTheme    = CreateWindowW(BTN_CLASSW, g_dark ? L"Light mode" : L"Dark mode",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        638, 14, 118, 30, hwnd, (HMENU)IDC_THEME, h, (LPVOID)(INT_PTR)BSTYLE_TOGGLE);
        {
            HWND btns[8]; int i;
            btns[0] = g_hAdd; btns[1] = g_hStart; btns[2] = g_hStartAll; btns[3] = g_hStop;
            btns[4] = g_hStopAll; btns[5] = g_hDel; btns[6] = g_hOpt; btns[7] = g_hTheme;
            for (i = 0; i < 8; i++) SendMessageW(btns[i], WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
        }

        /* download list (virtual) */
        g_hList = CreateWindowW(WC_LISTVIEWW, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                    LVS_SHOWSELALWAYS | WS_CLIPCHILDREN,
                    36, 224, 708, 312, hwnd, (HMENU)IDC_LIST, h, NULL);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        AddColumns(g_hList);
        SendMessageW(g_hList, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        EngineDefaultDir(g_defDir, MAX_PATH);
        SettingsGetStr(L"ClipExts", g_clipExts, 512,
            L"zip rar 7z exe msi msix iso img apk mp4 mkv avi mov mp3 flac wav ogg pdf epub "
            L"doc docx xls xlsx ppt pptx dmg pkg deb rpm tar gz tgz bz2 xz");

        SetTimer(hwnd, 1, 500, NULL);   /* engine tick: speeds + queue + status */
        SetTimer(hwnd, 2, 45,  NULL);   /* brand animation                      */

        TrayAdd();
        AddClipboardFormatListener(hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            EngineTick();
            RefreshList(1);
            InvalidateRect(hwnd, &g_rcChip, FALSE);
            InvalidateRect(hwnd, &g_rcStatus, FALSE);
        } else if (wp == 2) {
            g_tick++;
            g_phase += 0.045;
            if (!IsIconic(hwnd))
                InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_APP_BRIDGE: {
        BridgeAdd *ba = (BridgeAdd *)lp;
        if (ba) {
            wchar_t dir[MAX_PATH], nm[160];
            EngineDefaultDir(dir, MAX_PATH);
            nm[0] = 0;
            if (ba->name[0]) {
                lstrcpynW(nm, ba->name, 160);
                SanitizeFilenameW(nm, 160);
            }
            EngineAdd(ba->url, dir, nm, TRUE);
            EngineTick();
            RefreshList(1);
            InvalidateRect(hwnd, &g_rcChip, FALSE);
            InvalidateRect(hwnd, &g_rcStatus, FALSE);
            HeapFree(GetProcessHeap(), 0, ba);
        }
        return 0;
    }
    case WM_APP_JOB: {
        Job *j = EngineById((int)wp);
        RefreshList(1);
        InvalidateRect(hwnd, &g_rcChip, FALSE);
        InvalidateRect(hwnd, &g_rcStatus, FALSE);
        if (j && lp == 1) {
            if (j->state == ST_DONE) {
                wchar_t msg2[300];
                _snwprintf(msg2, 300, L"Download complete: %s", JobDisplayName(j));
                TrayBalloon(L"GrabStorm", msg2, NIIF_INFO);
            } else if (j->state == ST_ERROR) {
                wchar_t msg2[300];
                _snwprintf(msg2, 300, L"%s — %s", JobDisplayName(j),
                           j->err[0] ? j->err : L"failed");
                TrayBalloon(L"GrabStorm", msg2, NIIF_ERROR);
            }
        }
        return 0;
    }
    case WM_APP_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            POINT pt;
            HMENU menu = CreatePopupMenu();
            GetCursorPos(&pt);
            AppendMenuW(menu, MF_STRING, MNU_SHOW, L"Show GrabStorm");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, MNU_STARTALL, L"Start all");
            AppendMenuW(menu, MF_STRING, MNU_STOPALL, L"Stop all");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, MNU_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(menu);
        } else if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP) {
            ShowMain();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_THEME:  ToggleTheme(hwnd); return 0;
        case IDC_ADD:    OpenAddDlg(NULL);  return 0;
        case IDC_START: {
            Job *j = SelJob();
            if (j) EngineResume(j->id);
            return 0;
        }
        case IDC_STARTALL: {
            Job *j;
            for (j = EngineFirst(); j; j = j->next)
                if (j->state == ST_PAUSED || j->state == ST_ERROR || j->state == ST_QUEUED)
                    EngineResume(j->id);
            return 0;
        }
        case IDC_STOP: {
            Job *j = SelJob();
            if (j) EngineStop(j->id);
            return 0;
        }
        case IDC_STOPALL: {
            Job *j;
            for (j = EngineFirst(); j; j = j->next)
                if (j->state == ST_DOWNLOADING || j->state == ST_CONNECTING)
                    EngineStop(j->id);
            return 0;
        }
        case IDC_DEL: {
            Job *j = SelJob();
            if (j) {
                int r = MessageBoxW(hwnd,
                    L"Delete the downloaded file too?\n\nYes = delete file + list entry\nNo = remove from list only\nCancel = abort",
                    APP_TITLEW, MB_YESNOCANCEL | MB_ICONQUESTION);
                if (r != IDCANCEL) {
                    int id = j->id;
                    EngineRemove(id, r == IDYES);
                    RefreshList(1);
                    InvalidateRect(hwnd, &g_rcStatus, FALSE);
                }
            }
            return 0;
        }
        case IDC_OPT:    OpenOptsDlg(); return 0;
        case MNU_SHOW:     ShowMain(); return 0;
        case MNU_STARTALL: PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_STARTALL, 0), 0); return 0;
        case MNU_STOPALL:  PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_STOPALL, 0), 0); return 0;
        case MNU_EXIT:     DestroyWindow(hwnd); return 0;
        case MNU_OPENFILE:  OpenJobFile(SelJob(), 0); return 0;
        case MNU_OPENFOLD:  OpenJobFile(SelJob(), 1); return 0;
        case MNU_PAUSE:
        case MNU_STOP: {
            Job *j = SelJob();
            if (j) EngineStop(j->id);
            return 0;
        }
        case MNU_RESUME:
        case MNU_RETRY: {
            Job *j = SelJob();
            if (j) EngineResume(j->id);
            return 0;
        }
        case MNU_DELETE: PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_DEL, 0), 0); return 0;
        case MNU_COPYURL: CopyJobUrl(SelJob()); return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->idFrom != IDC_LIST) break;
        if (nm->code == LVN_GETDISPINFO) {
            NMLVDISPINFOW *di = (NMLVDISPINFOW *)lp;
            Job *j = EngineAt((int)di->item.iItem);
            if (j && (di->item.mask & LVIF_TEXT)) {
                wchar_t tmp[128];
                tmp[0] = 0;
                switch (di->item.iSubItem) {
                case 0: lstrcpynW(tmp, JobDisplayName(j), 128); break;
                case 1:
                    if (j->total) FormatSizeBytes(j->total, tmp, 128);
                    else lstrcpynW(tmp, L"--", 128);
                    break;
                case 2: lstrcpynW(tmp, L"", 128); break;   /* custom-drawn */
                case 3:
                    if (j->state == ST_DOWNLOADING && j->speed > 1) FormatSpeedBits(j->speed, tmp, 128);
                    else lstrcpynW(tmp, L"--", 128);
                    break;
                case 4:
                    if (j->state == ST_DOWNLOADING && j->total && j->speed > 8)
                        FormatEta((double)((unsigned __int64)j->got >= j->total ? 0 :
                                 (double)(j->total - (unsigned __int64)j->got)) / j->speed, tmp, 128);
                    else lstrcpynW(tmp, L"--", 128);
                    break;
                case 5: {
                    const wchar_t *st = JobStatusText(j);
                    if (j->state == ST_ERROR && j->err[0])
                        _snwprintf(tmp, 128, L"Error — %s", j->err);
                    else lstrcpynW(tmp, st, 128);
                    break;
                }
                }
                lstrcpynW(di->item.pszText, tmp, di->item.cchTextMax);
            }
            return 0;
        }
        if (nm->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *ncd = (NMLVCUSTOMDRAW *)lp;
            if (ncd->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (ncd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                ncd->clrText = g_pal.txt;
                ncd->clrTextBk = g_pal.card;
                if (ncd->nmcd.uItemState & CDIS_SELECTED)
                    ncd->clrTextBk = Shade(g_pal.cyan, g_dark ? 0.16 : 0.82);
                return CDRF_NOTIFYSUBITEMDRAW;
            }
            if ((ncd->nmcd.dwDrawStage & CDDS_ITEMPREPAINT) &&
                (ncd->nmcd.dwDrawStage & CDDS_SUBITEM)) {
                if (ncd->iSubItem == 2) {
                    PaintListRow(ncd);
                    return CDRF_SKIPDEFAULT;
                }
                ncd->clrText = g_pal.txt;
                ncd->clrTextBk = g_pal.card;
                if (ncd->nmcd.uItemState & CDIS_SELECTED)
                    ncd->clrTextBk = Shade(g_pal.cyan, g_dark ? 0.16 : 0.82);
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }
        if (nm->code == NM_RCLICK) {
            Job *j = SelJob();
            if (j) {
                POINT pt;
                HMENU menu = CreatePopupMenu();
                GetCursorPos(&pt);
                AppendMenuW(menu, MF_STRING, MNU_OPENFILE, L"Open file");
                AppendMenuW(menu, MF_STRING, MNU_OPENFOLD, L"Open folder");
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, MF_STRING, MNU_RESUME, L"Resume");
                AppendMenuW(menu, MF_STRING, MNU_PAUSE, L"Pause");
                AppendMenuW(menu, MF_STRING, MNU_RETRY, L"Restart download");
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, MF_STRING, MNU_COPYURL, L"Copy URL");
                AppendMenuW(menu, MF_STRING, MNU_DELETE, L"Delete...");
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(menu);
            }
            return 0;
        }
        if (nm->code == NM_DBLCLK) {
            OpenJobFile(SelJob(), 0);
            return 0;
        }
        break;
    }
    case WM_CLIPBOARDUPDATE:
        OnClipboardUpdate();
        return 0;
    case WM_COPYDATA: {
        COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lp;
        if (cds && cds->dwData == 1 && cds->lpData) {
            wchar_t u[2048], out[2048];
            lstrcpynW(u, (const wchar_t *)cds->lpData, 2048);
            if (ParseProtoUrl(u, out, 2048)) {
                ShowMain();
                OpenAddDlg(out);
            }
        }
        return TRUE;
    }
    case WM_PAINT:
        OnPaint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);   /* keep downloading in the tray */
        return 0;
    case WM_DESTROY: {
        Job *j;
        RemoveClipboardFormatListener(hwnd);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        BridgeStop();
        EngineShutdown();
        for (j = EngineFirst(); j; j = j->next) {
            if (j->hThread) { WaitForSingleObject(j->hThread, 100); }
        }
        if (g_hFont)    DeleteObject(g_hFont);
        if (g_hFontBtn) DeleteObject(g_hFontBtn);
        if (g_hMono)    DeleteObject(g_hMono);
        if (g_hH1)      DeleteObject(g_hH1);
        if (g_hSub)     DeleteObject(g_hSub);
        if (g_hLbl)     DeleteObject(g_hLbl);
        if (g_hChip)    DeleteObject(g_hChip);
        if (g_hFoot)    DeleteObject(g_hFoot);
        if (g_brBg)     DeleteObject(g_brBg);
        if (g_brField)  DeleteObject(g_brField);
        if (g_imgLogo)   { p_GdipDisposeImage(g_imgLogo);   g_imgLogo = NULL; }
        if (g_imgSpiral) { p_GdipDisposeImage(g_imgSpiral); g_imgSpiral = NULL; }
        if (g_imgOrb)    { p_GdipDisposeImage(g_imgOrb);    g_imgOrb = NULL; }
        if (g_attrDark) { p_GdipDisposeImageAttributes(g_attrDark); g_attrDark = NULL; }
        if (g_attrLite) { p_GdipDisposeImageAttributes(g_attrLite); g_attrLite = NULL; }
        {
            int i;
            for (i = 0; i < 3; i++) {
                if (g_streams[i]) { g_streams[i]->lpVtbl->Release(g_streams[i]); g_streams[i] = NULL; }
            }
        }
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */
static wchar_t *g_pendingProto = NULL;   /* heap — freed after use */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)
{
    WNDCLASSW wc, bc, ac, oc;
    MSG msg;
    RECT wr;
    GpStartupInput gsin;
    LPWSTR *arg;
    int na = 0;
    HANDLE mtx;

    (void)hPrev; (void)cmdLine;
    g_hInst = hInst;

    arg = CommandLineToArgvW(GetCommandLineW(), &na);
    if (arg && na > 1 && arg[1] && _wcsnicmp(arg[1], L"grabstorm://", 12) == 0) {
        g_pendingProto = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                        (wcslen(arg[1]) + 1) * sizeof(WCHAR));
        if (g_pendingProto) lstrcpyW(g_pendingProto, arg[1]);
    }
    if (arg) LocalFree(arg);

    /* single instance: forward a grabstorm:// link to the running copy */
    mtx = CreateMutexW(NULL, TRUE, L"3SVerse_GrabStorm_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_pendingProto) {
            HWND o = FindWindowW(APP_CLASSW, NULL);
            if (o) {
                COPYDATASTRUCT cds;
                cds.dwData = 1;
                cds.cbData = (DWORD)((lstrlenW(g_pendingProto) + 1) * sizeof(WCHAR));
                cds.lpData = (PVOID)g_pendingProto;
                SendMessageW(o, WM_COPYDATA, (WPARAM)0, (LPARAM)&cds);
                ShowWindow(o, SW_RESTORE);
                SetForegroundWindow(o);
            }
        }
        if (g_pendingProto) HeapFree(GetProcessHeap(), 0, g_pendingProto);
        CloseHandle(mtx);
        return 0;
    }

    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&icc);
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    ZeroMemory(&gsin, sizeof(gsin));
    gsin.GdiplusVersion = 1;
    if (LoadGdiPlus() && p_GdiplusStartup(&g_gdipToken, &gsin, NULL) == 0) {
        g_imgLogo   = ImageFromResource(RES_LOGO,   &g_streams[0]);
        g_imgSpiral = ImageFromResource(RES_SPIRAL, &g_streams[1]);
        g_imgOrb    = ImageFromResource(RES_ORB,    &g_streams[2]);
        g_attrDark  = MakeAlphaAttr(0.90);
        g_attrLite  = MakeAlphaAttr(0.25);
    }

    LoadTheme();
    ApplyPalette();
    RebuildBrushes();

    ZeroMemory(&bc, sizeof(bc));
    bc.lpfnWndProc = BtnProc;
    bc.hInstance = hInst;
    bc.hCursor = LoadCursor(NULL, IDC_HAND);
    bc.hbrBackground = NULL;
    bc.lpszClassName = BTN_CLASSW;
    RegisterClassW(&bc);

    ZeroMemory(&ac, sizeof(ac));
    ac.lpfnWndProc = AddDlgProc;
    ac.hInstance = hInst;
    ac.hCursor = LoadCursor(NULL, IDC_ARROW);
    ac.hbrBackground = NULL;
    ac.lpszClassName = ADD_CLASSW;
    RegisterClassW(&ac);

    ZeroMemory(&oc, sizeof(oc));
    oc.lpfnWndProc = OptDlgProc;
    oc.hInstance = hInst;
    oc.hCursor = LoadCursor(NULL, IDC_ARROW);
    oc.hbrBackground = NULL;
    oc.lpszClassName = OPT_CLASSW;
    RegisterClassW(&oc);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    g_hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIcon = g_hIcon ? g_hIcon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hbrBackground = NULL;
    wc.lpszClassName = APP_CLASSW;
    RegisterClassW(&wc);

    wr.left = 0; wr.top = 0; wr.right = CLIENT_W; wr.bottom = CLIENT_H;
    AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    g_hMain = CreateWindowW(APP_CLASSW, APP_TITLEW,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, hInst, NULL);
    if (!g_hMain) return 1;

    EngineInit(g_hMain);
    BridgeStart(g_hMain);
    ApplyDwm(g_hMain);
    ShowWindow(g_hMain, nShow);
    UpdateWindow(g_hMain);

    if (g_pendingProto) {
        wchar_t out[2048];
        {
            wchar_t *keep = g_pendingProto;
            g_pendingProto = NULL;
            if (ParseProtoUrl(keep, out, 2048)) OpenAddDlg(out);
            HeapFree(GetProcessHeap(), 0, keep);
        }
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(g_hMain, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_gdipToken && p_GdiplusShutdown) p_GdiplusShutdown(g_gdipToken);
    CoUninitialize();
    if (mtx) CloseHandle(mtx);
    return (int)msg.wParam;
}



