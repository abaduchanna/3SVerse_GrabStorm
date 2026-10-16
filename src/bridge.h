/* GrabStorm — 3SVerse download accelerator for Windows
 * bridge.h — loopback HTTP bridge for the browser extension.
 * MIT License — (c) 2026 Abad Umair Channa / 3S Verse
 */
#ifndef GS_BRIDGE_H
#define GS_BRIDGE_H

#include <windows.h>

#define WM_APP_BRIDGE (WM_APP + 11)  /* lp=HeapAlloc'd BridgeAdd* from bridge */

/* Extension capture request handed to the UI thread over WM_APP_BRIDGE.
 * Allocated with HeapAlloc by the bridge thread, freed with HeapFree
 * by the UI thread after use. */
typedef struct BridgeAdd {
    wchar_t url[2048];
    wchar_t referrer[2048];
    wchar_t name[160];
    wchar_t cookie[1024];
    unsigned __int64 size;   /* 0 = unknown */
} BridgeAdd;

/* Start the loopback listener thread (no-op if BridgeOn=0). Reads
 * BridgePort / BridgeToken settings. Safe to call once at startup. */
void BridgeStart(HWND main);

/* Stop listener + close sockets. Safe from any thread; joins worker. */
void BridgeStop(void);

/* Current listen port (registry value, default 14599). */
int  BridgePort(void);

/* TRUE when the listener socket is bound and accepting. */
BOOL BridgeRunning(void);

/* Generate a fresh random token, persist it, restart the listener. */
void BridgeRenewToken(HWND main);

/* Apply changed port/token/on settings: restart listener. */
void BridgeRestart(HWND main);

#endif /* GS_BRIDGE_H */
