<script setup>
// Settings (§8.1, §9): effective configuration with its origin, edits to the config file,
// export (JSON document and KEY=value) and import.
import { computed, onMounted, ref } from "vue";
import { api, copyText, download, live } from "../api.js";

const settings = ref([]);
const edits = ref({}); // key -> new string, or null to remove the file value
const msg = ref({ kind: "", text: "" });
const exportDoc = ref("");
const envText = ref("");
const importText = ref("");
const importMsg = ref({ kind: "", text: "" });
const exportMsg = ref("");

const GROUPS = [
  { title: "Output and layout", test: /^(MV_OUTPUTS|MV_OUTPUT_FORMAT|MV_OUT\d|MV_ACTIVE_LAYOUT|MV_LAYOUTS_FILE|MV_BACKGROUND_FILE|MV_GRID|MV_PREVIEW_|MV_OVERLAY_HZ|MV_AUDIO_|MV_TIMEZONE)/ },
  { title: "Inputs and alarms", test: /^(MV_MAX_INPUTS|MV_INPUT_OFFSET|MV_HOLD_MS|MV_BLACK_Y|MV_SILENCE|MV_CLIP|MV_ALARM|MV_FREEZE)/ },
  { title: "NMOS", test: /^NMOS_/ },
  { title: "Web and TSL", test: /^(WEB_|TSL_)/ },
  { title: "MXL, state and process", test: /./ },
];
// One line per key, from SPECIFICATION.md §9.
const DESCRIPTIONS = {
  HOST_ID: "Node label when NMOS_LABEL is empty, and the default id seed.",
  MXL_DOMAIN_SCAN_PATH: "MXL root: the folder that holds the domains.",
  MV_OUTPUT_DOMAIN_DIR: "Folder of the output domain this multiviewer writes.",
  MV_OUTPUT_DOMAIN_ID: "Output domain id when the domain is created (empty: from NMOS_SEED).",
  MV_STATE_DIR: "Folder for config.json, layouts.json and routes.json.",
  MXL_CLEANUP_ON_EXIT: "Remove the output domain when the process stops.",
  MV_BACKEND: "Compositor: auto, cuda or cpu.",
  MV_MAX_INPUTS: "Number of inputs (1–32).",
  MV_OUTPUTS: "Number of output heads (1–3).",
  MV_OUTPUT_FORMAT: "Raster and rate of output 1, e.g. 1920x1080p50.",
  MV_INPUT_OFFSET_GRAINS: "Frames each input is read behind the output time.",
  MV_HOLD_MS: "How long a missing input keeps its last frame before the NO SIGNAL slate.",
  MV_HISTORY_DURATION_NS: "Ring length of a new output domain.",
  MV_LAYOUTS_FILE: "Layout file (empty: layouts.json in the state folder).",
  MV_ACTIVE_LAYOUT: "Layout the outputs start with.",
  MV_AUDIO_CHANNELS: "Output audio channels: 0 (off), 2 or 16.",
  MV_AUDIO_FOLLOW: "Input whose audio goes to the output (0: none).",
  MV_OVERLAY_HZ: "Redraws per second of captions, meters and clocks.",
  MV_PREVIEW_FPS: "Preview pictures per second.",
  MV_PREVIEW_WIDTH: "Preview width in pixels.",
  MV_GRID: "Snap grid of the layout editor (cells per side).",
  MV_BLACK_Y: "Black alarm: average luma at or below this 10-bit value.",
  MV_SILENCE_DBFS: "Silence alarm: every channel below this level (dBFS).",
  MV_CLIP_LINEAR: "Clip alarm: a sample at or above this value (1 is full scale).",
  MV_ALARM_DEBOUNCE_MS: "How long a condition lasts before its alarm rises.",
  MV_ALARM_CLEAR_MS: "How long a condition is gone before its alarm clears.",
  MV_FREEZE_MS: "Freeze alarm: the picture has not changed for this long (at least 1000 ms).",
  MV_BACKGROUND_FILE: "JPEG or PNG picture under the tiles.",
  MV_TIMEZONE: "Time zone of clocks with local time, e.g. Europe/Zurich (empty: TZ).",
  MV_CONFIG_FILE: "This configuration file.",
  NMOS_ENABLE: "Run the NMOS node (IS-04 registration, IS-05 routing).",
  NMOS_REGISTRY_ADDRESS: "Registry address.",
  NMOS_REGISTRY_PORT: "Registry registration port.",
  NMOS_QUERY_ADDRESS: "Query API address (empty: the registry).",
  NMOS_QUERY_PORT: "Query API port (empty: registry port + 1).",
  NMOS_DNS_SD: "Find the registry and advertise with DNS-SD.",
  NMOS_PORT: "Node and Connection API port; the WebSocket uses the next port.",
  NMOS_SEED: "Seed of every NMOS id and of the default domain id.",
  NMOS_LABEL: "Node label and device label prefix.",
  NMOS_HOST_ADDRESS: "IPv4 address announced to NMOS.",
  NMOS_TAGS: "Tags on the node and device (JSON object of string lists).",
  WEB_ENABLE: "Serve this UI, the preview, and changes over the API.",
  WEB_PORT: "Port of this UI and the API.",
  TSL_ENABLE: "Listen for TSL UMD tally.",
  TSL_UDP_PORT: "TSL 5.0 UDP port.",
  TSL_TCP_PORT: "TSL 5.0 TCP port.",
  TSL_V31: "Also accept TSL 3.1 on UDP.",
  TSL_SCREEN: "Only this TSL screen (-1: every screen).",
  TSL_MAP: "TSL display to input, e.g. 0:1,1:2 (empty: display i is input i+1).",
  LOG_LEVEL: "trace, debug, info, warn or error.",
  LOG_FORMAT: "json or text.",
  SHUTDOWN_TIMEOUT_S: "Seconds to stop cleanly after SIGTERM.",
};
const HEAD_KEYS = { FORMAT: "raster and rate", LAYOUT: "start layout", AUDIO_FOLLOW: "audio-follow input", AUDIO_CHANNELS: "audio channels" };

