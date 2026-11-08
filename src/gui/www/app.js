// softswitch web GUI: polls each switch's /api/state, streams /events, and draws the topology,
// a live packet trace and the switch tables. Read-only; no frameworks, no external requests.
"use strict";

// ---- which switches -------------------------------------------------------------------------
// ?sw=host:port,host:port overrides the default of ports 8081-8083 on the serving host.
const params = new URLSearchParams(location.search);
const host = location.hostname || "localhost";
const endpoints = (params.get("sw") || `${host}:8081,${host}:8082,${host}:8083`)
  .split(",").map((s) => s.trim()).filter(Boolean).map((hp) => `${location.protocol}//${hp}`);

const MAX_TRACE_ROWS = 400;
const state = new Map();     // endpoint -> latest /api/state
const previous = new Map();  // endpoint -> previous state (for rates)
const online = new Map();
const traces = [];
let paused = false;
let received = 0;

const $ = (id) => document.getElementById(id);
const el = (tag, attrs = {}, text) => {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) node.setAttribute(k, v);
  if (text !== undefined) node.textContent = text;
  return node;
};
const svgEl = (tag, attrs = {}, text) => {
  const node = document.createElementNS("http://www.w3.org/2000/svg", tag);
  for (const [k, v] of Object.entries(attrs)) node.setAttribute(k, v);
  if (text !== undefined) node.textContent = text;
  return node;
};

// ---- polling ----------------------------------------------------------------------------------
async function poll(endpoint) {
  try {
    const res = await fetch(`${endpoint}/api/state`, { cache: "no-store" });
    if (!res.ok) throw new Error(res.status);
    const s = await res.json();
    if (state.has(endpoint)) previous.set(endpoint, state.get(endpoint));
    state.set(endpoint, s);
    online.set(endpoint, true);
  } catch {
    online.set(endpoint, false);
  }
}

async function refresh() {
  await Promise.all(endpoints.map(poll));
  renderStatus();
  renderTopology();
  renderSwitchSelectors();
  renderTable();
}

// ---- live trace -------------------------------------------------------------------------------
function protoName(t) {
  if (t.ethertype === 0x0806) return "ARP";
  if (t.ethertype < 0x0600) return "STP";
  if (t.ethertype === 0x0800) {
    return { 1: "ICMP", 2: "IGMP", 6: "TCP", 17: "UDP" }[t.proto] || `IP/${t.proto}`;
  }
  return `0x${t.ethertype.toString(16)}`;
}

function traceText(t) {
  const who = t.sip ? `${t.sip} → ${t.dip}` : `${t.src} → ${t.dst}`;
  const vlan = t.out_vlan && t.out_vlan !== t.vlan ? `${t.vlan}→${t.out_vlan}` : `${t.vlan}`;
  const decision = t.decision === "drop" ? `drop (${t.reason})` : t.decision;
  return { who, vlan, decision, proto: protoName(t), out: t.out.join(",") || "-" };
}

function matches(t) {
  const sw = $("trace-switch").value;
  if (sw && t.sw !== sw) return false;
  const q = $("trace-filter").value.trim().toLowerCase();
  if (!q) return true;
  const x = traceText(t);
  const hay = `${t.sw} ${t.in} vlan ${x.vlan} ${x.who} ${t.src} ${t.dst} ${x.proto} ${x.decision} ${x.out}`.toLowerCase();
  return q.split(/\s+/).every((word) => hay.includes(word));
}

function addTraceRow(t, atTop = true) {
  const x = traceText(t);
  const tr = el("tr");
  const time = new Date().toLocaleTimeString([], { hour12: false });
  tr.append(el("td", { class: "mono" }, time), el("td", {}, t.sw), el("td", { class: "mono" }, t.in),
            el("td", {}, x.vlan), el("td", { class: "mono" }, x.who), el("td", {}, x.proto),
            el("td", { class: `decision ${t.decision}` }, x.decision), el("td", { class: "mono" }, x.out));
  const body = $("trace").tBodies[0];
  if (atTop) body.prepend(tr); else body.append(tr);
  while (body.rows.length > MAX_TRACE_ROWS) body.deleteRow(body.rows.length - 1);
}

