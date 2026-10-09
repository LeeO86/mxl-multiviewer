<script setup>
// Settings of the selected tile (§6.2), grouped by what they change on the wall.
import { computed } from "vue";
import { inputLabel, live } from "../api.js";
import Field from "./Field.vue";

const props = defineProps({
  tile: { type: Object, required: true },
  layout: { type: Object, required: true },
  grid: { type: Number, required: true },
  maxInputs: { type: Number, required: true },
  layer: { type: String, default: "" },
});

const CONTENTS = [
  { id: "input", label: "Input" },
  { id: "clock", label: "Clock" },
  { id: "label", label: "Label" },
  { id: "empty", label: "Empty" },
];
const RATES = [
  { value: "", label: "Off" },
  { value: "25", label: "25 fps" },
  { value: "50", label: "50 fps" },
  { value: "30000/1001", label: "29.97 fps" },
  { value: "60000/1001", label: "59.94 fps" },
  { value: "24", label: "24 fps" },
  { value: "30", label: "30 fps" },
  { value: "60", label: "60 fps" },
];
const MARKERS = ["16:9", "4:3", "1:1", "9:16"];
const ALIGNS = [
  { id: "left", label: "Left" },
  { id: "centre", label: "Centre" },
  { id: "right", label: "Right" },
];
const OVERLAYS = [
  { key: "tally_border", label: "Tally border" },
  { key: "tally_lamp", label: "Tally lamps" },
  { key: "format_label", label: "Format label" },
  { key: "latency", label: "Latency" },
  { key: "safe_area", label: "Safe areas (90 / 80 %)" },
  { key: "centre", label: "Centre cross" },
];

/** Changing the type sets the defaults of the new type (an input tile shows caption and bars). */
function setContent(kind) {
  const t = props.tile;
  if (t.content === kind) return;
  const input = kind === "input";
  t.content = kind;
  t.umd = input;
  t.audio_bars = input;
  t.tally_border = input;
  t.tally_lamp = input;
  t.format_label = input;
  if (kind === "label" && !t.label_text) t.label_text = "Label";
}

// Position and size in grid cells.
function cells(key) {
  return Math.round(props.tile.rect[key] * props.grid);
}
function setCells(key, value) {
  const g = props.grid;
  const r = props.tile.rect;
  let v = Math.round(Number(value));
  if (!Number.isFinite(v)) return;
  if (key === "w" || key === "h") v = Math.max(1, Math.min(v, Math.round((1 - (key === "w" ? r.x : r.y)) * g)));
  else v = Math.max(0, Math.min(v, Math.round((1 - (key === "x" ? r.w : r.h)) * g)));
  r[key] = v / g;
}

const hex2 = (n) => Math.max(0, Math.min(255, Math.round(n))).toString(16).padStart(2, "0");
const umdColor = computed({
  get: () => (props.tile.umd_bg || "#000000c0").slice(0, 7),
  set: (v) => (props.tile.umd_bg = v + hex2(umdAlpha.value * 2.55)),
});
const umdAlpha = computed({
  get: () => Math.round(parseInt((props.tile.umd_bg || "").slice(7, 9) || "ff", 16) / 2.55),
  set: (v) => (props.tile.umd_bg = umdColor.value + hex2(Number(v) * 2.55)),
});
// 16 channels per input are metered: first channel + channel count stay within 1-16.
const barChannels = computed({
  get: () => props.tile.audio_bar_channels,
  set: (v) => {
    const t = props.tile;
    t.audio_bar_channels = Math.max(1, Math.min(16, Math.round(Number(v) || 1)));
    t.audio_bar_first = Math.min(t.audio_bar_first, 16 - t.audio_bar_channels);
  },
});
const firstChannel = computed({
  get: () => props.tile.audio_bar_first + 1,
  set: (v) => (props.tile.audio_bar_first = Math.max(0, Math.min(16 - props.tile.audio_bar_channels, Math.round(Number(v) || 1) - 1))),
});

// tally_text: null follows the layout, true or false overrides it.
const tallyText = computed({
  get: () => props.tile.tally_text ?? null,
  set: (v) => (props.tile.tally_text = v),
});

const textLabel = computed(() => (props.tile.umd_source === "manual" ? "Caption text" : "Fallback text"));
const textHelp = computed(() => {
  if (props.tile.umd_source === "manual") return "Always shown as written.";
  if (props.tile.umd_source === "tsl") return "Shown until a tally controller sends a text for this input.";
  return "Shown when the NMOS registry has no name for the routed sender.";
});

const BAR_KEYS = ["audio_bars", "audio_bar_channels", "audio_bar_first", "audio_bar_position", "audio_bar_rms", "zone_green", "zone_amber"];
const inputTiles = computed(() => props.layout.tiles.filter((t) => t.content === "input"));

