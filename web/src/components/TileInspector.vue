<script setup>
// Settings of the selected tile (§6.2), grouped by what they change on the wall.
import { computed, onMounted, ref, watch } from "vue";
import { api, inputLabel, live } from "../api.js";
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
  { id: "image", label: "Image" },
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
const ALARM_SPOTS = [
  { value: "top-left", label: "Top left" },
  { value: "top", label: "Top centre" },
  { value: "top-right", label: "Top right" },
  { value: "bottom-left", label: "Bottom left" },
  { value: "bottom", label: "Bottom centre" },
  { value: "bottom-right", label: "Bottom right" },
];
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

const BAR_KEYS = [
  "audio_bars",
  "audio_bar_channels",
  "audio_bar_first",
  "audio_bar_position",
  "audio_bar_overlay",
  "audio_bar_rms",
  "audio_bar_scale",
  "zone_green",
  "zone_amber",
];
const UMD_SIDES = [
  { id: "top", label: "Top" },
  { id: "bottom", label: "Bottom" },
];
const BAR_SIDES = [
  { id: "left", label: "Left" },
  { id: "centre", label: "Centre" },
  { id: "right", label: "Right" },
];
const inputTiles = computed(() => props.layout.tiles.filter((t) => t.content === "input"));

/** Gives every input tile of the layout this tile's audio bar settings. */
function barsToAll() {
  for (const t of inputTiles.value) {
    if (t !== props.tile) for (const key of BAR_KEYS) t[key] = props.tile[key];
  }
}

// ---- image tiles: a web address or a picture stored on the multiviewer ----
const pictures = ref([]);
const pictureMsg = ref({ kind: "", text: "" });
const imageMode = ref("");
const imageSource = computed(() => imageMode.value || (props.tile.image_file && !props.tile.image_url ? "file" : "url"));
watch(
  () => props.tile,
  () => (imageMode.value = ""),
);

/** Picks the web address or a stored picture; the other one is cleared (a tile shows one). */
function useSource(kind) {
  imageMode.value = kind;
  if (kind === "url") props.tile.image_file = "";
  else props.tile.image_url = "";
}

async function loadPictures() {
  try {
    pictures.value = (await api.get("/api/v1/images")).images || [];
  } catch (e) {
    pictureMsg.value = { kind: "err", text: e.message };
  }
}

async function upload(ev) {
  const file = ev.target.files[0];
  ev.target.value = "";
  if (!file) return;
  const name = file.name.replace(/[^A-Za-z0-9._-]/g, "_").replace(/^\.+/, "").slice(0, 100) || "picture";
  try {
    const info = await api.upload(`/api/v1/images/${encodeURIComponent(name)}`, file);
    props.tile.image_file = name;
    props.tile.image_url = "";
    pictureMsg.value = { kind: "ok", text: `Stored ${name}: ${info.width} × ${info.height}${info.frames > 1 ? `, ${info.frames} frames` : ""}.` };
    await loadPictures();
  } catch (e) {
    pictureMsg.value = { kind: "err", text: e.message };
  }
}

async function removePicture() {
  const name = props.tile.image_file;
  try {
    await api.del(`/api/v1/images/${encodeURIComponent(name)}`);
    props.tile.image_file = "";
    pictureMsg.value = { kind: "ok", text: `Deleted ${name}.` };
    await loadPictures();
  } catch (e) {
    pictureMsg.value = { kind: "err", text: e.message };
  }
}

const kib = (bytes) => `${Math.max(1, Math.round(bytes / 1024))} KiB`;

