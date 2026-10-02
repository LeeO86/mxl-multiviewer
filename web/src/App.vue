<script setup>
import { computed, onMounted, onUnmounted, ref } from "vue";

const tab = ref("preview");
const previewSrc = ref("/preview.jpg");
const info = ref(null);
const inputs = ref([]);
const outputs = ref([]);
const layouts = ref({ layouts: [], active: "" });
const alarms = ref([]);
const envText = ref("");
const error = ref("");
const selected = ref(0);
const grid = 24;
const wsState = ref("offline");
let timer = 0;
let socket = null;
let wsTimer = 0;

async function load() {
  const [i, inn, out, lay, al, env] = await Promise.all([
    fetch("/api/v1/info").then((r) => r.json()),
    fetch("/api/v1/inputs").then((r) => r.json()),
    fetch("/api/v1/outputs").then((r) => r.json()),
    fetch("/api/v1/layouts").then((r) => r.json()),
    fetch("/api/v1/alarms").then((r) => r.json()),
    fetch("/api/v1/config/env").then((r) => r.text()),
  ]);
  info.value = i;
  inputs.value = inn.inputs || [];
  outputs.value = out.outputs || [];
  layouts.value = lay;
  alarms.value = al.alarms || [];
  envText.value = env;
}

const activeLayout = computed(() => (layouts.value.layouts || []).find((l) => l.name === (outputs.value[0]?.layout || layouts.value.active)));

function snap(v) {
  return Math.round(v * grid) / grid;
}

function trackPointer(event, tile, resize) {
  selected.value = activeLayout.value.tiles.indexOf(tile);
  const canvas = event.currentTarget.closest(".canvas").getBoundingClientRect();
  const startX = event.clientX;
  const startY = event.clientY;
  const orig = { ...tile.rect };
  const move = (ev) => {
    const dx = (ev.clientX - startX) / canvas.width;
    const dy = (ev.clientY - startY) / canvas.height;
    if (resize) {
      tile.rect.w = Math.min(1 - tile.rect.x, Math.max(1 / grid, snap(orig.w + dx)));
      tile.rect.h = Math.min(1 - tile.rect.y, Math.max(1 / grid, snap(orig.h + dy)));
    } else {
      tile.rect.x = Math.min(1 - tile.rect.w, Math.max(0, snap(orig.x + dx)));
      tile.rect.y = Math.min(1 - tile.rect.h, Math.max(0, snap(orig.y + dy)));
    }
  };
  const up = () => {
    window.removeEventListener("pointermove", move);
    window.removeEventListener("pointerup", up);
  };
  window.addEventListener("pointermove", move);
  window.addEventListener("pointerup", up);
}

function onPointerDown(event, tile) {
  if (event.target.classList.contains("handle")) return;
  trackPointer(event, tile, false);
}

function onResizeDown(event, tile) {
  trackPointer(event, tile, true);
}

function connectEvents() {
  if (socket) {
    socket.onclose = null;
    socket.close();
  }
  const proto = location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(proto + "://" + location.host + "/api/v1/events");
  socket.onopen = () => (wsState.value = "live");
  socket.onmessage = (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (msg.inputs) inputs.value = msg.inputs;
      if (msg.outputs) outputs.value = msg.outputs;
      if (msg.alarms) alarms.value = msg.alarms;
    } catch (err) {
      error.value = String(err);
    }
  };
  socket.onclose = () => {
    wsState.value = "offline";
    wsTimer = setTimeout(connectEvents, 2000);
  };
}

async function saveLayout() {
  const layout = activeLayout.value;
  if (!layout) return;
  const response = await fetch("/api/v1/layouts/" + encodeURIComponent(layout.name), {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(layout),
  });
  if (!response.ok) error.value = await response.text();
  else error.value = "";
  await load();
}

async function activate(name) {
  await fetch("/api/v1/layouts/" + encodeURIComponent(name) + "/activate", { method: "POST" });
  await load();
}

