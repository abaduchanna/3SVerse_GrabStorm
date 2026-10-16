/* localsrv.h — TurboGrab browser integration (localhost HTTP server)
 * The browser extension POSTs download links to 127.0.0.1:<port>/add;
 * the request is forwarded to the main window and queued as a download.
 *
 * MIT License — Copyright (c) 2026 Abad Umair Channa / 3S Verse
 */
#ifndef TURBOGRAB_LOCALSRV_H
#define TURBOGRAB_LOCALSRV_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* posted to the main window with LPARAM = BrowserReq* (heap, UI frees it) */
#define WM_APP_BROWSE (WM_APP + 3)

/* one pending request from the browser extension (heap allocated) */
typedef struct BrowserReq {
    char url[2048];
    char name[300];
    int  autoStart;   /* 1 = queue immediately, 0 = open "Add download" dialog */
} BrowserReq;

/* server configuration passed in from main.c */
typedef struct BrowserSrvCfg {
    HWND        hwnd;     /* window that receives WM_APP_BROWSE messages   */
    int         enabled;  /* 0 = stop / do not run the server              */
    int         port;     /* TCP port on 127.0.0.1 (default 8777)          */
    const char *token;    /* required X-TurboGrab-Token header value       */
} BrowserSrvCfg;

/* (Re)start the server with the given config. Safe to call again to apply
 * new settings — the previous thread is stopped first. No-op if disabled. */
void BrowserSrvStart(const BrowserSrvCfg *cfg);

/* Stop the server thread (called on app exit). */
void BrowserSrvStop(void);

#ifdef __cplusplus
}
#endif

#endif /* TURBOGRAB_LOCALSRV_H */
