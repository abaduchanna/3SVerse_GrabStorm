/* TurboGrab v1.3.0 — free & open download accelerator for Windows
 * Multi-segment HTTP(S) downloads with pause/resume, queue, clipboard
 * monitoring, category folders and browser extension integration.
 * Modern dark UI (custom-drawn), in-app extension auto-registration.
 * Clean-room C (Win32 + WinHTTP + Winsock).
 *
 * MIT License — Copyright (c) 2026 Abad Umair Channa / 3S Verse
 * https://github.com/abaduchanna/3SVerse_TurboGrab
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <windowsx.h>
#include <winhttp.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "localsrv.h"
#include "browext.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#define APP_TITLE   "TurboGrab v1.3.0"
#define APP_CLASS   "TurboGrabMainWnd"
#define ADDPOP_CLASS "TurboGrabAddWnd"
#define SETPOP_CLASS "TurboGrabSetWnd"

#define WM_APP_JOB  (WM_APP + 1)
#define WM_APP_TRAY (WM_APP + 2)

/* main window control ids */
#define IDC_LIST    201
#define IDC_ADD     202
#define IDC_RESUME  203
#define IDC_PAUSE   204
#define IDC_DELETE  205
#define IDC_FOLDER  206
#define IDC_SET     207
#define IDC_BROWSERS 208

/* add-url popup ids */
#define IDC_AURL    301
#define IDC_ADIR    302
#define IDC_ABROWSE 303
#define IDC_ASTART  304
#define IDC_ACANCEL 305

/* settings popup ids */
#define IDC_SSEG    401
#define IDC_SCON    402
#define IDC_SDIR    403
#define IDC_SBROWSE 404
#define IDC_SCLIP   405
#define IDC_SCAT    406
#define IDC_SSAVE   407
#define IDC_SCLOSE  408
#define IDC_SBROWSER 409
#define IDC_SPORT    410
#define IDC_STOKEN   411
#define IDC_SCOPYTOK 412

/* browser-extension window ids */
#define BRW_CLASS   "TurboGrabBrwWnd"
#define IDC_BALL    451
#define IDC_BCLOSE  452
#define IDC_BREG(i) (455 + (i))    /* register button per browser row   */
#define IDC_BNAME(i) (465 + (i))   /* browser name static               */
#define IDC_BSTAT(i) (475 + (i))   /* browser status static             */
#define IDC_BNOROWS 485            /* "no browsers found" note          */

/* tray / context menu ids */
#define IDM_SHOW     501
#define IDM_EXIT     502
#define IDM_RESUME   601
#define IDM_PAUSE    602
#define IDM_DEL      603
#define IDM_OPEN     604
#define IDM_COPYURL  605

/* job states */
#define ST_QUEUED   0
#define ST_CONNECT  1
#define ST_DOWN     2
#define ST_PAUSED   3
#define ST_DONE     4
#define ST_ERROR    5

#define MAX_SEGMENTS 16

typedef struct {
    DWORD64 start, end;      /* inclusive byte range */
    volatile LONG64 done;    /* bytes completed in this segment */
    int retries;
} Segment;

typedef struct Job {
    char   url[2048];
    char   dir[MAX_PATH];            /* final directory (category applied) */
    char   name[300];                /* file name, UTF-8 */
    WCHAR  wdir[MAX_PATH];
    WCHAR  wname[300];
    WCHAR  wfinal[MAX_PATH + 320];
    WCHAR  wpart[MAX_PATH + 320];
    WCHAR  host[256];
    WCHAR  path[2048];
    int    https;
    WORD   port;
    DWORD64 total;                   /* 0 = unknown size */
    int    useRanges;
    int    nseg;
    Segment *seg;
    volatile LONG   pauseFlag;
    volatile LONG   failFlag;
    int    deletePending;
    volatile LONG64 got;
    /* speed sampling (UI thread only) */
    DWORD64 lastGot;
    DWORD   lastTick;
    DWORD   speedBps;
    int     state;
    HANDLE  hThread;
    char    err[160];
    struct Job *next;
} Job;

typedef struct {
    int  segments;      /* 1..16 */
    int  concurrent;    /* 1..10 */
    char dir[MAX_PATH];
    int  clipboard;
    int  categories;
} Settings;

static Settings g_set = { 8, 3, "", 1, 1 };

static HWND  g_hMain, g_hList;
static HWND  g_hAddWnd, g_hSetWnd, g_hBrowsWnd;
static HFONT g_hFont, g_hFontB;
static HICON g_hIcon;
static HBRUSH g_hBrBg, g_hBrEdit;

static Job  *g_jobs = NULL;
static int   g_count = 0;
static CRITICAL_SECTION g_cs;

static NOTIFYICONDATAA g_nid;
static BOOL  g_trayOn = FALSE;
static char  g_iniPath[MAX_PATH];
static DWORD g_lastClipTick = 0;
static char  g_clipLast[2048];

/* browser extension integration */
static int  g_browserOn   = 1;
static int  g_browserPort = 8777;
static char g_browserToken[64] = "";

/* status strip (painted by the main window) */
static int  g_statActive, g_statQueued, g_statDone;
static char g_statSpeed[48] = "";

/* ---- forward declarations ---- */
static void KickQueue(void);
static void UpdateRow(Job *j);
static void RefreshAll(void);
static void UpdateStatusLine(void);
static void RebuildList(void);
static void SaveJobs(void);
static Job *JobCreate(const char *url, const char *dir, const char *nameGiven);

/* ============================= helpers ============================= */

static void FormatSize(DWORD64 v, char *out, int cap)
{
    if (v >= 1099511627776ULL)      _snprintf(out, cap, "%.2f TB", (double)(v / 1099511627776.0));
    else if (v >= 1073741824ULL)    _snprintf(out, cap, "%.2f GB", (double)(v / 1073741824.0));
    else if (v >= 1048576ULL)       _snprintf(out, cap, "%.1f MB", (double)(v / 1048576.0));
    else if (v >= 1024ULL)          _snprintf(out, cap, "%.1f KB", (double)(v / 1024.0));
    else                            _snprintf(out, cap, "%u B", (unsigned)v);
    out[cap - 1] = 0;
}

static DWORD64 ParseU64A(const char *s)
{
    DWORD64 v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (DWORD64)(*s - '0'); s++; }
    return v;
}

static DWORD64 ParseU64W(const WCHAR *s)
{
    DWORD64 v = 0;
    while (*s >= L'0' && *s <= L'9') { v = v * 10 + (DWORD64)(*s - L'0'); s++; }
    return v;
}

/* UTF-8 (name) -> display ANSI via wide round-trip */
static void DisplayOf(const char *utf8, char *out, int cap)
{
    WCHAR w[600];
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 600) <= 0) {
        lstrcpynA(out, utf8, cap);
        return;
    }
    if (WideCharToMultiByte(CP_ACP, 0, w, -1, out, cap, NULL, NULL) <= 0)
        lstrcpynA(out, utf8, cap);
    out[cap - 1] = 0;
}

static void SanitizeName(char *name)
{
    char *p;
    size_t L;
    for (p = name; *p; p++) {
        if ((unsigned char)*p < 32) *p = '_';
        if (strchr("\\/:*?\"<>|", *p)) *p = '_';
    }
    L = strlen(name);
    while (L && (name[L - 1] == ' ' || name[L - 1] == '.')) name[--L] = 0;
    if (!name[0]) lstrcpynA(name, "download", 300);
}

/* extract file name from URL path (percent-decoded UTF-8) */
static void UrlFileName(const char *url, char *out, int cap)
{
    const char *p = strstr(url, "://");
    const char *scan = p ? p + 3 : url;
    const char *last = scan;
    const char *s;
    char path[600], dec[600];
    int i, j;

    for (s = scan; *s; s++)
        if (*s == '/') last = s;
    if (*last == '/') last++;
    for (i = 0; *last && *last != '?' && *last != '#' && i < 580; i++, last++)
        path[i] = *last;
    path[i] = 0;

    for (i = 0, j = 0; path[i] && j < 580; i++) {
        if (path[i] == '%' && isxdigit((unsigned char)path[i + 1]) &&
            isxdigit((unsigned char)path[i + 2])) {
            char hex[3];
            hex[0] = path[i + 1]; hex[1] = path[i + 2]; hex[2] = 0;
            dec[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dec[j++] = path[i];
        }
    }
    dec[j] = 0;
    if (!dec[0]) lstrcpynA(dec, "download", sizeof(dec));
    lstrcpynA(out, dec, cap);
}

static int ExtEq(const char *ext, const char *lit)
{
    while (*ext && *lit) {
        if (tolower((unsigned char)*ext) != *lit) return 0;
        ext++; lit++;
    }
    return *ext == 0 && *lit == 0;
}

static const char *CategoryFor(const char *name)
{
    static const char *vid[] = { "mp4","mkv","avi","mov","webm","flv","wmv","m4v","mpg","mpeg","ts","3gp",NULL };
    static const char *mus[] = { "mp3","flac","wav","m4a","aac","ogg","opus","wma","mid",NULL };
    static const char *doc[] = { "pdf","doc","docx","xls","xlsx","ppt","pptx","txt","rtf","epub","csv",NULL };
    static const char *prg[] = { "exe","msi","msix","apk","bat","cmd","ps1",NULL };
    static const char *cmp[] = { "zip","rar","7z","gz","bz2","xz","tar","iso",NULL };
    static const char *img[] = { "png","jpg","jpeg","gif","webp","bmp","svg","ico",NULL };
    const char **grp[5] = { vid, mus, doc, prg, cmp };
    static const char *catName[5] = { "Videos", "Music", "Documents", "Programs", "Compressed" };
    const char *ext = strrchr(name, '.');
    int g, k;

    if (!ext || !ext[1]) return NULL;
    ext++;
    for (g = 0; g < 5; g++)
        for (k = 0; grp[g][k]; k++)
            if (ExtEq(ext, grp[g][k])) return catName[g];
    if (img[0]) {
        for (k = 0; img[k]; k++)
            if (ExtEq(ext, img[k])) return "Images";
    }
    return NULL;
}

static void ApplyCategory(const char *dir, const char *name, char *out, int cap)
{
    const char *cat = g_set.categories ? CategoryFor(name) : NULL;
    if (!cat) { lstrcpynA(out, dir, cap); return; }
    _snprintf(out, cap, "%s\\%s", dir, cat);
    out[cap - 1] = 0;
    CreateDirectoryA(out, NULL);
}

/* rebuild name-dependent wide paths */
static void SetJobName(Job *j, const char *nameUtf8)
{
    lstrcpynA(j->name, nameUtf8, sizeof(j->name));
    MultiByteToWideChar(CP_UTF8, 0, j->name, -1, j->wname, 300);
    MultiByteToWideChar(CP_ACP, 0, j->dir, -1, j->wdir, MAX_PATH);
    _snwprintf(j->wfinal, MAX_PATH + 320, L"%s\\%s", j->wdir, j->wname);
    _snwprintf(j->wpart,  MAX_PATH + 320, L"%s\\%s.part", j->wdir, j->wname);
    j->wfinal[MAX_PATH + 319] = 0;
    j->wpart[MAX_PATH + 319] = 0;
}

/* ============================= settings / ini ============================= */

static void BuildIniPath(void)
{
    char appdata[MAX_PATH];
    if (!GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH))
        GetWindowsDirectoryA(appdata, MAX_PATH);
    CreateDirectoryA(appdata, NULL);
    lstrcpynA(g_iniPath, appdata, MAX_PATH);
    lstrcatA(g_iniPath, "\\TurboGrab");
    CreateDirectoryA(g_iniPath, NULL);
    lstrcatA(g_iniPath, "\\turbograb.ini");
}

static void LoadSettings(void)
{
    g_set.segments   = GetPrivateProfileIntA("Settings", "segments", 8, g_iniPath);
    g_set.concurrent = GetPrivateProfileIntA("Settings", "concurrent", 3, g_iniPath);
    g_set.clipboard  = GetPrivateProfileIntA("Settings", "clipboard", 1, g_iniPath);
    g_set.categories = GetPrivateProfileIntA("Settings", "categories", 1, g_iniPath);
    GetPrivateProfileStringA("Settings", "dir", "", g_set.dir, MAX_PATH, g_iniPath);
    g_browserOn   = GetPrivateProfileIntA("Browser", "enabled", 1, g_iniPath);
    g_browserPort = GetPrivateProfileIntA("Browser", "port", 8777, g_iniPath);
    GetPrivateProfileStringA("Browser", "token", "", g_browserToken,
                             sizeof(g_browserToken), g_iniPath);
    if (g_browserPort < 1024 || g_browserPort > 65535) g_browserPort = 8777;
    if (g_set.segments   < 1)  g_set.segments = 1;
    if (g_set.segments   > MAX_SEGMENTS) g_set.segments = MAX_SEGMENTS;
    if (g_set.concurrent < 1)  g_set.concurrent = 1;
    if (g_set.concurrent > 10) g_set.concurrent = 10;
    if (!g_set.dir[0]) {
        char home[MAX_PATH];
        if (GetEnvironmentVariableA("USERPROFILE", home, MAX_PATH)) {
            lstrcpynA(g_set.dir, home, MAX_PATH);
            lstrcatA(g_set.dir, "\\Downloads");
        } else {
            lstrcpynA(g_set.dir, "C:", MAX_PATH);
        }
    }
    CreateDirectoryA(g_set.dir, NULL);
}

