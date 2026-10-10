<script setup>
// Operator-screen widgets (SPECIFICATION.md §8.5): /widget/head?head=<h> and /widget/tile-editor?head=<h>
// [&theme=dark|light|transparent], without the app around them, on this multiviewer's own API. The page
// posts {type: "widget-ready"} at its first picture and {type: "widget-size", w, h} then and on every resize.
import { onMounted, onUnmounted, ref } from "vue";
import { live, startLive, stopLive } from "../api.js";
import HeadView from "./HeadView.vue";
import TileEditorWidget from "./TileEditorWidget.vue";

const params = new URLSearchParams(location.search);
const kind = location.pathname.slice("/widget/".length);
const head = Number(params.get("head"));
const theme = params.get("theme");
if (theme) document.documentElement.dataset.theme = theme;
// A frame is see-through only when its colour scheme matches its parent's; without the meta it is the default.
if (theme === "transparent") document.querySelector('meta[name="color-scheme"]')?.remove();

const root = ref(null);
let ready = false;
let observer = null;

function post(message) {
  if (window.parent !== window) window.parent.postMessage(message, "*");
}
function postSize() {
  const rect = root.value.getBoundingClientRect();
  post({ type: "widget-size", w: Math.round(rect.width), h: Math.round(rect.height) });
}
function shown() {
  if (ready) return;
  ready = true;
  post({ type: "widget-ready" });
  postSize();
}

onMounted(() => {
  startLive();
  observer = new ResizeObserver(() => ready && postSize());
  observer.observe(root.value);
});
onUnmounted(() => {
  observer?.disconnect();
  stopLive();
});
</script>

<template>
  <div ref="root" class="widget-root">
    <div v-if="live.error" class="widget-empty">{{ live.error }}</div>
    <HeadView v-else-if="kind === 'head'" :head="head" fill @shown="shown" />
    <TileEditorWidget v-else-if="kind === 'tile-editor'" :head="head" @shown="shown" />
  </div>
</template>
