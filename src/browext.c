/* browext.c — browser detection + TurboGrab Integration extension registration
 *
 * Chromium browsers (Chrome / Edge / Brave / Vivaldi / Opera / Yandex):
 *   HKCU\Software\Policies\<root>\ExtensionInstallForcelist
 *       "1" = "<extid>;https://abaduchanna.github.io/3SVerse_TurboGrab/update.xml"
 *   HKCU\Software\Policies\<root>\ExtensionInstallSources
 *       "1" = "https://abaduchanna.github.io/*"
 * Firefox:
 *   HKCU\Software\Policies\Mozilla\Firefox\Extensions\Install
 *       "1" = "https://abaduchanna.github.io/3SVerse_TurboGrab/TurboGrab.xpi"
 *
 * Per-user HKCU policy needs no admin rights and is picked up the next time
 * the browser starts. Release-channel Firefox only honours policy installs
 * for signed extensions, so there the entry is best-effort (ESR/Dev/Nightly
 * install it silently; regular Firefox needs manual load — see README).
 *
 * MIT License — Copyright (c) 2026 Abad Umair Channa / 3S Verse
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "browext.h"

#define EXT_ID    "ooclbgcgolihefahehfjjcmlfdlhncno"
#define UPD_XML   "https://abaduchanna.github.io/3SVerse_TurboGrab/update.xml"
#define XPI_URL   "https://abaduchanna.github.io/3SVerse_TurboGrab/TurboGrab.xpi"
#define SRC_RULE  "https://abaduchanna.github.io/*"

typedef struct {
    const char *name;
    const char *smi;      /* Clients\StartMenuInternet subkey fragment      */
    const char *root;     /* Policies\<root> for HKCU                       */
    const char *exe1;     /* literal fallback paths                         */
    const char *exe2;
    const char *exe3;
} ChromiumDef;

static const ChromiumDef kChromium[] = {
    { "Google Chrome",  "Google.Chrome",        "Google\\Chrome",
      "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
      "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
      NULL },
    { "Microsoft Edge", "Microsoft.Edge",       "Microsoft\\Edge",
      "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
      "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
      NULL },
    { "Brave",          "BraveSoftware.Brave",  "BraveSoftware\\Brave",
      "C:\\Program Files\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
      "C:\\Program Files (x86)\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
      NULL },
    { "Vivaldi",        "Vivaldi.Vivaldi",      "Vivaldi",
      "C:\\Program Files\\Vivaldi\\Application\\vivaldi.exe",
      "C:\\Program Files (x86)\\Vivaldi\\Application\\vivaldi.exe",
      NULL },
    { "Opera",          "Opera.stable",         "Opera Software\\Opera",
      NULL, NULL, NULL },  /* exe found via appdata path below */
    { "Yandex Browser", "Yandex.YandexBrowser", "Yandex\\YandexBrowser",
      NULL, NULL, NULL },
};

#define kChromiumN ((int)(sizeof(kChromium) / sizeof(kChromium[0])))

/* ---------------- registry helpers ---------------- */

static int KeyExists(HKEY base, const char *sub)
{
    HKEY h;
    LONG r = RegOpenKeyExA(base, sub, 0, KEY_READ, &h);
    if (r == ERROR_SUCCESS) { RegCloseKey(h); return 1; }
    return 0;
}

static int FileExistsA(const char *p)
{
    DWORD a;
    if (!p || !p[0]) return 0;
    a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int OperaInstalled(void)
{
    char p[MAX_PATH];
    const char *appdata = getenv("LOCALAPPDATA");
    if (appdata && appdata[0]) {
        _snprintf(p, MAX_PATH, "%s\\Programs\\Opera\\opera.exe", appdata);
        p[MAX_PATH - 1] = 0;
        if (FileExistsA(p)) return 1;
        _snprintf(p, MAX_PATH, "%s\\Programs\\Opera GX\\opera.exe", appdata);
        p[MAX_PATH - 1] = 0;
        if (FileExistsA(p)) return 1;
    }
    return FileExistsA("C:\\Program Files\\Opera\\opera.exe") ||
           FileExistsA("C:\\Program Files (x86)\\Opera\\opera.exe");
}

static int YandexInstalled(void)
{
    char p[MAX_PATH];
    const char *appdata = getenv("LOCALAPPDATA");
    if (appdata && appdata[0]) {
        _snprintf(p, MAX_PATH, "%s\\Yandex\\YandexBrowser\\Application\\browser.exe", appdata);
        p[MAX_PATH - 1] = 0;
        if (FileExistsA(p)) return 1;
    }
    return FileExistsA("C:\\Program Files\\Yandex\\YandexBrowser\\browser.exe") ||
           FileExistsA("C:\\Program Files (x86)\\Yandex\\YandexBrowser\\browser.exe");
}

/* StartMenuInternet fragment match, e.g. "Google.Chrome" matches
 * "Google.Chrome", "Google.Chrome.CanaryUser" etc. */
static int SmiPresent(const char *frag)
{
    char sub[300];
    HKEY bases[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    int b;
    for (b = 0; b < 2; b++) {
        _snprintf(sub, sizeof(sub), "SOFTWARE\\Clients\\StartMenuInternet\\%s", frag);
        sub[sizeof(sub) - 1] = 0;
        if (KeyExists(bases[b], sub)) return 1;
        if (b == 0) {
            _snprintf(sub, sizeof(sub),
                      "SOFTWARE\\WOW6432Node\\Clients\\StartMenuInternet\\%s", frag);
            sub[sizeof(sub) - 1] = 0;
            if (KeyExists(HKEY_LOCAL_MACHINE, sub)) return 1;
        }
    }
    return 0;
}

static int ChromiumDetected(const ChromiumDef *d)
{
    if (SmiPresent(d->smi)) return 1;
    if (FileExistsA(d->exe1) || FileExistsA(d->exe2) || FileExistsA(d->exe3)) return 1;
    if (strcmp(d->name, "Opera") == 0)        return OperaInstalled();
    if (strcmp(d->name, "Yandex Browser") == 0) return YandexInstalled();
    return 0;
}

/* ---------------- policy read / write ---------------- */

static void WritePolicyValue(const char *sub, const char *valName, const char *data)
{
    HKEY h;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &h, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(h, valName, 0, REG_SZ, (const BYTE *)data,
                       (DWORD)strlen(data) + 1);
        RegCloseKey(h);
    }
}

static int ReadPolicyValue(const char *sub, const char *valName,
                           char *out, DWORD cap)
{
    HKEY h;
    DWORD type = 0, cb = cap;
    LONG r;
    out[0] = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, sub, 0, KEY_QUERY_VALUE, &h)
        != ERROR_SUCCESS) return 0;
    r = RegQueryValueExA(h, valName, NULL, &type, (BYTE *)out, &cb);
    RegCloseKey(h);
    return (r == ERROR_SUCCESS && type == REG_SZ);
}

