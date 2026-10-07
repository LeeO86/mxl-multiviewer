// REST client and the live state from /api/v1/events (SPECIFICATION.md §8).
import { reactive } from "vue";

async function request(path, { method = "GET", body, text = false } = {}) {
  const headers = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const resp = await fetch(path, {
    method,
    headers,
    body: body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
    cache: "no-store",
  });
  const raw = await resp.text();
  let data = raw;
  if (!text) {
    try {
      data = raw ? JSON.parse(raw) : null;
    } catch {
      data = raw;
    }
  }
  if (!resp.ok) {
    const reason = data && typeof data === "object" ? data.error : String(data || "").slice(0, 200);
    throw new Error(reason || `HTTP ${resp.status}`);
  }
  return data;
}

export const api = {
  get: (path) => request(path),
  text: (path) => request(path, { text: true }),
  put: (path, body) => request(path, { method: "PUT", body }),
  post: (path, body) => request(path, { method: "POST", body: body ?? {} }),
  del: (path) => request(path, { method: "DELETE" }),
};

/** Live state shared by every tab. `inputs`, `outputs` and `alarms` follow the WebSocket. */
export const live = reactive({
  info: null,
  inputs: [],
  outputs: [],
  alarms: [],
  connected: false,
  everConnected: false,
  error: "",
  // Keys a settings change marked restart-required (until the page reloads).
  restartKeys: [],
});

let socket = null;
let retry = 0;
let poll = 0;

async function pollOnce() {
  try {
    const [inputs, outputs, alarms] = await Promise.all([api.get("/api/v1/inputs"), api.get("/api/v1/outputs"), api.get("/api/v1/alarms")]);
    live.inputs = inputs.inputs || [];
    live.outputs = outputs.outputs || [];
    live.alarms = alarms.alarms || [];
    live.error = "";
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
}

function connect() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  socket = new WebSocket(`${proto}//${location.host}/api/v1/events`);
  socket.onopen = () => {
    live.connected = true;
    live.everConnected = true;
    live.error = "";
  };
  socket.onmessage = (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (msg.inputs) live.inputs = msg.inputs;
      if (msg.outputs) live.outputs = msg.outputs;
      if (msg.alarms) live.alarms = msg.alarms;
    } catch {
      /* a broken frame is dropped; the next one replaces it */
    }
  };
  socket.onclose = () => {
    live.connected = false;
    retry = setTimeout(connect, 2000);
  };
}

/** Loads /api/v1/info, then follows the WebSocket; polls (never the layouts) while it is down. */
export async function startLive() {
  try {
    live.info = await api.get("/api/v1/info");
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
  await pollOnce();
  connect();
  poll = setInterval(() => {
    if (!live.connected) pollOnce();
  }, 2000);
}

export function stopLive() {
  clearInterval(poll);
  clearTimeout(retry);
  if (socket) {
    socket.onclose = null;
    socket.close();
  }
}

// ---- helpers ---------------------------------------------------------------

/** Pill kind for an input leg state (§5.3). */
export function stateKind(state) {
  return { running: "ok", holding: "warn", waiting: "warn", no_signal: "bad", not_routed: "neutral" }[state] || "neutral";
}

/** Human text for an input leg state. */
export function stateText(state) {
  return { running: "running", holding: "holding", waiting: "waiting", no_signal: "no signal", not_routed: "not routed" }[state] || state || "–";
}

export const ALARM_NAMES = {
  no_signal: "No signal",
  black: "Black",
  freeze: "Freeze",
  silence: "Silence",
  clip: "Clip",
  format_mismatch: "Format mismatch",
};

/** The first 8 characters of an id; the whole id goes into a title. */
export const shortId = (id) => (id ? String(id).slice(0, 8) : "–");

/** The source label of input n: the registry sender label, the MXL flow label, else MV In n. */
export function inputLabel(n) {
  const input = live.inputs.find((i) => i.index === n);
  return input?.video?.label || `MV In ${n}`;
}

/** Percentage of a PPM bar for a dBFS value (-60..0). */
export function meterPct(db) {
  const v = typeof db === "number" && Number.isFinite(db) ? db : -120;
  return Math.max(0, Math.min(100, ((v + 60) / 60) * 100));
}

/** Copies text; falls back to a hidden textarea on plain http, where navigator.clipboard is missing. */
export async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    const area = document.createElement("textarea");
    area.value = text;
    area.style.position = "fixed";
    area.style.opacity = "0";
    document.body.appendChild(area);
    area.select();
    const ok = document.execCommand("copy");
    area.remove();
    return ok;
  }
}

export function download(name, text, type = "application/json") {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

export const clone = (v) => JSON.parse(JSON.stringify(v));
