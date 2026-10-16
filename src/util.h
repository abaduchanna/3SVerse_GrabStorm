/* GrabStorm — 3SVerse download accelerator for Windows
 * util.h — settings (registry), formatting (network speeds in BITS: Kb/s, Mb/s),
 * filename/URL helpers. MIT License — (c) 2026 Abad Umair Channa / 3S Verse
 */
#ifndef GS_UTIL_H
#define GS_UTIL_H

#include <windows.h>

#define SETTINGS_KEY L"Software\\3SVerse\\GrabStorm"

void SettingsSetStr(const wchar_t *name, const wchar_t *val);
void SettingsSetInt(const wchar_t *name, int val);
BOOL SettingsGetStr(const wchar_t *name, wchar_t *out, DWORD cap, const wchar_t *def);
int  SettingsGetInt(const wchar_t *name, int def);

/* Network speed = BITS per second (owner rule: lowercase b — Kb/s, Mb/s, never KB/s).
 * Auto-scale: one unit at a time — "854.2 Kb/s" or "1.54 Mb/s" or "2.10 Gb/s". */
void FormatSpeedBits(double bytesPerSec, wchar_t *out, int cap);

/* File SIZE is bytes (correctly capital B here — the speed rule is about Mb/s vs MB/s). */
void FormatSizeBytes(unsigned __int64 bytes, wchar_t *out, int cap);

void FormatEta(double seconds, wchar_t *out, int cap); /* "3m 12s", "1h 05m", "--" */

int  WideToUtf8(const wchar_t *w, char *out, int cap);
int  Utf8ToWide(const char *u, wchar_t *out, int cap);

/* %xx and '+' decoding (URL query values) with UTF-8 support */
void PercentDecodeW(const wchar_t *in, wchar_t *out, int cap);

/* strip \/:*?"<>| and control chars, trim dots/spaces, cap length */
void SanitizeFilenameW(wchar_t *name, int cap);

/* if path exists, insert " (n)" before the extension */
void UniquifyPathW(wchar_t *path, int cap);

/* url ends with one of the space-separated extensions in list (case-insensitive) */
BOOL ExtInListW(const wchar_t *url, const wchar_t *list);

#endif