static void ChromiumRegister(const char *root)
{
    char k[400], v[512];
    _snprintf(k, sizeof(k), "Software\\Policies\\%s\\ExtensionInstallForcelist", root);
    k[sizeof(k) - 1] = 0;
    _snprintf(v, sizeof(v), "%s;%s", EXT_ID, UPD_XML);
    v[sizeof(v) - 1] = 0;
    WritePolicyValue(k, "1", v);

    _snprintf(k, sizeof(k), "Software\\Policies\\%s\\ExtensionInstallSources", root);
    k[sizeof(k) - 1] = 0;
    WritePolicyValue(k, "1", SRC_RULE);
}

static void FirefoxRegister(void)
{
    WritePolicyValue("Software\\Policies\\Mozilla\\Firefox\\Extensions\\Install",
                     "1", XPI_URL);
}

static int ChromiumRegistered(const char *root)
{
    char k[400], v[512], want[512];
    _snprintf(k, sizeof(k), "Software\\Policies\\%s\\ExtensionInstallForcelist", root);
    k[sizeof(k) - 1] = 0;
    _snprintf(want, sizeof(want), "%s;%s", EXT_ID, UPD_XML);
    want[sizeof(want) - 1] = 0;
    if (!ReadPolicyValue(k, "1", v, sizeof(v))) return 0;
    return strcmp(v, want) == 0;
}

static int FirefoxRegistered(void)
{
    char v[512];
    if (!ReadPolicyValue("Software\\Policies\\Mozilla\\Firefox\\Extensions\\Install",
                         "1", v, sizeof(v))) return 0;
    return strcmp(v, XPI_URL) == 0;
}

/* ---------------- public API ---------------- */

static TgBrowser g_last[TG_MAX_BROWSERS];
static const char *g_lastRoot[TG_MAX_BROWSERS];
static int g_lastN = 0;

int BrowsersScan(TgBrowser *out, int cap)
{
    int i, n = 0;
    g_lastN = 0;

    for (i = 0; i < kChromiumN && n < cap && g_lastN < TG_MAX_BROWSERS; i++) {
        if (!ChromiumDetected(&kChromium[i])) continue;
        ZeroMemory(&out[n], sizeof(out[n]));
        lstrcpynA(out[n].name, kChromium[i].name, sizeof(out[n].name));
        out[n].chromium   = 1;
        out[n].detected   = 1;
        out[n].registered = ChromiumRegistered(kChromium[i].root);
        g_last[g_lastN] = out[n];
        g_lastRoot[g_lastN] = kChromium[i].root;
        g_lastN++;
        n++;
    }

    if (n < cap && g_lastN < TG_MAX_BROWSERS) {
        /* Firefox: StartMenuInternet + exe paths */
        int det = SmiPresent("MozillaFirefox") ||
                  FileExistsA("C:\\Program Files\\Mozilla Firefox\\firefox.exe") ||
                  FileExistsA("C:\\Program Files (x86)\\Mozilla Firefox\\firefox.exe");
        if (det) {
            ZeroMemory(&out[n], sizeof(out[n]));
            lstrcpynA(out[n].name, "Firefox", sizeof(out[n].name));
            out[n].chromium   = 0;
            out[n].detected   = 1;
            out[n].registered = FirefoxRegistered();
            g_last[g_lastN] = out[n];
            g_lastRoot[g_lastN] = NULL;
            g_lastN++;
            n++;
        }
    }
    return n;
}

int BrowsersApplyOne(int idx)
{
    if (idx < 0 || idx >= g_lastN) return 0;
    if (g_last[idx].chromium && g_lastRoot[idx]) {
        ChromiumRegister(g_lastRoot[idx]);
        g_last[idx].registered = ChromiumRegistered(g_lastRoot[idx]);
    } else if (!g_last[idx].chromium) {
        FirefoxRegister();
        g_last[idx].registered = FirefoxRegistered();
    } else {
        return 0;
    }
    return g_last[idx].registered;
}

int BrowsersApplyAll(void)
{
    TgBrowser tmp[TG_MAX_BROWSERS];
    int n, i, done = 0;

    n = BrowsersScan(tmp, TG_MAX_BROWSERS);
    for (i = 0; i < n; i++) {
        if (g_last[i].chromium && g_lastRoot[i]) {
            ChromiumRegister(g_lastRoot[i]);
            done++;
        } else if (!g_last[i].chromium) {
            FirefoxRegister();
            done++;
        }
    }
    return done;
}
