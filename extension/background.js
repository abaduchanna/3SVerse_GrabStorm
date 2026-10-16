/* GrabStorm Integration — background service worker (MV3) / background script (FF MV2)
 * Routes file links to the GrabStorm desktop app through the grabstorm:// protocol.
 */
const DEF = { autoCapture: true };

function protoUrl(u, ref) {
  return 'grabstorm://add?u=' + encodeURIComponent(u) +
         '&r=' + encodeURIComponent(ref || '');
}

/* Try an in-page protocol hand-off first (no extra tab flash); fall back to a
 * new tab that triggers the external-protocol dialog. */
async function launch(u, ref, tab) {
  const url = protoUrl(u, ref);
  try {
    if (tab && tab.id != null) {
      await chrome.tabs.sendMessage(tab.id, { type: 'gs-proto', url: url });
      return;
    }
  } catch (e) { /* no receiver in that tab */ }
  chrome.tabs.create({ url: url });
}

chrome.runtime.onInstalled.addListener(() => {
  if (chrome.contextMenus && chrome.contextMenus.removeAll) {
    chrome.contextMenus.removeAll(() => {
      chrome.contextMenus.create({
        id: 'gs-dl-link',
        title: 'Download with GrabStorm',
        contexts: ['link']
      });
      chrome.contextMenus.create({
        id: 'gs-dl-media',
        title: 'Download media with GrabStorm',
        contexts: ['video', 'audio']
      });
    });
  }
});

if (chrome.contextMenus && chrome.contextMenus.onClicked) {
  chrome.contextMenus.onClicked.addListener((info, tab) => {
    const u = info.linkUrl || info.srcUrl || info.pageUrl;
    if (!u) return;
    launch(u, info.pageUrl || (tab && tab.url) || '', tab);
  });
}

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (msg && msg.type === 'gs-launch' && msg.url) {
    launch(msg.url, msg.ref || '', sender.tab);
    sendResponse({ ok: true });
  }
  return false;
});

chrome.storage.onChanged.addListener((changes, area) => {
  if (area === 'sync' && changes.autoCapture) DEF.autoCapture = changes.autoCapture.newValue;
});
