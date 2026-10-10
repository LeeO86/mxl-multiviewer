<script setup>
// One head's preview (§8.4), by MV_PREVIEW_MODE: its region of the WebRTC mosaic (the page's one
// stream, cropped with object-view-box), or its JPEG at MV_PREVIEW_FPS. Emits `shown` at the first picture.
import { computed, nextTick, onUnmounted, ref, watch } from "vue";
import { live } from "../api.js";
import { headStyle, loadMap, preview, releasePreview, usePreview } from "../preview.js";

const props = defineProps({
  head: { type: Number, required: true },
  // Fill the parent (widget) instead of a 16:9 frame.
  fill: { type: Boolean, default: false },
});
const emit = defineEmits(["shown"]);

const mode = computed(() => live.info?.preview_mode);
const video = ref(null);
const stamp = ref(Date.now());
const failed = ref(false);
let timer = 0;
let using = false;

const fps = computed(() => live.info?.preview_fps || 5);
const src = computed(() => `/preview.jpg?head=${props.head}&t=${stamp.value}`);
const videoStyle = computed(() => headStyle(props.head));
const message = computed(() => {
  if (mode.value === "webrtc") {
    if (preview.state === "error") return `WebRTC preview unavailable (${preview.error}), retrying…`;
    return preview.state === "playing" ? "" : "Connecting to the WebRTC preview…";
  }
  return failed.value ? "No preview yet: the output has not produced a frame." : "";
});

function attach() {
  if (!video.value) return;
  video.value.srcObject = preview.stream;
  if (preview.stream) video.value.play().catch(() => {});
}

function start(current) {
  clearInterval(timer);
  if (current === "webrtc" && !using) {
    using = true;
    usePreview();
    nextTick(attach);
  } else if (current === "jpeg") {
    timer = setInterval(() => (stamp.value = Date.now()), 1000 / fps.value);
  }
}

watch(mode, start, { immediate: true });
watch(fps, () => mode.value === "jpeg" && start("jpeg"));
watch(() => preview.stream, attach);
// A head's raster decides its place in the mosaic.
watch(
  () => live.outputs.map((o) => o.format).join(),
  () => mode.value === "webrtc" && loadMap(),
);
watch(
  () => props.head,
  () => (failed.value = false),
);

onUnmounted(() => {
  clearInterval(timer);
  if (using) releasePreview();
});
</script>

<template>
  <div class="preview-frame" :class="{ fill }">
    <video
      v-if="mode === 'webrtc'"
      ref="video"
      :style="videoStyle"
      :aria-label="`Output ${head} preview`"
      muted
      autoplay
      playsinline
      @loadeddata="emit('shown')"
    ></video>
    <img
      v-else-if="mode === 'jpeg'"
      v-show="!failed"
      :src="src"
      :alt="`Output ${head} preview`"
      @load="
        failed = false;
        emit('shown');
      "
      @error="failed = true"
    />
    <div v-if="message" class="stage-empty">{{ message }}</div>
  </div>
</template>
