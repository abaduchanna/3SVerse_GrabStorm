/* GrabStorm Integration — content script
 * 1. Click capture: direct file links go to GrabStorm (silent bridge first,
 *    grabstorm:// fallback) instead of the browser's own downloader.
 * 2. Video panel: a small GrabStorm pill appears next to <video> players;
 *    clicking it captures the current media (background merges sniffed
 *    candidates from webRequest for the best match).
 */
(function () {
  'use strict';

  const FILE_RE = /\.(zip|rar|7z|exe|msi|msix|iso|img|apk|mp4|mkv|avi|mov|mp3|flac|wav|ogg|pdf|epub|docx?|xlsx?|pptx?|dmg|pkg|deb|rpm|tar|gz|tgz|bz2|xz)([?#]|$)/i;
  const MEDIA_RE = /\.(mp4|m4v|mkv|webm|mov|avi|flv|ts|m2ts|mp3|m4a|flac|aac|ogg|wav)([?#]|$)/i;

  let autoCapture = true;
  let videoPanel = true;
  let protoPending = false;

  function settings() {
    if (typeof chrome !== 'undefined' && chrome.storage && chrome.storage.sync) {
      chrome.storage.sync.get({ autoCapture: true, videoPanel: true }, (s) => {
        autoCapture = !!s.autoCapture;
        videoPanel = !!s.videoPanel;
        if (videoPanel) setTimeout(scanVideos, 400);
      });
      chrome.storage.onChanged.addListener((ch, area) => {
        if (area !== 'sync') return;
        if (ch.autoCapture) autoCapture = !!ch.autoCapture.newValue;
        if (ch.videoPanel) { videoPanel = !!ch.videoPanel.newValue; if (videoPanel) scanVideos(); else removeAllPills(); }
      });
    }
  }
  settings();

  /* ------------------------------------------------------------------ */
  /* helpers                                                             */
  /* ------------------------------------------------------------------ */
  function send(msg) {
    try {
      if (typeof chrome !== 'undefined' && chrome.runtime && chrome.runtime.sendMessage) {
        chrome.runtime.sendMessage(msg, () => void chrome.runtime.lastError);
      }
    } catch (e) { /* extension context gone */ }
  }

  /* ------------------------------------------------------------------ */
  /* 1. click capture                                                    */
  /* ------------------------------------------------------------------ */
  document.addEventListener('click', (e) => {
    if (!autoCapture) return;
    if (e.defaultPrevented || e.button !== 0 || e.ctrlKey || e.metaKey || e.shiftKey || e.altKey) return;
    const a = e.target && e.target.closest ? e.target.closest('a') : null;
    if (!a) return;
    const href = a.href;
    if (!href || !/^https?:/i.test(href)) return;
    let path = href;
    try { path = new URL(href).pathname; } catch (err) { /* keep href */ }
    if (!FILE_RE.test(path)) return;
    e.preventDefault();
    e.stopPropagation();
    send({ type: 'gs-launch', url: href, referrer: location.href });
  }, true);

  /* protocol fallback receiver (window hand-off from background) */
  if (typeof chrome !== 'undefined' && chrome.runtime && chrome.runtime.onMessage) {
    chrome.runtime.onMessage.addListener((msg) => {
      if (msg && msg.type === 'gs-proto' && msg.url && !protoPending) {
        protoPending = true;
        location.href = msg.url;
      }
    });
  }

  /* ------------------------------------------------------------------ */
  /* 2. video panel                                                      */
  /* ------------------------------------------------------------------ */
  const pills = new Set();
  let scanning = false;

  function bestMediaUrl(vid) {
    if (vid.currentSrc) return vid.currentSrc;
    if (vid.src) return vid.src;
    const s = vid.querySelector('source[src]');
    if (s && s.src) return s.src;
    return '';
  }

  function makePill(vid) {
    const pill = document.createElement('div');
    pill.textContent = 'GrabStorm';
    pill.setAttribute('data-gs-pill', '1');
    pill.style.cssText =
      'position:fixed;z-index:2147483646;display:inline-flex;align-items:center;gap:6px;' +
      'margin:6px;padding:5px 12px;border-radius:999px;cursor:pointer;' +
      'background:linear-gradient(90deg,#6ee7ef,#e44bd7);color:#07060b;' +
      'font:600 11px/1.4 -apple-system,Segoe UI,Roboto,Arial,sans-serif;' +
      'letter-spacing:.06em;border:1px solid rgba(255,255,255,.25);box-shadow:0 4px 14px rgba(0,0,0,.35);';
    pill.addEventListener('mouseenter', () => { pill.style.filter = 'brightness(1.08)'; });
    pill.addEventListener('mouseleave', () => { pill.style.filter = ''; });
    pill.addEventListener('click', (e) => {
      e.preventDefault();
      e.stopPropagation();
      const url = bestMediaUrl(vid);
      chrome.runtime.sendMessage({ type: 'gs-media-list' }, (rep) => {
        void chrome.runtime.lastError;
        const list = (rep && rep.list) || [];
        const cand = (url && MEDIA_RE.test(url)) ? url : (list[0] && list[0].url) || url;
        if (cand) send({ type: 'gs-launch', url: cand, referrer: location.href, size: (list[0] && list[0].size) || 0 });
        pill.textContent = cand ? 'Sent to GrabStorm' : 'No media found';
        setTimeout(() => { pill.textContent = 'GrabStorm'; }, 1600);
      });
    }, true);
    document.documentElement.appendChild(pill);
    positionPill(pill, vid);
    pills.add(pill);
    return pill;
  }

  function positionPill(pill, vid) {
    try {
      const r = vid.getBoundingClientRect();
      if (r.width < 60 || r.height < 40) { pill.style.display = 'none'; return; }
      pill.style.display = 'inline-flex';
      const x = Math.min(Math.max(r.right - 130, 8), window.innerWidth - 140);
      const y = Math.min(Math.max(r.bottom - 34, 8), window.innerHeight - 40);
      pill.style.left = x + 'px';
      pill.style.top = y + 'px';
    } catch (err) { /* detached */ }
  }

  function removeAllPills() {
    for (const p of pills) { try { p.remove(); } catch (e) {} }
    pills.clear();
  }

  function scanVideos() {
    if (scanning) return;
    scanning = true;
    try {
      if (!videoPanel) { removeAllPills(); return; }
      const vids = document.querySelectorAll('video, audio');
      /* drop pills whose player disappeared */
      for (const p of pills) {
        if (!document.contains(p._vid || null)) { p.remove(); pills.delete(p); }
      }
      vids.forEach((vid) => {
        const url = bestMediaUrl(vid);
        if (!url) return;
        let pill = null;
        for (const p of pills) if (p._vid === vid) { pill = p; break; }
        if (!pill) pill = makePill(vid);
        pill._vid = vid;
        positionPill(pill, vid);
      });
    } finally {
      scanning = false;
    }
  }

  window.addEventListener('scroll', () => { for (const p of pills) if (p._vid) positionPill(p, p._vid); }, { passive: true });
  window.addEventListener('resize', () => { for (const p of pills) if (p._vid) positionPill(p, p._vid); });
  window.addEventListener('DOMContentLoaded', () => setTimeout(scanVideos, 600));
  setTimeout(scanVideos, 1200);
  setInterval(scanVideos, 2500);
})();
