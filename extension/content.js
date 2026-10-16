/* GrabStorm Integration — content script
 * Auto-captures clicks on direct file links (configurable) and hands them to
 * the GrabStorm desktop app via the grabstorm:// protocol.
 */
(function () {
  'use strict';

  const FILE_RE = /\.(zip|rar|7z|exe|msi|msix|iso|img|apk|mp4|mkv|avi|mov|mp3|flac|wav|ogg|pdf|epub|docx?|xlsx?|pptx?|dmg|pkg|deb|rpm|tar|gz|tgz|bz2|xz)([?#]|$)/i;

  let autoCapture = true;
  if (typeof chrome !== 'undefined' && chrome.storage && chrome.storage.sync) {
    chrome.storage.sync.get({ autoCapture: true }, (s) => { autoCapture = !!s.autoCapture; });
  }

  document.addEventListener('click', (e) => {
    if (!autoCapture) return;
    if (e.defaultPrevented || e.button !== 0 || e.ctrlKey || e.metaKey || e.shiftKey || e.altKey) return;
    const a = e.target && e.target.closest ? e.target.closest('a[href]') : null;
    if (!a) return;
    const href = a.href;
    if (!href || !/^https?:/i.test(href)) return;
    let path = href;
    try { path = new URL(href).pathname; } catch (err) { /* keep href */ }
    if (!FILE_RE.test(path)) return;
    e.preventDefault();
    e.stopPropagation();
    location.href = 'grabstorm://add?u=' + encodeURIComponent(href) +
                    '&r=' + encodeURIComponent(location.href);
  }, true);

  if (typeof chrome !== 'undefined' && chrome.runtime && chrome.runtime.onMessage) {
    chrome.runtime.onMessage.addListener((msg) => {
      if (msg && msg.type === 'gs-proto' && msg.url) {
        location.href = msg.url;
      }
    });
  }
})();
