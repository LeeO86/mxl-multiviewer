<script setup>
// A tile on the editor canvas, drawn the way the output shows it: input number and source,
// caption (UMD), audio bars with live levels, the clock, or the label text.
import { computed } from "vue";
import { live, localClock, meterPct, stateText } from "../api.js";

const props = defineProps({
  tile: { type: Object, required: true },
  selected: { type: Boolean, default: false },
  now: { type: Number, required: true },
});
defineEmits(["grab", "resize"]);

const input = computed(() => live.inputs.find((i) => i.index === props.tile.input));
const source = computed(() => input.value?.video?.label || `MV In ${props.tile.input}`);

// The caption the output draws (§4.3).
const caption = computed(() => {
  const t = props.tile;
  const name = `MV In ${t.input}`;
  if (t.umd_source === "manual") return t.umd_text || "(empty caption)";
  if (t.umd_source === "tsl") return input.value?.tsl_text || t.umd_text || name;
  return input.value?.video?.label || t.umd_text || name;
});

const channels = computed(() => {
  const t = props.tile;
  const ppm = input.value?.ppm_dbfs || [];
  const out = [];
  for (let c = 0; c < Math.min(16, t.audio_bar_channels); ++c) {
    const db = ppm[t.audio_bar_first + c] ?? -120;
    out.push({ pct: meterPct(db), color: db >= t.zone_amber ? "#dc2828" : db >= t.zone_green ? "#dca628" : "#28be46" });
  }
  return out;
});

function rgba(hex) {
  const m = /^#([0-9a-f]{6})([0-9a-f]{2})?$/i.exec(hex || "");
  if (!m) return "rgba(0,0,0,.75)";
  const n = parseInt(m[1], 16);
  const a = m[2] ? parseInt(m[2], 16) / 255 : 1;
  return `rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${a.toFixed(2)})`;
}

// Clock time in the tile's zone; TAI is UTC plus 37 s, local is the multiviewer's zone.
const clock = computed(() => {
  const t = props.tile;
  const at = new Date(props.now + (t.clock_zone === "tai" ? 37000 : 0));
  const { h, m, s } = t.clock_zone === "local" ? localClock(props.now) : { h: at.getUTCHours(), m: at.getUTCMinutes(), s: at.getUTCSeconds() };
  const pad = (v) => String(v).padStart(2, "0");
  return { text: `${pad(h)}:${pad(m)}:${pad(s)}`, h, m, s };
});

function hand(degrees, length) {
  const rad = ((degrees - 90) * Math.PI) / 180;
  return { x2: 50 + Math.cos(rad) * length, y2: 50 + Math.sin(rad) * length };
}
</script>

<template>
  <div
    class="tile"
    :class="[tile.content, { selected }]"
    :style="{
      left: tile.rect.x * 100 + '%',
      top: tile.rect.y * 100 + '%',
      width: tile.rect.w * 100 + '%',
      height: tile.rect.h * 100 + '%',
      zIndex: tile.z + 1,
    }"
    :title="`${tile.id} · ${tile.content}`"
    @pointerdown="$emit('grab', $event)"
  >
    <template v-if="tile.content === 'input'">
      <span class="tag">IN {{ tile.input }}</span>
      <div class="centre">
        <div class="bignum">{{ tile.input }}</div>
        <div class="source">{{ source }}</div>
        <div class="state">{{ stateText(input?.video?.state) }}</div>
      </div>
      <div v-if="tile.audio_bars" class="bars" :class="[tile.audio_bar_position, { off: !input?.audio?.enable }]">
        <span v-for="(ch, i) in channels" :key="i"><i :style="{ height: ch.pct + '%', background: ch.color }"></i></span>
      </div>
      <div v-if="tile.umd" class="umd" :class="tile.umd_position" :style="{ background: rgba(tile.umd_bg), textAlign: { centre: 'center', right: 'right' }[tile.umd_align] || 'left' }">
        {{ caption }}
      </div>
    </template>
    <div v-else-if="tile.content === 'clock'" class="centre">
      <svg v-if="tile.clock_style === 'analogue'" class="analogue" viewBox="0 0 100 100" aria-hidden="true">
        <circle cx="50" cy="50" r="46" fill="none" stroke="currentColor" stroke-width="3" opacity=".6" />
        <line x1="50" y1="50" v-bind="hand(((clock.h % 12) + clock.m / 60) * 30, 24)" stroke="currentColor" stroke-width="6" stroke-linecap="round" />
        <line x1="50" y1="50" v-bind="hand((clock.m + clock.s / 60) * 6, 36)" stroke="currentColor" stroke-width="4" stroke-linecap="round" />
        <line x1="50" y1="50" v-bind="hand(clock.s * 6, 40)" stroke="#ff4040" stroke-width="2" stroke-linecap="round" />
      </svg>
      <div v-else class="clockface">{{ clock.text }}</div>
      <div class="state">{{ tile.clock_zone.toUpperCase() }}{{ tile.timecode_rate ? ` · timecode ${tile.timecode_rate}` : "" }}</div>
    </div>
    <div v-else-if="tile.content === 'label'" class="centre">
      <div class="labeltext">{{ tile.label_text || "(no text)" }}</div>
    </div>
    <div v-else class="centre"><div class="state">Empty</div></div>
    <span v-if="selected" class="handle" title="Drag to resize" @pointerdown.stop="$emit('resize', $event)"></span>
  </div>
</template>
