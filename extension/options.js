/* GrabStorm Integration — options page */
const el = (id) => document.getElementById(id);
const box = el('auto'), panel = el('panel'), all = el('all');
const port = el('port'), token = el('token'), status = el('status');

const DEF = { autoCapture: true, videoPanel: true, captureAll: false, port: 14599, token: '' };

chrome.storage.sync.get(DEF, (s) => {
  box.checked = !!s.autoCapture;
  panel.checked = !!s.videoPanel;
  all.checked = !!s.captureAll;
  port.value = s.port || 14599;
  token.value = s.token || '';
});

el('save').addEventListener('click', () => {
  const p = parseInt(port.value, 10);
  chrome.storage.sync.set({
    autoCapture: box.checked,
    videoPanel: panel.checked,
    captureAll: all.checked,
    port: (p >= 1024 && p <= 65535) ? p : 14599,
    token: token.value.trim()
  }, () => {
    status.textContent = 'Saved.';
    status.className = 'ok';
    setTimeout(() => { status.textContent = ''; status.className = ''; }, 1800);
  });
});

el('test').addEventListener('click', async () => {
  const p = parseInt(port.value, 10) || 14599;
  status.textContent = 'Probing…';
  status.className = '';
  try {
    const r = await fetch(`http://127.0.0.1:${p}/status/204+grabstorm`,
      { cache: 'no-store', signal: AbortSignal.timeout(2500) });
    if (r.status === 204 || r.status === 200) {
      status.textContent = 'GrabStorm desktop app is reachable.';
      status.className = 'ok';
    } else {
      status.textContent = 'Unexpected response: HTTP ' + r.status;
      status.className = 'bad';
    }
  } catch (e) {
    status.textContent = 'Not reachable — is GrabStorm running?';
    status.className = 'bad';
  }
});
