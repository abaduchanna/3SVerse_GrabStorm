/* GrabStorm — engine.c — multi-segment HTTP(S) download engine (WinHTTP)
 * Dynamic file segmentation (a finished connection steals the tail of the
 * largest remaining segment, the classic download-manager technique),
 * .gstmeta resume, retry with backoff, queue with max-active scheduling.
 * MIT — (c) 2026 Abad Umair Channa / 3S Verse
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include "engine.h"
#include "util.h"

static HWND g_ui = NULL;
static Job *g_jobs = NULL;        /* UI thread owns list structure mutations */
static int  g_nextId = 1;
static int  g_segments = 8;
static int  g_maxActive = 3;
static int  g_active = 0, g_queued = 0, g_done = 0;
static double g_agg = 0.0;
static ULONGLONG g_lastCheckpoint = 0;

#define QUEUE_MAGIC   0x31515347u /* GSQ1 */
#define QUEUE_VERSION 1u

typedef struct {
    DWORD magic, version, count, nextId;
} QUEUEHDR;

typedef struct {
    int id, state;
    unsigned __int64 total, got;
    wchar_t url[2048], referrer[2048], cookie[1024];
    wchar_t file[MAX_PATH], dir[MAX_PATH];
    wchar_t etag[128], lastmod[96], err[256];
} QUEUEREC;

static void QueuePath(wchar_t *out, int cap)
{
    wchar_t base[MAX_PATH], d1[MAX_PATH], d2[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        out[0] = 0;
        return;
    }
    _snwprintf(d1, MAX_PATH, L"%s\\3SVerse", base);
    _snwprintf(d2, MAX_PATH, L"%s\\GrabStorm", d1);
    CreateDirectoryW(d1, NULL);
    CreateDirectoryW(d2, NULL);
    _snwprintf(out, cap, L"%s\\queue.dat", d2);
    out[cap - 1] = 0;
}

static void InitLoadedJob(Job *j)
{
    InitializeCriticalSection(&j->cs);
    j->cancelEvt = CreateEventW(NULL, TRUE, FALSE, NULL);
    j->hThread = NULL;
    j->resume = 1;
    j->nseg = g_segments;
    if (j->state == ST_CONNECTING || j->state == ST_DOWNLOADING)
        j->state = ST_PAUSED;
}

static void LoadQueueState(void)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    QUEUEHDR h;
    DWORD i;
    Job *tail = NULL;
    QueuePath(path, MAX_PATH);
    if (!path[0] || !(f = _wfopen(path, L"rb"))) return;
    if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != QUEUE_MAGIC ||
        h.version != QUEUE_VERSION || h.count > 10000) {
        fclose(f);
        return;
    }
    for (i = 0; i < h.count; i++) {
        QUEUEREC r;
        Job *j;
        if (fread(&r, sizeof(r), 1, f) != 1) break;
        r.url[2047] = r.referrer[2047] = r.cookie[1023] = 0;
        r.file[MAX_PATH - 1] = r.dir[MAX_PATH - 1] = 0;
        r.etag[127] = r.lastmod[95] = r.err[255] = 0;
        if (!r.url[0]) continue;
        j = (Job *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Job));
        if (!j) break;
        j->id = r.id;
        j->state = r.state;
        j->total = r.total;
        j->got = (LONG64)r.got;
        lstrcpynW(j->url, r.url, 2048);
        lstrcpynW(j->referrer, r.referrer, 2048);
        lstrcpynW(j->cookie, r.cookie, 1024);
        lstrcpynW(j->file, r.file, MAX_PATH);
        lstrcpynW(j->dir, r.dir, MAX_PATH);
        lstrcpynW(j->etag, r.etag, 128);
        lstrcpynW(j->lastmod, r.lastmod, 96);
        lstrcpynW(j->err, r.err, 256);
        InitLoadedJob(j);
        if (tail) tail->next = j; else g_jobs = j;
        tail = j;
        if (j->id >= g_nextId) g_nextId = j->id + 1;
    }
    if ((int)h.nextId > g_nextId) g_nextId = (int)h.nextId;
    fclose(f);
}

