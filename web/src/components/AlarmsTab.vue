<script setup>
// Alarms (§6.3, §8.1): the active alarms with their severity and since when.
import { computed, onMounted, onUnmounted, ref } from "vue";
import { ALARM_NAMES, inputLabel, live } from "../api.js";
import Pill from "./Pill.vue";

const now = ref(Date.now());
let timer = 0;

const ORDER = Object.keys(ALARM_NAMES);
const sorted = computed(() => [...live.alarms].sort((a, b) => a.input - b.input || ORDER.indexOf(a.name) - ORDER.indexOf(b.name)));

function since(ms) {
  if (!ms) return "–";
  const at = new Date(ms);
  const pad = (v) => String(v).padStart(2, "0");
  const ago = Math.max(0, Math.round((now.value - ms) / 1000));
  const rel = ago < 60 ? `${ago} s ago` : ago < 3600 ? `${Math.floor(ago / 60)} min ago` : `${Math.floor(ago / 3600)} h ago`;
  return `${pad(at.getHours())}:${pad(at.getMinutes())}:${pad(at.getSeconds())} (${rel})`;
}

onMounted(() => (timer = setInterval(() => (now.value = Date.now()), 1000)));
onUnmounted(() => clearInterval(timer));
</script>

<template>
  <div class="panel">
    <h3>Active alarms</h3>
    <div v-if="!sorted.length" class="empty-state">
      <Pill text="all clear" kind="ok" />
      <p>No active alarms.</p>
    </div>
    <table v-else>
      <thead>
        <tr>
          <th class="num">Input</th>
          <th>Source</th>
          <th>Alarm</th>
          <th>Severity</th>
          <th>Since</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="a in sorted" :key="`${a.input}-${a.name}`">
          <td class="num">{{ a.input }}</td>
          <td>{{ inputLabel(a.input) }}</td>
          <td>{{ ALARM_NAMES[a.name] || a.name }}</td>
          <td><Pill :text="a.severity === 'amber' ? 'warning' : 'critical'" :kind="a.severity === 'amber' ? 'warn' : 'bad'" /></td>
          <td class="nowrap">{{ since(a.since) }}</td>
        </tr>
      </tbody>
    </table>
  </div>
  <div class="panel">
    <h3>What the alarms mean</h3>
    <dl class="kv">
      <dt>No signal</dt>
      <dd>Video is routed, but no new frame came for MV_HOLD_MS, or the flow is missing.</dd>
      <dt>Black</dt>
      <dd>The picture is black (average luma at or below MV_BLACK_Y).</dd>
      <dt>Freeze</dt>
      <dd>The picture does not change from frame to frame.</dd>
      <dt>Silence</dt>
      <dd>Every audio channel is below MV_SILENCE_DBFS, or routed audio does not arrive.</dd>
      <dt>Clip</dt>
      <dd>An audio sample reached full scale (MV_CLIP_LINEAR).</dd>
      <dt>Format mismatch</dt>
      <dd>The routed video has a raster, rate or format the receivers do not support.</dd>
    </dl>
    <p class="note">
      Critical alarms draw a red border on the tile, warnings an amber one. An alarm is raised after MV_ALARM_DEBOUNCE_MS and cleared after
      MV_ALARM_CLEAR_MS.
    </p>
  </div>
</template>