function rerenderTrace() {
  const body = $("trace").tBodies[0];
  body.replaceChildren();
  for (let i = traces.length - 1; i >= 0 && body.rows.length < MAX_TRACE_ROWS; --i) {
    if (matches(traces[i])) addTraceRow(traces[i], false);
  }
}

function onTrace(t) {
  received += 1;
  traces.push(t);
  if (traces.length > 2000) traces.shift();
  flashLinks(t);
  if (!paused && matches(t)) addTraceRow(t);
  $("trace-stats").textContent = `${received} sampled packets received` + (paused ? " (paused)" : "");
}

function subscribe(endpoint) {
  const source = new EventSource(`${endpoint}/events`);
  source.addEventListener("trace", (e) => onTrace(JSON.parse(e.data)));
  source.onerror = () => online.set(endpoint, false);
}

// ---- topology -----------------------------------------------------------------------------
// Links come from port descriptions: "peer sw2 s2p2" (switch to switch) and "host h1".
const linkElements = new Map();  // "sw:port" -> SVG line

function buildModel() {
  const switches = [...state.values()];
  const names = switches.map((s) => s.name).sort();
  const links = [];
  const seen = new Set();
  const hosts = [];
  for (const s of switches) {
    for (const p of s.ports) {
      const peer = /^peer\s+(\S+)\s+(\S+)/.exec(p.description);
      const hostMatch = /^host\s+(\S+)/.exec(p.description);
      if (peer) {
        const key = [`${s.name}:${p.name}`, `${peer[1]}:${peer[2]}`].sort().join("|");
        if (!seen.has(key)) {
          seen.add(key);
          links.push({ a: s.name, ap: p.name, b: peer[1], bp: peer[2] });
        }
      } else if (hostMatch) {
        hosts.push({ name: hostMatch[1], sw: s.name, port: p.name });
      }
    }
  }
  return { names, links, hosts, byName: new Map(switches.map((s) => [s.name, s])) };
}

function layout(names) {
  const pos = new Map();
  const cx = 360, cy = 255, r = names.length > 1 ? 120 : 0;
  names.forEach((n, i) => {
    const a = -Math.PI / 2 + (2 * Math.PI * i) / Math.max(names.length, 1);
    pos.set(n, { x: cx + r * Math.cos(a), y: cy + r * Math.sin(a), a });
  });
  return pos;
}

function portState(sw, port) {
  const p = sw?.ports.find((q) => q.name === port);
  if (!p) return "down";
  if (!p.admin_up || !p.oper_up) return "down";
  return p.stp_state;
}

