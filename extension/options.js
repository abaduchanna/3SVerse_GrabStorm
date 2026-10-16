/* GrabStorm Integration — options page */
const box = document.getElementById('auto');
chrome.storage.sync.get({ autoCapture: true }, (s) => { box.checked = !!s.autoCapture; });
box.addEventListener('change', () => {
  chrome.storage.sync.set({ autoCapture: box.checked });
});