static void SaveSettings(void)
{
    char b[16];
    sprintf(b, "%d", g_set.segments);
    WritePrivateProfileStringA("Settings", "segments", b, g_iniPath);
    sprintf(b, "%d", g_set.concurrent);
    WritePrivateProfileStringA("Settings", "concurrent", b, g_iniPath);
    sprintf(b, "%d", g_set.clipboard);
    WritePrivateProfileStringA("Settings", "clipboard", b, g_iniPath);
    sprintf(b, "%d", g_set.categories);
    WritePrivateProfileStringA("Settings", "categories", b, g_iniPath);
    WritePrivateProfileStringA("Settings", "dir", g_set.dir, g_iniPath);
}

/* ------------------------- browser integration --------------------- */

/* generate the extension token on first run and store it in the INI */
static void BrowserTokenEnsure(void)
{
    static const char *hx = "0123456789abcdef";
    unsigned char raw[16];
    int i;
    HCRYPTPROV h = 0;

    if (g_browserToken[0]) return;
    ZeroMemory(raw, sizeof(raw));
    if (CryptAcquireContextA(&h, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(h, sizeof(raw), raw);
        CryptReleaseContext(h, 0);
    } else {
        DWORD t = GetTickCount();
        for (i = 0; i < 16; i++)
            raw[i] = (unsigned char)(((t * (i + 7919)) >> ((i % 7) + 1)) & 0xFF);
    }
    for (i = 0; i < 16; i++) {
        g_browserToken[i * 2]     = hx[raw[i] >> 4];
        g_browserToken[i * 2 + 1] = hx[raw[i] & 15];
    }
    g_browserToken[32] = 0;
    WritePrivateProfileStringA("Browser", "token", g_browserToken, g_iniPath);
}

/* (re)start the localhost integration server with current settings */
static void BrowserSrvApply(void)
{
    BrowserSrvCfg c;
    if (!g_hMain) return;
    ZeroMemory(&c, sizeof(c));
    c.hwnd    = g_hMain;
    c.enabled = g_browserOn;
    c.port    = g_browserPort;
    c.token   = g_browserToken;
    BrowserSrvStart(&c);
}

/* ============================= dark skin engine ============================= */

#define TG_BG        RGB(24, 26, 32)      /* window background            */
#define TG_ROW1      RGB(27, 29, 36)      /* list row even                */
#define TG_ROW2      RGB(32, 35, 43)      /* list row odd                 */
#define TG_ROWSEL    RGB(40, 56, 90)      /* selected row                 */
#define TG_BORDER    RGB(50, 55, 67)      /* borders / separators         */
#define TG_HDR       RGB(24, 26, 32)      /* list header bg               */
#define TG_TXT       RGB(232, 235, 242)   /* primary text                 */
#define TG_TXT_DIM   RGB(152, 158, 172)   /* secondary text               */
#define TG_TXT_MUTE  RGB(106, 112, 126)   /* muted text                   */
#define TG_ACCENT    RGB(74, 138, 246)    /* accent blue                  */
#define TG_GREEN     RGB(52, 199, 123)    /* completed / progress         */
#define TG_ORANGE    RGB(242, 166, 62)    /* paused / queued              */
#define TG_RED       RGB(236, 91, 91)     /* error                        */
#define TG_BTN       RGB(38, 42, 51)      /* button face                  */
#define TG_BTN_HOV   RGB(50, 56, 69)      /* button hover                 */
#define TG_BTN_DN    RGB(28, 32, 40)      /* button pressed               */
#define TG_BTN_PRIM  RGB(38, 62, 110)     /* primary button face          */
#define TG_EDITBG    RGB(20, 22, 28)      /* edit box background          */

static void FillSolid(HDC hdc, RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, r, b);
    DeleteObject(b);
}

static void FillRoundRect(HDC hdc, RECT *r, int rad, COLORREF c, COLORREF border)
{
    HBRUSH b = CreateSolidBrush(c);
    HPEN   p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(hdc, b), op = SelectObject(hdc, p);
    RoundRect(hdc, r->left, r->top, r->right + 1, r->bottom + 1, rad, rad);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(b);
    DeleteObject(p);
}

/* dark title bar + rounded corners (Win10 19041+ / Win11; older: no-op) */
static void DarkTitle(HWND hw)
{
    typedef HRESULT (WINAPI *DwmSetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE m = LoadLibraryA("dwmapi.dll");
    DwmSetAttrFn f;
    BOOL on = TRUE;
    DWORD pref = 2;   /* DWMWCP_ROUND */

    if (!m) return;
    f = (DwmSetAttrFn)GetProcAddress(m, "DwmSetWindowAttribute");
    if (f) {
        f(hw, 20, &on, sizeof(on));      /* DWMWA_USE_IMMERSIVE_DARK_MODE */
        f(hw, 33, &pref, sizeof(pref));  /* DWMWA_WINDOW_CORNER_PREFERENCE */
    }
    FreeLibrary(m);
}

static HBRUSH SkinBgBrush(void)
{
    if (!g_hBrBg) g_hBrBg = CreateSolidBrush(TG_BG);
    return g_hBrBg;
}

static HBRUSH SkinEditBrush(void)
{
    if (!g_hBrEdit) g_hBrEdit = CreateSolidBrush(TG_EDITBG);
    return g_hBrEdit;
}

/* --------------- skinned owner-draw buttons --------------- */

static int GlyphForId(int id)
{
    switch (id) {
    case IDC_ADD:      return 0;   /* plus     */
    case IDC_RESUME:   return 1;   /* play     */
    case IDC_ASTART:   return 1;   /* play     */
    case IDC_PAUSE:    return 2;   /* pause    */
    case IDC_DELETE:   return 3;   /* x        */
    case IDC_FOLDER:   return 4;   /* folder   */
    case IDC_BROWSERS: return 5;   /* globe    */
    case IDC_SET:      return 6;   /* sliders  */
    case IDC_BALL:     return 0;   /* plus     */
    }
    return -1;                     /* text only */
}

static int IsPrimaryBtn(int id)
{
    return id == IDC_ASTART || id == IDC_SSAVE || id == IDC_BALL;
}

static void DrawGlyph(HDC hdc, RECT *box, int id, COLORREF c)
{
    HPEN   pen = CreatePen(PS_SOLID, 2, c);
    HBRUSH br  = CreateSolidBrush(c);
    HGDIOBJ op = SelectObject(hdc, pen);
    HGDIOBJ ob = SelectObject(hdc, br);
    int cx = (box->left + box->right) / 2;
    int cy = (box->top + box->bottom) / 2;

    switch (id) {
    case 0:                                   /* plus */
        MoveToEx(hdc, cx - 6, cy, NULL); LineTo(hdc, cx + 6, cy);
        MoveToEx(hdc, cx, cy - 6, NULL); LineTo(hdc, cx, cy + 6);
        break;
    case 1: {                                 /* play */
        POINT t[3] = { { cx - 4, cy - 7 }, { cx - 4, cy + 7 }, { cx + 8, cy } };
        Polygon(hdc, t, 3);
        break; }
    case 2:                                   /* pause */
        Rectangle(hdc, cx - 6, cy - 7, cx - 2, cy + 7);
        Rectangle(hdc, cx + 2, cy - 7, cx + 6, cy + 7);
        break;
    case 3:                                   /* x */
        MoveToEx(hdc, cx - 5, cy - 5, NULL); LineTo(hdc, cx + 5, cy + 5);
        MoveToEx(hdc, cx + 5, cy - 5, NULL); LineTo(hdc, cx - 5, cy + 5);
        break;
    case 4:                                   /* folder (outline) */
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        MoveToEx(hdc, cx - 8, cy + 5, NULL);
        LineTo(hdc, cx - 8, cy - 4); LineTo(hdc, cx - 3, cy - 4);
        LineTo(hdc, cx - 1, cy - 2); LineTo(hdc, cx + 8, cy - 2);
        LineTo(hdc, cx + 8, cy + 5); LineTo(hdc, cx - 8, cy + 5);
        break;
    case 5:                                   /* globe (outline) */
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Ellipse(hdc, cx - 7, cy - 7, cx + 7, cy + 7);
        MoveToEx(hdc, cx, cy - 7, NULL); LineTo(hdc, cx, cy + 7);
        MoveToEx(hdc, cx - 6, cy - 2, NULL); LineTo(hdc, cx + 6, cy - 2);
        MoveToEx(hdc, cx - 6, cy + 2, NULL); LineTo(hdc, cx + 6, cy + 2);
        break;
    case 6:                                   /* sliders */
        Rectangle(hdc, cx - 8, cy - 6, cx + 8, cy - 4);
        Rectangle(hdc, cx - 8, cy - 1, cx + 8, cy + 1);
        Rectangle(hdc, cx - 8, cy + 4, cx + 8, cy + 6);
        SelectObject(hdc, br);
        Ellipse(hdc, cx - 3, cy - 8, cx + 3, cy - 2);
        Ellipse(hdc, cx + 3, cy - 3, cx + 9, cy + 3);
        Ellipse(hdc, cx - 5, cy + 2, cx + 1, cy + 8);
        break;
    }
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(pen);
    DeleteObject(br);
}

static void SkinDrawBtn(const DRAWITEMSTRUCT *di)
{
    HDC    hdc = di->hDC;
    RECT   rc = di->rcItem;
    char   txt[96];
    int    glyph = GlyphForId((int)di->CtlID);
    int    hover = (int)GetWindowLongPtrA(di->hwndItem, GWLP_USERDATA);
    int    down  = (di->itemState & ODS_SELECTED) != 0;
    int    prim  = IsPrimaryBtn((int)di->CtlID);
    COLORREF face = down ? TG_BTN_DN : (hover ? TG_BTN_HOV : (prim ? TG_BTN_PRIM : TG_BTN));
    COLORREF edge = prim ? TG_ACCENT : TG_BORDER;
    SIZE   tsz;
    int    total, x0, cy;
    HGDIOBJ of;
    RECT   tr;

    GetWindowTextA(di->hwndItem, txt, sizeof(txt));
    of = SelectObject(hdc, g_hFont);
    GetTextExtentPoint32A(hdc, txt, (int)strlen(txt), &tsz);
    total = tsz.cx + ((glyph >= 0 && txt[0]) ? 22 : 0);
    x0 = rc.left + ((rc.right - rc.left) - total) / 2;
    cy = (rc.top + rc.bottom) / 2;

    FillRoundRect(hdc, &rc, 9, face, edge);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, (di->itemState & ODS_DISABLED) ? TG_TXT_MUTE : TG_TXT);

    if (glyph >= 0) {
        RECT gb = { x0, cy - 8, x0 + 16, cy + 8 };
        DrawGlyph(hdc, &gb, glyph, TG_TXT);
    }
    if (txt[0]) {
        tr = rc;
        tr.left  = x0 + (glyph >= 0 ? 22 : 0);
        tr.right = tr.left + tsz.cx + 2;
        DrawTextA(hdc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(hdc, of);
}

/* hover tracking for the skinned buttons — classic subclassing (no comctl32
 * v6 dependency; SetWindowSubclass would need a v6 manifest) */
static WNDPROC s_btnOld = NULL;

static LRESULT CALLBACK SkinBtnProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!GetWindowLongPtrA(hw, GWLP_USERDATA)) {
            SetWindowLongPtrA(hw, GWLP_USERDATA, 1);
            InvalidateRect(hw, NULL, FALSE);
            {
                TRACKMOUSEEVENT t;
                t.cbSize = sizeof(t);
                t.dwFlags = TME_LEAVE;
                t.hwndTrack = hw;
                TrackMouseEvent(&t);
            }
        }
        break;
    case WM_MOUSELEAVE:
        SetWindowLongPtrA(hw, GWLP_USERDATA, 0);
        InvalidateRect(hw, NULL, FALSE);
        break;
    }
    return CallWindowProcA(s_btnOld, hw, msg, wp, lp);
}