void EnginePersist(void)
{
    wchar_t path[MAX_PATH], tmp[MAX_PATH + 8];
    FILE *f;
    Job *j;
    QUEUEHDR h;
    QueuePath(path, MAX_PATH);
    if (!path[0]) return;
    _snwprintf(tmp, MAX_PATH + 8, L"%s.tmp", path);
    f = _wfopen(tmp, L"wb");
    if (!f) return;
    h.magic = QUEUE_MAGIC;
    h.version = QUEUE_VERSION;
    h.count = (DWORD)EngineCount();
    h.nextId = (DWORD)g_nextId;
    if (fwrite(&h, sizeof(h), 1, f) != 1) { fclose(f); DeleteFileW(tmp); return; }
    for (j = g_jobs; j; j = j->next) {
        QUEUEREC r;
        ZeroMemory(&r, sizeof(r));
        r.id = j->id; r.state = j->state;
        r.total = j->total; r.got = (unsigned __int64)j->got;
        lstrcpynW(r.url, j->url, 2048);
        lstrcpynW(r.referrer, j->referrer, 2048);
        /* Session cookies are intentionally not persisted as plaintext. */
        lstrcpynW(r.file, j->file, MAX_PATH);
        lstrcpynW(r.dir, j->dir, MAX_PATH);
        lstrcpynW(r.etag, j->etag, 128);
        lstrcpynW(r.lastmod, j->lastmod, 96);
        lstrcpynW(r.err, j->err, 256);
        if (fwrite(&r, sizeof(r), 1, f) != 1) { fclose(f); DeleteFileW(tmp); return; }
    }
    fflush(f);
    fclose(f);
    MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

/* ------------------------------------------------------------------ */
/* settings                                                           */
/* ------------------------------------------------------------------ */
void EngineSetSegments(int n)
{
    if (n < 1) n = 1;
    if (n > GS_MAXSEG) n = GS_MAXSEG;
    g_segments = n;
    SettingsSetInt(L"Segments", n);
}
void EngineSetMaxActive(int n)
{
    if (n < 1) n = 1;
    if (n > 10) n = 10;
    g_maxActive = n;
    SettingsSetInt(L"MaxActive", n);
}
int EngineGetSegments(void)   { return g_segments; }
int EngineGetMaxActive(void)  { return g_maxActive; }

void EngineDefaultDir(wchar_t *out, int cap)
{
    wchar_t home[MAX_PATH];
    if (SettingsGetStr(L"Dir", out, cap, NULL) && out[0]) return;
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH))
        _snwprintf(out, cap, L"%s\\Downloads", home);
    else
        lstrcpynW(out, L"C:\\Users\\Public\\Downloads", cap);
}

void EngineInit(HWND main)
{
    g_ui = main;
    g_jobs = NULL;
    g_nextId = 1;
    g_segments = SettingsGetInt(L"Segments", 8);
    if (g_segments < 1) g_segments = 1;
    if (g_segments > GS_MAXSEG) g_segments = GS_MAXSEG;
    g_maxActive = SettingsGetInt(L"MaxActive", 3);
    if (g_maxActive < 1) g_maxActive = 1;
    if (g_maxActive > 10) g_maxActive = 10;
    LoadQueueState();
}

int EngineCount(void)
{
    Job *j; int n = 0;
    for (j = g_jobs; j; j = j->next) n++;
    return n;
}
Job *EngineFirst(void)   { return g_jobs; }
Job *EngineById(int id)
{
    Job *j;
    for (j = g_jobs; j; j = j->next) if (j->id == id) return j;
    return NULL;
}
Job *EngineAt(int index)
{
    Job *j; int i = 0;
    for (j = g_jobs; j; j = j->next, i++) if (i == index) return j;
    return NULL;
}
double EngineAggSpeed(void)   { return g_agg; }
int EngineActiveCount(void)   { return g_active; }
int EngineQueuedCount(void)   { return g_queued; }
int EngineDoneCount(void)     { return g_done; }

/* ------------------------------------------------------------------ */
/* URL helpers                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    wchar_t host[256];
    wchar_t path[2048];
    INTERNET_PORT port;
    BOOL https;
} URLPARTS;

static BOOL CrackUrl(const wchar_t *url, URLPARTS *up)
{
    URL_COMPONENTSW uc;
    wchar_t host[256], path[1024], extra[512];
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;      uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path;      uc.dwUrlPathLength  = 1024;
    uc.lpszExtraInfo = extra;    uc.dwExtraInfoLength = 512;
    if (!WinHttpCrackUrl(url, (DWORD)wcslen(url), 0, &uc)) return FALSE;
    lstrcpynW(up->host, host, 256);
    _snwprintf(up->path, 2048, L"%s%s", path, extra);
    up->path[2047] = 0;
    up->port = uc.nPort;
    up->https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return TRUE;
}

static HINTERNET OpenSession(void)
{
    HINTERNET s = WinHttpOpen(L"GrabStorm/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 10000, 15000, 15000, 40000);
    return s;
}

static HINTERNET OpenRequest(HINTERNET c, const URLPARTS *up, const wchar_t *method,
                            const wchar_t *range, const Job *j)
{
    DWORD flags = up->https ? WINHTTP_FLAG_SECURE : 0;
    DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    HINTERNET r = WinHttpOpenRequest(c, method ? method : L"GET", up->path, NULL,
                                     (j && j->referrer[0]) ? j->referrer : WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!r) return NULL;
    WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof(pol));
    if (range && range[0]) {
        wchar_t hdr[160];
        _snwprintf(hdr, 160, L"Range: %s\r\n", range);
        WinHttpAddRequestHeaders(r, hdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (j && j->cookie[0]) {
        wchar_t hdr[1200];
        _snwprintf(hdr, 1200, L"Cookie: %s\r\n", j->cookie);
        WinHttpAddRequestHeaders(r, hdr, (DWORD)-1,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    if (!WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r, NULL)) {
        WinHttpCloseHandle(r);
        return NULL;
    }
    return r;
}

static int QueryStatus(HINTERNET r)
{
    DWORD code = 0, cb = sizeof(code);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &cb, WINHTTP_NO_HEADER_INDEX);
    return (int)code;
}

static BOOL QueryHeaderStr(HINTERNET r, DWORD what, wchar_t *out, DWORD cap)
{
    DWORD cb = cap * sizeof(WCHAR);
    out[0] = 0;
    return WinHttpQueryHeaders(r, what, WINHTTP_HEADER_NAME_BY_INDEX, out, &cb,
                               WINHTTP_NO_HEADER_INDEX) && out[0];
}

/* pull filename= out of a Content-Disposition value */
static void FilenameFromCD(const wchar_t *cd, wchar_t *out, int cap)
{
    const wchar_t *f = wcsstr(cd, L"filename*=");
    const wchar_t *e;
    if (f) {
        f += 10;
        e = wcsstr(f, L"''");
        if (e) { PercentDecodeW(e + 2, out, cap); SanitizeFilenameW(out, cap); return; }
    }
    f = wcsstr(cd, L"filename=");
    if (f) {
        f += 9;
        if (*f == L'"') {
            f++;
            e = wcschr(f, L'"');
        } else {
            e = f;
            while (*e && *e != L';' && *e != L' ') e++;
        }
        if (e && e > f) {
            int n = (int)(e - f);
            if (n > cap - 1) n = cap - 1;
            memcpy(out, f, (size_t)n * sizeof(WCHAR));
            out[n] = 0;
            SanitizeFilenameW(out, cap);
            return;
        }
    }
    out[0] = 0;
}