function describe(key) {
  const head = /^MV_OUT(\d)_(.+)$/.exec(key);
  if (head) return `Output ${head[1]}: ${HEAD_KEYS[head[2]] || head[2].toLowerCase()}.`;
  return DESCRIPTIONS[key] || "";
}

// MV_OUT1_* repeat the unscoped keys, and MV_OUT<h>_* only matter with that many heads.
function shown(s) {
  const head = /^MV_OUT(\d)_/.exec(s.key);
  if (!head || s.source !== "default") return true;
  return Number(head[1]) > 1 && Number(head[1]) <= (live.info?.outputs || 1);
}

const ORIGIN = { environment: { text: "ENV", cls: "env" }, file: { text: "FILE", cls: "file" }, default: { text: "DEFAULT", cls: "default" } };

const groups = computed(() => {
  const left = settings.value.filter(shown);
  return GROUPS.map((g) => {
    const items = left.filter((s) => g.test.test(s.key));
    items.forEach((s) => left.splice(left.indexOf(s), 1));
    return { title: g.title, items };
  }).filter((g) => g.items.length);
});
const changed = computed(() => Object.keys(edits.value));

function value(s) {
  return s.key in edits.value ? (edits.value[s.key] ?? "") : s.value;
}
function edit(s, v) {
  if (v === s.value) delete edits.value[s.key];
  else edits.value[s.key] = v;
}
function reset(s) {
  edits.value[s.key] = null;
}

async function load() {
  settings.value = (await api.get("/api/v1/config")).settings;
}

async function save() {
  try {
    await api.put("/api/v1/config", edits.value);
    // The process reads its configuration at start: every saved key applies after a restart.
    live.restartKeys = [...new Set([...live.restartKeys, ...changed.value])];
    msg.value = { kind: "ok", text: `Saved ${changed.value.join(", ")} to the configuration file. Restart the multiviewer to apply.` };
    edits.value = {};
    await load();
  } catch (e) {
    msg.value = { kind: "err", text: e.message };
  }
}

async function loadExport() {
  exportDoc.value = JSON.stringify(await api.get("/api/v1/config/export"), null, 2);
  envText.value = await api.text("/api/v1/config/env");
}

async function copy(text, what) {
  exportMsg.value = (await copyText(text)) ? `${what} copied.` : "Copy failed; select the text and copy it by hand.";
}

function onFile(ev) {
  const file = ev.target.files[0];
  if (file) file.text().then((t) => (importText.value = t));
}

