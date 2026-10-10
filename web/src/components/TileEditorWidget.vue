<script setup>
// Widget "tile-editor" (§8.5): the tiles of the layout one head shows and the inspector of the selected
// tile, with Save and Discard. It uses the layout editor's state and components (editor.js, TileView,
// TileInspector); tiles are picked on the canvas and moved or sized in the inspector.
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import { live } from "../api.js";
import { byZ, discard, draftOf, editor, isDirty, loadBook, saveAs } from "../editor.js";
import Pill from "./Pill.vue";
import TileInspector from "./TileInspector.vue";
import TileView from "./TileView.vue";

const props = defineProps({ head: { type: Number, required: true } });
const emit = defineEmits(["shown"]);

const msg = ref({ kind: "", text: "" });
const now = ref(Date.now());
let clock = 0;

// The layout on air on this head; when the head switches, the new one opens (unsaved edits stay drafts).
const name = computed(() => live.outputs.find((o) => o.index === props.head)?.layout || "");
const layout = computed(() => (name.value ? editor.drafts[name.value] : null));
const ordered = computed(() => (layout.value ? byZ(layout.value) : []));
const selected = computed(() => layout.value?.tiles.find((t) => t.id === editor.selectedId) || null);
const dirty = computed(() => isDirty(name.value));
const grid = computed(() => live.info?.grid || 24);
const maxInputs = computed(() => live.info?.max_inputs || 16);
const layer = computed(() => (selected.value ? `layer ${ordered.value.indexOf(selected.value) + 1} of ${ordered.value.length}` : ""));

async function open(current) {
  if (!current) return;
  try {
    if (editor.saved[current] === undefined) await loadBook(current);
    if (editor.current !== current) editor.selectedId = "";
    editor.current = current;
    draftOf(current);
    emit("shown");
  } catch (e) {
    msg.value = { kind: "err", text: e.message };
  }
}
watch(name, open, { immediate: true });

async function save() {
  try {
    editor.current = name.value;
    await saveAs(name.value);
    msg.value = { kind: "ok", text: `Saved ${name.value}: it is on air at the next frame.` };
  } catch (e) {
    msg.value = { kind: "err", text: e.message };
  }
}

function revert() {
  discard(name.value);
  msg.value = { kind: "ok", text: `Changes to ${name.value} discarded.` };
}

onMounted(() => (clock = setInterval(() => (now.value = Date.now()), 1000)));
onUnmounted(() => clearInterval(clock));
</script>

<template>
  <div class="widget-editor">
    <div class="toolbar">
      <strong>Output {{ head }}</strong>
      <span class="muted small">{{ name || "…" }}</span>
      <Pill v-if="layout" :text="dirty ? 'unsaved changes' : 'saved'" :kind="dirty ? 'warn' : 'ok'" />
      <span class="spacer"></span>
      <button class="btn small secondary" :disabled="!dirty" @click="revert">Discard</button>
      <button class="btn small" :disabled="!dirty" @click="save">Save</button>
    </div>
    <div v-if="msg.text" class="msg" :class="msg.kind">{{ msg.text }}</div>
    <div v-if="layout" class="widget-editor-body">
      <div class="canvas" :style="{ background: layout.background }" aria-label="Tiles of the layout: click one to edit it">
        <TileView
          v-for="tile in ordered"
          :key="tile.id"
          :tile="tile"
          :selected="tile.id === editor.selectedId"
          :now="now"
          @grab="editor.selectedId = tile.id"
          @resize="editor.selectedId = tile.id"
        />
      </div>
      <div class="inspector">
        <TileInspector v-if="selected" :tile="selected" :layout="layout" :grid="grid" :max-inputs="maxInputs" :layer="layer" />
        <p v-else class="note">Click a tile to change what it shows, its caption, audio bars and overlays.</p>
      </div>
    </div>
    <div v-else class="widget-empty">Waiting for output {{ head }}…</div>
  </div>
</template>
