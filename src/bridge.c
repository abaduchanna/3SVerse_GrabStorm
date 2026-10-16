/* GrabStorm — 3SVerse download accelerator for Windows
 * bridge.c — loopback HTTP bridge the browser extension talks to.
 *
 * Endpoints (loopback only, DNS-rebinding shielded):
 *   GET  /status/204+grabstorm   presence probe -> 204 (tokenless)
 *   GET  /ping                   -> 200 "grabstorm" (tokenless)
 *   OPTIONS *                    -> 204 + CORS (preflight)
 *   POST /add                    -> 200 {"ok":true}  (needs token)
 *
 * POST /add body: JSON {url, referrer, name, cookie, size}
 * Auth: "X-GrabStorm-Token: <token>" header or ?t=<token> query.
 *
 * MIT License — (c) 2026 Abad Umair Channa / 3S Verse
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge.h"
#include "util.h"

#define BRIDGE_DEFAULT_PORT 14599
#define MAX_HDR   16384
#define MAX_BODY  65536
#define RECV_TIMEOUT_MS 4000

static HWND      g_main   = NULL;
static SOCKET    g_listen = INVALID_SOCKET;
static HANDLE    g_thread = NULL;
static volatile LONG g_run   = 0;
static volatile LONG g_started = 0;
static int       g_port   = BRIDGE_DEFAULT_PORT;
static wchar_t   g_token[128] = L"";
static CRITICAL_SECTION g_cs;
static LONG      g_csInit = 0;

static void EnsureCs(void)
{
    if (InterlockedCompareExchange(&g_csInit, 1, 0) == 0)
        InitializeCriticalSection(&g_cs);
}

/* ------------------------------------------------------------------ */
/* settings                                                            */
/* ------------------------------------------------------------------ */
static void LoadSettings(void)
{
    g_port = SettingsGetInt(L"BridgePort", BRIDGE_DEFAULT_PORT);
    if (g_port < 1024 || g_port > 65535) g_port = BRIDGE_DEFAULT_PORT;
    SettingsGetStr(L"BridgeToken", g_token, 128, L"");
    if (!g_token[0]) {
        /* first run: mint a token so /add is never open by default */
        unsigned __int64 a, b;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        a = (unsigned __int64)t.QuadPart ^ (unsigned __int64)(DWORD_PTR)&g_main;
        b = GetTickCount64() ^ ((unsigned __int64)GetCurrentProcessId() << 32);
        _snwprintf(g_token, 128, L"%04x%08llx%08llx",
                   (unsigned)(a & 0xffff),
                   (unsigned long long)(a >> 16) & 0xffffffffULL,
                   (unsigned long long)b & 0xffffffffULL);
        SettingsSetStr(L"BridgeToken", g_token);
    }
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */
static BOOL HostOk(const char *host)
{
    if (!host || !*host) return FALSE;
    return strstr(host, "127.0.0.1") != NULL || strstr(host, "localhost") != NULL;
}

static void SendAll(SOCKET s, const char *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = send(s, buf + off, len - off, 0);
        if (n <= 0) return;
        off += n;
    }
}

static void SendResponse(SOCKET s, int code, const char *ctype,
                         const char *body, int blen, int cors)
{
    const char *rs = (code == 200) ? "OK" : (code == 204) ? "No Content"
                     : (code == 404) ? "Not Found" : (code == 403) ? "Forbidden"
                     : (code == 400) ? "Bad Request" : (code == 503) ? "Service Unavailable"
                     : "Error";
    char hdr[512];
    int n = _snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "%s%s"
        "\r\n",
        code, rs, ctype ? ctype : "text/plain", blen,
        cors ? "Access-Control-Allow-Origin: *\r\n"
               "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
               "Access-Control-Allow-Headers: Content-Type, X-GrabStorm-Token\r\n"
             : "");
    if (n < 0 || n >= (int)sizeof(hdr)) return;
    SendAll(s, hdr, n);
    if (body && blen > 0) SendAll(s, body, blen);
}

/* extract "key":"value" from a JSON body, decoding \", \\, \/, \n, \t, \uXXXX.
 * Returns utf-8 length in out (cap). out is NUL-terminated. */
static int JsonGetStr(const char *body, const char *key, char *out, int cap)
{
    const char *p = body;
    char pat[64];
    int klen = _snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (klen <= 0) return 0;
    p = strstr(body, pat);
    if (!p) return 0;
    p += klen;
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    int n = 0;
    while (*p && *p != '"' && n < cap - 5) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  out[n++] = '"';  break;
            case '\\': out[n++] = '\\'; break;
            case '/':  out[n++] = '/';  break;
            case 'n':  out[n++] = '\n'; break;
            case 't':  out[n++] = '\t'; break;
            case 'r':  out[n++] = '\r'; break;
            case 'b':  out[n++] = '\b'; break;
            case 'f':  out[n++] = '\f'; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 1; i <= 4 && p[i]; i++) {
                    char c = p[i];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                    else { cp = 0xFFFD; break; }
                }
                p += 4;
                /* encode as UTF-8 (BMP only; extensions send UTF-8 names) */
                if (cp < 0x80) out[n++] = (char)cp;
                else if (cp < 0x800) {
                    out[n++] = (char)(0xC0 | (cp >> 6));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[n++] = (char)(0xE0 | (cp >> 12));
                    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out[n++] = *p; break;
            }
            p++;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return n;
}