/** Gives every input tile of the layout this tile's audio bar settings. */
function barsToAll() {
  for (const t of inputTiles.value) {
    if (t !== props.tile) for (const key of BAR_KEYS) t[key] = props.tile[key];
  }
}

function toggleMarker(marker) {
  const list = props.tile.aspect_markers;
  const at = list.indexOf(marker);
  if (at >= 0) list.splice(at, 1);
  else list.push(marker);
}
</script>

<template>
  <div class="panel">
    <h3>Tile {{ tile.id }} <span class="spacer"></span><span class="small muted">{{ layer }}</span></h3>
    <Field label="Shows">
      <span class="seg" role="group" aria-label="Tile content">
        <button v-for="c in CONTENTS" :key="c.id" type="button" :aria-pressed="tile.content === c.id" @click="setContent(c.id)">{{ c.label }}</button>
      </span>
    </Field>
    <div class="four">
      <Field v-for="key in ['x', 'y', 'w', 'h']" :key="key" :label="{ x: 'Left', y: 'Top', w: 'Width', h: 'Height' }[key]" :id="`tile-${key}`">
        <input :id="`tile-${key}`" type="number" :value="cells(key)" min="0" :max="grid" @change="setCells(key, $event.target.value)" />
      </Field>
    </div>
    <p class="note">In grid cells of {{ grid }} × {{ grid }}. Drag the tile, or use the arrow keys (Shift + arrows resize).</p>
  </div>

  <template v-if="tile.content === 'input'">
    <div class="panel">
      <h3>Picture</h3>
      <div class="two">
        <Field label="Input" id="tile-input">
          <select id="tile-input" v-model.number="tile.input">
            <option v-for="n in maxInputs" :key="n" :value="n">{{ n }} · {{ inputLabel(n) }}</option>
          </select>
        </Field>
        <Field label="Scale" id="tile-scale">
          <select id="tile-scale" v-model="tile.scale">
            <option value="fit">Fit (whole picture)</option>
            <option value="fill">Fill (crop to tile)</option>
          </select>
        </Field>
      </div>
      <p class="note">A tile shows an input number. Which source is on that input is set by NMOS routing (IS-05), not here.</p>
    </div>

    <div class="panel">
      <h3>Caption (UMD) <span class="spacer"></span><label class="check"><input v-model="tile.umd" type="checkbox" /> Show</label></h3>
      <p class="note">The UMD (under-monitor display) is the caption bar with the source name, like the name strip under a monitor.</p>
      <Field label="Text from" id="tile-umd-source">
        <select id="tile-umd-source" v-model="tile.umd_source" :disabled="!tile.umd">
          <option value="is04">NMOS sender label (is04)</option>
          <option value="manual">Fixed text (manual)</option>
          <option value="tsl">Tally controller (TSL)</option>
        </select>
      </Field>
      <Field :label="textLabel" :help="textHelp" id="tile-umd-text">
        <input id="tile-umd-text" v-model="tile.umd_text" maxlength="200" :disabled="!tile.umd" placeholder="e.g. CAM 1" />
      </Field>
      <div class="two">
        <Field label="Position" id="tile-umd-pos">
          <select id="tile-umd-pos" v-model="tile.umd_position" :disabled="!tile.umd">
            <optgroup label="In the tile, over the picture">
              <option value="bottom-inside">Bottom, over the picture</option>
              <option value="top-inside">Top, over the picture</option>
            </optgroup>
            <optgroup label="Outside the tile">
              <option value="bottom-outside">Below the tile</option>
              <option value="top-outside">Above the tile</option>
            </optgroup>
          </select>
        </Field>
        <Field label="Font size (px at 1080p)" id="tile-umd-font">
          <input id="tile-umd-font" v-model.number="tile.umd_font" type="number" min="8" max="200" :disabled="!tile.umd" />
        </Field>
      </div>
      <p class="note">
        Over the picture: the bar covers the bottom or top of the picture, which keeps its full size. Below or above the tile: the bar is drawn on the
        area next to the tile, so keep that area free in the layout.
      </p>
      <Field label="Text alignment" help="Text longer than the bar is cut and ends with …">
        <span class="seg" role="group" aria-label="Caption text alignment">
          <button v-for="a in ALIGNS" :key="a.id" type="button" :aria-pressed="(tile.umd_align || 'left') === a.id" :disabled="!tile.umd" @click="tile.umd_align = a.id">
            {{ a.label }}
          </button>
        </span>
      </Field>
      <Field label="Background colour and opacity">
        <div class="colorrow">
          <input v-model="umdColor" type="color" aria-label="Caption background colour" :disabled="!tile.umd" />
          <input v-model.number="umdAlpha" type="range" min="0" max="100" aria-label="Caption background opacity" :disabled="!tile.umd" />
          <span class="small muted nowrap">{{ umdAlpha }} %</span>
        </div>
      </Field>
    </div>

    <div class="panel">
      <h3>Audio bars <span class="spacer"></span><label class="check"><input v-model="tile.audio_bars" type="checkbox" /> Show</label></h3>
      <div class="two">
        <Field label="Channels" id="tile-ch">
          <input id="tile-ch" v-model.number="barChannels" type="number" min="1" max="16" :disabled="!tile.audio_bars" />
        </Field>
        <Field label="First channel" id="tile-first">
          <input id="tile-first" v-model.number="firstChannel" type="number" min="1" :max="17 - tile.audio_bar_channels" :disabled="!tile.audio_bars" />
        </Field>
        <Field label="Position" id="tile-bars-pos">
          <select id="tile-bars-pos" v-model="tile.audio_bar_position" :disabled="!tile.audio_bars">
            <option value="right">Right edge</option>
            <option value="left">Left edge</option>
            <option value="overlay">Centre, over picture</option>
          </select>
        </Field>
        <Field label="RMS">
          <label class="check"><input v-model="tile.audio_bar_rms" type="checkbox" :disabled="!tile.audio_bars" /> Show RMS tick</label>
        </Field>
        <Field label="Amber from (dBFS)" id="tile-zone-a">
          <input id="tile-zone-a" v-model.number="tile.zone_green" type="number" min="-60" max="0" :disabled="!tile.audio_bars" />
        </Field>
        <Field label="Red from (dBFS)" id="tile-zone-r">
          <input id="tile-zone-r" v-model.number="tile.zone_amber" type="number" min="-60" max="0" :disabled="!tile.audio_bars" />
        </Field>
      </div>
      <p class="note">Peak meter with a scale from 0 to −60 dBFS, a 2 s peak hold and a clip light, for channels 1 to 16. Dim bars with a cross: no audio is routed to this input.</p>
      <div class="actions" style="margin-top: 0.4rem">
        <button class="btn small secondary" :disabled="inputTiles.length < 2" @click="barsToAll">Apply to all {{ inputTiles.length }} input tiles</button>
      </div>
    </div>

    <div class="panel">
      <h3>Tally and overlays</h3>
      <div class="checks">
        <label v-for="o in OVERLAYS" :key="o.key" class="check"><input v-model="tile[o.key]" type="checkbox" /> {{ o.label }}</label>
      </div>
      <Field
        label="Text tally as caption background"
        id="tile-tally-text"
        help="The left lamp shows the TSL left-hand tally, the right lamp the right-hand tally. This colours the caption with the text tally."
      >
        <select id="tile-tally-text" v-model="tallyText" :disabled="!tile.umd">
          <option :value="null">As the layout ({{ layout.tally_text ? "on" : "off" }})</option>
          <option :value="true">On</option>
          <option :value="false">Off</option>
        </select>
      </Field>
      <Field label="Aspect ratio markers">
        <div class="row tight">
          <label v-for="m in MARKERS" :key="m" class="check">
            <input type="checkbox" :checked="tile.aspect_markers.includes(m)" @change="toggleMarker(m)" /> {{ m }}
          </label>
        </div>
      </Field>
    </div>
  </template>

  <div v-else-if="tile.content === 'clock'" class="panel">
    <h3>Clock</h3>
    <Field label="Style">
      <span class="seg" role="group" aria-label="Clock style">
        <button type="button" :aria-pressed="tile.clock_style === 'analogue'" @click="tile.clock_style = 'analogue'">Analogue</button>
        <button type="button" :aria-pressed="tile.clock_style === 'digital'" @click="tile.clock_style = 'digital'">Digital</button>
      </span>
    </Field>
    <div class="two">
      <Field label="Time" id="tile-zone">
        <select id="tile-zone" v-model="tile.clock_zone">
          <option value="local">Local time</option>
          <option value="utc">UTC</option>
          <option value="tai">TAI (house time)</option>
        </select>
      </Field>
      <Field label="Timecode" id="tile-tc">
        <select id="tile-tc" v-model="tile.timecode_rate">
          <option v-for="r in RATES" :key="r.value" :value="r.value">{{ r.label }}</option>
        </select>
      </Field>
    </div>
    <p class="note">
      Local time is the multiviewer's time zone ({{ live.info?.timezone || "unknown" }}: MV_TIMEZONE, else TZ). Timecode is HH:MM:SS:FF counted from TAI at the
      chosen rate.
    </p>
  </div>

  <div v-else-if="tile.content === 'label'" class="panel">
    <h3>Label</h3>
    <Field label="Text" id="tile-label">
      <input id="tile-label" v-model="tile.label_text" maxlength="200" />
    </Field>
    <p class="note">The text is centred and sized to fit the tile.</p>
  </div>

  <div v-else class="panel">
    <h3>Empty</h3>
    <p class="note">An empty tile keeps an area free; the layout background shows there.</p>
  </div>
</template>
