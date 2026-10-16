/* browext.h — browser detection + TurboGrab Integration extension registration
 * TurboGrab registers its own browser extension via per-user policy (HKCU),
 * the same keys the Inno Setup installer writes. This gives the app a second,
 * self-service path: every launch re-applies the policy, so a browser only
 * needs one restart to pick the extension up.
 *
 * MIT License — Copyright (c) 2026 Abad Umair Channa / 3S Verse
 */
#ifndef TURBOGRAB_BROWEXT_H
#define TURBOGRAB_BROWEXT_H

#define TG_MAX_BROWSERS 8

typedef struct {
    char name[64];      /* display name, e.g. "Google Chrome"            */
    int  chromium;      /* 1 = Chromium policy, 0 = Firefox policy       */
    int  detected;      /* installed on this machine                     */
    int  registered;    /* our policy value is present and up to date    */
} TgBrowser;

/* Scan the machine for supported browsers and fill `out` (max `cap`).
 * Returns the number of entries written (0..TG_MAX_BROWSERS). */
int BrowsersScan(TgBrowser *out, int cap);

/* Write/refresh the extension policy for every detected browser.
 * Returns the number of browsers the policy was written for. */
int BrowsersApplyAll(void);

/* Register the extension policy for one browser (index into the last
 * BrowsersScan result). Returns 1 on success. */
int BrowsersApplyOne(int idx);

#endif /* TURBOGRAB_BROWEXT_H */