onMounted(() => {
  if (props.tile.content === "image") loadPictures();
});
watch(
  () => props.tile.content,
  (content) => content === "image" && loadPictures(),
);

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
        <Field label="Position">
          <span class="seg" role="group" aria-label="Caption position">
            <button v-for="p in UMD_SIDES" :key="p.id" type="button" :aria-pressed="tile.umd_position === p.id" :disabled="!tile.umd" @click="tile.umd_position = p.id">
              {{ p.label }}
            </button>
          </span>
          <label class="check"><input v-model="tile.umd_overlay" type="checkbox" :disabled="!tile.umd" /> Over the video</label>
        </Field>
        <Field label="Font size (px at 1080p)" id="tile-umd-font">
          <input id="tile-umd-font" v-model.number="tile.umd_font" type="number" min="8" max="200" :disabled="!tile.umd" />
        </Field>
      </div>
      <p class="note">
        The bar is always inside the tile. Over the video: it covers the top or bottom of the picture, which keeps its size. Off: the bar has its own strip
        of the tile and the picture is scaled into the rest.
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
        <Field label="Position">
          <span class="seg" role="group" aria-label="Audio bar position">
            <button
              v-for="p in BAR_SIDES"
              :key="p.id"
              type="button"
              :aria-pressed="tile.audio_bar_position === p.id"
              :disabled="!tile.audio_bars"
              @click="tile.audio_bar_position = p.id"
            >
              {{ p.label }}
            </button>
          </span>
          <label class="check" :title="tile.audio_bar_position === 'centre' ? 'Centred bars are always over the video' : ''">
            <input
              type="checkbox"
              :checked="tile.audio_bar_overlay || tile.audio_bar_position === 'centre'"
              :disabled="!tile.audio_bars || tile.audio_bar_position === 'centre'"
              @change="tile.audio_bar_overlay = $event.target.checked"
            />
            Over the video
          </label>
        </Field>
        <Field label="Show">
          <label class="check"><input v-model="tile.audio_bar_rms" type="checkbox" :disabled="!tile.audio_bars" /> RMS tick</label>
          <label class="check"><input v-model="tile.audio_bar_scale" type="checkbox" :disabled="!tile.audio_bars" /> Level scale (0 −6 −12 …)</label>
        </Field>
        <Field label="Amber from (dBFS)" id="tile-zone-a">
          <input id="tile-zone-a" v-model.number="tile.zone_green" type="number" min="-60" max="0" :disabled="!tile.audio_bars" />
        </Field>
        <Field label="Red from (dBFS)" id="tile-zone-r">
          <input id="tile-zone-r" v-model.number="tile.zone_amber" type="number" min="-60" max="0" :disabled="!tile.audio_bars" />
        </Field>
      </div>
      <p class="note">
        Peak meter with a scale from 0 to −60 dBFS, a 2 s peak hold and a clip light, for channels 1 to 16. Dim bars with a cross: no audio is routed to this
        input. Not over the video: the bars get a strip of the tile and the picture is scaled into the rest (with a caption strip too, into what is left).
      </p>
      <div class="actions" style="margin-top: 0.4rem">
        <button class="btn small secondary" :disabled="inputTiles.length < 2" @click="barsToAll">Apply to all {{ inputTiles.length }} input tiles</button>
      </div>
    </div>

    <div class="panel">
      <h3>Alarms</h3>
      <div class="checks">
        <label class="check"><input v-model="tile.alarm_border" type="checkbox" /> Alarm border</label>
        <label class="check"><input v-model="tile.alarm_labels" type="checkbox" /> Alarm labels</label>
      </div>
      <Field label="Labels at" id="tile-alarm-pos">
        <select id="tile-alarm-pos" v-model="tile.alarm_label_position" :disabled="!tile.alarm_labels">
          <option v-for="p in ALARM_SPOTS" :key="p.value" :value="p.value">{{ p.label }}</option>
        </select>
      </Field>
      <p class="note">
        One label per active alarm (no signal, black, freeze, clip, silence, format), stacked from that edge into the tile. The border is red or amber,
        inside the tally border. Turn both off to show no alarms on this tile; the Alarms tab still lists them.
      </p>
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

  <div v-else-if="tile.content === 'image'" class="panel">
    <h3>Image</h3>
    <Field label="Picture from">
      <span class="seg" role="group" aria-label="Picture source">
        <button type="button" :aria-pressed="imageSource === 'url'" @click="useSource('url')">Web address</button>
        <button type="button" :aria-pressed="imageSource === 'file'" @click="useSource('file')">Stored picture</button>
      </span>
    </Field>
    <Field v-if="imageSource === 'url'" label="Address (http or https)" id="tile-image-url">
      <input id="tile-image-url" v-model.trim="tile.image_url" type="url" maxlength="2048" placeholder="https://example.org/logo.png" />
    </Field>
    <template v-else>
      <Field label="Stored picture" id="tile-image-file">
        <select id="tile-image-file" v-model="tile.image_file">
          <option value="">(none)</option>
          <option v-for="p in pictures" :key="p.name" :value="p.name">{{ p.name }} · {{ kib(p.bytes) }}</option>
        </select>
      </Field>
      <div class="row tight">
        <input type="file" accept="image/png,image/jpeg,image/gif,image/webp" aria-label="Upload a picture" @change="upload" />
        <button class="btn small danger" :disabled="!tile.image_file" @click="removePicture">Delete picture</button>
      </div>
    </template>
    <Field label="Scale" id="tile-image-scale">
      <select id="tile-image-scale" v-model="tile.scale">
        <option value="fit">Fit (whole picture)</option>
        <option value="fill">Fill (crop to tile)</option>
      </select>
    </Field>
    <div v-if="pictureMsg.text" class="msg" :class="pictureMsg.kind">{{ pictureMsg.text }}</div>
    <p class="note">
      PNG, JPEG, GIF (animated GIFs play), or WebP; at most 8 MiB and 4096 × 4096 pixels. The multiviewer fetches a web address itself, through
      its proxy, within 10 s; while it loads the tile is empty, and when it fails the tile shows NO IMAGE with the reason (retried after 30 s).
      Uploaded pictures are kept in the configuration volume. Image tiles are drawn over the video tiles.
    </p>
  </div>

  <div v-else class="panel">
    <h3>Empty</h3>
    <p class="note">An empty tile keeps an area free; the layout background shows there.</p>
  </div>
</template>
