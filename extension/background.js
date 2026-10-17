/* GrabStorm Integration — background worker (MV3 service worker / FF MV2 script)
 *
 * Routes downloads into the GrabStorm desktop app:
 *   1. Silent bridge capture — probes the app's loopback HTTP bridge
 *      (GET /status/204+grabstorm) and POSTs captures to /add with a token.
 *   2. Download takeover — browser download events for capturable file types
 *      are cancelled and re-routed into GrabStorm (accelerated, resumable).
 *   3. Media sniffing — media/object requests are recorded as candidates so
 *      the in-page panel and the context menu can offer them.
 *   4. Fallback — grabstorm:// protocol hand-off when the app is not running.
 */
'use strict';

const DEF = {
  autoCapture: true,   // intercept browser downloads of capturable types
  videoPanel: true,    // in-page panel on video players
  captureAll: false,   // intercept every download, not just known types
  port: 14599,
  token: ''
};

let SET = { ...DEF };
let online = false;
let lastProbe = 0;

/* file types GrabStorm intercepts by default (accelerators pay off here) */
const CAPTURE_EXTS = 'exe msi msix zip rar 7z cab tar gz tgz bz2 xz iso img bin apk ' +
  'mp4 mkv avi mov wmv flv webm m4v ts m2ts mp3 flac wav ogg m4a aac ' +
  'pdf doc docx xls xlsx ppt pptx epub dmg pkg deb rpm jar';

chrome.storage.sync.get(DEF, (s) => { SET = { ...DEF, ...s }; probe(true); });
chrome.storage.onChanged.addListener((ch, area) => {
  if (area !== 'sync') return;
  for (const k of Object.keys(DEF)) if (ch[k]) SET[k] = ch[k].newValue;
  probe(true);
});

/* ------------------------------------------------------------------ */
/* bridge transport                                                    */
/* ------------------------------------------------------------------ */
function bridgeBase() {
  return `http://127.0.0.1:${SET.port || DEF.port}`;
}

async function probe(force) {
  const now = Date.now();
  if (!force && now - lastProbe < 20000) return online;
  lastProbe = now;
  try {
    const r = await fetch(`${bridgeBase()}/status/204+grabstorm`, {
      method: 'GET', cache: 'no-store', signal: AbortSignal.timeout(2500)
    });
    online = (r.status === 204 || r.status === 200);
  } catch (e) { online = false; }
  return online;
}

async function bridgeAdd(url, meta) {
  if (!(await probe(false))) return false;
  for (let attempt = 0; attempt < 2; attempt++) {
    if (!SET.token && !(await acquireBridgeToken())) return false;
    try {
    const body = JSON.stringify({
      url,
      referrer: meta.referrer || '',
      name: meta.name || '',
      cookie: meta.cookie || '',
      size: meta.size || 0
    });
    const r = await fetch(`${bridgeBase()}/add`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'X-GrabStorm-Token': SET.token || ''
      },
      body, cache: 'no-store', signal: AbortSignal.timeout(5000)
    });
      if (r.ok) { setBadge(''); return true; }
      if (r.status === 403 && attempt === 0) {
        SET.token = '';
        continue;
      }
      if (r.status === 403) setBadge('!');
      return false;
    } catch (e) { return false; }
  }
  return false;
}

async function acquireBridgeToken() {
  try {
    const r = await fetch(`${bridgeBase()}/token`, {
      method: 'GET', cache: 'no-store', signal: AbortSignal.timeout(2500)
    });
    if (!r.ok) return false;
    const data = await r.json();
    if (!data || typeof data.token !== 'string' || data.token.length < 16) return false;
    SET.token = data.token;
    return true;
  } catch (e) { return false; }
}

function protoFallback(u, ref) {
  const url = 'grabstorm://add?u=' + encodeURIComponent(u) +
              '&r=' + encodeURIComponent(ref || '');
  chrome.tabs.create({ url });
}

async function capture(url, meta, tab) {
  if (!url) return;
  const ok = await bridgeAdd(url, meta || {});
  if (ok) return;
  if (tab && tab.id != null) {
    try {
      await chrome.tabs.sendMessage(tab.id, { type: 'gs-proto',
        url: 'grabstorm://add?u=' + encodeURIComponent(url) +
             '&r=' + encodeURIComponent((meta && meta.referrer) || '') });
      return;
    } catch (e) { /* fall through */ }
  }
  protoFallback(url, (meta && meta.referrer) || '');
}

function setBadge(txt, color) {
  if (!chrome.action) return;
  chrome.action.setBadgeText({ text: txt || '' });
  if (color) chrome.action.setBadgeBackgroundColor({ color });
}

function extOf(u) {
  try {
    const p = new URL(u).pathname;
    const m = p.match(/\.([a-z0-9]{2,5})$/i);
    return m ? m[1].toLowerCase() : '';
  } catch (e) { return ''; }
}

function isCapturable(u) {
  if (SET.captureAll) return true;
  const e = extOf(u);
  if (!e) return false;
  return (' ' + CAPTURE_EXTS + ' ').includes(' ' + e + ' ');
}