static HWND SkinBtn(HWND parent, HINSTANCE h, int id, const char *text,
                    int x, int y, int w, int hgt)
{
    HWND b = CreateWindowA("BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        x, y, w, hgt, parent, (HMENU)(INT_PTR)id, h, NULL);
    SendMessageA(b, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    if (!s_btnOld) s_btnOld = (WNDPROC)GetWindowLongPtrA(b, GWLP_WNDPROC);
    SetWindowLongPtrA(b, GWLP_WNDPROC, (LONG_PTR)SkinBtnProc);
    SetWindowLongPtrA(b, GWLP_USERDATA, 0);
    return b;
}

/* --------------- list view: empty-state text --------------- */

static WNDPROC s_listOld = NULL;

static LRESULT CALLBACK ListProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT res = CallWindowProcA(s_listOld, hw, msg, wp, lp);
    if (msg == WM_PAINT && ListView_GetItemCount(hw) == 0) {
        HDC  hdc = GetDC(hw);
        RECT rc;
        GetClientRect(hw, &rc);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, TG_TXT_MUTE);
        SelectObject(hdc, g_hFont);
        DrawTextA(hdc,
            "No downloads yet.\r\n\r\n"
            "Click \x93Add URL\x94 above, or just copy a download link in your\r\n"
            "browser \x97 TurboGrab catches it automatically.",
            -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        ReleaseDC(hw, hdc);
    }
    return res;
}

/* ============================= job list ============================= */

static Job *JobCreate(const char *url, const char *dir, const char *nameGiven)
{
    Job *j = (Job *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Job));
    char raw[600];
    if (!j) return NULL;

    lstrcpynA(j->url, url, sizeof(j->url));
    if (nameGiven && nameGiven[0]) {
        /* restore path from saved job — category already baked into dir */
        lstrcpynA(j->dir, dir, MAX_PATH);
        lstrcpynA(raw, nameGiven, sizeof(raw));
    } else {
        ApplyCategory(dir, url, j->dir, MAX_PATH);
        UrlFileName(j->url, raw, sizeof(raw));
    }
    SanitizeName(raw);
    SetJobName(j, raw);
    j->state = ST_QUEUED;

    EnterCriticalSection(&g_cs);
    j->next = g_jobs;
    g_jobs = j;
    g_count++;
    LeaveCriticalSection(&g_cs);
    return j;
}

static void JobUnlink(Job *j)
{
    Job **pp = &g_jobs;
    while (*pp && *pp != j) pp = &(*pp)->next;
    if (*pp) {
        *pp = j->next;
        g_count--;
    }
}

static void FreeJob(Job *j)
{
    if (j->seg) HeapFree(GetProcessHeap(), 0, j->seg);
    HeapFree(GetProcessHeap(), 0, j);
}

static int JobInList(Job *j)
{
    Job *p;
    for (p = g_jobs; p; p = p->next)
        if (p == j) return 1;
    return 0;
}

static int ActiveCount(void)
{
    int n = 0;
    Job *j;
    for (j = g_jobs; j; j = j->next)
        if (j->state == ST_CONNECT || j->state == ST_DOWN) n++;
    return n;
}

static BOOL IsUrlOk(const char *s)
{
    if (!s) return FALSE;
    if (strnicmp(s, "http://", 7) == 0) return TRUE;
    if (strnicmp(s, "https://", 8) == 0) return TRUE;
    return FALSE;
}

static void Trim(char *s)
{
    size_t L;
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    L = strlen(s);
    while (L && (s[L - 1] == ' ' || s[L - 1] == '\t' || s[L - 1] == '\r' || s[L - 1] == '\n'))
        s[--L] = 0;
}

/* ============================= download engine ============================= */

/* case-insensitive wide compare of n chars */
static int wnicmp(const WCHAR *a, const WCHAR *b, int n)
{
    while (n > 0 && *a && *b) {
        WCHAR ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca += 32;
        if (cb >= L'A' && cb <= L'Z') cb += 32;
        if (ca != cb) return (int)(ca - cb);
        a++; b++; n--;
    }
    if (n == 0) return 0;
    return (int)(*a - *b);
}

/* probe: GET with Range: bytes=0-0 -> learn size + range support + filename */
static int ProbeUrl(Job *j)
{
    WCHAR wurl[2048];
    URL_COMPONENTSW uc;
    HINTERNET hs, hc, hr;
    DWORD status = 0, sz = sizeof(status);
    WCHAR cd[512];

    MultiByteToWideChar(CP_ACP, 0, j->url, -1, wurl, 2048);

    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = j->host;   uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = j->path;   uc.dwUrlPathLength  = 2048;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        lstrcpynA(j->err, "Invalid URL", sizeof(j->err));
        return 1;
    }
    j->https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    j->port  = uc.nPort;

    hs = WinHttpOpen(L"TurboGrab/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { lstrcpynA(j->err, "WinHTTP init failed", sizeof(j->err)); return 1; }
    hc = WinHttpConnect(hs, j->host, j->port, 0);
    if (!hc) { WinHttpCloseHandle(hs); lstrcpynA(j->err, "Host connect failed", sizeof(j->err)); return 1; }
    hr = WinHttpOpenRequest(hc, L"GET", j->path, NULL, WINHTTP_NO_REFERER,
                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                            j->https ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) { WinHttpCloseHandle(hc); WinHttpCloseHandle(hs); lstrcpynA(j->err, "Request failed", sizeof(j->err)); return 1; }

    WinHttpAddRequestHeaders(hr, L"Range: bytes=0-0", (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hr, NULL)) {
        WinHttpCloseHandle(hr); WinHttpCloseHandle(hc); WinHttpCloseHandle(hs);
        lstrcpynA(j->err, "Server did not respond", sizeof(j->err));
        return 1;
    }
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

    if (status == 206) {
        WCHAR cr[160];
        j->useRanges = 1;
        sz = sizeof(cr);
        if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_CUSTOM, L"Content-Range",
                                cr, &sz, WINHTTP_NO_HEADER_INDEX)) {
            WCHAR *slash = wcsrchr(cr, L'/');
            if (slash) j->total = ParseU64W(slash + 1);
        }
    } else if (status == 200) {
        DWORD len = 0;
        sz = sizeof(len);
        j->useRanges = 0;
        if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &len, &sz, WINHTTP_NO_HEADER_INDEX))
            j->total = (DWORD64)len;
    } else {
        WCHAR msg[64];
        _snwprintf(msg, 64, L"HTTP status %u", status);
        WideCharToMultiByte(CP_UTF8, 0, msg, -1, j->err, sizeof(j->err), NULL, NULL);
        WinHttpCloseHandle(hr); WinHttpCloseHandle(hc); WinHttpCloseHandle(hs);
        return 1;
    }

    /* server-provided filename wins over URL name */
    sz = sizeof(cd);
    if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_CUSTOM, L"Content-Disposition",
                            cd, &sz, WINHTTP_NO_HEADER_INDEX)) {
        WCHAR *f = cd, *end;
        int n;
        while (*f) {
            if ((*f == L'f' || *f == L'F') && wnicmp(f, L"filename", 8) == 0) break;
            f++;
        }
        if (*f) {
            f += 8;
            while (*f == L' ' || *f == L'=') f++;
            if (*f == L'"') f++;
            end = f;
            while (*end && *end != L'"' && *end != L';') end++;
            n = (int)(end - f);
            if (n > 0 && n < 290) {
                char cand[600];
                WideCharToMultiByte(CP_UTF8, 0, f, n, cand, sizeof(cand), NULL, NULL);
                cand[n] = 0;
                SanitizeName(cand);
                if (cand[0]) SetJobName(j, cand);
            }
        }
    }

    /* drain first chunk so connection closes cleanly */
    {
        DWORD avail = 0;
        if (WinHttpQueryDataAvailable(hr, &avail) && avail > 0) {
            BYTE *b = (BYTE *)HeapAlloc(GetProcessHeap(), 0, avail);
            DWORD rd = 0;
            if (b) { WinHttpReadData(hr, b, avail, &rd); HeapFree(GetProcessHeap(), 0, b); }
        }
    }
    WinHttpCloseHandle(hr);
    WinHttpCloseHandle(hc);
    WinHttpCloseHandle(hs);
    return 0;
}

/* deterministic segment distribution (also used when loading saved jobs) */
static void DistributeRanges(Job *j)
{
    int i;
    DWORD64 chunk;
    if (!j->seg || j->nseg <= 0) return;
    chunk = j->total / (DWORD64)j->nseg;
    for (i = 0; i < j->nseg; i++) {
        j->seg[i].start = (DWORD64)i * chunk;
        j->seg[i].end   = (i == j->nseg - 1) ? (j->total - 1) : (((DWORD64)(i + 1)) * chunk - 1);
    }
}

static void AllocateSegments(Job *j)
{
    int want = g_set.segments;
    j->nseg = 0;
    if (!j->useRanges || j->total < 2 * 1024 * 1024) {
        j->nseg = 1;
    } else {
        if (want > MAX_SEGMENTS) want = MAX_SEGMENTS;
        if (want < 1) want = 1;
        while (want > 1 && j->total / (DWORD64)want < (DWORD64)65536) want--;
        j->nseg = want;
    }
    if (j->seg) HeapFree(GetProcessHeap(), 0, j->seg);
    j->seg = (Segment *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  sizeof(Segment) * j->nseg);
    DistributeRanges(j);
}

