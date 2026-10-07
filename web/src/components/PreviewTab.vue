<script setup>
// Preview (§8.1): the JPEG of one output head at MV_PREVIEW_FPS, its layout and counters.
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import { api, live, shortId } from "../api.js";
import Pill from "./Pill.vue";

const head = ref(1);
const stamp = ref(Date.now());
const failed = ref(false);
const names = ref([]);
const chosen = ref("");
const msg = ref({ kind: "", text: "" });
// When the late counter last went up, to tell "late now" from "late once".
const lateAt = ref(0);
let timer = 0;

const heads = computed(() => live.info?.outputs || 1);
const fps = computed(() => live.info?.preview_fps || 5);
const output = computed(() => live.outputs.find((o) => o.index === head.value));
const src = computed(() => `/preview.jpg?head=${head.value}&t=${stamp.value}`);
const lateNow = computed(() => stamp.value - lateAt.value < 5000);

watch(
  () => output.value?.late,
  (late, before) => {
    if (before !== undefined && late > before) lateAt.value = Date.now();
  },
);
watch(
  () => output.value?.layout,
  (layout) => {
    if (layout) chosen.value = layout;
  },
  { immediate: true },
);
watch(head, () => {
  failed.value = false;
  chosen.value = output.value?.layout || "";
  msg.value = { kind: "", text: "" };
});
watch(fps, restart);

function restart() {
  clearInterval(timer);
  timer = setInterval(() => (stamp.value = Date.now()), 1000 / fps.value);
}

async function activate() {
  const name = chosen.value;
  try {
    // One head: activate in the book (kept across restarts). Several: switch this head only.
    if (heads.value > 1) await api.put(`/api/v1/outputs/${head.value}`, { layout: name });
    else await api.post(`/api/v1/layouts/${encodeURIComponent(name)}/activate`);
    msg.value = { kind: "ok", text: `Layout ${name} is on output ${head.value}.` };
  } catch (e) {
    msg.value = { kind: "err", text: e.message };
  }
}

onMounted(async () => {
  restart();
  try {
    names.value = (await api.get("/api/v1/layouts")).layouts.map((l) => l.name);
  } catch (e) {
    msg.value = { kind: "err", text: e.message };
  }
});
onUnmounted(() => clearInterval(timer));
</script>

<template>
  <div class="preview-grid">
    <div class="panel">
      <h3>
        Output {{ head }}
        <span class="spacer"></span>
        <span v-if="heads > 1" class="seg" role="group" aria-label="Output head">
          <button v-for="h in heads" :key="h" :aria-pressed="head === h" @click="head = h">Head {{ h }}</button>
        </span>
      </h3>
      <div class="preview-frame">
        <img v-show="!failed" :src="src" :alt="`Output ${head} preview`" @load="failed = false" @error="failed = true" />
        <div v-if="failed" class="stage-empty">No preview yet: the output has not produced a frame.</div>
      </div>
      <p class="note">Picture of the output at {{ fps }} frames per second (MV_PREVIEW_FPS, MV_PREVIEW_WIDTH). Audio is not previewed.</p>
    </div>
    <div>
      <div class="panel">
        <h3>Layout on air</h3>
        <div class="row tight">
          <select v-model="chosen" class="grow" aria-label="Layout">
            <option v-for="name in names" :key="name" :value="name">{{ name }}</option>
          </select>
          <button class="btn" :disabled="!chosen || chosen === output?.layout" @click="activate">Activate</button>
        </div>
        <p class="note">The layout switches at the next frame. The output flow and the routing stay as they are.</p>
        <div v-if="msg.text" class="msg" :class="msg.kind">{{ msg.text }}</div>
      </div>
      <div class="panel">
        <h3>Status</h3>
        <dl v-if="output" class="kv">
          <dt>Format</dt>
          <dd><Pill :text="output.format" kind="neutral" /></dd>
          <dt>Timing</dt>
          <dd><Pill :text="lateNow ? 'late frames' : 'on time'" :kind="lateNow ? 'warn' : 'ok'" /></dd>
          <dt>Frames</dt>
          <dd>{{ output.frames.toLocaleString() }}</dd>
          <dt>Late / missed</dt>
          <dd>{{ output.late }} / {{ output.missed }}</dd>
          <dt>Compose</dt>
          <dd>{{ output.compose_ms.toFixed(1) }} ms on {{ output.backend.toUpperCase() }}</dd>
          <dt>Audio follows</dt>
          <dd>{{ output.audio_follow ? `input ${output.audio_follow} (${output.audio_channels} channels)` : "off" }}</dd>
          <dt>Video flow</dt>
          <dd><code :title="output.video_flow_id">{{ shortId(output.video_flow_id) }}</code></dd>
          <dt>Audio flow</dt>
          <dd><code v-if="output.audio_flow_id" :title="output.audio_flow_id">{{ shortId(output.audio_flow_id) }}</code><span v-else class="muted">none</span></dd>
          <dt>Domain</dt>
          <dd><code :title="output.domain_id">{{ shortId(output.domain_id) }}</code></dd>
        </dl>
        <div v-else class="muted">Waiting for the output…</div>
      </div>
    </div>
  </div>
</template>
