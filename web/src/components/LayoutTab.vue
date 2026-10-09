<script setup>
// Layout editor (§6, §8.1): tiles on a 16:9 canvas snapped to MV_GRID, the inspector of
// the selected tile, and the layout book (save, save as, delete, activate, import, export).
import { computed, nextTick, onMounted, onUnmounted, ref } from "vue";
import { api, clone, download, live } from "../api.js";
import {
  PRESETS,
  byZ,
  discard,
  draftOf,
  editor,
  freeRect,
  isDirty,
  layoutNames,
  loadBook,
  makeTile,
  newLayout,
  nextTileId,
  removeLayout,
  restack,
  saveAs,
} from "../editor.js";
import Field from "./Field.vue";
import Pill from "./Pill.vue";
import TileInspector from "./TileInspector.vue";
import TileView from "./TileView.vue";

const msg = ref({ kind: "", text: "" });
const form = ref(""); // "", "new", "saveas", "delete", "import"
const formName = ref("");
const formCopy = ref(false);
const importText = ref("");
const canvasEl = ref(null);
const nameEl = ref(null);
const now = ref(Date.now());
let clockTimer = 0;

const grid = computed(() => live.info?.grid || 24);
const maxInputs = computed(() => live.info?.max_inputs || 16);
const layout = computed(() => editor.drafts[editor.current]);
const dirty = computed(() => isDirty(editor.current));
const isNew = computed(() => editor.saved[editor.current] === undefined);
const onAir = computed(() => live.outputs.some((o) => o.layout === editor.current));
const ordered = computed(() => (layout.value ? byZ(layout.value) : []));
const selected = computed(() => layout.value?.tiles.find((t) => t.id === editor.selectedId) || null);
const layer = computed(() => {
  if (!selected.value) return "";
  return `layer ${ordered.value.indexOf(selected.value) + 1} of ${ordered.value.length}`;
});
const gridStyle = computed(() => {
  const step = 100 / grid.value;
  const line = "rgba(255, 255, 255, .18)";
  return {
    backgroundImage: `linear-gradient(to right, ${line} 1px, transparent 1px), linear-gradient(to bottom, ${line} 1px, transparent 1px)`,
    backgroundSize: `${step}% ${step}%`,
  };
});

function say(kind, text) {
  msg.value = { kind, text };
}

async function run(action, done) {
  try {
    await action();
    if (done) say("ok", done);
  } catch (e) {
    say("err", e.message);
  }
}

function pick(name) {
  editor.current = name;
  editor.selectedId = "";
  draftOf(name);
  form.value = "";
  say("", "");
}

// ---- book actions -------------------------------------------------------------

function openForm(kind) {
  form.value = form.value === kind ? "" : kind;
  formName.value = kind === "saveas" ? `${editor.current} copy` : "";
  formCopy.value = false;
  say("", "");
  if (kind === "new" || kind === "saveas") nextTick(() => nameEl.value?.focus());
}

function validName(name) {
  if (!name.trim()) return "Enter a name.";
  if (name.includes("/")) return "A name cannot contain /.";
  return "";
}

function submitNew() {
  const name = formName.value.trim();
  const problem = validName(name) || (layoutNames().includes(name) ? `A layout called ${name} exists already.` : "");
  if (problem) return say("err", problem);
  newLayout(name, formCopy.value);
  form.value = "";
  say("ok", `New layout ${name}. Add tiles, then Save.`);
}

function submitSaveAs() {
  const name = formName.value.trim();
  const problem = validName(name);
  if (problem) return say("err", problem);
  run(async () => {
    await saveAs(name);
    form.value = "";
  }, `Saved as ${name}.`);
}

function save() {
  run(() => saveAs(editor.current), `Saved ${editor.current}.`);
}

function activate() {
  const name = editor.current;
  run(async () => {
    await api.post(`/api/v1/layouts/${encodeURIComponent(name)}/activate`);
    editor.book.active = name;
  }, `${name} is on air on every output.`);
}

