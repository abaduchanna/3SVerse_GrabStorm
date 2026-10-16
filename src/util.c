/* GrabStorm — util.c — settings, formatting, helpers
 * MIT License — (c) 2026 Abad Umair Channa / 3S Verse
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include "util.h"

/* ---------------- settings (HKCU\Software\3SVerse\GrabStorm) ---------------- */

void SettingsSetStr(const wchar_t *name, const wchar_t *val)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val,
                   (DWORD)((lstrlenW(val) + 1) * sizeof(WCHAR)));
    RegCloseKey(k);
}

void SettingsSetInt(const wchar_t *name, int val)
{
    wchar_t b[24];
    _snwprintf(b, 24, L"%d", val);
    SettingsSetStr(name, b);
}

BOOL SettingsGetStr(const wchar_t *name, wchar_t *out, DWORD cap, const wchar_t *def)
{
    HKEY k;
    out[0] = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        DWORD type = 0, cb = cap * sizeof(WCHAR);
        if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &cb) == ERROR_SUCCESS
            && type == REG_SZ && out[0]) {
            RegCloseKey(k);
            return TRUE;
        }
        RegCloseKey(k);
    }
    if (def) { lstrcpynW(out, def, cap); return TRUE; }
    return FALSE;
}

int SettingsGetInt(const wchar_t *name, int def)
{
    wchar_t b[32];
    if (SettingsGetStr(name, b, 32, NULL)) {
        int v = (int)wcstoul(b, NULL, 10);
        return v;
    }
    return def;
}

/* ---------------- formatting ---------------- */

/* Network speed: BITS, lowercase b, auto-scaled single unit (owner standing rule). */
void FormatSpeedBits(double bytesPerSec, wchar_t *out, int cap)
{
    double bits = bytesPerSec * 8.0;
    if (bits >= 1e9)
        _snwprintf(out, cap, L"%.2f Gb/s", bits / 1e9);
    else if (bits >= 1e6)
        _snwprintf(out, cap, L"%.2f Mb/s", bits / 1e6);
    else if (bits >= 1000.0)
        _snwprintf(out, cap, L"%.1f Kb/s", bits / 1000.0);
    else if (bits > 0.0)
        _snwprintf(out, cap, L"%.0f b/s", bits);
    else
        lstrcpynW(out, L"--", cap);
}

void FormatSizeBytes(unsigned __int64 bytes, wchar_t *out, int cap)
{
    double b = (double)bytes;
    if (b >= 1073741824.0)
        _snwprintf(out, cap, L"%.2f GB", b / 1073741824.0);
    else if (b >= 1048576.0)
        _snwprintf(out, cap, L"%.1f MB", b / 1048576.0);
    else if (b >= 1024.0)
        _snwprintf(out, cap, L"%.0f KB", b / 1024.0);
    else if (bytes > 0)
        _snwprintf(out, cap, L"%u B", (unsigned)bytes);
    else
        lstrcpynW(out, L"--", cap);
}

void FormatEta(double seconds, wchar_t *out, int cap)
{
    int h, m, s;
    if (!(seconds >= 0.0) || seconds > 7.0 * 86400.0) { lstrcpynW(out, L"--", cap); return; }
    if (seconds < 1.0) { lstrcpynW(out, L"<1s", cap); return; }
    s = (int)(seconds + 0.5);
    h = s / 3600; s %= 3600;
    m = s / 60;   s %= 60;
    if (h) _snwprintf(out, cap, L"%dh %02dm", h, m);
    else if (m) _snwprintf(out, cap, L"%dm %02ds", m, s);
    else        _snwprintf(out, cap, L"%ds", s);
}

/* ---------------- strings ---------------- */

int WideToUtf8(const wchar_t *w, char *out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    return n > 0 ? n - 1 : 0;
}

int Utf8ToWide(const char *u, wchar_t *out, int cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, out, cap);
    return n > 0 ? n - 1 : 0;
}