function renderTopology() {
  const svg = $("topology");
  svg.replaceChildren();
  linkElements.clear();
  const model = buildModel();
  const pos = layout(model.names);

  const portDot = (from, to, sw, port) => {
    // Just outside the 76x40 switch box, whatever the link's angle.
    const dx = to.x - from.x, dy = to.y - from.y;
    const len = Math.hypot(dx, dy);
    const edge = Math.min(Math.abs(42 / (dx / len || 1e-9)), Math.abs(24 / (dy / len || 1e-9)));
    const t = (edge + 7) / len;
    svg.append(svgEl("circle", { cx: from.x + (to.x - from.x) * t, cy: from.y + (to.y - from.y) * t, r: 6,
                                 class: `port ${portState(sw, port)}` }));
  };

  for (const l of model.links) {
    const a = pos.get(l.a), b = pos.get(l.b);
    if (!a || !b) continue;
    const sa = model.byName.get(l.a), sb = model.byName.get(l.b);
    const blocked = portState(sa, l.ap) !== "forwarding" || portState(sb, l.bp) !== "forwarding";
    const line = svgEl("line", { x1: a.x, y1: a.y, x2: b.x, y2: b.y, class: `link${blocked ? " blocked" : ""}` });
    svg.append(line);
    linkElements.set(`${l.a}:${l.ap}`, line);
    linkElements.set(`${l.b}:${l.bp}`, line);
    portDot(a, b, sa, l.ap);
    portDot(b, a, sb, l.bp);
    const mid = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
    svg.append(svgEl("text", { x: mid.x + 6, y: mid.y - 6, class: "small" }, `${l.ap} – ${l.bp}`));
  }

  const perSwitch = new Map();
  for (const h of model.hosts) {
    const list = perSwitch.get(h.sw) || [];
    list.push(h);
    perSwitch.set(h.sw, list);
  }
  for (const [swName, list] of perSwitch) {
    const p = pos.get(swName);
    if (!p) continue;
    list.forEach((h, i) => {
      const spread = (i - (list.length - 1) / 2) * 0.55;
      const a = p.a + spread;
      const hp = { x: p.x + 100 * Math.cos(a), y: p.y + 100 * Math.sin(a) };
      const line = svgEl("line", { x1: p.x, y1: p.y, x2: hp.x, y2: hp.y, class: "link" });
      svg.append(line);
      linkElements.set(`${swName}:${h.port}`, line);
      portDot(p, hp, model.byName.get(swName), h.port);
      const g = svgEl("g", { class: "host" });
      g.append(svgEl("circle", { cx: hp.x, cy: hp.y, r: 18 }), svgEl("text", { x: hp.x - 9, y: hp.y + 4 }, h.name));
      svg.append(g);
    });
  }

  for (const name of model.names) {
    const p = pos.get(name);
    const s = model.byName.get(name);
    const g = svgEl("g", { class: `switch${s.stp?.is_root ? " root" : ""}` });
    g.append(svgEl("rect", { x: p.x - 38, y: p.y - 20, width: 76, height: 40, rx: 8 }),
             svgEl("text", { x: p.x - 13, y: p.y + 1 }, name),
             svgEl("text", { x: p.x - 30, y: p.y + 14, class: "small" }, s.stp?.is_root ? "STP root" : `cost ${s.stp?.root_cost ?? "-"}`));
    svg.append(g);
  }
}

function flashLinks(t) {
  if (t.decision === "drop" || t.decision === "punt") return;
  for (const port of [t.in, ...t.out]) {
    const line = linkElements.get(`${t.sw}:${port}`);
    if (!line) continue;
    line.classList.add("flash");
    clearTimeout(line._timer);
    line._timer = setTimeout(() => line.classList.remove("flash"), 350);
  }
}

// ---- status chips and tables ----------------------------------------------------------------
function renderStatus() {
  const box = $("switch-status");
  box.replaceChildren();
  for (const e of endpoints) {
    const s = state.get(e);
    const ok = online.get(e) && s?.dataplane_attached;
    const label = s ? `${s.name} ${s.version}${s.dataplane_attached ? "" : " (data plane down)"}` : `${e} unreachable`;
    box.append(el("span", { class: `chip ${ok ? "ok" : "bad"}` }, label));
  }
}

function renderSwitchSelectors() {
  const names = [...state.values()].map((s) => s.name).sort();
  for (const id of ["trace-switch", "table-switch"]) {
    const sel = $(id);
    const keep = sel.value;
    const have = [...sel.options].map((o) => o.value).filter(Boolean);
    if (have.join() === names.join()) continue;
    for (const n of names) if (!have.includes(n)) sel.append(el("option", { value: n }, n));
    sel.value = keep || (id === "table-switch" ? names[0] || "" : "");
  }
}

function table(headers, rows) {
  const t = el("table");
  const head = el("tr");
  headers.forEach((h) => head.append(el("th", {}, h)));
  const thead = el("thead");
  thead.append(head);
  t.append(thead);
  const body = el("tbody");
  for (const row of rows) {
    const tr = el("tr");
    row.forEach((cell) => {
      const td = el("td", cell.cls ? { class: cell.cls } : {}, cell.text ?? String(cell));
      tr.append(td);
    });
    body.append(tr);
  }
  t.append(body);
  return t;
}