static BOOL PreallocatePart(Job *j)
{
    HANDLE fh;
    LARGE_INTEGER li;
    fh = CreateFileW(j->wpart, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return FALSE;
    if (j->total > 0) {
        li.QuadPart = (LONGLONG)(j->total - 1);
        if (!SetFilePointerEx(fh, li, NULL, FILE_BEGIN) || !SetEndOfFile(fh)) {
            CloseHandle(fh);
            return FALSE;
        }
    }
    CloseHandle(fh);
    return TRUE;
}

static DWORD64 PartFileSize(Job *j)
{
    HANDLE fh;
    LARGE_INTEGER sz;
    DWORD64 v = 0;
    fh = CreateFileW(j->wpart, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh != INVALID_HANDLE_VALUE) {
        if (GetFileSizeEx(fh, &sz)) v = (DWORD64)sz.QuadPart;
        CloseHandle(fh);
    }
    return v;
}

typedef struct {
    Job *j;
    int  idx;
} SegArg;

static DWORD WINAPI SegThread(LPVOID p)
{
    SegArg  sa = *(SegArg *)p;
    Job    *j = sa.j;
    Segment *s = &j->seg[sa.idx];
    BYTE    *buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 262144);
    int      attempt;

    HeapFree(GetProcessHeap(), 0, p);
    if (!buf) return 0;

    for (attempt = 0; attempt < 4 && !j->pauseFlag; attempt++) {
        HINTERNET hs, hc, hr = NULL;
        HANDLE fh;
        LARGE_INTEGER li;
        DWORD64 pos, remaining, segLen;
        BOOL segErr = FALSE, connected = FALSE;
        char rngA[96];
        WCHAR rngW[96];

        if (j->useRanges && j->total > 0) {
            DWORD64 cur = s->start + (DWORD64)s->done;
            if (s->done < 0) s->done = 0;
            if (cur > s->end) break;                    /* segment complete */
            segLen = s->end - cur + 1;
        } else {
            segLen = 0;                                  /* unbounded / whole file */
        }
        if (j->useRanges && j->total > 0 && segLen == 0) break;

        hs = WinHttpOpen(L"TurboGrab/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hs) { Sleep(800); continue; }
        hc = WinHttpConnect(hs, j->host, j->port, 0);
        if (!hc) { WinHttpCloseHandle(hs); Sleep(800); continue; }
        hr = WinHttpOpenRequest(hc, L"GET", j->path, NULL, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                j->https ? WINHTTP_FLAG_SECURE : 0);
        if (!hr) { WinHttpCloseHandle(hc); WinHttpCloseHandle(hs); Sleep(800); continue; }
        {
            DWORD t = 20000;
            WinHttpSetOption(hr, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
            WinHttpSetOption(hr, WINHTTP_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
            WinHttpSetOption(hr, WINHTTP_OPTION_SEND_TIMEOUT, &t, sizeof(t));
        }
        if (j->useRanges && j->total > 0) {
            sprintf(rngA, "Range: bytes=%I64u-%I64u",
                    s->start + (DWORD64)s->done, s->end);
            MultiByteToWideChar(CP_ACP, 0, rngA, -1, rngW, 96);
            WinHttpAddRequestHeaders(hr, rngW, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        }
        if (!WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(hr, NULL)) {
            WinHttpCloseHandle(hr); WinHttpCloseHandle(hc); WinHttpCloseHandle(hs);
            s->retries++;
            if (s->retries >= 4) {
                InterlockedExchange(&j->failFlag, 1);
                break;
            }
            Sleep(1000 * s->retries);
            continue;
        }
        {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                WINHTTP_NO_HEADER_INDEX);
            if (j->useRanges && j->total > 0 && status != 206) {
                WinHttpCloseHandle(hr); WinHttpCloseHandle(hc); WinHttpCloseHandle(hs);
                s->retries++;
                if (s->retries >= 4) { InterlockedExchange(&j->failFlag, 1); break; }
                Sleep(1000 * s->retries);
                continue;
            }
        }

        connected = TRUE;

        fh = CreateFileW(j->wpart, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE) {
            WinHttpCloseHandle(hr); WinHttpCloseHandle(hc); WinHttpCloseHandle(hs);
            InterlockedExchange(&j->failFlag, 1);
            break;
        }
        pos = (j->useRanges && j->total > 0) ? (s->start + (DWORD64)s->done) : 0;
        li.QuadPart = (LONGLONG)pos;
        SetFilePointerEx(fh, li, NULL, FILE_BEGIN);

        while (!j->pauseFlag) {
            DWORD avail = 0, rd = 0, wr = 0;
            if (j->useRanges && j->total > 0) {
                DWORD64 cur = s->start + (DWORD64)s->done;
                if (cur > s->end) break;
                remaining = s->end - cur + 1;
            } else {
                remaining = 0;   /* unbounded */
            }
            if (!WinHttpQueryDataAvailable(hr, &avail)) { segErr = TRUE; break; }
            if (avail == 0) break;                        /* stream end */
            if (avail > 262144) avail = 262144;
            if (j->useRanges && j->total > 0 && (DWORD64)avail > remaining)
                avail = (DWORD)remaining;
            if (!WinHttpReadData(hr, buf, avail, &rd) || rd == 0) { segErr = TRUE; break; }
            if (!WriteFile(fh, buf, rd, &wr, NULL) || wr != rd) {
                InterlockedExchange(&j->failFlag, 1);
                segErr = TRUE;
                break;
            }
            s->done += (LONG64)rd;
            InterlockedAdd64(&j->got, (LONG64)rd);
        }
        CloseHandle(fh);
        (void)connected;
        WinHttpCloseHandle(hr);
        WinHttpCloseHandle(hc);
        WinHttpCloseHandle(hs);

        if (j->pauseFlag) break;
        if (!segErr) {
            BOOL complete = TRUE;
            if (j->useRanges && j->total > 0) {
                DWORD64 cur = s->start + (DWORD64)s->done;
                if (cur <= s->end) complete = FALSE;      /* short read -> retry */
            }
            if (complete) break;
            segErr = TRUE;
        }
        s->retries++;
        if (s->retries >= 4) { InterlockedExchange(&j->failFlag, 1); break; }
        Sleep(1000 * s->retries);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return 0;
}

static void SaveJobs(void);

static DWORD WINAPI JobThread(LPVOID p)
{
    Job *j = (Job *)p;
    HANDLE th[MAX_SEGMENTS];
    SegArg *args[MAX_SEGMENTS];
    int i, n;
    BOOL havePart;

    j->state = ST_CONNECT;

    if (ProbeUrl(j) != 0) {
        j->state = ST_ERROR;
        PostMessage(g_hMain, WM_APP_JOB, 0, (LPARAM)j);
        return 0;
    }
    if (j->pauseFlag) {   /* paused while probing */
        j->state = ST_PAUSED;
        SaveJobs();
        PostMessage(g_hMain, WM_APP_JOB, 0, (LPARAM)j);
        return 0;
    }
    j->state = ST_DOWN;
    PostMessage(g_hMain, WM_APP_JOB, 0, (LPARAM)j);

    havePart = (GetFileAttributesW(j->wpart) != INVALID_FILE_ATTRIBUTES);

    /* resume only when: server said ranges, size known, seg table present, part exists */
    if (!(j->useRanges && j->total > 0 && j->seg && j->nseg > 0 && havePart)) {
        /* fresh start */
        InterlockedExchange64(&j->got, 0);
        j->failFlag = 0;
        AllocateSegments(j);
        if (!PreallocatePart(j)) {
            lstrcpynA(j->err, "Cannot create file in save folder", sizeof(j->err));
            j->state = ST_ERROR;
            PostMessage(g_hMain, WM_APP_JOB, 0, (LPARAM)j);
            return 0;
        }
    } else {
        DWORD64 psize = PartFileSize(j);
        DWORD64 sum = 0;
        int k;
        if (psize != j->total) {
            /* corrupt/incompatible part -> restart fresh */
            InterlockedExchange64(&j->got, 0);
            AllocateSegments(j);
            if (!PreallocatePart(j)) {
                lstrcpynA(j->err, "Cannot create file in save folder", sizeof(j->err));
                j->state = ST_ERROR;
                PostMessage(g_hMain, WM_APP_JOB, 0, (LPARAM)j);
                return 0;
            }
        } else {
            for (k = 0; k < j->nseg; k++) sum += (DWORD64)j->seg[k].done;
            InterlockedExchange64(&j->got, (LONG64)sum);
        }
    }

    n = j->nseg;
    if (n > MAX_SEGMENTS) n = MAX_SEGMENTS;
    for (i = 0; i < n; i++) {
        args[i] = (SegArg *)HeapAlloc(GetProcessHeap(), 0, sizeof(SegArg));
        if (!args[i]) { th[i] = NULL; continue; }
        args[i]->j = j;
        args[i]->idx = i;
        th[i] = CreateThread(NULL, 0, SegThread, (LPVOID)args[i], 0, NULL);
        if (!th[i]) { HeapFree(GetProcessHeap(), 0, args[i]); args[i] = NULL; }
    }
    for (i = 0; i < n; i++) {
        if (th[i]) {
            WaitForSingleObject(th[i], INFINITE);
            CloseHandle(th[i]);
        }
    }

    if (j->pauseFlag) {
        j->state = ST_PAUSED;
    } else if (j->failFlag) {
        j->state = ST_ERROR;
        if (!j->err[0])
            lstrcpynA(j->err, "Download failed (retries exhausted)", sizeof(j->err));
    } else {
        if (j->total == 0) j->total = (DWORD64)j->got;   /* unknown size now known */
        if (MoveFileExW(j->wpart, j->wfinal, MOVEFILE_REPLACE_EXISTING))
            j->state = ST_DONE;
        else {
            lstrcpynA(j->err, "Cannot write final file", sizeof(j->err));
            j->state = ST_ERROR;
        }
    }
    SaveJobs();
    PostMessage(g_hMain, WM_APP_JOB, 0, (LPARAM)j);
    return 0;
}

/* ============================= persistence ============================= */

static void SaveJobs(void)
{
    char tmp[64], sec[16];
    int idx;
    Job *j;

    EnterCriticalSection(&g_cs);
    for (idx = g_count; idx < g_count + 60; idx++) {
        sprintf(sec, "Job%d", idx);
        WritePrivateProfileStringA(sec, NULL, NULL, g_iniPath);
    }
    sprintf(tmp, "%d", g_count);
    WritePrivateProfileStringA("Jobs", "count", tmp, g_iniPath);
    idx = 0;
    for (j = g_jobs; j; j = j->next, idx++) {
        char *list;
        size_t cap;
        int k, off = 0;
        sprintf(sec, "Job%d", idx);
        WritePrivateProfileStringA(sec, "url", j->url, g_iniPath);
        WritePrivateProfileStringA(sec, "dir", j->dir, g_iniPath);
        WritePrivateProfileStringA(sec, "name", j->name, g_iniPath);
        sprintf(tmp, "%I64u", j->total);
        WritePrivateProfileStringA(sec, "total", tmp, g_iniPath);
        sprintf(tmp, "%d", j->useRanges ? 1 : 0);
        WritePrivateProfileStringA(sec, "ranges", tmp, g_iniPath);
        sprintf(tmp, "%d", j->nseg);
        WritePrivateProfileStringA(sec, "nseg", tmp, g_iniPath);
        cap = (size_t)j->nseg * 24 + 8;
        list = (char *)HeapAlloc(GetProcessHeap(), 0, cap);
        if (list) {
            list[0] = 0;
            for (k = 0; k < j->nseg; k++) {
                int wr = _snprintf(list + off, cap - (size_t)off, "%I64u,",
                                   (DWORD64)j->seg[k].done);
                if (wr < 0) break;
                off += wr;
            }
            WritePrivateProfileStringA(sec, "done", list, g_iniPath);
            HeapFree(GetProcessHeap(), 0, list);
        }
    }
    LeaveCriticalSection(&g_cs);
}

static void LoadJobs(void)
{
    int n = GetPrivateProfileIntA("Jobs", "count", 0, g_iniPath);
    int i;

    if (n < 0 || n > 200) n = 0;
    for (i = 0; i < n; i++) {
        char sec[16], url[2048], dir[MAX_PATH], name[600], list[16 * 24 + 8];
        Job *j;
        DWORD64 total;
        int useRanges, nseg, k;
        char tmp[64];

        sprintf(sec, "Job%d", i);
        GetPrivateProfileStringA(sec, "url", "", url, sizeof(url), g_iniPath);
        GetPrivateProfileStringA(sec, "dir", "", dir, sizeof(dir), g_iniPath);
        GetPrivateProfileStringA(sec, "name", "", name, sizeof(name), g_iniPath);
        if (!url[0] || !dir[0] || !name[0]) continue;

        GetPrivateProfileStringA(sec, "total", "0", tmp, sizeof(tmp), g_iniPath);
        total = ParseU64A(tmp);
        useRanges = GetPrivateProfileIntA(sec, "ranges", 0, g_iniPath);
        nseg = GetPrivateProfileIntA(sec, "nseg", 0, g_iniPath);

        j = JobCreate(url, dir, name);
        if (!j) continue;
        j->total = total;
        j->useRanges = useRanges;

        if (useRanges && total > 0 && nseg > 0 && nseg <= MAX_SEGMENTS) {
            GetPrivateProfileStringA(sec, "done", "", list, sizeof(list), g_iniPath);
            j->nseg = nseg;
            j->seg = (Segment *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                          sizeof(Segment) * nseg);
            DistributeRanges(j);
            {
                char *p = list;
                for (k = 0; k < nseg && *p; k++) {
                    j->seg[k].done = (LONG64)ParseU64A(p);
                    while (*p && *p != ',') p++;
                    if (*p == ',') p++;
                }
            }
            /* part file must exist to resume */
            if (GetFileAttributesW(j->wpart) == INVALID_FILE_ATTRIBUTES) {
                HeapFree(GetProcessHeap(), 0, j->seg);
                j->seg = NULL;
                j->nseg = 0;
            }
        }
        j->state = ST_PAUSED;
        j->err[0] = 0;
    }
    SaveJobs();
}

/* ============================= queue ============================= */

static void KickQueue(void)
{
    int active;
    Job *j;
    int started = 0;

    EnterCriticalSection(&g_cs);
    active = ActiveCount();
    for (j = g_jobs; j && active < g_set.concurrent; j = j->next) {
        if (j->state == ST_QUEUED && !j->hThread) {
            j->state = ST_CONNECT;
            j->err[0] = 0;
            j->hThread = CreateThread(NULL, 0, JobThread, j, 0, NULL);
            if (!j->hThread) {
                j->state = ST_ERROR;
                lstrcpynA(j->err, "Could not start worker thread", sizeof(j->err));
            } else {
                active++;
                started++;
            }
        }
    }
    LeaveCriticalSection(&g_cs);
    if (started) RefreshAll();
}

/* ============================= UI: list ============================= */

static int FindRow(Job *j)
{
    int i, n = (int)SendMessageA(g_hList, LVM_GETITEMCOUNT, 0, 0);
    LVITEMA li;
    for (i = 0; i < n; i++) {
        ZeroMemory(&li, sizeof(li));
        li.mask = LVIF_PARAM;
        li.iItem = i;
        if (SendMessageA(g_hList, LVM_GETITEMA, 0, (LPARAM)&li) && (Job *)li.lParam == j)
            return i;
    }
    return -1;
}

static void InsertRow(Job *j)
{
    LVITEMA li;
    char disp[320];
    int idx, c;

    DisplayOf(j->name, disp, sizeof(disp));
    ZeroMemory(&li, sizeof(li));
    li.mask = LVIF_TEXT | LVIF_PARAM;
    li.iItem = (int)SendMessageA(g_hList, LVM_GETITEMCOUNT, 0, 0);
    li.iSubItem = 0;
    li.pszText = disp;
    li.lParam = (LPARAM)j;
    idx = (int)SendMessageA(g_hList, LVM_INSERTITEMA, 0, (LPARAM)&li);
    for (c = 1; c <= 6; c++)
        ListView_SetItemText(g_hList, idx, c, "");
}

static void RebuildList(void)
{
    Job *j;
    SendMessageA(g_hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hList);
    for (j = g_jobs; j; j = j->next) InsertRow(j);
    SendMessageA(g_hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hList, NULL, TRUE);
}

static const char *StateText(Job *j)
{
    switch (j->state) {
    case ST_QUEUED:  return "Queued";
    case ST_CONNECT: return "Connecting...";
    case ST_DOWN:    return "Downloading";
    case ST_PAUSED:  return "Paused";
    case ST_DONE:    return "Completed";
    case ST_ERROR:   return j->err[0] ? j->err : "Failed";
    }
    return "";
}

static void UpdateRow(Job *j)
{
    int idx = FindRow(j);
    char b[64];

    if (idx < 0) return;
    if (j->total > 0) FormatSize(j->total, b, sizeof(b));
    else if (j->state == ST_DONE) FormatSize(j->got > 0 ? (DWORD64)j->got : 0, b, sizeof(b));
    else { FormatSize((DWORD64)j->got, b, sizeof(b)); lstrcatA(b, " / ?"); }
    ListView_SetItemText(g_hList, idx, 1, b);

    ListView_SetItemText(g_hList, idx, 2, "");

    if (j->state == ST_DOWN && j->speedBps > 0) {
        FormatSize((DWORD64)j->speedBps, b, sizeof(b));
        lstrcatA(b, "/s");
    } else {
        lstrcpynA(b, "", sizeof(b));
    }
    ListView_SetItemText(g_hList, idx, 3, b);

    if (j->state == ST_DOWN && j->total > 0 && j->speedBps > 0) {
        DWORD64 left = (j->total > (DWORD64)j->got) ? (j->total - (DWORD64)j->got) : 0;
        DWORD64 secs = left / (j->speedBps ? j->speedBps : 1);
        if (secs >= 3600) sprintf(b, "%uh %02um", (unsigned)(secs / 3600), (unsigned)((secs / 60) % 60));
        else if (secs >= 60) sprintf(b, "%um %02us", (unsigned)(secs / 60), (unsigned)(secs % 60));
        else sprintf(b, "%us", (unsigned)secs);
    } else {
        lstrcpynA(b, "", sizeof(b));
    }
    ListView_SetItemText(g_hList, idx, 4, b);

    ListView_SetItemText(g_hList, idx, 5, (char *)StateText(j));
    ListView_SetItemText(g_hList, idx, 6, j->dir);

    ListView_RedrawItems(g_hList, idx, idx);
}

static void RefreshAll(void)
{
    Job *j;
    EnterCriticalSection(&g_cs);
    for (j = g_jobs; j; j = j->next) UpdateRow(j);
    LeaveCriticalSection(&g_cs);
    UpdateStatusLine();
}

static void UpdateStatusLine(void)
{
    int active = 0, queued = 0, done = 0;
    DWORD64 speed = 0;
    Job *j;
    char sb[48];

    EnterCriticalSection(&g_cs);
    for (j = g_jobs; j; j = j->next) {
        if (j->state == ST_CONNECT || j->state == ST_DOWN) { active++; speed += j->speedBps; }
        else if (j->state == ST_QUEUED) queued++;
        else if (j->state == ST_DONE) done++;
    }
    LeaveCriticalSection(&g_cs);
    if (speed > 0) {
        FormatSize(speed, sb, sizeof(sb));
        lstrcatA(sb, "/s");
    } else {
        lstrcpynA(sb, "", sizeof(sb));
    }
    g_statActive = active;
    g_statQueued = queued;
    g_statDone   = done;
    lstrcpynA(g_statSpeed, sb, sizeof(g_statSpeed));
    if (g_hMain) InvalidateRect(g_hMain, NULL, TRUE);
}

static Job *SelectedJob(void)
{
    int idx = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    LVITEMA li;
    if (idx < 0) return NULL;
    ZeroMemory(&li, sizeof(li));
    li.mask = LVIF_PARAM;
    li.iItem = idx;
    if (!SendMessageA(g_hList, LVM_GETITEMA, 0, (LPARAM)&li)) return NULL;
    return (Job *)li.lParam;
}

/* ============================= popups ============================= */

static void PopupClose(HWND hw, HWND *tracker)
{
    EnableWindow(g_hMain, TRUE);
    *tracker = NULL;
    DestroyWindow(hw);
}

static void AddPopupStart(HWND hw)
{
    char url[2048], dir[MAX_PATH];
    Job *j;

    GetWindowTextA(GetDlgItem(hw, IDC_AURL), url, sizeof(url));
    GetWindowTextA(GetDlgItem(hw, IDC_ADIR), dir, sizeof(dir));
    Trim(url);
    Trim(dir);
    if (!IsUrlOk(url)) {
        MessageBoxA(hw, "Enter a valid http:// or https:// URL.", APP_TITLE, MB_ICONEXCLAMATION);
        return;
    }
    if (!dir[0]) {
        MessageBoxA(hw, "Choose a folder to save into.", APP_TITLE, MB_ICONEXCLAMATION);
        return;
    }
    CreateDirectoryA(dir, NULL);
    j = JobCreate(url, dir, NULL);
    if (j) {
        InsertRow(j);
        UpdateRow(j);
        KickQueue();
        UpdateStatusLine();
    }
    PopupClose(hw, &g_hAddWnd);
}

static void BrowseForDir(HWND hwnd, int idEdit)
{
    BROWSEINFOA bi;
    char path[MAX_PATH];
    LPITEMIDLIST pidl;

    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.lpszTitle = "Choose folder:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        if (SHGetPathFromIDListA(pidl, path)) SetWindowTextA(GetDlgItem(hwnd, idEdit), path);
        CoTaskMemFree(pidl);
    }
}

static LRESULT CALLBACK AddWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE h = ((LPCREATESTRUCTA)lp)->hInstance;
        HWND c;

        CreateWindowA("STATIC", "Download link (URL):", WS_CHILD | WS_VISIBLE,
            14, 14, 180, 18, hw, NULL, h, NULL);
        c = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            14, 36, 500, 26, hw, (HMENU)IDC_AURL, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        CreateWindowA("STATIC", "Save to:", WS_CHILD | WS_VISIBLE,
            14, 74, 80, 18, hw, NULL, h, NULL);
        c = CreateWindowA("EDIT", g_set.dir, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            14, 96, 390, 26, hw, (HMENU)IDC_ADIR, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SkinBtn(hw, h, IDC_ABROWSE, "Browse...", 414, 94, 100, 30);

        SkinBtn(hw, h, IDC_ASTART, "Start Download", 14, 138, 140, 34);
        SkinBtn(hw, h, IDC_ACANCEL, "Cancel", 164, 138, 84, 34);
        DarkTitle(hw);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hw, &rc);
        FillSolid(hdc, &rc, TG_BG);
        return 1;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, TG_EDITBG);
        SetTextColor(hdc, TG_TXT);
        return (LRESULT)SkinEditBrush();
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        HWND ch = (HWND)lp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetDlgCtrlID(ch) == IDC_STOKEN ? TG_TXT : TG_TXT_DIM);
        return (LRESULT)SkinBgBrush();
    }
    case WM_DRAWITEM:
        SkinDrawBtn((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ASTART:  AddPopupStart(hw); return 0;
        case IDC_ACANCEL: PopupClose(hw, &g_hAddWnd); return 0;
        case IDC_ABROWSE: BrowseForDir(hw, IDC_ADIR); return 0;
        }
        break;
    case WM_CLOSE:
        PopupClose(hw, &g_hAddWnd);
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static void OpenAddPopup(const char *prefill)
{
    RECT r;
    if (g_hAddWnd) {
        if (prefill) SetWindowTextA(GetDlgItem(g_hAddWnd, IDC_AURL), prefill);
        SetForegroundWindow(g_hAddWnd);
        return;
    }
    EnableWindow(g_hMain, FALSE);
    GetWindowRect(g_hMain, &r);
    g_hAddWnd = CreateWindowA(ADDPOP_CLASS, "Add download - TurboGrab",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        r.left + 60, r.top + 80, 540, 215, g_hMain, NULL,
        (HINSTANCE)GetWindowLongPtrA(g_hMain, GWLP_HINSTANCE), NULL);
    if (g_hAddWnd) {
        if (prefill) SetWindowTextA(GetDlgItem(g_hAddWnd, IDC_AURL), prefill);
        ShowWindow(g_hAddWnd, SW_SHOW);
        SetForegroundWindow(g_hAddWnd);
    } else {
        EnableWindow(g_hMain, TRUE);
    }
}

static void SettingsSave(HWND hw)
{
    char b[64];
    int v;

    GetWindowTextA(GetDlgItem(hw, IDC_SSEG), b, sizeof(b));
    v = atoi(b);
    if (v < 1) v = 1;
    if (v > MAX_SEGMENTS) v = MAX_SEGMENTS;
    g_set.segments = v;

    GetWindowTextA(GetDlgItem(hw, IDC_SCON), b, sizeof(b));
    v = atoi(b);
    if (v < 1) v = 1;
    if (v > 10) v = 10;
    g_set.concurrent = v;

    GetWindowTextA(GetDlgItem(hw, IDC_SDIR), g_set.dir, MAX_PATH);
    Trim(g_set.dir);
    if (!g_set.dir[0]) lstrcpynA(g_set.dir, "C:", MAX_PATH);
    CreateDirectoryA(g_set.dir, NULL);

    g_set.clipboard  = (SendMessageA(GetDlgItem(hw, IDC_SCLIP), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    g_set.categories = (SendMessageA(GetDlgItem(hw, IDC_SCAT),  BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;

    g_browserOn = (SendMessageA(GetDlgItem(hw, IDC_SBROWSER), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    GetWindowTextA(GetDlgItem(hw, IDC_SPORT), b, sizeof(b));
    v = atoi(b);
    if (v < 1024 || v > 65535) v = 8777;
    g_browserPort = v;

    SaveSettings();
    {
        char pb[16];
        sprintf(pb, "%d", g_browserPort);
        WritePrivateProfileStringA("Browser", "enabled", g_browserOn ? "1" : "0", g_iniPath);
        WritePrivateProfileStringA("Browser", "port", pb, g_iniPath);
    }
    BrowserSrvApply();
    MessageBoxA(hw, "Settings saved.", APP_TITLE, MB_ICONINFORMATION);
}

static LRESULT CALLBACK SetWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE h = ((LPCREATESTRUCTA)lp)->hInstance;
        HWND c;
        char b[16];

        CreateWindowA("STATIC", "Segments per download (1-16):", WS_CHILD | WS_VISIBLE,
            14, 16, 230, 18, hw, NULL, h, NULL);
        sprintf(b, "%d", g_set.segments);
        c = CreateWindowA("EDIT", b, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            260, 14, 70, 26, hw, (HMENU)IDC_SSEG, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        CreateWindowA("STATIC", "Downloads at the same time (1-10):", WS_CHILD | WS_VISIBLE,
            14, 50, 230, 18, hw, NULL, h, NULL);
        sprintf(b, "%d", g_set.concurrent);
        c = CreateWindowA("EDIT", b, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            260, 48, 70, 26, hw, (HMENU)IDC_SCON, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        CreateWindowA("STATIC", "Default download folder:", WS_CHILD | WS_VISIBLE,
            14, 86, 220, 18, hw, NULL, h, NULL);
        c = CreateWindowA("EDIT", g_set.dir, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            14, 108, 330, 26, hw, (HMENU)IDC_SDIR, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SkinBtn(hw, h, IDC_SBROWSE, "Browse...", 354, 106, 100, 30);

        c = CreateWindowA("BUTTON", "Watch clipboard for download links",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            14, 148, 330, 22, hw, (HMENU)IDC_SCLIP, h, NULL);
        SendMessageA(c, BM_SETCHECK, g_set.clipboard ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        c = CreateWindowA("BUTTON", "Sort files into category folders",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            14, 174, 330, 22, hw, (HMENU)IDC_SCAT, h, NULL);
        SendMessageA(c, BM_SETCHECK, g_set.categories ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        /* browser extension integration */
        c = CreateWindowA("BUTTON", "Browser extension integration (localhost server)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            14, 206, 340, 22, hw, (HMENU)IDC_SBROWSER, h, NULL);
        SendMessageA(c, BM_SETCHECK, g_browserOn ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        CreateWindowA("STATIC", "Server port:", WS_CHILD | WS_VISIBLE,
            14, 240, 90, 18, hw, NULL, h, NULL);
        sprintf(b, "%d", g_browserPort);
        c = CreateWindowA("EDIT", b, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            110, 238, 70, 26, hw, (HMENU)IDC_SPORT, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        CreateWindowA("STATIC", "Extension token (paste into the extension options):",
            WS_CHILD | WS_VISIBLE, 14, 272, 360, 18, hw, NULL, h, NULL);
        c = CreateWindowA("EDIT", g_browserToken,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
            14, 294, 330, 26, hw, (HMENU)IDC_STOKEN, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SkinBtn(hw, h, IDC_SCOPYTOK, "Copy token", 354, 292, 100, 30);

        SkinBtn(hw, h, IDC_SSAVE, "Save", 14, 336, 92, 34);
        SkinBtn(hw, h, IDC_SCLOSE, "Close", 116, 336, 84, 34);
        DarkTitle(hw);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hw, &rc);
        FillSolid(hdc, &rc, TG_BG);
        return 1;
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, TG_TXT);
        return (LRESULT)SkinBgBrush();
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, TG_EDITBG);
        SetTextColor(hdc, TG_TXT);
        return (LRESULT)SkinEditBrush();
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        HWND ch = (HWND)lp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetDlgCtrlID(ch) == IDC_STOKEN ? TG_TXT : TG_TXT_DIM);
        return (LRESULT)SkinBgBrush();
    }
    case WM_DRAWITEM:
        SkinDrawBtn((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SSAVE:   SettingsSave(hw); return 0;
        case IDC_SCLOSE:  PopupClose(hw, &g_hSetWnd); return 0;
        case IDC_SBROWSE: BrowseForDir(hw, IDC_SDIR); return 0;
        case IDC_SCOPYTOK: {
            int len = (int)strlen(g_browserToken) + 1;
            if (OpenClipboard(hw)) {
                HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)len);
                if (hg) {
                    char *p = (char *)GlobalLock(hg);
                    if (p) { memcpy(p, g_browserToken, (size_t)len); GlobalUnlock(hg); }
                    SetClipboardData(CF_TEXT, hg);
                }
                CloseClipboard();
                MessageBoxA(hw, "Token copied. Paste it into the TurboGrab extension options page.",
                            APP_TITLE, MB_ICONINFORMATION);
            }
            return 0;
        }
        }
        break;
    case WM_CLOSE:
        PopupClose(hw, &g_hSetWnd);
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static void OpenSetPopup(void)
{
    RECT r;
    if (g_hSetWnd) { SetForegroundWindow(g_hSetWnd); return; }
    EnableWindow(g_hMain, FALSE);
    GetWindowRect(g_hMain, &r);
    g_hSetWnd = CreateWindowA(SETPOP_CLASS, "TurboGrab settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        r.left + 80, r.top + 40, 500, 432, g_hMain, NULL,
        (HINSTANCE)GetWindowLongPtrA(g_hMain, GWLP_HINSTANCE), NULL);
    if (g_hSetWnd) {
        ShowWindow(g_hSetWnd, SW_SHOW);
        SetForegroundWindow(g_hSetWnd);
    } else {
        EnableWindow(g_hMain, TRUE);
    }
}

/* ============================= browsers window ============================= */

static TgBrowser g_br[TG_MAX_BROWSERS];
static int       g_brN = 0;

static void BrowsersRowUpdate(HWND hw, int i)
{
    char s[128];
    HWND st = GetDlgItem(hw, IDC_BSTAT(i));
    if (!st) return;
    if (g_br[i].registered)
        lstrcpynA(s, "Registered \x97 restart this browser once to finish", sizeof(s));
    else
        lstrcpynA(s, "Not registered yet", sizeof(s));
    SetWindowTextA(st, s);
    InvalidateRect(st, NULL, TRUE);
    {
        HWND b = GetDlgItem(hw, IDC_BREG(i));
        if (b) {
            SetWindowTextA(b, g_br[i].registered ? "Repair" : "Register");
            InvalidateRect(b, NULL, TRUE);
        }
    }
}

static void BrowsersRescan(HWND hw)
{
    int i;
    g_brN = BrowsersScan(g_br, TG_MAX_BROWSERS);
    for (i = 0; i < TG_MAX_BROWSERS; i++) {
        ShowWindow(GetDlgItem(hw, IDC_BNAME(i)), (i < g_brN) ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(hw, IDC_BSTAT(i)), (i < g_brN) ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(hw, IDC_BREG(i)),  (i < g_brN) ? SW_SHOW : SW_HIDE);
        if (i < g_brN) {
            SetWindowTextA(GetDlgItem(hw, IDC_BNAME(i)), g_br[i].name);
            BrowsersRowUpdate(hw, i);
        }
    }
    ShowWindow(GetDlgItem(hw, IDC_BNOROWS), g_brN ? SW_HIDE : SW_SHOW);
}

static void BrowsersAfterApply(HWND hw)
{
    int i;
    /* rescan to refresh registered flags from the registry */
    g_brN = BrowsersScan(g_br, TG_MAX_BROWSERS);
    for (i = 0; i < g_brN; i++) BrowsersRowUpdate(hw, i);
}

static LRESULT CALLBACK BrowsWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE h = ((LPCREATESTRUCTA)lp)->hInstance;
        int i;
        HWND c;

        c = CreateWindowA("STATIC",
            "The TurboGrab Integration extension sends download links from your browser "
            "straight into TurboGrab. TurboGrab registers it for every browser below "
            "automatically. If a browser still shows \x93Not registered\x94, click Register, "
            "then fully close and reopen that browser.",
            WS_CHILD | WS_VISIBLE, 16, 14, 520, 66, hw, NULL, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        for (i = 0; i < TG_MAX_BROWSERS; i++) {
            int y = 92 + i * 36;
            c = CreateWindowA("STATIC", "", WS_CHILD,
                16, y, 150, 20, hw, (HMENU)(INT_PTR)IDC_BNAME(i), h, NULL);
            SendMessageA(c, WM_SETFONT, (WPARAM)g_hFontB, TRUE);
            c = CreateWindowA("STATIC", "", WS_CHILD,
                174, y, 280, 20, hw, (HMENU)(INT_PTR)IDC_BSTAT(i), h, NULL);
            SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SkinBtn(hw, h, IDC_BREG(i), "Register", 462, y - 4, 92, 28);
        }

        c = CreateWindowA("STATIC", "No supported browsers were found on this PC.",
            WS_CHILD, 16, 100, 400, 20, hw, (HMENU)(INT_PTR)IDC_BNOROWS, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        SkinBtn(hw, h, IDC_BALL,   "Register / Repair all", 16, 396, 170, 34);
        SkinBtn(hw, h, IDC_BCLOSE, "Close", 536 - 100, 396, 84, 34);
        DarkTitle(hw);
        BrowsersRescan(hw);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hw, &rc);
        FillSolid(hdc, &rc, TG_BG);
        return 1;
    }
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, TG_EDITBG);
        SetTextColor((HDC)wp, TG_TXT);
        return (LRESULT)SkinEditBrush();
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        SetBkMode(hdc, TRANSPARENT);
        if (id >= IDC_BSTAT(0) && id < IDC_BSTAT(TG_MAX_BROWSERS)) {
            int i = id - IDC_BSTAT(0);
            SetTextColor(hdc, (i < g_brN && g_br[i].registered) ? TG_GREEN : TG_ORANGE);
        } else if (id >= IDC_BNAME(0) && id < IDC_BNAME(TG_MAX_BROWSERS)) {
            SetTextColor(hdc, TG_TXT);
        } else if (id == IDC_BNOROWS) {
            SetTextColor(hdc, TG_TXT_MUTE);
        } else {
            SetTextColor(hdc, TG_TXT_DIM);
        }
        return (LRESULT)SkinBgBrush();
    }
    case WM_DRAWITEM:
        SkinDrawBtn((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_BCLOSE) { PopupClose(hw, &g_hBrowsWnd); return 0; }
        if (id == IDC_BALL) {
            int n = BrowsersApplyAll();
            BrowsersAfterApply(hw);
            { char m[160];
              sprintf(m, "%d browser(s) registered.\n\nFully close and reopen each browser "
                         "(check the tray too) \x97 the extension appears on its own.", n);
              MessageBoxA(hw, m, APP_TITLE, MB_ICONINFORMATION); }
            return 0;
        }
        if (id >= IDC_BREG(0) && id < IDC_BREG(TG_MAX_BROWSERS)) {
            int i = id - IDC_BREG(0);
            if (i < g_brN) {
                BrowsersApplyOne(i);
                BrowsersAfterApply(hw);
                if (g_br[i].registered) {
                    MessageBoxA(hw, "Registered. Now fully close and reopen this browser "
                                "\x97 the TurboGrab extension installs itself.",
                                APP_TITLE, MB_ICONINFORMATION);
                } else {
                    MessageBoxA(hw, "Could not write the browser policy.",
                                APP_TITLE, MB_ICONEXCLAMATION);
                }
            }
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        PopupClose(hw, &g_hBrowsWnd);
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static void OpenBrowsersPopup(void)
{
    RECT r;
    if (g_hBrowsWnd) { SetForegroundWindow(g_hBrowsWnd); return; }
    EnableWindow(g_hMain, FALSE);
    GetWindowRect(g_hMain, &r);
    g_hBrowsWnd = CreateWindowA(BRW_CLASS, "Browser extension \x96 TurboGrab",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        r.left + 40, r.top + 40, 552, 480, g_hMain, NULL,
        (HINSTANCE)GetWindowLongPtrA(g_hMain, GWLP_HINSTANCE), NULL);
    if (g_hBrowsWnd) {
        ShowWindow(g_hBrowsWnd, SW_SHOW);
        SetForegroundWindow(g_hBrowsWnd);
    } else {
        EnableWindow(g_hMain, TRUE);
    }
}

/* ============================= actions ============================= */

static void JobResume(Job *j)
{
    if (!j || j->deletePending) return;
    if (j->state != ST_PAUSED && j->state != ST_ERROR) return;
    InterlockedExchange(&j->pauseFlag, 0);
    InterlockedExchange(&j->failFlag, 0);
    j->err[0] = 0;
    j->lastGot = 0;
    j->speedBps = 0;
    j->state = ST_QUEUED;
    UpdateRow(j);
    UpdateStatusLine();
    KickQueue();
}

static void JobPause(Job *j)
{
    if (!j) return;
    if (j->state != ST_DOWN && j->state != ST_CONNECT && j->state != ST_QUEUED) return;
    InterlockedExchange(&j->pauseFlag, 1);
    if (j->state == ST_QUEUED) {
        /* never started: go straight to paused */
        j->state = ST_PAUSED;
        UpdateRow(j);
        UpdateStatusLine();
    }
}

static void JobDelete(Job *j)
{
    if (!j) return;
    EnterCriticalSection(&g_cs);
    if (j->state == ST_CONNECT || j->state == ST_DOWN) {
        int ans = MessageBoxA(g_hMain,
            "This download is running. Pause it and remove from the list?\n"
            "(The partial .part file stays on disk.)", APP_TITLE, MB_YESNO | MB_ICONQUESTION);
        if (ans != IDYES) { LeaveCriticalSection(&g_cs); return; }
        j->deletePending = 1;
        InterlockedExchange(&j->pauseFlag, 1);
        j->state = ST_PAUSED;   /* UI feedback; worker confirms */
    } else {
        JobUnlink(j);
        FreeJob(j);
    }
    LeaveCriticalSection(&g_cs);
    SaveJobs();
    RebuildList();
    UpdateStatusLine();
}

static void JobOpenFolder(Job *j)
{
    if (j) ShellExecuteA(g_hMain, "open", j->dir, NULL, NULL, SW_SHOWNORMAL);
}

static void JobOpenFile(Job *j)
{
    if (j && j->state == ST_DONE) {
        char disp[MAX_PATH * 2];
        DisplayOf(j->name, disp, sizeof(disp));
        { char full[MAX_PATH * 2 + 4];
          _snprintf(full, sizeof(full), "%s\\%s", j->dir, disp);
          full[sizeof(full) - 1] = 0;
          ShellExecuteA(g_hMain, "open", full, NULL, NULL, SW_SHOWNORMAL); }
    } else if (j) {
        JobOpenFolder(j);
    }
}

static void JobCopyUrl(Job *j)
{
    if (!j) return;
    if (OpenClipboard(g_hMain)) {
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, strlen(j->url) + 1);
        if (hg) {
            char *p = (char *)GlobalLock(hg);
            if (p) {
                lstrcpyA(p, j->url);
                GlobalUnlock(hg);
                EmptyClipboard();
                SetClipboardData(CF_TEXT, hg);
            } else {
                GlobalFree(hg);
            }
        }
        CloseClipboard();
    }
}

/* ============================= tray ============================= */

static void TrayAdd(void)
{
    if (g_trayOn) return;
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hMain;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_hIcon;
    lstrcpynA(g_nid.szTip, "TurboGrab", sizeof(g_nid.szTip));
    if (Shell_NotifyIconA(NIM_ADD, &g_nid)) g_trayOn = TRUE;
}

static void TrayRemove(void)
{
    if (!g_trayOn) return;
    g_nid.cbSize = sizeof(g_nid);
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
    g_trayOn = FALSE;
}

static void MainRestore(void)
{
    TrayRemove();
    ShowWindow(g_hMain, SW_RESTORE);
    SetForegroundWindow(g_hMain);
}

/* ============================= clipboard ============================= */

static void OnClipboardChange(void)
{
    char text[2048];
    HANDLE hg;
    const char *p;

    if (!g_set.clipboard) return;
    if (!IsClipboardFormatAvailable(CF_TEXT)) return;
    if (!OpenClipboard(g_hMain)) return;
    hg = GetClipboardData(CF_TEXT);
    text[0] = 0;
    if (hg) {
        p = (const char *)GlobalLock(hg);
        if (p) {
            lstrcpynA(text, p, sizeof(text));
            GlobalUnlock(hg);
        }
    }
    CloseClipboard();
    Trim(text);
    if (!IsUrlOk(text)) return;
    if (GetTickCount() - g_lastClipTick < 1500) return;
    if (strcmp(text, g_clipLast) == 0) return;
    lstrcpynA(g_clipLast, text, sizeof(g_clipLast));
    g_lastClipTick = GetTickCount();
    OpenAddPopup(text);
}

/* ============================= main wndproc ============================= */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE h = ((LPCREATESTRUCTA)lp)->hInstance;
        LVCOLUMNA lvc;
        HIMAGELIST il;
        int i;
        static const char *cols[7] = { "File", "Size", "Progress", "Speed", "Time left", "Status", "Save to" };
        static const int colw[7] = { 260, 88, 190, 100, 96, 220, 240 };

        SkinBtn(hwnd, h, IDC_ADD,     "Add URL",      12, 12, 104, 36);
        SkinBtn(hwnd, h, IDC_RESUME,  "Resume",       122, 12,  96, 36);
        SkinBtn(hwnd, h, IDC_PAUSE,   "Pause",        224, 12,  88, 36);
        SkinBtn(hwnd, h, IDC_DELETE,  "Delete",       318, 12,  88, 36);
        SkinBtn(hwnd, h, IDC_FOLDER,  "Open Folder",  412, 12, 116, 36);
        SkinBtn(hwnd, h, IDC_BROWSERS,"Browsers",     534, 12, 100, 36);
        SkinBtn(hwnd, h, IDC_SET,     "Settings",     640, 12,  98, 36);

        g_hList = CreateWindowA(WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            12, 62, 990, 424, hwnd, (HMENU)IDC_LIST, h, NULL);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(g_hList, TG_ROW1);
        ListView_SetTextBkColor(g_hList, TG_ROW1);
        ListView_SetTextColor(g_hList, TG_TXT);
        il = ImageList_Create(1, 46, ILC_COLOR32, 1, 1);
        ListView_SetImageList(g_hList, il, LVSIL_SMALL);
        if (!s_listOld) s_listOld = (WNDPROC)GetWindowLongPtrA(g_hList, GWLP_WNDPROC);
        SetWindowLongPtrA(g_hList, GWLP_WNDPROC, (LONG_PTR)ListProc);

        ZeroMemory(&lvc, sizeof(lvc));
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        for (i = 0; i < 7; i++) {
            lvc.iSubItem = i;
            lvc.pszText = (LPSTR)cols[i];
            lvc.cx = colw[i];
            ListView_InsertColumn(g_hList, i, &lvc);
        }
        SendMessageA(g_hList, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        RebuildList();

        SetTimer(hwnd, 1, 500, NULL);
        AddClipboardFormatListener(hwnd);
        DarkTitle(hwnd);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 880;
        mmi->ptMinTrackSize.y = 540;
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillSolid(hdc, &rc, TG_BG);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc, sr, sep, tr;
        char buf[128];
        int x, cy;
        SIZE tsz;

        GetClientRect(hwnd, &rc);
        sr = rc;
        sr.top = sr.bottom - 36;
        FillSolid(hdc, &sr, RGB(20, 22, 28));
        sep.left = sr.left; sep.top = sr.top;
        sep.right = sr.right; sep.bottom = sr.top + 1;
        FillSolid(hdc, &sep, TG_BORDER);

        SelectObject(hdc, g_hFont);
        SetBkMode(hdc, TRANSPARENT);
        cy = (sr.top + sr.bottom) / 2;
        x = sr.left + 16;

        /* Active: n */
        SetTextColor(hdc, TG_TXT_MUTE);
        TextOutA(hdc, x, cy - 9, "Active:", 7);
        GetTextExtentPoint32A(hdc, "Active: ", 8, &tsz);
        x += tsz.cx;
        sprintf(buf, "%d", g_statActive);
        SetTextColor(hdc, TG_ACCENT);
        TextOutA(hdc, x, cy - 9, buf, (int)strlen(buf));
        GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &tsz);
        x += tsz.cx + 22;

        /* Speed */
        if (g_statSpeed[0]) {
            SetTextColor(hdc, TG_TXT_MUTE);
            TextOutA(hdc, x, cy - 9, "Speed:", 6);
            GetTextExtentPoint32A(hdc, "Speed: ", 7, &tsz);
            x += tsz.cx;
            SetTextColor(hdc, TG_GREEN);
            TextOutA(hdc, x, cy - 9, g_statSpeed, (int)strlen(g_statSpeed));
            GetTextExtentPoint32A(hdc, g_statSpeed, (int)strlen(g_statSpeed), &tsz);
            x += tsz.cx + 22;
        }

        /* Queued: n */
        SetTextColor(hdc, TG_TXT_MUTE);
        TextOutA(hdc, x, cy - 9, "Queued:", 7);
        GetTextExtentPoint32A(hdc, "Queued: ", 8, &tsz);
        x += tsz.cx;
        sprintf(buf, "%d", g_statQueued);
        SetTextColor(hdc, TG_ORANGE);
        TextOutA(hdc, x, cy - 9, buf, (int)strlen(buf));
        GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &tsz);
        x += tsz.cx + 22;

        /* Completed: n */
        SetTextColor(hdc, TG_TXT_MUTE);
        TextOutA(hdc, x, cy - 9, "Completed:", 10);
        GetTextExtentPoint32A(hdc, "Completed: ", 11, &tsz);
        x += tsz.cx;
        sprintf(buf, "%d", g_statDone);
        SetTextColor(hdc, TG_GREEN);
        TextOutA(hdc, x, cy - 9, buf, (int)strlen(buf));

        /* right side branding */
        lstrcpynA(buf, "TurboGrab v1.3.0", sizeof(buf));
        GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &tsz);
        SetTextColor(hdc, TG_TXT_MUTE);
        tr.left = sr.right - tsz.cx - 16;
        tr.top = cy - 9;
        tr.right = sr.right - 16;
        tr.bottom = cy + 9;
        DrawTextA(hdc, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        int w, hgt;
        if (wp == SIZE_MINIMIZED) {
            TrayAdd();
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        w = LOWORD(lp);
        hgt = HIWORD(lp);
        GetClientRect(hwnd, &rc);
        (void)rc;
        MoveWindow(g_hList, 12, 62, w - 24, hgt - 62 - 42, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            Job *j;
            DWORD now = GetTickCount();
            EnterCriticalSection(&g_cs);
            for (j = g_jobs; j; j = j->next) {
                if (j->state == ST_DOWN) {
                    DWORD dt = now - j->lastTick;
                    DWORD64 dg = (DWORD64)j->got > j->lastGot ? ((DWORD64)j->got - j->lastGot) : 0;
                    if (!j->lastTick) { j->lastTick = now; j->lastGot = (DWORD64)j->got; continue; }
                    if (dt >= 200) {
                        DWORD bps = (DWORD)(dg * 1000 / dt);
                        j->speedBps = (j->speedBps + bps) / 2;
                        j->lastTick = now;
                        j->lastGot = (DWORD64)j->got;
                        UpdateRow(j);
                    }
                } else if (j->state == ST_CONNECT) {
                    UpdateRow(j);
                }
            }
            LeaveCriticalSection(&g_cs);
            UpdateStatusLine();
        }
        return 0;
    case WM_APP_JOB: {
        Job *j = (Job *)lp;
        int terminal;
        if (!j) return 0;
        EnterCriticalSection(&g_cs);
        if (!JobInList(j)) { LeaveCriticalSection(&g_cs); return 0; }
        terminal = (j->state == ST_DONE || j->state == ST_ERROR || j->state == ST_PAUSED);
        if (terminal && j->hThread) {
            HANDLE th = j->hThread;
            j->hThread = NULL;
            LeaveCriticalSection(&g_cs);
            WaitForSingleObject(th, 10000);
            CloseHandle(th);
            EnterCriticalSection(&g_cs);
        }
        if (j->deletePending && terminal) {
            JobUnlink(j);
            LeaveCriticalSection(&g_cs);
            FreeJob(j);
            SaveJobs();
            RebuildList();
            UpdateStatusLine();
            KickQueue();
            return 0;
        }
        LeaveCriticalSection(&g_cs);
        UpdateRow(j);
        UpdateStatusLine();
        if (terminal) KickQueue();
        return 0;
    }
    case WM_APP_BROWSE: {
        BrowserReq *br = (BrowserReq *)lp;
        if (br) {
            if (br->autoStart) {
                Job *j = JobCreate(br->url, g_set.dir, NULL);
                if (j) {
                    if (br->name[0]) SetJobName(j, br->name);
                    UpdateRow(j);
                    KickQueue();
                    UpdateStatusLine();
                }
            } else {
                OpenAddPopup(br->url);
            }
            HeapFree(GetProcessHeap(), 0, br);
        }
        return 0;
    }
    case WM_APP_TRAY: {
        POINT pt;
        HMENU m;
        if (lp == WM_LBUTTONDBLCLK) { MainRestore(); return 0; }
        if (lp == WM_RBUTTONUP) {
            SetForegroundWindow(hwnd);
            m = CreatePopupMenu();
            AppendMenuA(m, MF_STRING, IDM_SHOW, "Show TurboGrab");
            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            AppendMenuA(m, MF_STRING, IDM_EXIT, "Exit");
            GetCursorPos(&pt);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
        }
        return 0;
    }
    case WM_CLIPBOARDUPDATE:
        OnClipboardChange();
        return 0;
    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lp;

        /* --- dark list header (custom draw from the header child) --- */
        if (nm->hwndFrom == ListView_GetHeader(g_hList) && nm->code == NM_CUSTOMDRAW) {
            LPNMCUSTOMDRAW hd = (LPNMCUSTOMDRAW)nm;
            if (hd->dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (hd->dwDrawStage == CDDS_ITEMPREPAINT) {
                HDC hdc = hd->hdc;
                RECT rc = hd->rc, sep;
                char buf[64];
                HDITEMA hdi;
                FillSolid(hdc, &rc, TG_HDR);
                sep.left = rc.right - 1; sep.top = rc.top;
                sep.right = rc.right; sep.bottom = rc.bottom;
                FillSolid(hdc, &sep, TG_BORDER);
                sep.left = rc.left; sep.top = rc.bottom - 1;
                sep.right = rc.right; sep.bottom = rc.bottom;
                FillSolid(hdc, &sep, TG_BORDER);
                ZeroMemory(&hdi, sizeof(hdi));
                hdi.mask = HDI_TEXT;
                hdi.pszText = buf;
                hdi.cchTextMax = sizeof(buf);
                SendMessageA(hd->hdr.hwndFrom, HDM_GETITEMA, hd->dwItemSpec, (LPARAM)&hdi);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, TG_TXT_MUTE);
                SelectObject(hdc, g_hFont);
                { RECT tr = rc; tr.left += 10; tr.right -= 8;
                  DrawTextA(hdc, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX); }
                return CDRF_SKIPDEFAULT;
            }
            return CDRF_DODEFAULT;
        }

        if (nm->idFrom == IDC_LIST && nm->code == NM_CUSTOMDRAW) {
            LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)nm;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                return CDRF_NOTIFYSUBITEMDRAW;
            if (cd->nmcd.dwDrawStage == (CDDS_SUBITEM | CDDS_ITEMPREPAINT)) {
                Job *j;
                LVITEMA li;
                HDC hdc = cd->nmcd.hdc;
                int item = (int)cd->nmcd.dwItemSpec;
                BOOL sel = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;

                ZeroMemory(&li, sizeof(li));
                li.mask = LVIF_PARAM;
                li.iItem = item;
                j = NULL;
                if (SendMessageA(g_hList, LVM_GETITEMA, 0, (LPARAM)&li))
                    j = (Job *)li.lParam;

                /* row background (once per row, on the first subitem) */
                if (cd->iSubItem == 0) {
                    RECT rr;
                    ListView_GetItemRect(g_hList, item, &rr, LVIR_BOUNDS);
                    FillSolid(hdc, &rr, sel ? TG_ROWSEL : ((item & 1) ? TG_ROW2 : TG_ROW1));
                    if (sel) {
                        RECT ab = { rr.left, rr.top, rr.left + 3, rr.bottom };
                        FillSolid(hdc, &ab, TG_ACCENT);
                    }
                }
                if (!j) return CDRF_SKIPDEFAULT;

                SetBkMode(hdc, TRANSPARENT);

                if (cd->iSubItem == 0) {
                    /* file name: bold white */
                    RECT rc;
                    char disp[320];
                    ListView_GetItemRect(g_hList, item, &rc, LVIR_BOUNDS);
                    rc.left += 12;
                    rc.right = rc.left + ListView_GetColumnWidth(g_hList, 0) - 22;
                    ListView_GetItemText(g_hList, item, 0, disp, sizeof(disp));
                    SelectObject(hdc, g_hFontB);
                    SetTextColor(hdc, sel ? TG_TXT : TG_TXT);
                    DrawTextA(hdc, disp, -1, &rc,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    return CDRF_SKIPDEFAULT;
                }
                if (cd->iSubItem == 2) {
                    /* progress bar */
                    RECT rc, bar;
                    DWORD64 got, total;
                    int pct = -1;
                    char txt[64];
                    COLORREF fillc;
                    SIZE tsz;
                    HGDIOBJ of;

                    ListView_GetSubItemRect(g_hList, item, 2, LVIR_BOUNDS, &rc);
                    got = (DWORD64)j->got;
                    total = j->total;
                    if (j->state == ST_DONE && total == 0) total = got;
                    if (total > 0) pct = (int)(got * 100 / total);

                    bar = rc;
                    bar.left += 6; bar.right -= 8;
                    bar.top = (rc.top + rc.bottom) / 2 - 9;
                    bar.bottom = bar.top + 18;
                    FillRoundRect(hdc, &bar, 8, RGB(44, 49, 61), TG_BORDER);
                    if (pct > 0) {
                        RECT fillr = bar;
                        fillr.right = fillr.left + (int)((bar.right - bar.left - 2) * pct / 100);
                        if (fillr.right > fillr.left + 2) {
                            fillr.left += 1; fillr.right -= 0;
                            fillr.top += 1; fillr.bottom -= 1;
                            fillc = j->state == ST_ERROR ? TG_RED
                                  : j->state == ST_PAUSED ? TG_ORANGE
                                  : j->state == ST_DONE ? TG_GREEN : TG_ACCENT;
                            FillSolid(hdc, &fillr, fillc);
                        }
                    }
                    if (total > 0) sprintf(txt, "%d%%", pct);
                    else if (j->state == ST_DOWN || j->state == ST_PAUSED || j->state == ST_DONE) {
                        FormatSize(got, txt, sizeof(txt));
                    } else txt[0] = 0;
                    if (txt[0]) {
                        GetTextExtentPoint32A(hdc, txt, (int)strlen(txt), &tsz);
                        of = SelectObject(hdc, g_hFontB);
                        SetTextColor(hdc, TG_TXT);
                        DrawTextA(hdc, txt, -1, &bar, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                        SelectObject(hdc, of);
                    }
                    return CDRF_SKIPDEFAULT;
                }
                if (cd->iSubItem == 5) {
                    /* status: colored dot + text */
                    RECT rc, dot;
                    char disp[200];
                    COLORREF c;
                    HGDIOBJ of, ob;

                    switch (j->state) {
                    case ST_CONNECT:
                    case ST_DOWN:    c = TG_ACCENT;  break;
                    case ST_QUEUED:  c = TG_ORANGE;  break;
                    case ST_PAUSED:  c = TG_ORANGE;  break;
                    case ST_DONE:    c = TG_GREEN;   break;
                    case ST_ERROR:   c = TG_RED;     break;
                    default:         c = TG_TXT_DIM; break;
                    }
                    ListView_GetSubItemRect(g_hList, item, 5, LVIR_BOUNDS, &rc);
                    rc.left += 8;
                    dot.top = (rc.top + rc.bottom) / 2 - 3;
                    dot.bottom = dot.top + 6;
                    dot.left = rc.left;
                    dot.right = dot.left + 6;
                    ob = SelectObject(hdc, CreateSolidBrush(c));
                    SelectObject(hdc, GetStockObject(NULL_PEN));
                    Ellipse(hdc, dot.left, dot.top, dot.right + 1, dot.bottom + 1);
                    DeleteObject(SelectObject(hdc, ob));
                    rc.left += 12;
                    rc.right -= 6;
                    lstrcpynA(disp, StateText(j), sizeof(disp));
                    of = SelectObject(hdc, g_hFont);
                    SetTextColor(hdc, c);
                    DrawTextA(hdc, disp, -1, &rc,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    SelectObject(hdc, of);
                    return CDRF_SKIPDEFAULT;
                }
                {
                    /* remaining columns: dim text */
                    RECT rc;
                    char disp[340];
                    if (cd->iSubItem > 6) return CDRF_DODEFAULT;
                    ListView_GetSubItemRect(g_hList, item, cd->iSubItem, LVIR_BOUNDS, &rc);
                    if (rc.left < 0) return CDRF_DODEFAULT;
                    rc.left += 8;
                    rc.right -= 6;
                    disp[0] = 0;
                    ListView_GetItemText(g_hList, item, cd->iSubItem, disp, sizeof(disp));
                    SelectObject(hdc, g_hFont);
                    SetTextColor(hdc, TG_TXT_DIM);
                    DrawTextA(hdc, disp, -1, &rc,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    return CDRF_SKIPDEFAULT;
                }
            }
            return CDRF_DODEFAULT;
        }
        if (nm->idFrom == IDC_LIST && nm->code == NM_DBLCLK) {
            Job *j = SelectedJob();
            if (j) JobOpenFile(j);
            return 0;
        }
        if (nm->idFrom == IDC_LIST && nm->code == NM_RCLICK) {
            LPNMITEMACTIVATE ia = (LPNMITEMACTIVATE)lp;
            Job *j;
            LVHITTESTINFO ht;
            HMENU m;
            DWORD mp = GetMessagePos();
            POINT pt;
            int idx;

            ZeroMemory(&ht, sizeof(ht));
            ht.pt.x = GET_X_LPARAM(mp);
            ht.pt.y = GET_Y_LPARAM(mp);
            ScreenToClient(g_hList, &ht.pt);
            idx = ListView_HitTest(g_hList, &ht);
            if (idx < 0) return 0;
            ListView_SetItemState(g_hList, idx,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            j = SelectedJob();
            if (!j) return 0;
            m = CreatePopupMenu();
            if (j->state == ST_PAUSED || j->state == ST_ERROR)
                AppendMenuA(m, MF_STRING, IDM_RESUME, "Resume");
            if (j->state == ST_DOWN || j->state == ST_CONNECT || j->state == ST_QUEUED)
                AppendMenuA(m, MF_STRING, IDM_PAUSE, "Pause");
            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            AppendMenuA(m, MF_STRING, IDM_OPEN, "Open folder");
            AppendMenuA(m, MF_STRING, IDM_COPYURL, "Copy URL");
            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            AppendMenuA(m, MF_STRING, IDM_DEL, "Delete from list");
            pt.x = GET_X_LPARAM(mp);
            pt.y = GET_Y_LPARAM(mp);
            ClientToScreen(g_hList, &pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
            (void)ia;
            return 0;
        }
        break;
    }
    case WM_DRAWITEM:
        SkinDrawBtn((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ADD:
        case IDM_SHOW:
            MainRestore();
            if (wp == IDC_ADD) OpenAddPopup(NULL);
            return 0;
        case IDM_EXIT:
            SendMessageA(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case IDC_RESUME: case IDM_RESUME: JobResume(SelectedJob()); return 0;
        case IDC_PAUSE:  case IDM_PAUSE:  JobPause(SelectedJob());  return 0;
        case IDC_DELETE: case IDM_DEL:    JobDelete(SelectedJob()); return 0;
        case IDC_FOLDER: case IDM_OPEN:   JobOpenFolder(SelectedJob()); return 0;
        case IDC_SET:    OpenSetPopup(); return 0;
        case IDC_BROWSERS: OpenBrowsersPopup(); return 0;
        case IDM_COPYURL: JobCopyUrl(SelectedJob()); return 0;
        }
        break;
    case WM_CLOSE: {
        int act;
        Job *j;
        EnterCriticalSection(&g_cs);
        act = ActiveCount();
        if (act > 0) {
            int ans = MessageBoxA(hwnd,
                "Downloads are running. Pause them and exit?\n"
                "(They will resume next time you start TurboGrab.)",
                APP_TITLE, MB_YESNO | MB_ICONQUESTION);
            if (ans != IDYES) { LeaveCriticalSection(&g_cs); return 0; }
            for (j = g_jobs; j; j = j->next)
                if (j->state == ST_DOWN || j->state == ST_CONNECT)
                    InterlockedExchange(&j->pauseFlag, 1);
            {
                DWORD t0 = GetTickCount();
                while (ActiveCount() > 0 && GetTickCount() - t0 < 4000) {
                    LeaveCriticalSection(&g_cs);
                    Sleep(120);
                    EnterCriticalSection(&g_cs);
                }
            }
        }
        SaveJobs();
        LeaveCriticalSection(&g_cs);
        TrayRemove();
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        RemoveClipboardFormatListener(hwnd);
        BrowserSrvStop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ============================= main ============================= */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)
{
    WNDCLASSA wc, pc;
    MSG msg;

    (void)hPrev; (void)cmdLine;
    InitializeCriticalSection(&g_cs);
    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
        InitCommonControlsEx(&icc);
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    BuildIniPath();
    LoadSettings();
    LoadJobs();
    BrowserTokenEnsure();

    /* self-healing: every start re-registers the extension for all browsers,
     * even when the installer's integration task was skipped */
    if (g_browserOn) BrowsersApplyAll();

    g_hFont = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_hFontB = CreateFontA(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    g_hIcon = (HICON)LoadImageA(hInst, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                32, 32, LR_DEFAULTCOLOR);
    if (!g_hIcon) g_hIcon = LoadIcon(NULL, IDI_APPLICATION);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = g_hIcon;
    wc.hbrBackground = NULL;              /* fully custom-painted background */
    wc.lpszClassName = APP_CLASS;
    RegisterClassA(&wc);

    ZeroMemory(&pc, sizeof(pc));
    pc.lpfnWndProc = AddWndProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    pc.hIcon = g_hIcon;
    pc.hbrBackground = NULL;
    pc.lpszClassName = ADDPOP_CLASS;
    RegisterClassA(&pc);

    ZeroMemory(&pc, sizeof(pc));
    pc.lpfnWndProc = SetWndProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    pc.hIcon = g_hIcon;
    pc.hbrBackground = NULL;
    pc.lpszClassName = SETPOP_CLASS;
    RegisterClassA(&pc);

    ZeroMemory(&pc, sizeof(pc));
    pc.lpfnWndProc = BrowsWndProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    pc.hIcon = g_hIcon;
    pc.hbrBackground = NULL;
    pc.lpszClassName = BRW_CLASS;
    RegisterClassA(&pc);

    g_hMain = CreateWindowA(APP_CLASS, APP_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1080, 660, NULL, NULL, hInst, NULL);
    if (!g_hMain) return 1;

    KickQueue();
    UpdateStatusLine();

    ShowWindow(g_hMain, nShow);
    UpdateWindow(g_hMain);
    BrowserSrvApply();

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    DeleteCriticalSection(&g_cs);
    CoUninitialize();
    return (int)msg.wParam;
}