void PercentDecodeW(const wchar_t *in, wchar_t *out, int cap)
{
    char *u8 = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)cap * 2 + 4);
    int o = 0;
    if (!u8) { lstrcpynW(out, in, cap); return; }
    while (*in && o < cap) {
        if (*in == L'%' && in[1] && in[2]) {
            int hi = in[1], lo = in[2], v = 0;
            if (hi >= L'0' && hi <= L'9') v = (hi - L'0') << 4;
            else if (hi >= L'a' && hi <= L'f') v = (hi - L'a' + 10) << 4;
            else if (hi >= L'A' && hi <= L'F') v = (hi - L'A' + 10) << 4;
            else { break; }
            if (lo >= L'0' && lo <= L'9') v |= (lo - L'0');
            else if (lo >= L'a' && lo <= L'f') v |= (lo - L'a' + 10);
            else if (lo >= L'A' && lo <= L'F') v |= (lo - L'A' + 10);
            else break;
            u8[o++] = (char)v;
            in += 3;
        } else if (*in == L'+') {
            u8[o++] = ' ';
            in++;
        } else {
            u8[o++] = (char)(0xFF & *in);
            in++;
        }
    }
    u8[o] = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, u8, o, out, cap - 1) <= 0) {
        int i;
        for (i = 0; i < o && i < cap - 1; i++) out[i] = (wchar_t)(unsigned char)u8[i];
        out[i] = 0;
    } else {
        out[cap - 1] = 0;
    }
    HeapFree(GetProcessHeap(), 0, u8);
}

/* ---------------- filenames ---------------- */

void SanitizeFilenameW(wchar_t *name, int cap)
{
    int i, o = 0, n = (int)wcslen(name);
    wchar_t *tmp = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, ((size_t)n + 2) * sizeof(WCHAR));
    if (!tmp) return;
    for (i = 0; i < n && o < 120; i++) {
        wchar_t c = name[i];
        if (c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' ||
            c == L'\\' || c == L'|' || c == L'?' || c == L'*') c = L'_';
        if (c >= 32) tmp[o++] = c;
    }
    tmp[o] = 0;
    while (o && (tmp[o - 1] == L'.' || tmp[o - 1] == L' ')) tmp[--o] = 0;
    if (!o) lstrcpynW(tmp, L"download", 120);
    lstrcpynW(name, tmp, cap);
    HeapFree(GetProcessHeap(), 0, tmp);
}

void UniquifyPathW(wchar_t *path, int cap)
{
    wchar_t dir[MAX_PATH], base[MAX_PATH], ext[64], cand[1100];
    wchar_t *p, *q;
    int n = 1;
    p = wcsrchr(path, L'\\');
    if (!p) return;
    {
        size_t dl = (size_t)(p - path);
        if (dl >= MAX_PATH) dl = MAX_PATH - 1;
        memcpy(dir, path, dl * sizeof(WCHAR));
        dir[dl] = 0;
    }
    lstrcpynW(base, p + 1, MAX_PATH);
    ext[0] = 0;
    q = wcsrchr(base, L'.');
    if (q && q != base) {
        lstrcpynW(ext, q, 64);
        *q = 0;
    }
    if (!base[0]) return;
    do {
        if (n == 1)
            _snwprintf(cand, 1100, L"%s\\%s%s", dir, base, ext);
        else
            _snwprintf(cand, 1100, L"%s\\%s (%d)%s", dir, base, n, ext);
        n++;
    } while (GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES && n < 900);
    lstrcpynW(path, cand, cap);
}

BOOL ExtInListW(const wchar_t *url, const wchar_t *list)
{
    wchar_t ext[32];
    wchar_t clean[2048];
    const wchar_t *q, *p, *dot;
    int elen;
    q = wcspbrk(url, L"?#");
    if (q) {
        size_t L = (size_t)(q - url);
        if (L >= 2048) L = 2047;
        memcpy(clean, url, L * sizeof(WCHAR));
        clean[L] = 0;
    } else {
        lstrcpynW(clean, url, 2048);
    }
    p = wcsrchr(clean, L'/');
    if (!p) p = clean;
    dot = wcsrchr(p, L'.');
    if (!dot || wcslen(dot) < 2 || wcslen(dot) > 8) return FALSE;
    lstrcpynW(ext, dot + 1, 32);
    elen = (int)wcslen(ext);
    /* compare against space-separated token list */
    {
        const wchar_t *t = list;
        while (*t) {
            while (*t == L' ' || *t == L',') t++;
            if (!*t) break;
            {
                const wchar_t *e = t;
                while (*e && *e != L' ' && *e != L',') e++;
                if ((int)(e - t) == elen && _wcsnicmp(t, ext, elen) == 0) return TRUE;
                t = e;
            }
        }
    }
    return FALSE;
}
