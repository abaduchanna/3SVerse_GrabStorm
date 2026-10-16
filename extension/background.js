/* TurboGrab Integration — background service worker
 * Talks to the TurboGrab Windows app on http://127.0.0.1:8777 (default).
 * Protocol:
 *   GET  /ping            -> connection test (X-TurboGrab-Token header optional)
 *   POST /add {url,auto}  -> queue download (token required)
 *
 * MIT License — Copyright (c) 2026 Abad Umair Channa / 3S Verse
 */

const DEFAULTS = {
  serverUrl: "http://127.0.0.1:8777",
  token: "",
  takeover: true,           // intercept browser downloads and hand them to TurboGrab
  takeoverAll: false,       // intercept every http(s) download, not just known file types
  autoStart: true,          // queue directly (no "Add download" dialog in TurboGrab)
  notifyFail: true,         // show a notification when TurboGrab is not reachable
  exts: [
    "zip","rar","7z","tar","gz","bz2","xz","iso","dmg","exe","msi","apk","deb","rpm","appimage",
    "pdf","doc","docx","xls","xlsx","ppt","pptx","csv","epub",
    "mp4","mkv","avi","mov","wmv","flv","webm","m4v","ts",
    "mp3","wav","flac","ogg","m4a","opus","aac","mid"
  ].join(",")
};

async function cfg() {
  const got = await chrome.storage.local.get(DEFAULTS);
  return got;
}

function normalizeUrl(u) {
  if (!u) return null;
  u = String(u).trim();
  if (/^chrome|^about:|^moz-extension|^chrome-extension|^devtools/i.test(u)) return null;
  if (!/^https?:\/\//i.test(u)) return null;
  return u;
}

function isLocalServer(url) {
  return /^https?:\/\/(127\.0\.0\.1|localhost)(:\d+)?\//i.test(url);
}

function extOf(nameOrUrl) {
  const m = /\.([a-z0-9]{1,8})(?:[?#].*)?$/i.exec(nameOrUrl.split("?")[0]);
  return m ? m[1].toLowerCase() : "";
}

function wantsTakeover(c, url, filename) {
  if (!c.takeover) return false;
  const e = extOf(filename || url);
  if (c.takeoverAll) return true;
  const list = String(c.exts || "").split(",").map(s => s.trim().toLowerCase()).filter(Boolean);
  return e && list.indexOf(e) !== -1;
}

/* ------------------------------------------------------------------ */

async function ping() {
  const c = await cfg();
  const base = c.serverUrl.replace(/\/+$/, "");
  try {
    const r = await fetch(base + "/ping", {
      method: "GET",
      headers: { "X-TurboGrab-Token": c.token || "" }
    });
    if (!r.ok) return { ok: false, reason: "HTTP " + r.status };
    const j = await r.json();
    return { ok: !!j.ok, auth: !!j.auth, version: j.version || "" };
  } catch (e) {
    return { ok: false, reason: "not reachable" };
  }
}

async function sendToTurboGrab(url, auto) {
  const c = await cfg();
  const base = c.serverUrl.replace(/\/+$/, "");
  try {
    const r = await fetch(base + "/add", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-TurboGrab-Token": c.token || ""
      },
      body: JSON.stringify({ url: url, auto: !!auto })
    });
    if (r.status === 401) {
      failNote("TurboGrab rejected the token", "Open the extension options and paste the token from TurboGrab Settings.");
      return false;
    }
    if (!r.ok) {
      failNote("TurboGrab error", "The app answered with HTTP " + r.status + ".");
      return false;
    }
    return true;
  } catch (e) {
    failNote("TurboGrab is not running", "Start TurboGrab (Settings > Browser integration must be ON).");
    return false;
  }
}

function failNote(title, msg) {
  chrome.notifications.create({
    type: "basic",
    iconUrl: "icons/icon128.png",
    title: "TurboGrab — " + title,
    message: msg
  });
}

/* ------------------------------------------------------------------ */
/* context menu: "Download with TurboGrab"                            */
/* ------------------------------------------------------------------ */

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({
      id: "tg-root",
      title: "TurboGrab",
      contexts: ["link", "video", "audio", "image"]
    });
    chrome.contextMenus.create({
      id: "tg-link",
      parentId: "tg-root",
      title: "Download link with TurboGrab",
      contexts: ["link"]
    });
    chrome.contextMenus.create({
      id: "tg-media",
      parentId: "tg-root",
      title: "Download media with TurboGrab",
      contexts: ["video", "audio", "image"]
    });
    chrome.contextMenus.create({
      id: "tg-page",
      parentId: "tg-root",
      title: "Download this file (if it is one)",
      contexts: ["page", "frame"]
    });
  });
});

chrome.contextMenus.onClicked.addListener(async (info, tab) => {
  let url = null;
  if (info.menuItemId === "tg-link") url = info.linkUrl;
  else if (info.menuItemId === "tg-media") url = info.srcUrl;
  else url = info.linkUrl || info.srcUrl || info.pageUrl;
  url = normalizeUrl(url);
  if (!url) return;
  const c = await cfg();
  await sendToTurboGrab(url, c.autoStart);
});

/* ------------------------------------------------------------------ */
/* takeover of browser downloads (IDM-style)                          */
/* ------------------------------------------------------------------ */

chrome.downloads.onCreated.addListener(async (item) => {
  const c = await cfg();
  if (!c.takeover) return;
  const url = normalizeUrl(item.finalUrl || item.url);
  if (!url) return;
  if (isLocalServer(url)) return;
  if (!wantsTakeover(c, url, item.filename)) return;

  let cancelled = true;
  try { cancelled = await chrome.downloads.cancel(item.id); } catch (e) { cancelled = false; }
  if (!cancelled) return;             // browser finished it before we could cancel — do not double-download
  await sendToTurboGrab(url, c.autoStart);
});

/* ------------------------------------------------------------------ */

chrome.runtime.onMessage.addListener((msg, sender, reply) => {
  if (!msg || !msg.type) return;
  if (msg.type === "ping") {
    ping().then(reply);
    return true;                       // async reply
  }
  if (msg.type === "send") {
    sendToTurboGrab(msg.url, msg.auto).then(reply);
    return true;
  }
});
