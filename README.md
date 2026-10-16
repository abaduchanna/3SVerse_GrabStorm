# TurboGrab — Free & Open Download Accelerator for Windows

![License](https://img.shields.io/badge/license-MIT-green)
![Platform](https://img.shields.io/badge/Windows-10%2F11-0078d4)
![Language](https://img.shields.io/badge/C-Win32%20%2B%20WinHTTP-blue)
![Tracking](https://img.shields.io/badge/tracking-none-success)

**TurboGrab** is a free, open-source download manager and accelerator for
Windows. Paste a link, press **Start**, and the file is pulled down with
multiple parallel connections — then paused, resumed, queued and sorted into
folders exactly the way a modern download manager should.

It is a clean-room alternative to proprietary download accelerators such as
*Internet Download Manager (IDM)*: the same core ideas (multi-segment
downloads, resume capability, clipboard monitoring, category folders), but
**written from scratch in C** — no code, assets, trademarks or installers
from any proprietary product. TurboGrab is not affiliated with Tonec s.r.o.
It has **no accounts, no ads, no limits and no telemetry**, and it is
released under the MIT license.

## Features (v1.0.0)

- **Multi-segment downloads** — up to **16 parallel connections** per file
  (8 by default) via HTTP `Range` requests. Servers that do not support
  ranges are handled with a single-stream fallback automatically.
- **Pause & resume** — pause any download, close the app, and continue later
  exactly where it stopped. The download queue (with per-segment progress)
  survives restarts. Partial files are kept as `.part` and renamed to the
  final name on completion.
- **Download queue** — up to **10 downloads run at the same time**
  (3 by default); everything else waits in the queue and starts
  automatically as slots free up.
- **Live progress** — per-file progress bars, size, real-time speed and
  estimated time left, drawn directly in the list.
- **Clipboard monitor** — copy a `http(s)://` link anywhere in Windows and
  TurboGrab pops up the "Add download" window with the URL prefilled.
  Can be turned off in Settings.
- **Category folders** — files are auto-sorted into `Videos`, `Music`,
  `Documents`, `Programs`, `Compressed` and `Images` subfolders inside your
  download folder (by extension). Can be turned off in Settings.
- **System tray** — closing to the tray keeps every download running;
  double-click restores the window.
- **Retry logic** — every segment is retried automatically on transient
  network errors; a failed download can be resumed from where it failed.
- **Server filename support** — honours `Content-Disposition` filenames
  when the server provides them, with the URL name as fallback.
- **No dependencies** — one small `.exe`, nothing bundled, nothing
  phoning home. Downloads go straight from the web server to your disk.

## Install (Windows 10/11)

### Setup (recommended)
1. Download `TurboGrab_v1.0.0_Setup.exe` from the
   [releases page](https://github.com/abaduchanna/3SVerse_TurboGrab/releases/latest).
2. Run it — **no administrator rights needed** (installs per-user into
   `%LOCALAPPDATA%\3SVerse\TurboGrab`).
3. Optionally tick the desktop shortcut on the last page.

### Portable
Unzip `TurboGrab_Portable.zip` anywhere and run `TurboGrab.exe`.

## Usage

1. Copy a file URL, or press **Add URL** and paste it.
2. Confirm the **Save to** folder (category subfolders are added
   automatically when enabled).
3. Press **Start Download**. Watch the progress bar and speed.
4. Use **Pause** / **Resume** any time; **Delete** removes the entry from
   the list (the partial `.part` file stays on disk).
5. Double-click a completed row to open the file, or use
   **Open Folder**.

## Settings

| Setting | Range | Default | Meaning |
|---|---|---|---|
| Segments per download | 1–16 | 8 | Parallel connections per file |
| Downloads at the same time | 1–10 | 3 | Queue width |
| Default download folder | — | `Downloads` | Where new jobs start |
| Watch clipboard | on/off | on | Popup when a link is copied |
| Category folders | on/off | on | Auto-sort by file type |

Settings and the download queue are stored in
`%APPDATA%\TurboGrab\turbograb.ini`.

## Privacy

- No analytics, no telemetry, no accounts, no remote servers.
- The only network traffic is the downloads themselves, made by your
  machine, to the servers you choose.
- Nothing about your downloads ever leaves your computer.

## Roadmap

- Browser extension integration (Chrome / Firefox download interception)
- Batch import (list of URLs from a text file)
- Download scheduler (start at a given time, shut down after finishing)
- Site grabber and speed limiter
- Video page downloads — already available today via the sister project
  [XVidGrab desktop](https://github.com/abaduchanna/xvidgrab) (yt-dlp engine)

## Build from source

Any MinGW-w64 toolchain:

```
windres src/icon.rc -O coff -o src/icon_res.o
gcc src/main.c src/icon_res.o -o TurboGrab.exe -mwindows \
    -lwinhttp -lcomctl32 -lshell32 -lole32 -lgdi32 -luser32 -lkernel32 -s
```

CI (`.github/workflows/windows.yml`) compiles the exe with MinGW gcc and the
Inno Setup installer on every push; `v*` tags publish a GitHub release
automatically.

## Disclaimer

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY. Download only content you have the right to keep, and respect
the terms of service of the sites you download from. TurboGrab is an
independent project — it is not affiliated with, endorsed by, or derived
from Tonec s.r.o. or Internet Download Manager.

## License

[MIT](LICENSE) © 2026 Abad Umair Channa / 3S Verse