/* ------------------------------------------------------------------ */
/* probe (HEAD, fallback range-GET)                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    ULONGLONG total;
    int ranges;
    wchar_t etag[128], lastmod[96], fname[160];
    int status;
} PROBE;

static BOOL Probe(Job *j, PROBE *pr)
{
    URLPARTS up;
    HINTERNET s = NULL, c = NULL, r = NULL;
    BOOL ok = FALSE, probedHead = FALSE;
    int code;
    ZeroMemory(pr, sizeof(*pr));

    if (!CrackUrl(j->url, &up)) { lstrcpynW(j->err, L"Invalid URL", 256); return FALSE; }
    s = OpenSession();
    if (!s) { lstrcpynW(j->err, L"WinHTTP init failed", 256); goto done; }
    c = WinHttpConnect(s, up.host, up.port, 0);
    if (!c) { lstrcpynW(j->err, L"Connection failed", 256); goto done; }

    /* HEAD probe: server may reject it (405/501) — range-GET fallback below */
    {
        DWORD flags = up.https ? WINHTTP_FLAG_SECURE : 0;
        DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        r = WinHttpOpenRequest(c, L"HEAD", up.path, NULL,
                               j->referrer[0] ? j->referrer : WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (r) {
            WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof(pol));
            if (j->cookie[0]) {
                wchar_t hdr[1200];
                _snwprintf(hdr, 1200, L"Cookie: %s\r\n", j->cookie);
                WinHttpAddRequestHeaders(r, hdr, (DWORD)-1,
                                         WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            }
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(r, NULL)) {
                code = QueryStatus(r);
                pr->status = code;
                if (code == 200) {
                    wchar_t len[64], acc[32], cd[512];
                    probedHead = TRUE;
                    if (QueryHeaderStr(r, WINHTTP_QUERY_CONTENT_LENGTH, len, 64))
                        pr->total = wcstoull(len, NULL, 10);
                    pr->ranges = -1;
                    if (QueryHeaderStr(r, WINHTTP_QUERY_ACCEPT_RANGES, acc, 32) &&
                        wcsstr(acc, L"bytes")) pr->ranges = 1;
                    QueryHeaderStr(r, WINHTTP_QUERY_ETAG, pr->etag, 128);
                    QueryHeaderStr(r, WINHTTP_QUERY_LAST_MODIFIED, pr->lastmod, 96);
                    if (QueryHeaderStr(r, WINHTTP_QUERY_CONTENT_DISPOSITION, cd, 512))
                        FilenameFromCD(cd, pr->fname, 160);
                    ok = TRUE;
                }
            }
            WinHttpCloseHandle(r);
            r = NULL;
        }
    }

    if (!ok || pr->ranges != 1) {
        /* range-GET probe: bytes=0-0 */
        r = OpenRequest(c, &up, L"GET", L"bytes=0-0", j);
        if (r) {
            code = QueryStatus(r);
            if (code == 206) {
                wchar_t cr[128], cd[512];
                pr->ranges = 1;
                if (QueryHeaderStr(r, WINHTTP_QUERY_CONTENT_RANGE, cr, 128)) {
                    const wchar_t *sl = wcschr(cr, L'/');
                    if (sl && sl[1] && sl[1] != L'*')
                        pr->total = wcstoull(sl + 1, NULL, 10);
                }
                QueryHeaderStr(r, WINHTTP_QUERY_ETAG, pr->etag, 128);
                QueryHeaderStr(r, WINHTTP_QUERY_LAST_MODIFIED, pr->lastmod, 96);
                if (QueryHeaderStr(r, WINHTTP_QUERY_CONTENT_DISPOSITION, cd, 512))
                    FilenameFromCD(cd, pr->fname, 160);
                ok = TRUE;
            } else if (code == 200) {
                wchar_t len[64];
                pr->ranges = -1;
                if (QueryHeaderStr(r, WINHTTP_QUERY_CONTENT_LENGTH, len, 64))
                    pr->total = wcstoull(len, NULL, 10);
                QueryHeaderStr(r, WINHTTP_QUERY_ETAG, pr->etag, 128);
                QueryHeaderStr(r, WINHTTP_QUERY_LAST_MODIFIED, pr->lastmod, 96);
                ok = TRUE;
            } else if (!probedHead) {
                wchar_t st[64];
                QueryHeaderStr(r, WINHTTP_QUERY_STATUS_TEXT, st, 64);
                if (!st[0]) lstrcpynW(st, L"Error", 64);
                _snwprintf(j->err, 256, L"HTTP %d %s", code, st);
            }
            WinHttpCloseHandle(r);
            r = NULL;
        } else if (!probedHead && !j->err[0]) {
            lstrcpynW(j->err, L"Request failed", 256);
        }
    }

    /* filename fallback: URL path tail */
    if (ok && !pr->fname[0]) {
        const wchar_t *p = wcsrchr(up.path, L'/');
        wchar_t tmp[256];
        if (p && p[1]) {
            const wchar_t *q = wcspbrk(p + 1, L"?#");
            int n;
            if (q) n = (int)(q - (p + 1)); else n = (int)wcslen(p + 1);
            if (n > 0 && n < 256) {
                memcpy(tmp, p + 1, (size_t)n * sizeof(WCHAR));
                tmp[n] = 0;
                PercentDecodeW(tmp, pr->fname, 160);
                SanitizeFilenameW(pr->fname, 160);
            }
        }
        if (!pr->fname[0]) lstrcpynW(pr->fname, L"download", 160);
    }