static long long JsonGetInt(const char *body, const char *key)
{
    char pat[64];
    int klen = _snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (klen <= 0) return 0;
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p += klen;
    while (*p == ' ' || *p == ':') p++;
    if (*p < '0' || *p > '9') return 0;
    return _strtoi64(p, NULL, 10);
}

static BOOL TokenOk(const char *headers, const char *path)
{
    wchar_t got[128];
    /* header form */
    const char *h = strstr(headers, "X-GrabStorm-Token:");
    if (h) {
        h += 18;
        while (*h == ' ') h++;
        int n = 0;
        while (h[n] && h[n] != '\r' && h[n] != '\n' && n < 127) n++;
        MultiByteToWideChar(CP_UTF8, 0, h, n, got, 128);
        got[127] = 0;
        if (lstrcmpW(got, g_token) == 0) return TRUE;
    }
    /* query form ?t= */
    const char *q = strstr(path, "?t=");
    if (q) {
        q += 3;
        int n = 0;
        while (q[n] && q[n] != '&' && q[n] != ' ' && n < 127) n++;
        MultiByteToWideChar(CP_UTF8, 0, q, n, got, 128);
        got[127] = 0;
        if (lstrcmpW(got, g_token) == 0) return TRUE;
    }
    (void)path;
    return FALSE;
}

/* case-insensitive ASCII find */
static const char *StrIFind(const char *hay, const char *needle)
{
    if (!hay || !needle) return NULL;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               (char)tolower((unsigned char)p[i]) == (char)tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return p;
    }
    return NULL;
}

/* case-insensitive wide prefix match (returns str if str starts with prefix) */
static const wchar_t *PrefixIW(const wchar_t *str, const wchar_t *prefix)
{
    while (*prefix) {
        if (towlower((wint_t)*str) != towlower((wint_t)*prefix)) return NULL;
        str++; prefix++;
    }
    return str;
}

static BOOL SchemeOk(const wchar_t *u)
{
    if (!u[0]) return FALSE;
    return PrefixIW(u, L"http://") == u + 7
        || PrefixIW(u, L"https://") == u + 8
        || PrefixIW(u, L"ftp://") == u + 6;
}

