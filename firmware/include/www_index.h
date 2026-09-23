#pragma once
// Auto-embedded file explorer UI
static const char WWW_INDEX_HTML[] = R"INDEXHTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>RNDISStorage</title>
<style>
:root{
  --bg:#0e1116;--surface:#171d27;--elev:#222b38;--border:#2c3645;
  --text:#e8eef5;--muted:#8b97a8;--dim:#667384;--accent:#3aa89a;
  --accent-dim:#2a7d73;--danger:#d97868;--ok:#6fbf8a;--warn:#d4a84b;
  --select:#2a5f58;--tree:#121820;
}
*{box-sizing:border-box}
body{margin:0;font-family:"Segoe UI",system-ui,sans-serif;background:var(--bg);color:var(--text);min-height:100vh}
header{display:flex;align-items:center;justify-content:space-between;padding:14px 20px;background:#141a22;border-bottom:1px solid var(--border)}
.brand{font-size:1.25rem;font-weight:700;letter-spacing:.02em}
.tag{color:var(--muted);font-size:.8rem;margin-top:2px}
.pill{background:var(--elev);color:var(--muted);padding:6px 12px;border-radius:4px;font-size:.8rem;font-weight:600}
.strip{display:flex;gap:8px;align-items:center;flex-wrap:wrap;padding:12px 16px;background:var(--surface);border-bottom:1px solid var(--border)}
.path{flex:1;min-width:180px;background:#1a222e;border:1px solid var(--border);color:var(--text);padding:8px 10px;border-radius:4px;font-family:Consolas,monospace;font-size:.9rem}
button,.btn{background:#243041;color:var(--text);border:0;padding:8px 12px;border-radius:4px;cursor:pointer;font-size:.85rem}
button.primary{background:var(--accent);color:#0b1210;font-weight:600}
button:hover{filter:brightness(1.08)}
button:disabled{opacity:.45;cursor:not-allowed}
main{padding:12px 16px 24px}
.panel{background:var(--surface);border:1px solid var(--border);border-radius:6px;overflow:hidden}
.toolbar{display:flex;gap:6px;flex-wrap:wrap;padding:10px;background:#1a222e;border-bottom:1px solid var(--border)}
table{width:100%;border-collapse:collapse;font-size:.9rem}
th{text-align:left;padding:10px 12px;background:#1a222e;color:#9aa8ba;font-weight:600;border-bottom:1px solid var(--border);user-select:none}
td{padding:9px 12px;border-bottom:1px solid #243041;cursor:default}
tr:hover td{background:#1c2430}
tr.selected td{background:var(--select)}
.name{font-family:Consolas,"Cascadia Mono",monospace}
.icon{width:1.4em;display:inline-block;color:var(--accent)}
.muted{color:var(--muted)}
.footer{padding:10px 16px;color:var(--muted);font-size:.8rem;display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap}
#log{margin:12px 16px;background:#0c1016;border:1px solid var(--border);border-radius:6px;padding:10px;height:110px;overflow:auto;font-family:Consolas,monospace;font-size:.78rem;color:#a8b4c4}
.drop{outline:2px dashed var(--accent);outline-offset:-6px}
#toast{position:fixed;bottom:20px;right:20px;background:var(--elev);border:1px solid var(--border);padding:10px 14px;border-radius:6px;display:none;z-index:9}
input[type=file]{display:none}
</style>
</head>
<body>
<header>
  <div>
    <div class="brand">RNDISStorage</div>
    <div class="tag">WebDAV file explorer · lab PoC · no USB Mass Storage</div>
  </div>
  <div class="pill" id="connPill">Connecting…</div>
</header>
<div class="strip">
  <button type="button" id="btnUp" title="Parent">↑ Up</button>
  <button type="button" id="btnRefresh">Refresh</button>
  <input class="path" id="pathInput" value="/" spellcheck="false"/>
  <button type="button" class="primary" id="btnGo">Go</button>
  <span class="muted" id="storageLabel">Storage —</span>
</div>
<main>
  <div class="panel" id="dropZone">
    <div class="toolbar">
      <button type="button" id="btnMkdir">New folder</button>
      <button type="button" id="btnTouch">New file</button>
      <button type="button" id="btnUpload">Upload</button>
      <button type="button" id="btnDownload" disabled>Download</button>
      <button type="button" id="btnRename" disabled>Rename</button>
      <button type="button" id="btnDelete" disabled>Delete</button>
      <input type="file" id="fileInput" multiple/>
    </div>
    <table>
      <thead><tr><th style="width:44%">Name</th><th style="width:12%">Type</th><th style="width:16%">Size</th><th>Modified</th></tr></thead>
      <tbody id="rows"></tbody>
    </table>
  </div>
</main>
<div class="footer">
  <span id="status">Ready</span>
  <span id="davHint">WebDAV: <code id="davUrl"></code></span>
</div>
<div id="log"></div>
<div id="toast"></div>
<script>
const $ = (id) => document.getElementById(id);
let cwd = "/";
let selected = null;
let entries = [];

function log(msg) {
  const el = $("log");
  const t = new Date().toLocaleTimeString();
  el.textContent += `[${t}] ${msg}\n`;
  el.scrollTop = el.scrollHeight;
}
function toast(msg) {
  const el = $("toast");
  el.textContent = msg;
  el.style.display = "block";
  setTimeout(() => el.style.display = "none", 2200);
}
function fmtSize(n) {
  if (n == null) return "—";
  n = Number(n);
  if (n < 1024) return n + " B";
  if (n < 1048576) return (n/1024).toFixed(1) + " KB";
  return (n/1048576).toFixed(2) + " MB";
}
function joinPath(base, name) {
  if (!base || base === "/") return "/" + name.replace(/^\/+/, "");
  return base.replace(/\/+$/, "") + "/" + name.replace(/^\/+/, "");
}
function parentPath(p) {
  if (!p || p === "/") return "/";
  const parts = p.replace(/\/+$/, "").split("/");
  parts.pop();
  return parts.join("/") || "/";
}
async function api(path, opts = {}) {
  const res = await fetch(path, opts);
  const ct = res.headers.get("content-type") || "";
  let body = null;
  if (ct.includes("application/json")) body = await res.json();
  else body = await res.text();
  if (!res.ok) {
    const msg = (body && body.message) || (body && body.error) || res.statusText;
    throw new Error(msg || ("HTTP " + res.status));
  }
  return body;
}
function setSelection(name) {
  selected = name;
  document.querySelectorAll("#rows tr").forEach(tr => {
    tr.classList.toggle("selected", tr.dataset.name === name);
  });
  const has = !!name;
  $("btnDownload").disabled = !has || (entries.find(e => e.name === name) || {}).is_dir;
  $("btnRename").disabled = !has;
  $("btnDelete").disabled = !has;
}
function render() {
  $("pathInput").value = cwd;
  const tb = $("rows");
  tb.innerHTML = "";
  if (cwd !== "/") {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td class="name"><span class="icon">⬆</span> ..</td><td class="muted">dir</td><td></td><td></td>`;
    tr.onclick = () => { cwd = parentPath(cwd); refresh(); };
    tb.appendChild(tr);
  }
  for (const e of entries) {
    const tr = document.createElement("tr");
    tr.dataset.name = e.name;
    const icon = e.is_dir ? "📁" : "📄";
    tr.innerHTML = `<td class="name"><span class="icon">${icon}</span> ${escapeHtml(e.name)}</td>
      <td class="muted">${e.is_dir ? "dir" : "file"}</td>
      <td>${e.is_dir ? "—" : fmtSize(e.size)}</td>
      <td class="muted">${e.mtime || "—"}</td>`;
    tr.onclick = () => setSelection(e.name);
    tr.ondblclick = () => {
      if (e.is_dir) { cwd = joinPath(cwd, e.name); refresh(); }
      else downloadOne(e.name);
    };
    tb.appendChild(tr);
  }
  setSelection(null);
}
function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}
async function refreshInfo() {
  try {
    const info = await api("/api/info");
    $("connPill").textContent = "Online · " + (info.transport || "network");
    $("connPill").style.color = "var(--ok)";
    const st = info.storage || {};
    if (st.mounted === false) {
      $("storageLabel").textContent = "SD not mounted";
      $("connPill").textContent = "Online · no SD";
      $("connPill").style.color = "var(--warn)";
    } else {
      $("storageLabel").textContent =
        `SD ${fmtSize(st.used_bytes)} / ${fmtSize(st.total_bytes)} · free ${fmtSize(st.free_bytes)}`;
    }
    $("davUrl").textContent = location.origin + "/dav/";
    return info;
  } catch (err) {
    $("connPill").textContent = "Offline";
    $("connPill").style.color = "var(--danger)";
    throw err;
  }
}
async function refresh() {
  $("status").textContent = "Listing…";
  try {
    await refreshInfo();
  } catch (err) {
    $("status").textContent = "API unreachable: " + (err.message || err);
    log("info error: " + err.message);
    return;
  }
  try {
    const data = await api("/api/list?path=" + encodeURIComponent(cwd));
    entries = (data.entries || []).sort((a,b) => {
      if (a.is_dir !== b.is_dir) return a.is_dir ? -1 : 1;
      return a.name.localeCompare(b.name);
    });
    render();
    $("status").textContent = entries.length + " item(s)";
  } catch (err) {
    entries = [];
    render();
    $("status").textContent = String(err.message || err);
    log("list error: " + err.message);
  }
}
async function downloadOne(name) {
  const remote = joinPath(cwd, name);
  $("status").textContent = "Downloading " + name;
  try {
    const res = await fetch("/api/download?path=" + encodeURIComponent(remote));
    if (!res.ok) throw new Error("download failed");
    const blob = await res.blob();
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = name;
    a.click();
    URL.revokeObjectURL(a.href);
    log("downloaded " + remote);
    $("status").textContent = "Downloaded " + name;
  } catch (err) {
    toast(err.message); log(err.message);
  }
}
$("btnUp").onclick = () => { cwd = parentPath(cwd); refresh(); };
$("btnRefresh").onclick = () => refresh();
$("btnGo").onclick = () => { cwd = $("pathInput").value || "/"; refresh(); };
$("pathInput").onkeydown = (e) => { if (e.key === "Enter") $("btnGo").click(); };
$("btnMkdir").onclick = async () => {
  const name = prompt("Folder name");
  if (!name) return;
  try {
    await api("/api/mkdir", { method:"POST", headers:{"Content-Type":"application/json"},
      body: JSON.stringify({ path: joinPath(cwd, name) }) });
    log("mkdir " + name); refresh();
  } catch (err) { toast(err.message); }
};
$("btnTouch").onclick = async () => {
  const name = prompt("File name");
  if (!name) return;
  try {
    await api("/api/touch", { method:"POST", headers:{"Content-Type":"application/json"},
      body: JSON.stringify({ path: joinPath(cwd, name) }) });
    log("touch " + name); refresh();
  } catch (err) { toast(err.message); }
};
$("btnUpload").onclick = () => $("fileInput").click();
$("fileInput").onchange = async () => {
  for (const f of $("fileInput").files) {
    await uploadFile(f, joinPath(cwd, f.name));
  }
  $("fileInput").value = "";
  refresh();
};
async function uploadFile(file, remote) {
  $("status").textContent = "Uploading " + file.name;
  try {
    const res = await fetch("/api/upload?path=" + encodeURIComponent(remote), {
      method: "PUT",
      headers: { "Content-Type": "application/octet-stream" },
      body: file
    });
    if (!res.ok) {
      let msg = "upload failed";
      try { const j = await res.json(); msg = j.message || msg; } catch (_) {}
      throw new Error(msg);
    }
    log("uploaded " + remote + " (" + file.size + " B)");
    toast("Uploaded " + file.name);
  } catch (err) { toast(err.message); log(err.message); }
}
$("btnDownload").onclick = () => { if (selected) downloadOne(selected); };
$("btnRename").onclick = async () => {
  if (!selected) return;
  const neu = prompt("New name", selected);
  if (!neu || neu === selected) return;
  try {
    await api("/api/rename", { method:"POST", headers:{"Content-Type":"application/json"},
      body: JSON.stringify({ path: joinPath(cwd, selected), new_path: joinPath(cwd, neu) }) });
    log("rename " + selected + " -> " + neu); refresh();
  } catch (err) { toast(err.message); }
};
$("btnDelete").onclick = async () => {
  if (!selected) return;
  if (!confirm("Delete " + selected + "?")) return;
  try {
    await api("/api/delete", { method:"POST", headers:{"Content-Type":"application/json"},
      body: JSON.stringify({ path: joinPath(cwd, selected) }) });
    log("delete " + selected); refresh();
  } catch (err) { toast(err.message); }
};
const dz = $("dropZone");
dz.addEventListener("dragover", (e) => { e.preventDefault(); dz.classList.add("drop"); });
dz.addEventListener("dragleave", () => dz.classList.remove("drop"));
dz.addEventListener("drop", async (e) => {
  e.preventDefault(); dz.classList.remove("drop");
  for (const f of e.dataTransfer.files) {
    await uploadFile(f, joinPath(cwd, f.name));
  }
  refresh();
});
refresh();
</script>
</body>
</html>

)INDEXHTML";