async function doImport() {
  try {
    JSON.parse(importText.value);
  } catch (e) {
    importMsg.value = { kind: "err", text: `Not JSON: ${e.message}` };
    return;
  }
  try {
    const result = await api.post("/api/v1/config/import", importText.value);
    const skipped = result.skipped?.length ? ` Skipped (set by the environment): ${result.skipped.join(", ")}.` : "";
    const moved = result.heads_moved?.length ? ` Output ${result.heads_moved.join(", ")} switched to the active layout: its layout is not in the import.` : "";
    importMsg.value = {
      kind: "ok",
      text: `Imported. Settings and layouts are saved${result.routes_restart ? "; routes apply after the next restart" : ""}. Restart the multiviewer to apply settings.${skipped}${moved}`,
    };
    await load();
    await loadExport();
  } catch (e) {
    importMsg.value = { kind: "err", text: e.message };
  }
}

onMounted(async () => {
  try {
    await load();
    await loadExport();
  } catch (e) {
    msg.value = { kind: "err", text: e.message };
  }
});
</script>

<template>
  <div class="panel">
    <h3>Configuration</h3>
    <p class="note">
      Precedence: environment, then the configuration file, then the default. Settings set by an environment variable (ENV) are read-only here. Saved
      values go into the configuration file and apply when the multiviewer starts. RESTART marks the settings the specification says need a restart.
    </p>
    <div class="actions" style="margin-top: 0">
      <span class="msg" :class="msg.kind" style="margin: 0">{{ msg.text }}</span>
      <span class="spacer"></span>
      <button class="btn secondary" :disabled="!changed.length" @click="edits = {}">Discard</button>
      <button class="btn" :disabled="!changed.length" @click="save">Save {{ changed.length ? `(${changed.length})` : "" }}</button>
    </div>
  </div>

  <div v-for="g in groups" :key="g.title" class="panel">
    <h3>{{ g.title }}</h3>
    <table>
      <thead>
        <tr>
          <th style="width: 30%">Key</th>
          <th>Value</th>
          <th style="width: 7rem">Origin</th>
          <th style="width: 6rem"></th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="s in g.items" :key="s.key">
          <td>
            <code>{{ s.key }}</code>
            <span v-if="s.restart" class="badge restart" title="Applies after a restart">RESTART</span>
            <div class="desc">{{ describe(s.key) }}</div>
          </td>
          <td>
            <input
              :value="value(s)"
              :disabled="!s.editable"
              :class="{ dirty: s.key in edits }"
              :aria-label="s.key"
              :placeholder="s.key in edits && edits[s.key] === null ? '(default)' : ''"
              @input="edit(s, $event.target.value)"
            />
          </td>
          <td><span class="badge" :class="ORIGIN[s.source]?.cls">{{ ORIGIN[s.source]?.text || s.source }}</span></td>
          <td>
            <button v-if="s.source === 'file' && s.editable" class="btn small secondary" title="Remove the file value" @click="reset(s)">Default</button>
          </td>
        </tr>
      </tbody>
    </table>
  </div>

  <div class="grid wide">
    <div class="panel">
      <h3>Export</h3>
      <p class="note">One JSON document with every setting, the layouts and the routes. The multiviewer has no secrets.</p>
      <div class="actions" style="margin-top: 0">
        <span class="msg ok" style="margin: 0">{{ exportMsg }}</span>
        <span class="spacer"></span>
        <button class="btn secondary" :disabled="!exportDoc" @click="copy(exportDoc, 'JSON')">Copy JSON</button>
        <button class="btn" :disabled="!exportDoc" @click="download('mxl-multiviewer.json', exportDoc)">Download JSON</button>
      </div>
      <pre>{{ exportDoc }}</pre>
      <div class="actions">
        <button class="btn secondary" :disabled="!envText" @click="copy(envText, 'KEY=value list')">Copy KEY=value</button>
        <button class="btn secondary" :disabled="!envText" @click="download('mxl-multiviewer.env', envText, 'text/plain')">Download KEY=value</button>
      </div>
    </div>
    <div class="panel">
      <h3>Import</h3>
      <p class="note">An exported document. Settings and layouts are saved at once; routes apply after the next restart.</p>
      <input type="file" accept="application/json,.json" aria-label="Open an exported JSON file" @change="onFile" />
      <textarea v-model="importText" aria-label="Exported JSON" placeholder='{"version": 1, "settings": {...}, "layouts": {...}, "routes": {...}}'></textarea>
      <div class="actions">
        <span class="msg" :class="importMsg.kind" style="margin: 0">{{ importMsg.text }}</span>
        <span class="spacer"></span>
        <button class="btn" :disabled="!importText.trim()" @click="doImport">Import</button>
      </div>
    </div>
  </div>
</template>
