/* TurboGrab v1.0.0 — free & open download accelerator for Windows
 * Multi-segment HTTP(S) downloads with pause/resume, queue, clipboard
 * monitoring and category folders. Clean-room C (Win32 + WinHTTP).
 *
 * MIT License — Copyright (c) 2026 Abad Umair Channa / 3S Verse
 * https://github.com/abaduchanna/3SVerse_TurboGrab
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#define APP_TITLE   "TurboGrab v1.0.0"
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
#define IDC_STATUS  208

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

static HWND  g_hMain, g_hList, g_hStatus;
static HWND  g_hAddWnd, g_hSetWnd;
static HFONT g_hFont, g_hFontB;
static HICON g_hIcon;

static Job  *g_jobs = NULL;
static int   g_count = 0;
static CRITICAL_SECTION g_cs;

static NOTIFYICONDATAA g_nid;
static BOOL  g_trayOn = FALSE;
static char  g_iniPath[MAX_PATH];
static DWORD g_lastClipTick = 0;
static char  g_clipLast[2048];

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
    char sb[64], text[512];

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
        sprintf(text, "Active: %d   Speed: %s   Queued: %d   Completed: %d",
                active, sb, queued, done);
    } else {
        sprintf(text, "Active: %d   Queued: %d   Completed: %d", active, queued, done);
    }
    SetWindowTextA(g_hStatus, text);
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

        CreateWindowA("STATIC", "URL:", WS_CHILD | WS_VISIBLE,
            12, 14, 60, 18, hw, NULL, h, NULL);
        c = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            12, 34, 500, 24, hw, (HMENU)IDC_AURL, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        CreateWindowA("STATIC", "Save to:", WS_CHILD | WS_VISIBLE,
            12, 68, 60, 18, hw, NULL, h, NULL);
        c = CreateWindowA("EDIT", g_set.dir, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            12, 88, 380, 24, hw, (HMENU)IDC_ADIR, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        CreateWindowA("BUTTON", "Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            402, 86, 110, 27, hw, (HMENU)IDC_ABROWSE, h, NULL);

        CreateWindowA("BUTTON", "Start Download", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            12, 128, 130, 30, hw, (HMENU)IDC_ASTART, h, NULL);
        CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            152, 128, 80, 30, hw, (HMENU)IDC_ACANCEL, h, NULL);
        return 0;
    }
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
        r.left + 60, r.top + 80, 540, 205, g_hMain, NULL,
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
    SaveSettings();
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
            12, 14, 220, 18, hw, NULL, h, NULL);
        sprintf(b, "%d", g_set.segments);
        c = CreateWindowA("EDIT", b, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            250, 12, 60, 24, hw, (HMENU)IDC_SSEG, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        CreateWindowA("STATIC", "Downloads at the same time (1-10):", WS_CHILD | WS_VISIBLE,
            12, 46, 220, 18, hw, NULL, h, NULL);
        sprintf(b, "%d", g_set.concurrent);
        c = CreateWindowA("EDIT", b, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            250, 44, 60, 24, hw, (HMENU)IDC_SCON, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        CreateWindowA("STATIC", "Default download folder:", WS_CHILD | WS_VISIBLE,
            12, 82, 220, 18, hw, NULL, h, NULL);
        c = CreateWindowA("EDIT", g_set.dir, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            12, 102, 330, 24, hw, (HMENU)IDC_SDIR, h, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        CreateWindowA("BUTTON", "Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            352, 100, 110, 27, hw, (HMENU)IDC_SBROWSE, h, NULL);

        c = CreateWindowA("BUTTON", "Watch clipboard for download links",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            12, 142, 320, 22, hw, (HMENU)IDC_SCLIP, h, NULL);
        SendMessageA(c, BM_SETCHECK, g_set.clipboard ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        c = CreateWindowA("BUTTON", "Sort files into category folders",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            12, 168, 320, 22, hw, (HMENU)IDC_SCAT, h, NULL);
        SendMessageA(c, BM_SETCHECK, g_set.categories ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        CreateWindowA("BUTTON", "Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            12, 204, 90, 30, hw, (HMENU)IDC_SSAVE, h, NULL);
        CreateWindowA("BUTTON", "Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            112, 204, 90, 30, hw, (HMENU)IDC_SCLOSE, h, NULL);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SSAVE:   SettingsSave(hw); return 0;
        case IDC_SCLOSE:  PopupClose(hw, &g_hSetWnd); return 0;
        case IDC_SBROWSE: BrowseForDir(hw, IDC_SDIR); return 0;
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
        r.left + 80, r.top + 60, 500, 280, g_hMain, NULL,
        (HINSTANCE)GetWindowLongPtrA(g_hMain, GWLP_HINSTANCE), NULL);
    if (g_hSetWnd) {
        ShowWindow(g_hSetWnd, SW_SHOW);
        SetForegroundWindow(g_hSetWnd);
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
        HWND b;
        LVCOLUMNA lvc;
        Job *j;
        int i, n;
        static const char *cols[7] = { "File", "Size", "Progress", "Speed", "Time left", "Status", "Save to" };
        static const int colw[7] = { 200, 84, 150, 92, 84, 150, 190 };

        b = CreateWindowA("BUTTON", "Add URL", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            12, 12, 96, 30, hwnd, (HMENU)IDC_ADD, h, NULL);
        SendMessageA(b, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        b = CreateWindowA("BUTTON", "Resume", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            114, 12, 86, 30, hwnd, (HMENU)IDC_RESUME, h, NULL);
        SendMessageA(b, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        b = CreateWindowA("BUTTON", "Pause", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            206, 12, 86, 30, hwnd, (HMENU)IDC_PAUSE, h, NULL);
        SendMessageA(b, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        b = CreateWindowA("BUTTON", "Delete", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            298, 12, 86, 30, hwnd, (HMENU)IDC_DELETE, h, NULL);
        SendMessageA(b, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        b = CreateWindowA("BUTTON", "Open Folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            390, 12, 106, 30, hwnd, (HMENU)IDC_FOLDER, h, NULL);
        SendMessageA(b, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        b = CreateWindowA("BUTTON", "Settings", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            502, 12, 86, 30, hwnd, (HMENU)IDC_SET, h, NULL);
        SendMessageA(b, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hList = CreateWindowA(WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            12, 52, 990, 420, hwnd, (HMENU)IDC_LIST, h, NULL);
        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        ZeroMemory(&lvc, sizeof(lvc));
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        for (i = 0; i < 7; i++) {
            lvc.iSubItem = i;
            lvc.pszText = (LPSTR)cols[i];
            lvc.cx = colw[i];
            ListView_InsertColumn(g_hList, i, &lvc);
        }
        SendMessageA(g_hList, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hStatus = CreateWindowA("STATIC", "Ready.", WS_CHILD | WS_VISIBLE | SS_LEFT,
            12, 0, 990, 22, hwnd, (HMENU)IDC_STATUS, h, NULL);
        SendMessageA(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        n = 0;
        EnterCriticalSection(&g_cs);
        for (j = g_jobs; j; j = j->next) n++;
        LeaveCriticalSection(&g_cs);
        (void)n;
        RebuildList();

        SetTimer(hwnd, 1, 500, NULL);
        AddClipboardFormatListener(hwnd);
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
        MoveWindow(g_hList, 12, 52, w - 24, hgt - 52 - 34, TRUE);
        MoveWindow(g_hStatus, 12, hgt - 26, w - 24, 22, TRUE);
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
        if (nm->idFrom == IDC_LIST && nm->code == NM_CUSTOMDRAW) {
            LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)nm;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                return CDRF_NOTIFYSUBITEMDRAW;
            if (cd->nmcd.dwDrawStage == (CDDS_SUBITEM | CDDS_ITEMPREPAINT)) {
                if (cd->iSubItem == 2) {
                    Job *j;
                    LVITEMA li;
                    HDC hdc = cd->nmcd.hdc;
                    RECT rc, bar;
                    DWORD64 got, total;
                    int pct = -1;
                    char txt[64];
                    HBRUSH bg, fg;
                    SIZE tsz;
                    HGDIOBJ of;

                    ZeroMemory(&li, sizeof(li));
                    li.mask = LVIF_PARAM;
                    li.iItem = (int)cd->nmcd.dwItemSpec;
                    j = NULL;
                    if (SendMessageA(g_hList, LVM_GETITEMA, 0, (LPARAM)&li))
                        j = (Job *)li.lParam;
                    ListView_GetSubItemRect(g_hList, (int)cd->nmcd.dwItemSpec, 2, LVIR_BOUNDS, &rc);
                    if (!j) return CDRF_SKIPDEFAULT;
                    got = (DWORD64)j->got;
                    total = j->total;
                    if (j->state == ST_DONE && total == 0) total = got;
                    if (total > 0) pct = (int)(got * 100 / total);

                    bg = (cd->nmcd.uItemState & CDIS_SELECTED)
                       ? GetSysColorBrush(COLOR_HIGHLIGHT) : GetSysColorBrush(COLOR_WINDOW);
                    FillRect(hdc, &rc, bg);
                    bar = rc;
                    bar.left += 4; bar.right -= 4;
                    InflateRect(&bar, 0, -6);
                    if (bar.bottom - bar.top > 16) {
                        int cy = (bar.top + bar.bottom) / 2;
                        bar.top = cy - 8;
                        bar.bottom = cy + 8;
                    }
                    FillRect(hdc, &bar, GetSysColorBrush(COLOR_3DFACE));
                    FrameRect(hdc, &bar, GetSysColorBrush(COLOR_3DSHADOW));
                    if (pct > 0) {
                        RECT fillr = bar;
                        fillr.right = fillr.left + (int)((bar.right - bar.left) * pct / 100);
                        if (fillr.right > fillr.left + 1) {
                            fg = CreateSolidBrush(j->state == ST_ERROR ? RGB(200, 60, 60) : RGB(28, 120, 220));
                            InflateRect(&fillr, -1, -1);
                            FillRect(hdc, &fillr, fg);
                            DeleteObject(fg);
                        }
                    }
                    if (total > 0) sprintf(txt, "%d%%", pct);
                    else if (j->state == ST_DOWN || j->state == ST_PAUSED || j->state == ST_DONE) {
                        FormatSize(got, txt, sizeof(txt));
                    } else txt[0] = 0;
                    if (txt[0]) {
                        GetTextExtentPoint32A(hdc, txt, (int)strlen(txt), &tsz);
                        SetBkMode(hdc, TRANSPARENT);
                        SetTextColor(hdc, RGB(40, 40, 40));
                        of = SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
                        DrawTextA(hdc, txt, -1, &bar, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                        SelectObject(hdc, of);
                    }
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
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
        icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    BuildIniPath();
    LoadSettings();
    LoadJobs();

    g_hFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_hFontB = CreateFontA(16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    (void)g_hFontB;

    g_hIcon = (HICON)LoadImageA(hInst, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                32, 32, LR_DEFAULTCOLOR);
    if (!g_hIcon) g_hIcon = LoadIcon(NULL, IDI_APPLICATION);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = g_hIcon;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = APP_CLASS;
    RegisterClassA(&wc);

    ZeroMemory(&pc, sizeof(pc));
    pc.lpfnWndProc = AddWndProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    pc.hIcon = g_hIcon;
    pc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    pc.lpszClassName = ADDPOP_CLASS;
    RegisterClassA(&pc);

    ZeroMemory(&pc, sizeof(pc));
    pc.lpfnWndProc = SetWndProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    pc.hIcon = g_hIcon;
    pc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    pc.lpszClassName = SETPOP_CLASS;
    RegisterClassA(&pc);

    g_hMain = CreateWindowA(APP_CLASS, APP_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1040, 580, NULL, NULL, hInst, NULL);
    if (!g_hMain) return 1;

    KickQueue();
    UpdateStatusLine();

    ShowWindow(g_hMain, nShow);
    UpdateWindow(g_hMain);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    DeleteCriticalSection(&g_cs);
    CoUninitialize();
    return (int)msg.wParam;
}
