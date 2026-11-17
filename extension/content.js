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

  const DL_SVG =
    '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" ' +
    'style="flex:none"><path d="M12 3v12m0 0l-5-5m5 5l5-5M4 19h16" ' +
    'stroke="#07060b" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/></svg>';

  /* IDM-style: a small round badge sits top-right of the video, invisible
   * until the video (or the badge itself) is hovered, then it expands
   * into the GrabStorm label. */
  function makePill(vid) {
    const pill = document.createElement('div');
    pill.setAttribute('data-gs-pill', '1');
    pill.innerHTML = DL_SVG + '<span data-gs-label></span>';
    pill.style.cssText =
      'position:fixed;z-index:2147483646;display:inline-flex;align-items:center;gap:6px;' +
      'padding:7px;border-radius:999px;cursor:pointer;opacity:0;pointer-events:none;' +
      'transition:opacity .15s ease,padding .15s ease;' +
      'background:linear-gradient(90deg,#6ee7ef,#e44bd7);color:#07060b;' +
      'font:600 11px/1.4 -apple-system,Segoe UI,Roboto,Arial,sans-serif;' +
      'letter-spacing:.06em;border:1px solid rgba(255,255,255,.25);box-shadow:0 4px 14px rgba(0,0,0,.35);';
    const label = pill.querySelector('[data-gs-label]');
    label.style.cssText = 'max-width:0;overflow:hidden;white-space:nowrap;transition:max-width .15s ease;';
    label.textContent = 'GrabStorm';

    const show = () => {
      pill.style.opacity = '1';
      pill.style.pointerEvents = 'auto';
      pill.style.padding = '7px 12px 7px 9px';
      label.style.maxWidth = '120px';
    };
    const hide = () => {
      if (pill._hovered) return;
      pill.style.opacity = '0';
      pill.style.pointerEvents = 'none';
      pill.style.padding = '7px';
      label.style.maxWidth = '0';
    };
    pill._hovered = false;
    pill.addEventListener('mouseenter', () => { pill._hovered = true; pill.style.filter = 'brightness(1.08)'; show(); });
    pill.addEventListener('mouseleave', () => { pill._hovered = false; pill.style.filter = ''; hide(); });
    pill._show = show;
    pill._hide = hide;

    pill.addEventListener('click', (e) => {
      e.preventDefault();
      e.stopPropagation();
      const url = bestMediaUrl(vid);
      chrome.runtime.sendMessage({ type: 'gs-media-list' }, (rep) => {
        void chrome.runtime.lastError;
        const list = (rep && rep.list) || [];
        const cand = (url && MEDIA_RE.test(url)) ? url : (list[0] && list[0].url) || url;
        if (cand) send({ type: 'gs-launch', url: cand, referrer: location.href, size: (list[0] && list[0].size) || 0 });
        label.textContent = cand ? 'Sent to GrabStorm' : 'No media found';
        setTimeout(() => { label.textContent = 'GrabStorm'; }, 1600);
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
      if (r.width < 60 || r.height < 40) { pill._hidden = true; pill.style.display = 'none'; return; }
      pill._hidden = false;
      pill.style.display = 'inline-flex';
      /* IDM-style placement: top-right corner of the video. */
      const x = Math.min(Math.max(r.right - 44, 8), window.innerWidth - 44);
      const y = Math.min(Math.max(r.top + 8, 8), window.innerHeight - 40);
      pill.style.left = x + 'px';
      pill.style.top = y + 'px';
    } catch (err) { /* detached */ }
  }

  /* reveal the badge only while the cursor is over the video (or the
   * badge itself, handled by the pill's own mouseenter/mouseleave) */
  function wireHover(vid, pill) {
    if (vid._gsHoverWired) return;
    vid._gsHoverWired = true;
    vid.addEventListener('mouseenter', () => { if (!pill._hidden) pill._show(); }, true);
    vid.addEventListener('mouseleave', () => { pill._hide(); }, true);
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
        wireHover(vid, pill);
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