let activeTab = "ports";
function renderTable() {
  const view = $("table-view");
  const name = $("table-switch").value;
  const entry = [...state.entries()].find(([, s]) => s.name === name);
  if (!entry) { view.replaceChildren(el("p", { class: "hint" }, "no data yet")); return; }
  const [endpoint, s] = entry;
  const prev = previous.get(endpoint);
  const dt = prev ? (s.now_ns - prev.now_ns) / 1e9 : 0;
  const rate = (p, field) => {
    const q = prev?.ports.find((x) => x.name === p.name);
    return q && dt > 0 ? Math.max(0, Math.round((p[field] - q[field]) / dt)) : 0;
  };
  let node;
  switch (activeTab) {
    case "ports":
      node = table(["Port", "Link", "Mode", "PVID", "STP", "RX pps", "TX pps", "RX", "TX", "Drops", "Description"],
        s.ports.map((p) => [p.name, p.admin_up ? (p.oper_up ? "up" : "no carrier") : "shutdown", p.mode, p.pvid || "-",
          { text: `${p.stp_role}/${p.stp_state}`, cls: `state ${p.stp_state}` }, rate(p, "rx_packets"), rate(p, "tx_packets"),
          p.rx_packets, p.tx_packets, p.drops, p.description]));
      break;
    case "stp":
      node = el("div");
      node.append(el("p", { class: "mono" }, s.stp.enabled
        ? `bridge ${s.stp.bridge_id}  root ${s.stp.root_id}${s.stp.is_root ? " (this switch)" : ""}  cost ${s.stp.root_cost}  topology changes ${s.stp.topology_changes}`
        : "spanning tree disabled"));
      node.append(table(["Port", "Role", "State", "Edge"],
        s.ports.map((p) => [p.name, p.stp_role, { text: p.stp_state, cls: `state ${p.stp_state}` }, p.edge ? "yes" : "no"])));
      break;
    case "fdb":
      node = table(["VLAN", "MAC", "Port", "Type", "Age"],
        [...s.fdb].sort((a, b) => a.vlan - b.vlan || a.mac.localeCompare(b.mac))
          .map((f) => [f.vlan, { text: f.mac, cls: "mono" }, f.port, f.static ? "static" : "dynamic", f.static ? "-" : `${f.age}s`]));
      break;
    case "mdb":
      node = table(["VLAN", "Group", "Member ports"], s.mdb.map((g) => [g.vlan, { text: g.group, cls: "mono" }, g.ports.join(",")]));
      break;
    case "l3":
      node = el("div");
      node.append(table(["Prefix", "Type", "Next hop", "VLAN", "Status"],
        s.routes.map((r) => [{ text: r.prefix, cls: "mono" }, r.connected ? "connected" : "static", r.nexthop, r.vlan || "-", r.active ? "active" : "inactive"])));
      node.append(table(["Address", "MAC", "VLAN"], s.arp.map((a) => [{ text: a.ip, cls: "mono" }, { text: a.mac, cls: "mono" }, a.vlan])));
      break;
    default:
      node = table(["Drop reason", "Packets"], Object.entries(s.drops).map(([k, v]) => [k, v]));
  }
  view.replaceChildren(node);
}

// ---- wiring -------------------------------------------------------------------------------
$("trace-pause").addEventListener("click", (e) => {
  paused = !paused;
  e.target.textContent = paused ? "Resume" : "Pause";
  if (!paused) rerenderTrace();
});
$("trace-clear").addEventListener("click", () => { traces.length = 0; rerenderTrace(); });
$("trace-filter").addEventListener("input", rerenderTrace);
$("trace-switch").addEventListener("change", rerenderTrace);
$("table-switch").addEventListener("change", renderTable);
for (const b of document.querySelectorAll("#tabs button")) {
  b.addEventListener("click", () => {
    document.querySelectorAll("#tabs button").forEach((x) => x.classList.toggle("active", x === b));
    activeTab = b.dataset.tab;
    renderTable();
  });
}

endpoints.forEach(subscribe);
refresh();
setInterval(refresh, 1000);
