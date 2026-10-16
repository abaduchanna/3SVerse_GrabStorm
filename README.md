# TurboGrab — Free & Open Download Accelerator for Windows

![License](https://img.shields.io/badge/license-MIT-green)
![Platform](https://img.shields.io/badge/Windows-10%2F11-0078d4)
![Language](https://img.shields.io/badge/C-Win32%20%2B%20WinHTTP-blue)
![Tracking](https://img.shields.io/badge/tracking-none-success)

**TurboGrab** is a free, open-source download manager and accelerator for
Windows. Paste a link, press **Start**, and the file is pulled down with
multiple parallel connections — then paused, resumed, queued and sorted into
folders exactly the way a modern download manager should. A companion
**browser extension** (Chrome / Edge / Brave / Firefox) hands links and
browser downloads straight to the app, just like the classic download
managers do. The Setup installs that extension into every installed browser
automatically — and since **v1.3.0 the app itself also re-registers the
extension on every launch**, so a single restart of any browser is enough
for it to appear.

Since **v1.3.0 TurboGrab ships with a completely redesigned dark modern UI**:
a flat icon toolbar, custom-drawn download list with inline colored progress
bars, status dots, a live speed/counters strip, dark dialogs and a new
**Browsers** window that shows, per browser, whether the TurboGrab extension
is registered — with one-click Register / Repair buttons.

It is a clean-room alternative to proprietary download accelerators such as
*Internet Download Manager (IDM)*: the same core ideas (multi-segment
downloads, resume capability, clipboard monitoring, category folders), but
**written from scratch in C** — no code, assets, trademarks or installers
from any proprietary product. TurboGrab is not affiliated with Tonec s.r.o.
It has **no accounts, no ads, no limits and no telemetry**, and it is
released under the MIT license.

## Features (v1.3.0)

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
- **Browser extension integration (v1.1.0)** — install the TurboGrab
  Integration extension and downloads can be sent to the app from any page:
  right-click any link/media and choose **TurboGrab**, or let the extension
  **take over matching browser downloads** automatically (archive, program,
  media and document types by default — configurable). The extension talks to
  a tiny localhost server built into TurboGrab (127.0.0.1 only, protected by
  a per-install token). Works with Chrome, Edge, Brave, Opera and Firefox.
- **Modern dark UI (v1.3.0)** — fully custom-drawn interface: dark theme
  with rounded flat toolbar buttons (Add URL / Resume / Pause / Delete /
  Open Folder / Browsers / Settings), taller list rows with alternating
  colors, bold file names, rounded per-download progress bars that change
  color by state (blue downloading, orange paused, green completed, red
  failed), colored status dots, a live bottom strip (Active / Speed /
  Queued / Completed), dark title bars and dialogs, and a friendly
  empty-state hint.
- **In-app extension registration (v1.3.0)** — on every start the app
  detects the installed browsers (Chrome, Edge, Brave, Vivaldi, Opera,
  Yandex, Firefox) and re-writes the per-user extension policy for each of
  them — no installer needed, it self-heals even if the Setup task was
  skipped. The new **Browsers** window shows every detected browser with
  its registration status and **Register / Repair** buttons.
- **One-click extension auto-install (v1.2.0)** — the Setup registers the
  extension into **every Chromium browser it detects** (Chrome, Edge, Brave,
  Opera, Vivaldi, Yandex) plus Firefox, using per-user browser policies
  (`HKCU\Software\Policies\...` — no admin rights, nothing outside the
  browser's official policy system). The extension files are served from
  this repo's own GitHub Pages (`abaduchanna.github.io/3SVerse_TurboGrab`)
  as a signed CRX3 with a fixed extension id
  (`ooclbgcgolihefahehfjjcmlfdlhncno`) and a Chromium `update.xml` manifest,
  so browsers can install **and silently update** it without the Chrome Web
  Store. Restart the browser once after installing TurboGrab and the
  extension is there — "Installed by your administrator" is expected for
  policy installs. Uninstalling TurboGrab removes the policies and the
  extension. Firefox note: release-channel Firefox requires AMO-signed
  add-ons; the policy install works on ESR/Dev/Nightly, and everywhere else
  the extension can still be loaded manually (see below).
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
1. Download `TurboGrab_v1.3.0_Setup.exe` from the
   [releases page](https://github.com/abaduchanna/3SVerse_TurboGrab/releases/latest).
2. Run it — **no administrator rights needed** (installs per-user into
   `%LOCALAPPDATA%\3SVerse\TurboGrab`).
3. Leave **Auto-install the TurboGrab extension into all installed
   browsers** ticked (it is by default).
4. After the install finishes, **restart your browsers once** — the
   TurboGrab Integration extension appears automatically in Chrome, Edge,
   Brave, Opera, Vivaldi, Yandex and (on ESR/Dev/Nightly) Firefox.
5. If a browser did not pick the extension up, open **Browsers** in the
   TurboGrab toolbar: every detected browser is listed with its status —
   press **Register / Repair** next to it (or **Register / Repair all**)
   and restart that browser again.
6. Open TurboGrab > **Settings** and press **Copy token**, then open the
   extension's **Settings** page and paste the token. Press **Test
   connection** — it should say *Connected*.
7. Done. Right-click any link → **TurboGrab → Download link with
   TurboGrab**, or let the extension take over downloads automatically
   (toggle in the extension popup).

### Portable
Unzip `TurboGrab_Portable.zip` anywhere and run `TurboGrab.exe`.

### Manual extension install (if you unticked auto-install, or use Firefox)
1. `TurboGrab_Extension_Chrome.zip` (Chrome, Edge, Brave, Opera) or
   `TurboGrab_Extension_Firefox.zip` (Firefox) from the releases page (also
   bundled into `%LOCALAPPDATA%\3SVerse\TurboGrab\extensions` by the Setup).
2. Unzip it to a folder you keep around.
3. **Chrome/Edge/Brave:** open `chrome://extensions`, enable **Developer
   mode**, click **Load unpacked** and select the unzipped folder.
   **Firefox:** open `about:debugging#/runtime/this-firefox`, click
   **Load Temporary Add-on** and pick `manifest.json` (temporary), or on
   ESR/Dev/Nightly use `about:addons` > gear icon > **Install Add-on From
   File** for a permanent install.

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
| Browser integration | on/off | on | Localhost server the extension talks to (port 8777) |
| Access token | 32 hex chars | auto-generated | Shown in Settings; the extension must present it |

Settings and the download queue are stored in
`%APPDATA%\TurboGrab\turbograb.ini`.

## Privacy

- No analytics, no telemetry, no accounts, no remote servers.
- The only network traffic is the downloads themselves, made by your
  machine, to the servers you choose.
- The browser integration server listens on **127.0.0.1 only** (never on the
  network) and requires a random per-install token before it accepts a link.
- Nothing about your downloads ever leaves your computer.

## Roadmap

- Batch import (list of URLs from a text file)
- Download scheduler (start at a given time, shut down after finishing)
- Site grabber and speed limiter
- Video page downloads — already available today via the sister project
  [XVidGrab desktop](https://github.com/abaduchanna/xvidgrab) (yt-dlp engine)

## Build from source

Any MinGW-w64 toolchain:

```
windres src/icon.rc -O coff -o src/icon_res.o
gcc src/main.c src/localsrv.c src/browext.c src/icon_res.o -o TurboGrab.exe -mwindows \
    -lwinhttp -lws2_32 -ladvapi32 -lcomctl32 -lshell32 -lole32 -lgdi32 \
    -luser32 -lkernel32 -s
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
