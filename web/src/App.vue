<script setup>
import { computed, onMounted, onUnmounted, ref } from "vue";
import { live, startLive, stopLive } from "./api.js";
import Pill from "./components/Pill.vue";
import PreviewTab from "./components/PreviewTab.vue";
import LayoutTab from "./components/LayoutTab.vue";
import InputsTab from "./components/InputsTab.vue";
import AlarmsTab from "./components/AlarmsTab.vue";
import SettingsTab from "./components/SettingsTab.vue";

const tabs = [
  { id: "preview", label: "Preview", component: PreviewTab, full: true },
  { id: "layout", label: "Layout", component: LayoutTab, full: true },
  { id: "inputs", label: "Inputs", component: InputsTab },
  { id: "alarms", label: "Alarms", component: AlarmsTab },
  { id: "settings", label: "Settings", component: SettingsTab },
];

const current = ref("preview");
const tab = computed(() => tabs.find((t) => t.id === current.value));

function onHash() {
  const id = location.hash.slice(1);
  if (tabs.some((t) => t.id === id)) current.value = id;
}
function go(id) {
  current.value = id;
  location.hash = id;
}

const head = computed(() => live.outputs[0]);
const routed = computed(() => live.inputs.filter((i) => i.video?.enable).length);

onMounted(() => {
  onHash();
  window.addEventListener("hashchange", onHash);
  startLive();
});
onUnmounted(() => {
  window.removeEventListener("hashchange", onHash);
  stopLive();
});
</script>

<template>
  <header>
    <h1>mxl-multiviewer</h1>
    <span v-if="live.info" class="node">
      {{ live.info.label }} · {{ head?.format || "–" }} · {{ routed }} of {{ live.info.max_inputs }} inputs routed
    </span>
    <span class="spacer"></span>
    <Pill v-if="head" :text="head.backend.toUpperCase()" kind="neutral" />
    <Pill :text="live.alarms.length ? `${live.alarms.length} alarm${live.alarms.length > 1 ? 's' : ''}` : 'no alarms'" :kind="live.alarms.length ? 'bad' : 'ok'" />
    <Pill :text="live.connected ? 'live' : 'offline'" :kind="live.connected ? 'ok' : 'bad'" />
    <span v-if="live.info" class="muted small">
      v{{ live.info.version }} · MXL {{ live.info.mxl_revision.slice(0, 7) }} · {{ live.info.outputs }} head{{ live.info.outputs > 1 ? "s" : "" }}
    </span>
  </header>
  <div v-if="live.error" class="banner bad">{{ live.error }}</div>
  <div v-else-if="live.everConnected && !live.connected" class="banner warn">Live updates lost. Reconnecting; values refresh every 2 s meanwhile.</div>
  <div v-if="live.restartKeys.length" class="banner warn">
    Restart required to apply {{ live.restartKeys.join(", ") }}.
    <button class="btn small secondary" @click="go('settings')">Settings</button>
  </div>
  <nav>
    <button
      v-for="t in tabs"
      :key="t.id"
      :class="{ active: current === t.id }"
      :aria-current="current === t.id ? 'page' : undefined"
      @click="go(t.id)"
    >
      {{ t.label }}<span v-if="t.id === 'alarms' && live.alarms.length" class="count">{{ live.alarms.length }}</span>
    </button>
  </nav>
  <main :class="{ full: tab.full }">
    <component :is="tab.component" />
  </main>
</template>
