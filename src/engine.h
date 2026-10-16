/* GrabStorm — engine.h — multi-segment download engine (WinHTTP)
 * Dynamic file segmentation, resume, queue. MIT — (c) 2026 Abad Umair Channa / 3S Verse
 */
#ifndef GS_ENGINE_H
#define GS_ENGINE_H

#include <windows.h>

#define GS_MAXSEG     32
#define WM_APP_JOB    (WM_APP + 2)   /* wp=job id, lp=1 state/progress changed */
#define WM_APP_PROTO  (WM_APP + 3)   /* grabstorm:// forwarded at startup      */
#define WM_APP_TRAY   (WM_APP + 7)   /* tray icon callback                     */
#define WM_APP_MODAL  (WM_APP + 9)   /* modal helper pump quit                 */

enum {
    ST_QUEUED = 0,
    ST_CONNECTING,
    ST_DOWNLOADING,
    ST_PAUSED,
    ST_ERROR,
    ST_DONE
};

typedef struct Segment {
    unsigned __int64 start;   /* first byte owned by this segment            */
    unsigned __int64 end;     /* one past last byte (may be MAX for unknown) */
    unsigned __int64 pos;     /* absolute next byte to fetch                 */
    volatile LONG    quit;
} Segment;

typedef struct Job {
    int               id;
    wchar_t           url[2048];
    wchar_t           file[MAX_PATH];  /* full save path (may be empty until probe) */
    wchar_t           dir[MAX_PATH];
    unsigned __int64  total;           /* 0 = unknown */
    volatile LONG64   got;
    LONG64            gotPrev;         /* UI speed delta base */
    ULONGLONG         prevTick;
    wchar_t           etag[128];
    wchar_t           lastmod[96];
    int               ranges;          /* 1 yes, -1 no, 0 unknown */
    int               nseg;
    Segment           segs[GS_MAXSEG];
    volatile LONG     state;
    wchar_t           err[256];
    double            speed;           /* bytes/s EMA (UI-computed) */
    CRITICAL_SECTION  cs;
    HANDLE            cancelEvt;       /* pause/stop (resumable) */
    HANDLE            hThread;
    volatile LONG     activeSegs;
    volatile LONG     fatal;
    volatile LONG     noMoreData;
    int               resume;          /* try .gstmeta resume on start */
    struct Job       *next;
} Job;

void EngineInit(HWND main);
void EngineShutdown(void);
int  EngineCount(void);
Job *EngineFirst(void);
Job *EngineById(int id);
Job *EngineAt(int index);
int  EngineAdd(const wchar_t *url, const wchar_t *dir, const wchar_t *name, int startNow);
void EngineStart(int id);          /* queued -> run (with resume attempt) */
void EngineStop(int id);           /* pause/stop, resumable               */
void EngineResume(int id);         /* paused/error -> run again           */
void EngineRemove(int id, int deleteFile);
void EngineTick(void);             /* UI 500ms: speeds, queue scheduling  */
double EngineAggSpeed(void);       /* aggregate bytes/s (bits-formatted in UI) */
int  EngineActiveCount(void);
int  EngineQueuedCount(void);
int  EngineDoneCount(void);
void EngineSetSegments(int n);
void EngineSetMaxActive(int n);
int  EngineGetSegments(void);
int  EngineGetMaxActive(void);
void EngineDefaultDir(wchar_t *out, int cap);

#endif
