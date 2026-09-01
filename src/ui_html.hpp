// Self-contained web UI served by `sonycam --ui`. Embedded at compile time
// so the binary needs no external assets.
#pragma once

namespace sonycam {

inline const char kUiHtml[] = R"SONYCAM_HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>sonycam</title>
<style>
  :root {
    --bg: #0b0e14; --panel: #131824; --border: #222b3d;
    --text: #e6e9f0; --muted: #8b93a7; --accent: #ff7a18; --blue: #4da3ff;
    --err: #ff5566; --ok: #3ddc84;
    --mono: "SFMono-Regular", Menlo, Consolas, monospace;
  }
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    background: var(--bg); color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    max-width: 720px; margin: 0 auto; padding: 32px 20px;
  }
  header { display: flex; align-items: baseline; gap: 14px; margin-bottom: 8px; }
  h1 { font-size: 1.5rem; }
  h1 span { color: var(--accent); }
  #status { color: var(--muted); font-size: .9rem; }
  #status .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%;
                 background: var(--err); margin-right: 6px; }
  #status.connected .dot { background: var(--ok); }
  #hint { color: var(--muted); font-size: .85rem; margin-bottom: 24px; }
  .prop {
    display: flex; align-items: center; justify-content: space-between; gap: 16px;
    background: var(--panel); border: 1px solid var(--border); border-radius: 10px;
    padding: 12px 16px; margin-bottom: 10px;
  }
  .prop label { font-family: var(--mono); font-size: .95rem; }
  .prop .ro { color: var(--muted); font-family: var(--mono); font-size: .95rem; }
  .prop .ro::after { content: "  (read-only)"; font-size: .75rem; }
  select, input[type=text] {
    background: #0e1220; color: var(--text); border: 1px solid var(--border);
    border-radius: 8px; padding: 8px 12px; font-family: var(--mono); font-size: .95rem;
    min-width: 180px;
  }
  select:focus, input:focus { outline: none; border-color: var(--blue); }
  .prop.saving select, .prop.saving input { border-color: var(--accent); }
  #toast {
    position: fixed; bottom: 24px; left: 50%; transform: translateX(-50%);
    background: var(--err); color: #fff; padding: 10px 18px; border-radius: 8px;
    font-size: .9rem; opacity: 0; transition: opacity .3s; pointer-events: none;
    max-width: 90vw;
  }
  #toast.show { opacity: 1; }
</style>
</head>
<body>
<header>
  <h1><span>sony</span>cam</h1>
  <div id="status"><span class="dot"></span><span id="statusText">connecting…</span></div>
</header>
<p id="hint">Changes apply to the camera immediately.</p>
<div id="props"></div>
<div id="toast"></div>
<script>
"use strict";
const propsEl = document.getElementById("props");
const statusEl = document.getElementById("status");
const statusText = document.getElementById("statusText");
const toastEl = document.getElementById("toast");
let toastTimer = null;
let pending = 0;

function toast(msg) {
  toastEl.textContent = msg;
  toastEl.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toastEl.classList.remove("show"), 4000);
}

async function api(path, body) {
  const resp = await fetch(path, body === undefined ? {} : {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const j = await resp.json();
  if (!j.ok) throw new Error(j.error || "request failed");
  return j.result;
}

function rowId(name) { return "prop-" + name; }

function makeRow(p) {
  const row = document.createElement("div");
  row.className = "prop";
  row.id = rowId(p.name);
  const label = document.createElement("label");
  label.textContent = p.name;
  row.appendChild(label);

  if (!p.writable) {
    const v = document.createElement("span");
    v.className = "ro";
    v.dataset.role = "value";
    v.textContent = p.value;
    row.appendChild(v);
    return row;
  }

  let ctl;
  if (p.choices && p.choices.length) {
    ctl = document.createElement("select");
    for (const c of p.choices) {
      const o = document.createElement("option");
      o.value = c; o.textContent = c;
      ctl.appendChild(o);
    }
    ctl.value = p.value;
    ctl.addEventListener("change", () => setProp(p.name, ctl.value, row, ctl));
  } else {
    ctl = document.createElement("input");
    ctl.type = "text";
    ctl.value = p.value;
    ctl.addEventListener("change", () => setProp(p.name, ctl.value, row, ctl));
  }
  ctl.dataset.role = "value";
  ctl.dataset.prev = p.value;
  row.appendChild(ctl);
  return row;
}

async function setProp(name, value, row, ctl) {
  row.classList.add("saving");
  pending++;
  try {
    const r = await api("/api/set", { prop: name, value });
    ctl.dataset.prev = r.value;
    if (document.activeElement !== ctl) ctl.value = r.value;
  } catch (e) {
    toast(name + ": " + e.message);
    ctl.value = ctl.dataset.prev;
  } finally {
    pending--;
    row.classList.remove("saving");
  }
}

function render(props) {
  const seen = new Set();
  for (const p of props) {
    seen.add(rowId(p.name));
    let row = document.getElementById(rowId(p.name));
    const ctl = row && row.querySelector("[data-role=value]");
    const rebuilt = !row ||
      (p.writable !== (ctl && ctl.tagName !== "SPAN")) ||
      (ctl && ctl.tagName === "SELECT" &&
       JSON.stringify([...ctl.options].map(o => o.value)) !== JSON.stringify(p.choices || []));
    if (rebuilt) {
      const fresh = makeRow(p);
      if (row) row.replaceWith(fresh); else propsEl.appendChild(fresh);
      continue;
    }
    if (document.activeElement === ctl || pending > 0) continue;
    if (ctl.tagName === "SPAN") ctl.textContent = p.value;
    else { ctl.value = p.value; ctl.dataset.prev = p.value; }
  }
  for (const row of [...propsEl.children])
    if (!seen.has(row.id)) row.remove();
}

async function refresh() {
  try {
    const [status, props] = await Promise.all([api("/api/status"), api("/api/props")]);
    statusEl.classList.toggle("connected", !!status.connected);
    statusText.textContent = status.connected
      ? (status.model || "camera") + " · " + (status.transport || "")
      : "disconnected";
    render(props);
  } catch (e) {
    statusEl.classList.remove("connected");
    statusText.textContent = "daemon unreachable";
  }
}

refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)SONYCAM_HTML";

}  // namespace sonycam
