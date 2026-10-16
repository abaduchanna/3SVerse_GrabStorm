/* TurboGrab Integration — popup */
const $ = (id) => document.getElementById(id);

function setDot(state, text, sub) {
  const dot = $("dot");
  dot.className = "dot " + state;
  $("status").textContent = text;
  $("statusSub").textContent = sub || "";
}

async function refresh() {
  setDot("", "Checking…", "");
  const r = await chrome.runtime.sendMessage({ type: "ping" });
  if (r && r.ok) {
    setDot(r.auth ? "on" : "off",
           r.auth ? "Connected" : "Connected — token needed",
           r.version ? ("TurboGrab v" + r.version + (r.auth ? "" : " — paste token in Settings")) : "");
  } else {
    setDot("off", "TurboGrab not running", "Start the app, then retry.");
  }
  const c = await chrome.storage.local.get({ takeover: true });
  $("takeover").checked = !!c.takeover;
}

$("takeover").addEventListener("change", async () => {
  await chrome.storage.local.set({ takeover: $("takeover").checked });
});

$("openOpts").addEventListener("click", () => chrome.runtime.openOptionsPage());
$("openApp").addEventListener("click", () => window.close());

document.addEventListener("DOMContentLoaded", refresh);