function exportLayouts() {
  const blob = new Blob([JSON.stringify(layouts.value, null, 2)], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = "layouts.json";
  a.click();
  URL.revokeObjectURL(url);
}

onMounted(() => {
  load().catch((err) => (error.value = String(err)));
  connectEvents();
  timer = setInterval(() => {
    previewSrc.value = "/preview.jpg?t=" + Date.now();
    if (wsState.value !== "live") load().catch(() => {});
  }, 1000);
});
onUnmounted(() => {
  clearInterval(timer);
  clearTimeout(wsTimer);
  if (socket) socket.close();
});
</script>

<template>
  <main>
    <header>
      <strong>mxl-multiviewer</strong>
      <span v-if="info">{{ info.version }} · {{ info.backend }} · cuda {{ info.cuda_devices || 0 }} · {{ info.max_inputs }} inputs · {{ wsState }}</span>
      <nav>
        <button :class="{ on: tab === 'preview' }" @click="tab = 'preview'">Preview</button>
        <button :class="{ on: tab === 'layout' }" @click="tab = 'layout'">Layout</button>
        <button :class="{ on: tab === 'inputs' }" @click="tab = 'inputs'">Inputs</button>
        <button :class="{ on: tab === 'alarms' }" @click="tab = 'alarms'">Alarms</button>
        <button :class="{ on: tab === 'settings' }" @click="tab = 'settings'">Settings</button>
      </nav>
    </header>
    <p v-if="error" class="err">{{ error }}</p>
    <section v-if="tab === 'preview'">
      <img :src="previewSrc" alt="output preview" />
      <p v-if="outputs[0]">{{ outputs[0].format }} · {{ outputs[0].layout }} · frames {{ outputs[0].frames }} · late {{ outputs[0].late }}</p>
    </section>
    <section v-if="tab === 'layout' && activeLayout" class="editor">
      <div class="canvas">
        <div
          v-for="tile in activeLayout.tiles"
          :key="tile.id"
          class="tile"
          :style="{ left: tile.rect.x * 100 + '%', top: tile.rect.y * 100 + '%', width: tile.rect.w * 100 + '%', height: tile.rect.h * 100 + '%' }"
          @pointerdown="onPointerDown($event, tile)"
        >
          {{ tile.content }} {{ tile.content === "input" ? tile.input : "" }}
          <span class="handle" @pointerdown.stop="onResizeDown($event, tile)"></span>
        </div>
      </div>
      <aside>
        <label>Active
          <select :value="outputs[0]?.layout" @change="activate($event.target.value)">
            <option v-for="layout in layouts.layouts" :key="layout.name">{{ layout.name }}</option>
          </select>
        </label>
        <div v-if="activeLayout.tiles[selected]">
          <p>Tile {{ activeLayout.tiles[selected].id }}</p>
          <label>Input <input type="number" min="1" v-model.number="activeLayout.tiles[selected].input" /></label>
          <label>Scale
            <select v-model="activeLayout.tiles[selected].scale">
              <option>fit</option>
              <option>fill</option>
            </select>
          </label>
          <label>UMD <input v-model="activeLayout.tiles[selected].umd_text" /></label>
        </div>
        <button @click="saveLayout">Save layout</button>
        <button @click="exportLayouts">Export JSON</button>
      </aside>
    </section>
    <section v-if="tab === 'inputs'">
      <table>
        <thead><tr><th>#</th><th>Video</th><th>Flow</th><th>Format</th><th>Audio</th></tr></thead>
        <tbody>
          <tr v-for="input in inputs" :key="input.index">
            <td>{{ input.index }}</td>
            <td>{{ input.video.state }}</td>
            <td>{{ input.video.flow_id || "—" }}</td>
            <td>{{ input.video.format || "—" }}</td>
            <td>{{ input.audio.state }}</td>
          </tr>
        </tbody>
      </table>
    </section>
    <section v-if="tab === 'alarms'">
      <p v-if="!alarms.length">No active alarms.</p>
      <ul>
        <li v-for="(alarm, i) in alarms" :key="i">Input {{ alarm.input }} · {{ alarm.name }}</li>
      </ul>
    </section>
    <section v-if="tab === 'settings'">
      <p>Routing stays on IS-05. This page exports the effective configuration.</p>
      <textarea readonly :value="envText"></textarea>
    </section>
  </main>
</template>

<style>
body { margin: 0; font-family: sans-serif; background: #12141a; color: #e8e8e8; }
header { display: flex; gap: 1rem; align-items: center; padding: 0.75rem 1rem; background: #1c2030; }
nav button { margin-right: 0.4rem; background: #2a3148; color: inherit; border: 0; padding: 0.4rem 0.7rem; }
nav button.on { background: #3d6df0; }
section { padding: 1rem; }
img { max-width: 100%; background: #000; }
.editor { display: grid; grid-template-columns: 2fr 1fr; gap: 1rem; }
.canvas { position: relative; background: #0a0c10; aspect-ratio: 16/9; }
.tile { position: absolute; border: 1px solid #6cf; background: rgba(60, 120, 220, 0.25); box-sizing: border-box; padding: 4px; cursor: move; }
.handle { position: absolute; right: 0; bottom: 0; width: 12px; height: 12px; background: #6cf; cursor: nwse-resize; }
aside label { display: block; margin: 0.4rem 0; }
textarea { width: 100%; min-height: 16rem; background: #0a0c10; color: inherit; }
table { border-collapse: collapse; width: 100%; }
td, th { border-bottom: 1px solid #333; text-align: left; padding: 0.3rem; }
.err { color: #f88; }
</style>