/** "Use as start layout" of one output: it comes back to this layout after a restart (§6.1). */
function setStart(head, on) {
  const name = editor.current;
  run(
    () => api.put(`/api/v1/outputs/${head}`, { start_layout: on ? name : null }),
    on ? `Output ${head} starts on ${name} after a restart.` : `Output ${head} has no start layout now.`,
  );
}

/** What output `o` starts on, when that is not this layout. */
function startNote(o) {
  if (o.start_layout_env) return `The environment sets ${o.start_layout_env} (MV_OUT${o.index}_LAYOUT or MV_ACTIVE_LAYOUT); it wins.`;
  if (!o.start_layout) return "No start layout: it starts on the layout it showed last.";
  return o.start_layout === editor.current ? "" : `Starts on ${o.start_layout}.`;
}

function confirmDelete() {
  const name = editor.current;
  run(async () => {
    await removeLayout(name);
    form.value = "";
  }, `Deleted ${name}.`);
}

/** Replaces the tiles of a built-in preset with today's definition (unsaved until Save). */
function applyPreset() {
  const name = editor.current;
  run(async () => {
    const presets = await api.get("/api/v1/presets");
    const preset = presets.layouts.find((l) => l.name === name);
    if (!preset) throw new Error(`${name} is not a built-in preset`);
    editor.drafts[name] = clone(preset);
    editor.selectedId = "";
    form.value = "";
  }, `${name} now has the built-in preset defaults. Save to keep them.`);
}

function revert() {
  const name = editor.current;
  discard(name);
  say("ok", `Changes to ${name} discarded.`);
}

function exportLayout() {
  download(`${editor.current}.json`, JSON.stringify(layout.value, null, 2));
}

async function exportBook() {
  const book = await api.get("/api/v1/layouts");
  download("layouts.json", JSON.stringify(book, null, 2));
}

function onImportFile(ev) {
  const file = ev.target.files[0];
  if (file) file.text().then((text) => (importText.value = text));
}

/** One layout opens in the editor (unsaved); a book ({layouts: [...]}) replaces all layouts. */
function submitImport() {
  let doc;
  try {
    doc = JSON.parse(importText.value);
  } catch (e) {
    return say("err", `Not JSON: ${e.message}`);
  }
  if (Array.isArray(doc?.layouts)) {
    return run(async () => {
      const result = await api.post("/api/v1/config/import", { layouts: doc });
      editor.drafts = {};
      await loadBook(editor.current);
      form.value = "";
      importText.value = "";
      const moved = result?.heads_moved || [];
      const note = moved.length ? ` Output ${moved.join(", ")} now shows ${editor.book.active}: its layout is not in the import.` : "";
      say("ok", `Imported ${doc.layouts.length} layouts.${note}`);
    });
  }
  if (!Array.isArray(doc?.tiles)) return say("err", "Expected one layout (with tiles) or a layout book (with layouts).");
  const name = String(doc.name || "imported").trim();
  editor.drafts[name] = { version: 1, background: "#101010", ...clone(doc), name };
  editor.current = name;
  editor.selectedId = "";
  form.value = "";
  importText.value = "";
  say("ok", `Loaded ${name} into the editor. Save to keep it.`);
}

// ---- tiles --------------------------------------------------------------------

const SIZES = { input: [1 / 4, 1 / 4], clock: [1 / 6, 1 / 6], label: [1 / 3, 1 / 12], empty: [1 / 4, 1 / 4] };

function snap(v) {
  return Math.round(v * grid.value) / grid.value;
}

function addTile(kind) {
  const l = layout.value;
  const [w, h] = SIZES[kind].map((v) => Math.max(1, Math.round(v * grid.value)) / grid.value);
  const z = l.tiles.reduce((m, t) => Math.max(m, t.z + 1), 0);
  const tile = makeTile(kind, nextTileId(l), freeRect(l, w, h, grid.value), z);
  if (kind === "input") {
    const used = new Set(l.tiles.filter((t) => t.content === "input").map((t) => t.input));
    for (let n = 1; n <= maxInputs.value; ++n) {
      if (!used.has(n)) {
        tile.input = n;
        break;
      }
    }
  }
  l.tiles.push(tile);
  editor.selectedId = tile.id;
  canvasEl.value?.focus();
}

