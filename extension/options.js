/* TurboGrab Integration — options page */
const $ = (id) => document.getElementById(id);

const FIELDS = ["serverUrl", "token", "takeover", "takeoverAll", "exts", "autoStart", "notifyFail"];

function load() {
  chrome.storage.local.get(DEFAULTS_C).then((c) => {
    $("serverUrl").value = c.serverUrl;
    $("token").value = c.token;
    $("takeover").checked = !!c.takeover;
    $("takeoverAll").checked = !!c.takeoverAll;
    $("exts").value = c.exts;
    $("autoStart").checked = !!c.autoStart;
    $("notifyFail").checked = !!c.notifyFail;
  });
}

const DEFAULTS_C = {
  serverUrl: "http://127.0.0.1:8777",
  token: "",
  takeover: true,
  takeoverAll: false,
  autoStart: true,
  notifyFail: true,
  exts: "zip,rar,7z,tar,gz,bz2,xz,iso,dmg,exe,msi,apk,deb,rpm,appimage,pdf,doc,docx,xls,xlsx,ppt,pptx,csv,epub,mp4,mkv,avi,mov,wmv,flv,webm,m4v,ts,mp3,wav,flac,ogg,m4a,opus,aac,mid"
};

function save() {
  const o = {
    serverUrl: $("serverUrl").value.trim() || "http://127.0.0.1:8777",
    token: $("token").value.trim(),
    takeover: $("takeover").checked,
    takeoverAll: $("takeoverAll").checked,
    exts: $("exts").value.trim(),
    autoStart: $("autoStart").checked,
    notifyFail: $("notifyFail").checked
  };
  chrome.storage.local.set(o, () => {
    const out = $("testOut");
    out.className = "ok";
    out.textContent = "Saved.";
    setTimeout(() => { out.textContent = ""; out.className = ""; }, 2500);
  });
}

async function test() {
  const out = $("testOut");
  out.className = "";
  out.textContent = "Connecting…";
  await chrome.storage.local.set({ serverUrl: $("serverUrl").value.trim() || "http://127.0.0.1:8777",
                                   token: $("token").value.trim() });
  const r = await chrome.runtime.sendMessage({ type: "ping" });
  if (r && r.ok) {
    out.className = "ok";
    out.textContent = "Connected to TurboGrab" + (r.version ? " v" + r.version : "") +
                      (r.auth ? " — token OK." : " — but the token is missing/wrong. Copy it from the app settings.");
  } else {
    out.className = "bad";
    out.textContent = "Cannot reach TurboGrab. Is the app running and is browser integration enabled?";
  }
}

document.addEventListener("DOMContentLoaded", () => {
  load();
  $("save").addEventListener("click", save);
  $("test").addEventListener("click", test);
});