/* ------------------------------------------------------------------ */
/* download takeover                                                   */
/* ------------------------------------------------------------------ */
if (chrome.downloads && chrome.downloads.onCreated) {
  chrome.downloads.onCreated.addListener(async (item) => {
    if (!SET.autoCapture) return;
    if (!item || !item.url || item.url.startsWith('grabstorm://')) return;
    if (!/^https?:/i.test(item.url)) return;
    if (!isCapturable(item.finalUrl || item.url)) return;
    if (!(await probe(false))) return;
    let cookies = '';
    try {
      if (chrome.cookies) {
        const jar = await chrome.cookies.getAll({ url: item.finalUrl || item.url });
        cookies = jar.map(c => c.name + '=' + c.value).join('; ');
      }
    } catch (e) { /* optional */ }
    /* Hand off first.  Never cancel the browser download unless the desktop
       bridge accepted it; this prevents lost downloads on token mismatch. */
    const handedOff = await bridgeAdd(item.finalUrl || item.url, {
      referrer: item.referrer || '',
      name: item.filename ? item.filename.split(/[\\/]/).pop() : '',
      cookie: cookies,
      size: item.totalBytes > 0 ? item.totalBytes : 0
    });
    if (!handedOff) return;
    chrome.downloads.cancel(item.id, () => {
      if (chrome.runtime.lastError) return;
      chrome.downloads.erase({ id: item.id }, () => void chrome.runtime.lastError);
    });
  });
}

/* ------------------------------------------------------------------ */
/* media sniffing (webRequest candidates per tab)                      */
/* ------------------------------------------------------------------ */
const tabMedia = new Map();  /* tabId -> [{url,size,mime}] */

function rememberMedia(tabId, url, size, mime) {
  if (!tabId || tabId < 0) return;
  if (!/^https?:/i.test(url)) return;
  const isMedia = /\.(mp4|m4v|mkv|webm|mov|avi|flv|ts|m2ts|mp3|m4a|flac|aac|ogg|wav)(\?|$)/i.test(url)
    || /^video\//i.test(mime || '') || /^audio\//i.test(mime || '');
  if (!isMedia) return;
  if (!tabMedia.has(tabId)) tabMedia.set(tabId, []);
  const list = tabMedia.get(tabId);
  if (list.some(x => x.url === url)) return;
  list.unshift({ url, size: size || 0, mime: mime || '' });
  if (list.length > 8) list.pop();
  setBadge(String(list.length), '#6ee7ef');
}

if (chrome.webRequest && chrome.webRequest.onHeadersReceived) {
  const filter = { urls: ['<all_urls>'], types: ['media', 'object', 'xmlhttprequest'] };
  chrome.webRequest.onHeadersReceived.addListener((det) => {
    let mime = '', size = 0;
    for (const h of (det.responseHeaders || [])) {
      const n = h.name.toLowerCase();
      if (n === 'content-type') mime = h.value;
      if (n === 'content-length') size = parseInt(h.value, 10) || 0;
    }
    if (det.statusCode >= 200 && det.statusCode < 300) {
      rememberMedia(det.tabId, det.url, size, mime);
    }
  }, filter, ['responseHeaders']);
  chrome.tabs.onRemoved.addListener((tabId) => tabMedia.delete(tabId));
  chrome.tabs.onUpdated.addListener((tabId, info) => {
    if (info.status === 'loading' && info.url) { tabMedia.delete(tabId); setBadge(''); }
  });
}

/* ------------------------------------------------------------------ */
/* context menus                                                       */
/* ------------------------------------------------------------------ */
chrome.runtime.onInstalled.addListener(() => {
  if (chrome.contextMenus && chrome.contextMenus.removeAll) {
    chrome.contextMenus.removeAll(() => {
      chrome.contextMenus.create({ id: 'gs-dl-link', title: 'Download with GrabStorm', contexts: ['link'] });
      chrome.contextMenus.create({ id: 'gs-dl-media', title: 'Download media with GrabStorm', contexts: ['video', 'audio'] });
      chrome.contextMenus.create({ id: 'gs-sep', type: 'separator', contexts: ['video', 'audio', 'link'] });
      chrome.contextMenus.create({ id: 'gs-capture-on', title: 'Auto-capture: on', type: 'radio', checked: true, contexts: ['action'] });
      chrome.contextMenus.create({ id: 'gs-capture-off', title: 'Auto-capture: off', type: 'radio', checked: false, contexts: ['action'] });
    });
  }
  probe(true);
});

if (chrome.contextMenus && chrome.contextMenus.onClicked) {
  chrome.contextMenus.onClicked.addListener((info, tab) => {
    if (info.menuItemId === 'gs-capture-on' || info.menuItemId === 'gs-capture-off') {
      const on = info.menuItemId === 'gs-capture-on';
      chrome.storage.sync.set({ autoCapture: on });
      return;
    }
    let url = info.linkUrl || info.srcUrl || info.pageUrl;
    /* media context click: prefer a sniffed candidate from this tab */
    if (info.menuItemId === 'gs-dl-media' && tab && tabMedia.get(tab.id)) {
      const list = tabMedia.get(tab.id);
      if (list.length && (!info.srcUrl || list[0].url !== info.srcUrl)) {
        url = list[0].url;
      }
    }
    if (!url) return;
    capture(url, { referrer: info.pageUrl || (tab && tab.url) || '' }, tab);
  });
}

/* ------------------------------------------------------------------ */
/* content-script messaging                                            */
/* ------------------------------------------------------------------ */
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (!msg) return false;
  if (msg.type === 'gs-launch' && msg.url) {
    capture(msg.url, { referrer: msg.referrer || '', size: msg.size || 0 }, sender.tab);
    sendResponse({ ok: true });
    return false;
  }
  if (msg.type === 'gs-media-list') {
    const list = (sender.tab && tabMedia.get(sender.tab.id)) || [];
    sendResponse({ list, panel: SET.videoPanel });
    return false;
  }
  if (msg.type === 'gs-settings') {
    sendResponse({ panel: SET.videoPanel, auto: SET.autoCapture });
    return false;
  }
  return false;
});
