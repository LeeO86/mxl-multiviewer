<script setup>
// Inputs (§8.1): per input the video and audio leg, source, format, live PPM levels and alarms.
import { computed, ref } from "vue";
import { ALARM_NAMES, live, meterPct, shortId, stateKind, stateText } from "../api.js";
import Pill from "./Pill.vue";

const onlyRouted = ref(false);
const SEVERITY = { no_signal: "bad", black: "bad", freeze: "bad", clip: "bad", silence: "warn", format_mismatch: "warn" };
const REASONS = { domain_not_found: "domain not found", flow_not_found: "flow not found" };

const rows = computed(() => live.inputs.filter((i) => !onlyRouted.value || i.video.enable || i.audio.enable));
const receivers = computed(() => Object.fromEntries((live.info?.receivers || []).map((r) => [r.input, r])));

function meters(input) {
  const out = [];
  for (let c = 0; c < Math.min(16, input.audio.channels || 0); ++c) {
    const db = input.ppm_dbfs[c];
    out.push({
      db,
      pct: meterPct(db),
      hold: meterPct(input.hold_dbfs?.[c]),
      kind: db >= -9 ? "hot" : db >= -18 ? "warm" : "",
      clip: input.clip?.[c],
    });
  }
  return out;
}

const alarmsOf = (input) => Object.keys(ALARM_NAMES).filter((name) => input.alarms?.[name]);
const db = (v) => (v > -100 ? v.toFixed(1) : "-inf");
</script>

<template>
  <div class="panel">
    <h3>
      Inputs
      <span class="spacer"></span>
      <label class="check"><input v-model="onlyRouted" type="checkbox" /> Only routed inputs</label>
    </h3>
    <p class="note">
      Routing is NMOS IS-05 only: a controller connects a sender to the video and the audio receiver of an input (they may come from different
      senders). The receiver ids are in the last column. The multiviewer has no source picker.
    </p>
    <table>
      <thead>
        <tr>
          <th class="num">#</th>
          <th>Source</th>
          <th>Video</th>
          <th>Format</th>
          <th>Audio</th>
          <th>Levels (PPM)</th>
          <th>Alarms</th>
          <th>Receivers video / audio</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="input in rows" :key="input.index" :class="{ dim: !input.video.enable && !input.audio.enable }">
          <td class="num">{{ input.index }}</td>
          <td>
            <div>{{ input.video.label || "–" }}</div>
            <div v-if="input.video.flow_id" class="small muted">flow <code :title="input.video.flow_id">{{ shortId(input.video.flow_id) }}</code></div>
          </td>
          <td>
            <Pill :text="stateText(input.video.state)" :kind="stateKind(input.video.state)" />
            <div v-if="input.video.reason" class="small muted">{{ REASONS[input.video.reason] || input.video.reason }}</div>
          </td>
          <td class="nowrap">{{ input.video.format || "–" }}</td>
          <td>
            <Pill :text="stateText(input.audio.state)" :kind="stateKind(input.audio.state)" />
            <div v-if="input.audio.channels" class="small muted">{{ input.audio.channels }} channels</div>
            <div v-else-if="input.audio.reason" class="small muted">{{ REASONS[input.audio.reason] || input.audio.reason }}</div>
          </td>
          <td>
            <div v-if="input.audio.state === 'running'" class="meters" role="img" :aria-label="`Input ${input.index} audio levels`">
              <div v-for="(m, c) in meters(input)" :key="c" class="ch" :class="{ clip: m.clip }" :title="`channel ${c + 1}: ${db(m.db)} dBFS`">
                <div class="fill" :class="m.kind" :style="{ height: m.pct + '%' }"></div>
                <div v-if="m.hold > 0" class="hold" :style="{ bottom: `calc(${m.hold}% - 2px)` }"></div>
              </div>
            </div>
            <span v-else class="muted">–</span>
          </td>
          <td>
            <Pill v-for="name in alarmsOf(input)" :key="name" :text="ALARM_NAMES[name]" :kind="SEVERITY[name]" />
            <span v-if="!alarmsOf(input).length" class="muted">–</span>
          </td>
          <td class="small nowrap">
            <code :title="receivers[input.index]?.video">{{ shortId(receivers[input.index]?.video) }}</code>
            /
            <code :title="receivers[input.index]?.audio">{{ shortId(receivers[input.index]?.audio) }}</code>
          </td>
        </tr>
      </tbody>
    </table>
    <div v-if="!rows.length" class="empty-state">No routed inputs.</div>
  </div>
</template>