done:
    if (!ok && !j->err[0] && !pr->status) lstrcpynW(j->err, L"Network error", 256);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

/* ------------------------------------------------------------------ */
/* .gstmeta resume                                                    */
/* ------------------------------------------------------------------ */
static void MetaPath(const Job *j, wchar_t *out, int cap)
{
    _snwprintf(out, cap, L"%s.gstmeta", j->file);
}

static void SaveMeta(Job *j)
{
    wchar_t mp[MAX_PATH + 16], tmp[MAX_PATH + 24];
    FILE *f;
    char u8[4200];
    int i, nseg;
    Segment segs[GS_MAXSEG];
    unsigned __int64 total;
    if (!j->file[0]) return;
    MetaPath(j, mp, MAX_PATH + 16);
    _snwprintf(tmp, MAX_PATH + 24, L"%s.tmp", mp);
    EnterCriticalSection(&j->cs);
    nseg = j->nseg;
    total = j->total;
    if (nseg < 0) nseg = 0;
    if (nseg > GS_MAXSEG) nseg = GS_MAXSEG;
    memcpy(segs, j->segs, (size_t)nseg * sizeof(Segment));
    LeaveCriticalSection(&j->cs);
    f = _wfopen(tmp, L"wb");
    if (!f) return;
    WideToUtf8(j->url, u8, 4200);
    fprintf(f, "url=%s\n", u8);
    WideToUtf8(j->etag, u8, 4200);
    fprintf(f, "etag=%s\n", u8);
    WideToUtf8(j->lastmod, u8, 4200);
    fprintf(f, "lastmod=%s\n", u8);
    fprintf(f, "total=%I64u\n", total);
    fprintf(f, "nseg=%d\n", nseg);
    for (i = 0; i < nseg; i++)
        fprintf(f, "seg=%I64u %I64u %I64u\n",
                segs[i].start, segs[i].end, segs[i].pos);
    fflush(f);
    fclose(f);
    MoveFileExW(tmp, mp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

typedef struct {
    ULONGLONG total;
    int nseg;
    wchar_t url[2048];
    wchar_t etag[128], lastmod[96];
} META;

static BOOL LoadMeta(Job *j, META *m)
{
    wchar_t mp[MAX_PATH + 16];
    FILE *f;
    char line[4200];
    int cnt = 0, i;
    ZeroMemory(m, sizeof(*m));
    if (!j->file[0]) return FALSE;
    MetaPath(j, mp, MAX_PATH + 16);
    f = _wfopen(mp, L"rb");
    if (!f) return FALSE;
    ZeroMemory(j->segs, sizeof(j->segs));
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (strncmp(line, "url=", 4) == 0) {
            Utf8ToWide(line + 4, m->url, 2048);
        } else if (strncmp(line, "etag=", 5) == 0) {
            Utf8ToWide(line + 5, m->etag, 128);
        } else if (strncmp(line, "lastmod=", 8) == 0) {
            Utf8ToWide(line + 8, m->lastmod, 96);
        } else if (strncmp(line, "total=", 6) == 0) {
            m->total = strtoull(line + 6, NULL, 10);
        } else if (strncmp(line, "nseg=", 5) == 0) {
            m->nseg = atoi(line + 5);
        } else if (strncmp(line, "seg=", 4) == 0 && m->nseg > 0 && m->nseg <= GS_MAXSEG
                   && cnt < m->nseg) {
            unsigned __int64 a, b, c;
            if (sscanf(line + 4, "%I64u %I64u %I64u", &a, &b, &c) == 3 && cnt < GS_MAXSEG) {
                j->segs[cnt].start = a;
                j->segs[cnt].end = b;
                j->segs[cnt].pos = c;
                cnt++;
            }
        }
    }
    fclose(f);
    if (cnt <= 0 || !m->url[0] || lstrcmpW(m->url, j->url) != 0) return FALSE;
    /* sanity: pos inside [start,end] */
    for (i = 0; i < cnt; i++) {
        Segment *s = &j->segs[i];
        if (s->end != (unsigned __int64)(-1)) {
            if (s->pos < s->start || s->pos > s->end || s->start > s->end) return FALSE;
        } else if (s->pos < s->start) return FALSE;
    }
    m->nseg = cnt;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* segments                                                           */
/* ------------------------------------------------------------------ */
static void InitSegments(Job *j)
{
    int i;
    ULONGLONG per, off;
    ZeroMemory(j->segs, sizeof(j->segs));
    if (j->ranges == 1 && j->total > 2 * 1024 * 1024 && g_segments > 1) {
        j->nseg = g_segments;
        per = j->total / (unsigned __int64)j->nseg;
        off = 0;
        for (i = 0; i < j->nseg; i++) {
            j->segs[i].start = off;
            j->segs[i].end   = (i == j->nseg - 1) ? j->total : off + per;
            j->segs[i].pos   = off;
            off += per;
        }
    } else {
        j->nseg = 1;
        j->segs[0].start = 0;
        j->segs[0].end   = j->total ? j->total : (unsigned __int64)(-1);
        j->segs[0].pos   = 0;
    }
}

/* a finished connection steals the tail of the largest remaining segment */
static BOOL StealTail(Job *j, int idx)
{
    BOOL got = FALSE;
    int k, best = -1;
    ULONGLONG bestSpan = 8 * 1024 * 1024;
    EnterCriticalSection(&j->cs);
    for (k = 0; k < j->nseg; k++) {
        ULONGLONG rem;
        if (k == idx) continue;
        if (j->segs[k].end == (unsigned __int64)(-1)) continue;
        rem = j->segs[k].end - j->segs[k].pos;
        if (rem > bestSpan) { bestSpan = rem; best = k; }
    }
    if (best >= 0) {
        ULONGLONG mid = j->segs[best].pos +
                        (j->segs[best].end - j->segs[best].pos) / 2;
        j->segs[idx].start = mid;
        j->segs[idx].end   = j->segs[best].end;
        j->segs[idx].pos   = mid;
        j->segs[best].end  = mid;
        got = TRUE;
    }
    LeaveCriticalSection(&j->cs);
    return got;
}

/* ------------------------------------------------------------------ */
/* segment worker                                                     */
/* ------------------------------------------------------------------ */
typedef struct { Job *j; int idx; } SEGARG;

static void MakeRange(wchar_t *out, int cap, ULONGLONG from, ULONGLONG to)
{
    if (to == (unsigned __int64)(-1))
        _snwprintf(out, cap, L"bytes=%I64u-", from);
    else
        _snwprintf(out, cap, L"bytes=%I64u-%I64u", from, to);
}

static DWORD WINAPI SegThread(LPVOID p)
{
    SEGARG *a = (SEGARG *)p;
    Job *j = a->j;
    int idx = a->idx;
    URLPARTS up;
    HINTERNET s = NULL, c = NULL, r = NULL;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    BYTE *buf;
    DWORD retry = 0;
    wchar_t rng[80];
    ULONGLONG from, segEnd;

    HeapFree(GetProcessHeap(), 0, a);
    if (!CrackUrl(j->url, &up)) {
        lstrcpynW(j->err, L"Invalid URL", 256);
        InterlockedExchange(&j->fatal, 1);
        InterlockedDecrement(&j->activeSegs);
        return 0;
    }
    buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 64 * 1024);
    hFile = CreateFileW(j->file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE || !buf) {
        lstrcpynW(j->err, L"Cannot open file for writing", 256);
        InterlockedExchange(&j->fatal, 1);
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
        InterlockedDecrement(&j->activeSegs);
        return 0;
    }

    s = OpenSession();
    c = s ? WinHttpConnect(s, up.host, up.port, 0) : NULL;
    if (!c) {
        lstrcpynW(j->err, L"Connection failed", 256);
        InterlockedExchange(&j->fatal, 1);
        goto out;
    }

    for (;;) {
        int reconn = 0, quitReq = 0;
        if (j->segs[idx].quit) break;
        if (WaitForSingleObject(j->cancelEvt, 0) == WAIT_OBJECT_0) break;
        EnterCriticalSection(&j->cs);
        from = j->segs[idx].pos;
        segEnd = j->segs[idx].end;
        LeaveCriticalSection(&j->cs);
        if (segEnd != (unsigned __int64)(-1) && from >= segEnd) break;

        MakeRange(rng, 80, from, segEnd == (unsigned __int64)(-1) ? (unsigned __int64)(-1) : segEnd - 1);
        r = OpenRequest(c, &up, L"GET",
                        from == 0 && segEnd == (unsigned __int64)(-1) ? NULL : rng, j);
        if (!r) {
            if (++retry > 8) {
                lstrcpynW(j->err, L"Request failed", 256);
                InterlockedExchange(&j->fatal, 1);
                break;
            }
            Sleep(900 * retry);
            continue;
        }
        {
            int status = QueryStatus(r);
            if (status != 206 && !(status == 200 && from == 0)) {
                wchar_t st[64];
                QueryHeaderStr(r, WINHTTP_QUERY_STATUS_TEXT, st, 64);
                if (!st[0]) lstrcpynW(st, L"Error", 64);
                WinHttpCloseHandle(r); r = NULL;
                if (++retry > 8) {
                    _snwprintf(j->err, 256, L"HTTP %d %s", status, st);
                    InterlockedExchange(&j->fatal, 1);
                    break;
                }
                Sleep(900 * retry);
                continue;
            }
            if (status == 200 && from != 0) {
                /* server ignored the range header — stream starts at 0 */
                _snwprintf(j->err, 256, L"Server ignored range request");
                InterlockedExchange(&j->fatal, 1);
                WinHttpCloseHandle(r); r = NULL;
                break;
            }
        }
        {
            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)from;
            SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
        }
        retry = 0;

        for (;;) {
            DWORD n = 0;
            if (j->segs[idx].quit ||
                WaitForSingleObject(j->cancelEvt, 0) == WAIT_OBJECT_0) { quitReq = 1; break; }
            if (!WinHttpReadData(r, buf, 64 * 1024, &n)) { reconn = 1; break; }
            if (n == 0) break;   /* server EOF for this response */
            {
                DWORD w = 0;
                if (!WriteFile(hFile, buf, n, &w, NULL) || w != n) {
                    lstrcpynW(j->err, L"Disk write failed", 256);
                    InterlockedExchange(&j->fatal, 1);
                    break;
                }
            }
            EnterCriticalSection(&j->cs);
            j->segs[idx].pos += n;
            LeaveCriticalSection(&j->cs);
            InterlockedAdd64(&j->got, (LONG64)n);
        }
        if (r) { WinHttpCloseHandle(r); r = NULL; }
        if (j->fatal) break;
        if (quitReq) break;
        if (reconn) {
            if (++retry > 8) {
                if (!j->err[0]) lstrcpynW(j->err, L"Connection lost", 256);
                InterlockedExchange(&j->fatal, 1);
                break;
            }
            Sleep(900 * retry);
            continue;
        }
        /* natural EOF */
        if (segEnd == (unsigned __int64)(-1)) {
            InterlockedExchange(&j->noMoreData, 1);
            break;
        }
        EnterCriticalSection(&j->cs);
        from = j->segs[idx].pos;
        segEnd = j->segs[idx].end;
        LeaveCriticalSection(&j->cs);
        if (from < segEnd) {    /* truncated response — reconnect */
            if (++retry > 8) {
                lstrcpynW(j->err, L"Truncated response", 256);
                InterlockedExchange(&j->fatal, 1);
                break;
            }
            Sleep(500);
            continue;
        }
        if (StealTail(j, idx)) continue;
        break;
    }

out:
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    InterlockedDecrement(&j->activeSegs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* job manager thread                                                 */
/* ------------------------------------------------------------------ */
static void StartJobThread(Job *j);

static DWORD WINAPI JobThread(LPVOID p)
{
    Job *j = (Job *)p;
    PROBE pr;
    META meta;
    BOOL resumed = FALSE, cancelled = FALSE;
    int i;

    j->state = ST_CONNECTING;
    PostMessageW(g_ui, WM_APP_JOB, (WPARAM)j->id, 1);

    if (!Probe(j, &pr)) {
        j->state = ST_ERROR;
        PostMessageW(g_ui, WM_APP_JOB, (WPARAM)j->id, 1);
        return 0;
    }
    j->ranges = pr.ranges;
    if (pr.total) j->total = pr.total;
    lstrcpynW(j->etag, pr.etag, 128);
    lstrcpynW(j->lastmod, pr.lastmod, 96);

    if (!j->file[0]) {
        wchar_t nm[160], tmp[MAX_PATH];
        lstrcpynW(tmp, pr.fname, 160);
        SanitizeFilenameW(tmp, 160);
        lstrcpynW(nm, tmp, 160);
        _snwprintf(j->file, MAX_PATH, L"%s\\%s", j->dir, nm);
        j->file[MAX_PATH - 1] = 0;
        UniquifyPathW(j->file, MAX_PATH);
    }

    if (j->resume && LoadMeta(j, &meta)) {
        BOOL tagOk = (!j->etag[0] || !meta.etag[0] || lstrcmpiW(j->etag, meta.etag) == 0);
        BOOL modOk = (!j->lastmod[0] || !meta.lastmod[0] || lstrcmpiW(j->lastmod, meta.lastmod) == 0);
        BOOL totOk = (meta.total == 0 || j->total == 0 || meta.total == j->total);
        if (tagOk && modOk && totOk) {
            if (meta.total && !j->total) j->total = meta.total;
            j->nseg = meta.nseg;
            resumed = TRUE;
        }
    }
    /* A server that cannot honor Range cannot safely continue at an offset.
       Restart that single stream instead of repeatedly failing with HTTP 200. */
    if (resumed && j->ranges != 1 && j->nseg == 1 && j->segs[0].pos > 0)
        resumed = FALSE;
    if (!resumed) InitSegments(j);
    for (i = 0; i < j->nseg; i++) j->segs[i].quit = 0;

    j->got = 0;
    for (i = 0; i < j->nseg; i++) j->got += (LONG64)(j->segs[i].pos - j->segs[i].start);

    if (!resumed && j->total) {
        HANDLE hf = CreateFileW(j->file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)j->total;
            if (!SetFilePointerEx(hf, li, NULL, FILE_BEGIN)) { /* best effort */ }
            SetEndOfFile(hf);
            CloseHandle(hf);
        }
    }

    InterlockedExchange(&j->fatal, 0);
    InterlockedExchange(&j->noMoreData, 0);
    j->activeSegs = 0;
    j->state = ST_DOWNLOADING;
    PostMessageW(g_ui, WM_APP_JOB, (WPARAM)j->id, 1);

    for (i = 0; i < j->nseg; i++) {
        SEGARG *a = (SEGARG *)HeapAlloc(GetProcessHeap(), 0, sizeof(SEGARG));
        if (a) {
            HANDLE t;
            a->j = j;
            a->idx = i;
            InterlockedIncrement(&j->activeSegs);
            t = CreateThread(NULL, 0, SegThread, a, 0, NULL);
            if (t) CloseHandle(t);
            else InterlockedDecrement(&j->activeSegs);
        }
    }

    while (j->activeSegs > 0) {
        if (WaitForSingleObject(j->cancelEvt, 250) == WAIT_OBJECT_0) {
            int k;
            cancelled = TRUE;
            for (k = 0; k < j->nseg; k++) j->segs[k].quit = 1;
        }
    }

    if (cancelled) {
        SaveMeta(j);
        j->state = ST_PAUSED;
    } else if (j->fatal) {
        SaveMeta(j);
        j->state = ST_ERROR;
    } else if (j->noMoreData ||
               (j->total && (unsigned __int64)j->got >= j->total)) {
        wchar_t mp[MAX_PATH + 16];
        MetaPath(j, mp, MAX_PATH + 16);
        DeleteFileW(mp);
        j->state = ST_DONE;
    } else {
        SaveMeta(j);
        if (!j->err[0]) lstrcpynW(j->err, L"Incomplete", 256);
        j->state = ST_ERROR;
    }
    PostMessageW(g_ui, WM_APP_JOB, (WPARAM)j->id, 1);
    return 0;
}

static void StartJobThread(Job *j)
{
    ResetEvent(j->cancelEvt);
    j->resume = 1;
    j->fatal = 0;
    j->noMoreData = 0;
    j->err[0] = 0;
    j->speed = 0;
    j->gotPrev = 0;
    j->prevTick = 0;
    if (j->hThread) { CloseHandle(j->hThread); j->hThread = NULL; }
    j->hThread = CreateThread(NULL, 0, JobThread, j, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */
int EngineAddWithContext(const wchar_t *url, const wchar_t *dir, const wchar_t *name,
                         const wchar_t *referrer, const wchar_t *cookie,
                         unsigned __int64 expectedSize, int startNow)
{
    Job *j, *tail;
    if (!url || !url[0]) return -1;
    j = (Job *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Job));
    if (!j) return -1;
    j->id = g_nextId++;
    lstrcpynW(j->url, url, 2048);
    if (referrer) lstrcpynW(j->referrer, referrer, 2048);
    if (cookie) lstrcpynW(j->cookie, cookie, 1024);
    j->total = expectedSize;
    lstrcpynW(j->dir, dir && dir[0] ? dir : L"C:\\Users\\Public\\Downloads", MAX_PATH);
    if (name && name[0]) {
        wchar_t tmp[160];
        lstrcpynW(tmp, name, 160);
        SanitizeFilenameW(tmp, 160);
        _snwprintf(j->file, MAX_PATH, L"%s\\%s", j->dir, tmp);
        j->file[MAX_PATH - 1] = 0;
        UniquifyPathW(j->file, MAX_PATH);
    }
    j->state = ST_QUEUED;
    j->nseg = g_segments;
    InitializeCriticalSection(&j->cs);
    j->cancelEvt = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (tail = g_jobs; tail && tail->next; tail = tail->next) { }
    if (tail) tail->next = j; else g_jobs = j;
    if (startNow) StartJobThread(j);
    EnginePersist();
    PostMessageW(g_ui, WM_APP_JOB, (WPARAM)j->id, 1);
    return j->id;
}

int EngineAdd(const wchar_t *url, const wchar_t *dir, const wchar_t *name, int startNow)
{
    return EngineAddWithContext(url, dir, name, NULL, NULL, 0, startNow);
}

void EngineStart(int id)
{
    Job *j = EngineById(id);
    if (!j) return;
    if (j->state == ST_QUEUED || j->state == ST_PAUSED || j->state == ST_ERROR)
        StartJobThread(j);
}

void EngineStop(int id)
{
    Job *j = EngineById(id);
    if (!j) return;
    if (j->state == ST_DOWNLOADING || j->state == ST_CONNECTING)
        SetEvent(j->cancelEvt);
}

void EngineResume(int id)
{
    Job *j = EngineById(id);
    if (!j) return;
    if (j->state == ST_PAUSED || j->state == ST_ERROR || j->state == ST_QUEUED)
        StartJobThread(j);
}

void EngineRemove(int id, int deleteFile)
{
    Job *j = EngineById(id), *prev;
    if (!j) return;
    if (j->state == ST_DOWNLOADING || j->state == ST_CONNECTING) {
        SetEvent(j->cancelEvt);
        /* Never free a Job while its worker can still dereference it. */
        if (j->hThread) WaitForSingleObject(j->hThread, INFINITE);
    }
    if (deleteFile && j->file[0]) {
        wchar_t mp[MAX_PATH + 16];
        DeleteFileW(j->file);
        MetaPath(j, mp, MAX_PATH + 16);
        DeleteFileW(mp);
    }
    prev = NULL;
    {
        Job *t = g_jobs;
        while (t && t != j) { prev = t; t = t->next; }
        if (t == j) {
            if (prev) prev->next = j->next; else g_jobs = j->next;
        }
    }
    if (j->hThread) CloseHandle(j->hThread);
    CloseHandle(j->cancelEvt);
    DeleteCriticalSection(&j->cs);
    HeapFree(GetProcessHeap(), 0, j);
    EnginePersist();
}

void EngineShutdown(void)
{
    Job *j;
    for (j = g_jobs; j; j = j->next) {
        if (j->state == ST_DOWNLOADING || j->state == ST_CONNECTING) {
            SetEvent(j->cancelEvt);
        }
    }
    for (j = g_jobs; j; j = j->next) {
        if (j->hThread) WaitForSingleObject(j->hThread, INFINITE);
    }
    EnginePersist();
}

void EngineTick(void)
{
    Job *j;
    ULONGLONG now = GetTickCount64();
    int active = 0, queued = 0, done = 0;
    double agg = 0.0;

    for (j = g_jobs; j; j = j->next) {
        if (j->state == ST_DOWNLOADING || j->state == ST_CONNECTING) {
            active++;
            if (j->prevTick) {
                double dt = (double)(now - j->prevTick) / 1000.0;
                if (dt >= 0.4) {
                    double inst = (double)(j->got - j->gotPrev) / dt;
                    if (inst < 0) inst = 0;
                    j->speed = j->speed * 0.65 + inst * 0.35;
                    j->gotPrev = j->got;
                    j->prevTick = now;
                }
            } else {
                j->gotPrev = j->got;
                j->prevTick = now;
            }
            agg += j->speed;
        } else if (j->state == ST_QUEUED) {
            queued++;
        } else if (j->state == ST_DONE) {
            done++;
        }
    }
    g_active = active; g_queued = queued; g_done = done; g_agg = agg;

    /* Crash-safe progress checkpoints; normal pause/exit also saves immediately. */
    if (!g_lastCheckpoint || now - g_lastCheckpoint >= 5000) {
        for (j = g_jobs; j; j = j->next)
            if (j->state == ST_DOWNLOADING) SaveMeta(j);
        EnginePersist();
        g_lastCheckpoint = now;
    }

    if (active < g_maxActive) {
        for (j = g_jobs; j && active < g_maxActive; j = j->next) {
            if (j->state == ST_QUEUED) { StartJobThread(j); active++; }
        }
    }
}
