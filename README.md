# GrabStorm — 3SVerse Download Accelerator for Windows

**GrabStorm** is a free and open multi-segment download accelerator for Windows 10/11.
It moves files over HTTP(S) with up to 32 parallel connections, keeps per-download
state so nothing is lost when you pause or close the app, and integrates with every
major browser so one click sends the file straight into the queue.

> GrabStorm is an independent, clean-room implementation — it contains no code,
> assets or resources from any other download manager. MIT licensed.

![brand](https://img.shields.io/badge/3SVerse-GrabStorm-6ee7ef)

---

## What GrabStorm does

| Feature | Details |
|---|---|
| Multi-segment acceleration | Files split across up to **32 parallel HTTP range connections**; a finished connection dynamically steals the tail of the largest remaining segment, so slow segments never stall the whole download |
| Resume & pause | Every download keeps a `.gstmeta` state file with segment positions; pause, stop, close the app, even reboot — Resume continues exactly where it left off (validated against ETag / Last-Modified so the file can't be stitched from different versions) |
| Queue | Unlimited list with a configurable number of **max active downloads** (1–10); queued items start automatically as slots free up |
| Retry with backoff | Broken connections reconnect at the exact byte position (up to 8 attempts with growing delays) |
| Unknown-size streaming | Servers without range support are handled with a single robust stream, still fully resumable across restarts |
| Clipboard monitor | Copy a link to an installer/archive/media file and GrabStorm opens the grab dialog for you (on/off in Options) |
| Browser integration | The bundled **GrabStorm Integration** extension adds a right-click *Download with GrabStorm* action and auto-captures direct file links; Setup can auto-install it into every Chromium browser and Firefox via per-user policies |
| `grabstorm://` protocol | Any app or web page can hand a URL to GrabStorm via `grabstorm://add?u=<url>&r=<referrer>` |
| 3SVerse brand UI | Full xfer design system: the 3SVerse wordmark, hero spiral, glossy orb, cards and cyan→magenta CTAs — with proper **dark and light themes** (persisted, DWM-aware title bar) |
| Tray resident | Close the window and GrabStorm keeps working from the tray; completion and error balloon notifications |
| 100% local | No account, no cloud, no telemetry. WinHTTP engine, single native exe, no runtime dependencies |

## Speed units (read this once)

GrabStorm displays network speed in **bits per second** with the correct lowercase
**b**: `Kb/s` (kilobits/s), `Mb/s` (megabits/s), `Gb/s`. One value is shown at a
time, auto-scaled. This is the network-industry convention (the same unit your
internet package is sold in) and is deliberately different from **file sizes**,
which are shown in bytes (`KB`, `MB`, `GB`) — 8 bits = 1 byte.

## Install

### Setup (recommended)
1. Download `GrabStorm_v1.0.0_Setup.exe` from the [releases page](https://github.com/abaduchanna/3SVerse_TurboGrab/releases).
2. Run it — no admin rights needed (installs per-user).
3. Keep **Auto-install the GrabStorm browser extension** checked; the installer
   writes per-user policies for every Chromium browser it finds
   (Chrome, Edge, Brave, Opera, Vivaldi, Yandex) plus Firefox.
4. **Restart your browsers once** — policy-installed extensions load at browser start.
5. Right-click any file link → *Download with GrabStorm*.

### Portable
Unzip `GrabStorm_Portable.zip` anywhere and run `GrabStorm.exe`. Browser integration
and the `grabstorm://` protocol are not registered by the portable build.

## Browser extension details

- Chromium browsers load the extension through `ExtensionInstallForcelist` +
  `ExtensionInstallSources` user policies pointing at the self-hosted CRX3 on
  GitHub Pages (`update.xml` at `https://abaduchanna.github.io/3SVerse_TurboGrab/update.xml`).
- Firefox: the XPI is registered via the Firefox `Extensions\Install` policy — this
  works on **Firefox ESR / Dev / Nightly**; release-channel Firefox only accepts
  *signed* add-ons, so there it can be loaded temporarily via `about:debugging`.
- A full installer log (browsers detected, every policy write, verification
  read-back) is written to `%LOCALAPPDATA%\3SVerse\GrabStorm\install.log`.
- Uninstalling GrabStorm removes every policy it wrote (only if it still owns them).

Manual option: download `GrabStorm_Extension_Chrome.zip`, enable Developer mode on
`chrome://extensions`, and *Load unpacked*.

## Using GrabStorm

- **Add URL** — paste any HTTP(S) link; set the save-as name and folder.
- **Start / Start all / Stop / Stop all** — queue control.
- **Delete** — removes from list; you choose whether the file is deleted too.
- Right-click a row for: open file / open folder / resume / pause / restart /
  copy URL / delete. Double-click opens the finished file.
- **Options** — segments per download (1–32), max active downloads (1–10),
  download folder, clipboard monitor toggle.

Everything is stored under `HKCU\Software\3SVerse\GrabStorm` and the download
folder — leave nothing behind when you uninstall.

## Build from source

```
windres src/res.rc -O coff -o src/res_res.o
gcc src/main.c src/engine.c src/util.c src/res_res.o -o GrabStorm.exe ^
    -mwindows -O1 -lwinhttp -lws2_32 -ladvapi32 -lcomctl32 -lshell32 ^
    -lole32 -lgdi32 -luser32 -s
```

CI (`.github/workflows/windows.yml`) builds the exe, packs the browser extensions,
builds the installer with Inno Setup, publishes the CRX3/XPI/update.xml to the
`gh-pages` branch and attaches everything to the release on `grabstorm-v*` tags.

## Project layout

```
src/            GrabStorm.exe sources (main.c UI, engine.c downloader, util.c)
src/res/        icon, manifest, brand art (logo / spiral / orb RCDATA)
extension/      GrabStorm Integration browser extension (MV3 + Firefox)
scripts/        CRX3 packer/verifier + asset generator
signing/        extension signing key (pins the extension ID)
installer.iss   Inno Setup script (per-user, browser policies, protocol)
```

## Privacy

GrabStorm talks only to the server of the file you are downloading. No analytics,
no crash reporting, no phone-home. The extension talks only to the local
`grabstorm://` handler and to `api.sync` storage for its single setting.

## License

MIT — Copyright (c) 2026 Abad Umair Channa / 3S Verse (www.3sverse.com)