/* ------------------------------------------------------------------ */
/* one connection                                                      */
/* ------------------------------------------------------------------ */
static void HandleConn(SOCKET s)
{
    char  hdr[MAX_HDR + 1];
    char *body = NULL;
    int   hlen = 0, blen = 0;

    /* read until \r\n\r\n or cap */
    while (hlen < MAX_HDR) {
        int n = recv(s, hdr + hlen, MAX_HDR - hlen, 0);
        if (n <= 0) goto done;
        hlen += n;
        hdr[hlen] = 0;
        char *end = strstr(hdr, "\r\n\r\n");
        if (end) {
            body = end + 4;
            int have = hlen - (int)(body - hdr);
            /* find content-length */
            const char *cl = StrIFind(hdr, "Content-Length:");
            long want = 0;
            if (cl) want = strtol(cl + 15, NULL, 10);            if (want > MAX_BODY) { SendResponse(s, 400, "text/plain", "too large", 9, 1); goto done; }
            if (want > 0) {
                body = (char *)HeapAlloc(GetProcessHeap(), 0, want + 1);
                if (!body) goto done;
                memcpy(body, end + 4, (size_t)have);
                while (have < want) {
                    int m = recv(s, body + have, (int)(want - have), 0);
                    if (m <= 0) break;
                    have += m;
                }
                body[have] = 0;
                blen = have;
            } else {
                *end = 0;
                blen = 0;
                body = NULL;
            }
            break;
        }
    }
    if (hlen >= MAX_HDR) goto done;

    /* request line: METHOD SP PATH SP VER */
    char method[8] = "", path[2048] = "";
    {
        const char *sp = strchr(hdr, ' ');
        if (!sp || sp - hdr > 7) goto done;
        memcpy(method, hdr, (size_t)(sp - hdr)); method[sp - hdr] = 0;
        const char *sp2 = strchr(sp + 1, ' ');
        if (!sp2 || sp2 - (sp + 1) > 2047) goto done;
        memcpy(path, sp + 1, (size_t)(sp2 - sp - 1)); path[sp2 - sp - 1] = 0;
    }

    /* DNS-rebinding shield: Host must be loopback */
    const char *host = StrIFind(hdr, "Host:");
    if (!HostOk(host)) { SendResponse(s, 403, "text/plain", "bad host", 8, 0); goto done; }

    if (lstrcmpA(method, "OPTIONS") == 0) {
        SendResponse(s, 204, "text/plain", NULL, 0, 1);
        goto done;
    }

    if (lstrcmpA(method, "GET") == 0 || lstrcmpA(method, "HEAD") == 0) {
        if (strstr(path, "/status/204+grabstorm")) {
            SendResponse(s, 204, "text/plain", NULL, 0, 1);
            goto done;
        }
        if (strstr(path, "/ping")) {
            SendResponse(s, 200, "text/plain", "grabstorm", 9, 1);
            goto done;
        }
        SendResponse(s, 404, "text/plain", "nf", 2, 1);
        goto done;
    }

    if (lstrcmpA(method, "POST") == 0 && strstr(path, "/add")) {
    if (!TokenOk(hdr, path)) {
            SendResponse(s, 403, "text/plain", "bad token", 9, 1);
            goto done;
        }
        const char *jb = body ? body : "";
        char url8[4096] = "", ref8[4096] = "", nm8[512] = "", ck8[2048] = "";
        JsonGetStr(jb, "url", url8, 4096);
        JsonGetStr(jb, "referrer", ref8, 4096);
        JsonGetStr(jb, "name", nm8, 512);
        JsonGetStr(jb, "cookie", ck8, 2048);
        long long size = JsonGetInt(jb, "size");

        BridgeAdd *ba = (BridgeAdd *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(BridgeAdd));
        if (!ba) goto done;
        Utf8ToWide(url8, ba->url, 2048);
        Utf8ToWide(ref8, ba->referrer, 2048);
        Utf8ToWide(nm8, ba->name, 160);
        Utf8ToWide(ck8, ba->cookie, 1024);
        ba->size = (unsigned __int64)(size > 0 ? size : 0);

        if (!SchemeOk(ba->url)) {
            HeapFree(GetProcessHeap(), 0, ba);
            SendResponse(s, 400, "text/plain", "bad url", 7, 1);
            goto done;
        }
        if (PostMessageW(g_main, WM_APP_BRIDGE, 0, (LPARAM)ba)) {
            SendResponse(s, 200, "application/json", "{\"ok\":true}", 11, 1);
        } else {
            HeapFree(GetProcessHeap(), 0, ba);
            SendResponse(s, 503, "application/json", "{\"ok\":false}", 13, 1);
        }
        goto done;
    }

    SendResponse(s, 404, "text/plain", "nf", 2, 1);

done:
    if (body && blen > 0) HeapFree(GetProcessHeap(), 0, body);
}

/* ------------------------------------------------------------------ */
/* listener thread                                                     */
/* ------------------------------------------------------------------ */
static DWORD WINAPI BridgeThread(LPVOID param)
{
    (void)param;
    while (InterlockedCompareExchange(&g_run, 0, 0)) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) {
            if (!InterlockedCompareExchange(&g_run, 0, 0)) break;
            Sleep(120);
            continue;
        }
        DWORD to = RECV_TIMEOUT_MS;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (char *)&to, sizeof(to));
        HandleConn(c);
        closesocket(c);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */
static void CloseListener(void)
{
    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}

void BridgeStart(HWND main)
{
    if (InterlockedCompareExchange(&g_started, 1, 0)) return;
    EnsureCs();
    g_main = main;
    LoadSettings();
    if (!SettingsGetInt(L"BridgeOn", 1)) return;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return;
    BOOL reuse = TRUE;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((u_short)g_port);
    if (bind(l, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(l, 8) != 0) {
        closesocket(l);
        return;
    }
    g_listen = l;
    InterlockedExchange(&g_run, 1);
    g_thread = CreateThread(NULL, 0, BridgeThread, NULL, 0, NULL);
}

void BridgeStop(void)
{
    if (!InterlockedCompareExchange(&g_started, 0, 0)) return;
    InterlockedExchange(&g_run, 0);
    CloseListener();
    WSACleanup();
    /* note: g_cs intentionally never deleted — the bridge can be restarted */
}

int BridgePort(void) { return g_port; }

BOOL BridgeRunning(void)
{
    return g_listen != INVALID_SOCKET;
}

void BridgeRenewToken(HWND main)
{
    EnterCriticalSection(&g_cs);
    unsigned __int64 a = GetTickCount64() ^ (unsigned __int64)(DWORD_PTR)main;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    a ^= (unsigned __int64)t.QuadPart;
    unsigned __int64 b = (unsigned __int64)rand() << 32 | rand();
    _snwprintf(g_token, 128, L"%08llx%08llx",
               (unsigned long long)a & 0xffffffffULL,
               (unsigned long long)b & 0xffffffffULL);
    SettingsSetStr(L"BridgeToken", g_token);
    LeaveCriticalSection(&g_cs);
}

void BridgeRestart(HWND main)
{
    EnterCriticalSection(&g_cs);
    InterlockedExchange(&g_run, 0);
    CloseListener();
    LeaveCriticalSection(&g_cs);
    InterlockedExchange(&g_started, 0);
    BridgeStart(main);
}