function duplicateTile() {
  const l = layout.value;
  const src = selected.value;
  if (!src) return;
  const tile = { ...clone(src), id: nextTileId(l), z: l.tiles.reduce((m, t) => Math.max(m, t.z + 1), 0) };
  tile.rect = freeRect(l, src.rect.w, src.rect.h, grid.value);
  l.tiles.push(tile);
  editor.selectedId = tile.id;
}

function deleteTile() {
  const l = layout.value;
  if (!selected.value) return;
  l.tiles.splice(l.tiles.indexOf(selected.value), 1);
  editor.selectedId = "";
}

function moveLayer(step) {
  if (selected.value) restack(layout.value, selected.value, step);
}

// Drag to move or resize, snapped to the grid.
function track(event, tile, resize) {
  if (event.button !== 0) return;
  editor.selectedId = tile.id;
  canvasEl.value?.focus();
  const box = canvasEl.value.getBoundingClientRect();
  const startX = event.clientX;
  const startY = event.clientY;
  const orig = { ...tile.rect };
  const step = 1 / grid.value;
  const move = (ev) => {
    const dx = (ev.clientX - startX) / box.width;
    const dy = (ev.clientY - startY) / box.height;
    if (resize) {
      tile.rect.w = Math.min(1 - tile.rect.x, Math.max(step, snap(orig.w + dx)));
      tile.rect.h = Math.min(1 - tile.rect.y, Math.max(step, snap(orig.h + dy)));
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

function onCanvasDown(event) {
  if (event.target === canvasEl.value || event.target.classList.contains("gridlines")) editor.selectedId = "";
}

// Arrow keys move by one grid cell, Shift + arrows resize, Delete removes, Ctrl+D duplicates.
function onKey(event) {
  const tile = selected.value;
  if (!tile) return;
  const step = 1 / grid.value;
  const r = tile.rect;
  const dx = { ArrowLeft: -1, ArrowRight: 1 }[event.key] || 0;
  const dy = { ArrowUp: -1, ArrowDown: 1 }[event.key] || 0;
  if (dx || dy) {
    event.preventDefault();
    if (event.shiftKey) {
      r.w = Math.min(1 - r.x, Math.max(step, snap(r.w + dx * step)));
      r.h = Math.min(1 - r.y, Math.max(step, snap(r.h + dy * step)));
    } else {
      r.x = Math.min(1 - r.w, Math.max(0, snap(r.x + dx * step)));
      r.y = Math.min(1 - r.h, Math.max(0, snap(r.y + dy * step)));
    }
  } else if (event.key === "Delete" || event.key === "Backspace") {
    event.preventDefault();
    deleteTile();
  } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "d") {
    event.preventDefault();
    duplicateTile();
  } else if (event.key === "Escape") {
    editor.selectedId = "";
  }
}

onMounted(async () => {
  clockTimer = setInterval(() => (now.value = Date.now()), 1000);
  if (!editor.book) await run(() => loadBook(live.outputs[0]?.layout));
});
onUnmounted(() => clearInterval(clockTimer));
</script>

<template>
  <div v-if="!layout" class="panel muted">Loading layouts…</div>
  <template v-else>
    <div class="panel">
      <div class="toolbar">
        <div class="group">
          <label for="layout-pick">Layout</label>
          <select id="layout-pick" :value="editor.current" @change="pick($event.target.value)">
            <option v-for="name in layoutNames()" :key="name" :value="name">
              {{ name }}{{ isDirty(name) ? " •" : "" }}{{ live.outputs.some((o) => o.layout === name) ? " (on air)" : "" }}
            </option>
          </select>
          <Pill :text="dirty ? (isNew ? 'not saved yet' : 'unsaved changes') : 'saved'" :kind="dirty ? 'warn' : 'ok'" />
          <Pill v-if="onAir" text="on air" kind="bad" />
        </div>
        <span class="sep"></span>
        <div class="group">
          <button class="btn" :disabled="!dirty" @click="save">Save</button>
          <button class="btn secondary" :aria-pressed="form === 'saveas'" @click="openForm('saveas')">Save as…</button>
          <button class="btn secondary" :disabled="!dirty" @click="revert">Discard</button>
        </div>
        <span class="sep"></span>
        <div class="group">
          <button class="btn secondary" :aria-pressed="form === 'new'" @click="openForm('new')">New…</button>
          <button
            class="btn secondary"
            :disabled="isNew || onAir || editor.current === editor.book?.active || PRESETS.includes(editor.current)"
            :title="PRESETS.includes(editor.current) ? 'Built-in presets cannot be deleted' : onAir || editor.current === editor.book?.active ? 'The active layout cannot be deleted' : ''"
            :aria-pressed="form === 'delete'"
            @click="openForm('delete')"
          >
            Delete…
          </button>
          <button
            v-if="PRESETS.includes(editor.current)"
            class="btn secondary"
            title="Reset this preset to the built-in definition"
            :aria-pressed="form === 'preset'"
            @click="openForm('preset')"
          >
            Preset defaults…
          </button>
          <button class="btn secondary" :aria-pressed="form === 'import'" @click="openForm('import')">Import / export…</button>
        </div>
        <span class="spacer"></span>
        <button class="btn" :disabled="dirty || onAir" :title="dirty ? 'Save the layout first' : ''" @click="activate">Activate</button>
      </div>

      <form v-if="form === 'new'" class="inline-form" @submit.prevent="submitNew">
        <label for="form-name">New layout name</label>
        <input id="form-name" ref="nameEl" v-model="formName" maxlength="64" autocomplete="off" />
        <label class="check"><input v-model="formCopy" type="checkbox" /> copy the tiles of {{ editor.current }}</label>
        <button class="btn" type="submit">Create</button>
        <button class="btn secondary" type="button" @click="form = ''">Cancel</button>
      </form>
      <form v-if="form === 'saveas'" class="inline-form" @submit.prevent="submitSaveAs">
        <label for="form-name">Save as</label>
        <input id="form-name" ref="nameEl" v-model="formName" maxlength="64" autocomplete="off" />
        <button class="btn" type="submit">Save</button>
        <button class="btn secondary" type="button" @click="form = ''">Cancel</button>
        <span class="note">An existing layout with that name is replaced.</span>
      </form>
      <div v-if="form === 'preset'" class="inline-form">
        <span>
          Replace every tile of <strong>{{ editor.current }}</strong> with the built-in preset (audio bars on, default captions)? Your changes to it are
          lost when you save.
        </span>
        <button class="btn" @click="applyPreset">Apply preset defaults</button>
        <button class="btn secondary" @click="form = ''">Cancel</button>
      </div>
      <div v-if="form === 'delete'" class="inline-form">
        <span>Delete the layout <strong>{{ editor.current }}</strong>? This cannot be undone.</span>
        <button class="btn danger" @click="confirmDelete">Delete</button>
        <button class="btn secondary" @click="form = ''">Cancel</button>
      </div>
      <div v-if="form === 'import'" class="inline-form" style="display: block">
        <p class="note">Paste or open a JSON file: one layout opens in the editor (save it to keep it), a layout book (<code>{"layouts": [...]}</code>) replaces every layout.</p>
        <input type="file" accept="application/json,.json" aria-label="Open a JSON file" @change="onImportFile" />
        <textarea v-model="importText" aria-label="Layout JSON" placeholder='{"name": "my wall", "tiles": [...]}'></textarea>
        <div class="actions">
          <button class="btn secondary" @click="exportLayout">Export this layout</button>
          <button class="btn secondary" @click="exportBook">Export all layouts</button>
          <span class="spacer"></span>
          <button class="btn secondary" @click="form = ''">Close</button>
          <button class="btn" :disabled="!importText.trim()" @click="submitImport">Import</button>
        </div>
      </div>
      <div v-if="msg.text" class="msg" :class="msg.kind">{{ msg.text }}</div>
    </div>

    <div class="editor" style="margin-top: 0.9rem">
      <div>
        <div class="panel">
          <div class="toolbar">
            <div class="group">
              <span class="small muted">Add tile</span>
              <button class="btn small secondary" @click="addTile('input')">+ Input</button>
              <button class="btn small secondary" @click="addTile('clock')">+ Clock</button>
              <button class="btn small secondary" @click="addTile('label')">+ Label</button>
              <button class="btn small secondary" @click="addTile('empty')">+ Empty</button>
            </div>
            <span class="sep"></span>
            <div class="group">
              <span class="small muted">Selected tile</span>
              <button class="btn small secondary" :disabled="!selected" @click="duplicateTile">Duplicate</button>
              <button class="btn small secondary" :disabled="!selected" title="Draw over the tile above" @click="moveLayer(1)">Forward</button>
              <button class="btn small secondary" :disabled="!selected" title="Draw under the tile below" @click="moveLayer(-1)">Backward</button>
              <button class="btn small danger" :disabled="!selected" @click="deleteTile">Delete tile</button>
            </div>
            <span class="spacer"></span>
            <span class="small muted">{{ layout.tiles.length }} tiles · grid {{ grid }} × {{ grid }}</span>
          </div>
          <div
            ref="canvasEl"
            class="canvas"
            tabindex="0"
            aria-label="Layout canvas: arrow keys move the selected tile, Shift + arrows resize it"
            :style="{ background: layout.background, marginTop: '.7rem' }"
            @pointerdown="onCanvasDown"
            @keydown="onKey"
          >
            <div class="gridlines" :style="gridStyle"></div>
            <TileView
              v-for="tile in ordered"
              :key="tile.id"
              :tile="tile"
              :selected="tile.id === editor.selectedId"
              :now="now"
              @grab="track($event, tile, false)"
              @resize="track($event, tile, true)"
            />
            <div v-if="!layout.tiles.length" class="empty-state" style="position: absolute; inset: 0; display: flex; align-items: center; justify-content: center">
              No tiles yet. Add one with the buttons above.
            </div>
          </div>
        </div>
      </div>
      <div class="inspector">
        <TileInspector v-if="selected" :tile="selected" :layout="layout" :grid="grid" :max-inputs="maxInputs" :layer="layer" />
        <div v-else class="panel">
          <h3>Tile</h3>
          <p class="note">Click a tile on the canvas to change what it shows, its caption, audio bars and overlays.</p>
        </div>
        <div class="panel">
          <h3>Layout {{ editor.current }}</h3>
          <Field label="Background colour" id="layout-bg" help="Shown where no tile covers the output (MV_BACKGROUND_FILE, when set, covers it).">
            <div class="colorrow">
              <input id="layout-bg" v-model="layout.background" type="color" />
              <code>{{ layout.background }}</code>
            </div>
          </Field>
          <Field label="Text tally" help="Default of every tile that does not set its own. To use it on one output only, give that output its own layout.">
            <label class="check"><input v-model="layout.tally_text" type="checkbox" /> Text tally as caption background</label>
          </Field>
          <Field label="Start layout" help="Kept in the configuration volume: after a restart the output shows this layout, whatever was on air before.">
            <div v-for="o in live.outputs" :key="o.index">
              <label class="check">
                <input
                  type="checkbox"
                  :checked="o.start_layout === editor.current"
                  :disabled="isNew || !!o.start_layout_env"
                  :title="isNew ? 'Save the layout first' : ''"
                  @change="setStart(o.index, $event.target.checked)"
                />
                Use as start layout{{ live.outputs.length > 1 ? ` of output ${o.index}` : "" }}
              </label>
              <div v-if="startNote(o)" class="note">{{ startNote(o) }}</div>
            </div>
          </Field>
        </div>
      </div>
    </div>
  </template>
</template>
