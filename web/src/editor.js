// Layout editor state (§6). It lives outside the tab component, so switching tabs or a
// lost WebSocket never drops an unsaved edit; only Save, Discard, Delete and Import change it.
import { reactive } from "vue";
import { api, clone } from "./api.js";

// Built-in presets: recreated at start when missing, so the editor does not delete them.
export const PRESETS = ["1", "2x2", "3x3", "4x4", "5x5", "2+8", "1+5", "1+7", "2+6"];

export const editor = reactive({
  book: null, // { active, layouts } as last loaded or saved
  saved: {}, // name -> JSON text of the saved layout
  drafts: {}, // name -> layout opened in the editor
  current: "",
  selectedId: "",
});

export function isDirty(name) {
  const draft = editor.drafts[name];
  return draft !== undefined && JSON.stringify(draft) !== editor.saved[name];
}

export function draftOf(name) {
  if (!editor.drafts[name] && editor.saved[name] !== undefined) editor.drafts[name] = JSON.parse(editor.saved[name]);
  return editor.drafts[name];
}

/** Loads the book; drafts with unsaved edits stay. */
export async function loadBook(preferred) {
  const book = await api.get("/api/v1/layouts");
  editor.book = book;
  editor.saved = {};
  for (const layout of book.layouts) editor.saved[layout.name] = JSON.stringify(layout);
  for (const name of Object.keys(editor.drafts)) {
    if (!isDirty(name)) delete editor.drafts[name];
  }
  const names = book.layouts.map((l) => l.name);
  if (!editor.current || (!names.includes(editor.current) && !editor.drafts[editor.current])) {
    editor.current = names.includes(preferred) ? preferred : book.active;
  }
  draftOf(editor.current);
}

/** The saved names plus new layouts that only exist as drafts. */
export function layoutNames() {
  const names = (editor.book?.layouts || []).map((l) => l.name);
  for (const name of Object.keys(editor.drafts)) if (!names.includes(name)) names.push(name);
  return names;
}

function remember(layout) {
  editor.saved[layout.name] = JSON.stringify(layout);
  editor.drafts[layout.name] = clone(layout);
  const list = editor.book.layouts;
  const at = list.findIndex((l) => l.name === layout.name);
  if (at >= 0) list[at] = clone(layout);
  else list.push(clone(layout));
}

export async function saveAs(name) {
  const draft = { ...clone(editor.drafts[editor.current]), name };
  const stored = await api.put(`/api/v1/layouts/${encodeURIComponent(name)}`, draft);
  // The edits moved to the new name; the old layout keeps its saved version.
  if (name !== editor.current) delete editor.drafts[editor.current];
  remember(stored);
  editor.current = name;
}

export async function removeLayout(name) {
  await api.del(`/api/v1/layouts/${encodeURIComponent(name)}`);
  delete editor.drafts[name];
  delete editor.saved[name];
  editor.book.layouts = editor.book.layouts.filter((l) => l.name !== name);
  editor.current = editor.book.active;
  editor.selectedId = "";
  draftOf(editor.current);
}

export function discard(name) {
  delete editor.drafts[name];
  if (editor.saved[name] === undefined) editor.current = editor.book.active;
  editor.selectedId = "";
  draftOf(editor.current);
}

export function newLayout(name, copyCurrent) {
  const base = copyCurrent ? clone(editor.drafts[editor.current]) : { version: 1, background: "#101010", tiles: [] };
  editor.drafts[name] = { ...base, version: 1, name };
  editor.current = name;
  editor.selectedId = "";
}

// ---- tiles -------------------------------------------------------------------

/** A tile with every field the server stores, set to its defaults (§6.2). */
export function makeTile(content, id, rect, z) {
  const input = content === "input";
  return {
    id,
    content,
    input: 1,
    z,
    rect,
    scale: "fit",
    umd: input,
    umd_source: "is04",
    umd_text: "",
    umd_position: "bottom-inside",
    umd_font: 28,
    umd_bg: "#000000c0",
    tally_border: input,
    tally_lamp: input,
    tally_text: null,
    audio_bars: input,
    audio_bar_rms: false,
    audio_bar_channels: 2,
    audio_bar_first: 0,
    audio_bar_position: "right",
    zone_green: -18,
    zone_amber: -9,
    format_label: input,
    latency: false,
    safe_area: false,
    centre: false,
    aspect_markers: [],
    clock_style: "digital",
    clock_zone: "utc",
    timecode_rate: "",
    label_text: content === "label" ? "Label" : "",
  };
}

export function nextTileId(layout) {
  let n = 0;
  for (const tile of layout.tiles) {
    const m = /^t(\d+)$/.exec(tile.id);
    if (m) n = Math.max(n, Number(m[1]));
  }
  let id = `t${n + 1}`;
  while (layout.tiles.some((t) => t.id === id)) id = `t${++n + 1}`;
  return id;
}

function overlaps(a, b) {
  const e = 1e-6;
  return a.x < b.x + b.w - e && b.x < a.x + a.w - e && a.y < b.y + b.h - e && b.y < a.y + a.h - e;
}

/** The first free grid position for a w×h tile, scanning rows; top-left when the canvas is full. */
export function freeRect(layout, w, h, grid) {
  for (let row = 0; row + h * grid <= grid + 1e-9; ++row) {
    for (let col = 0; col + w * grid <= grid + 1e-9; ++col) {
      const rect = { x: col / grid, y: row / grid, w, h };
      if (!layout.tiles.some((t) => overlaps(rect, t.rect))) return rect;
    }
  }
  return { x: 0, y: 0, w, h };
}

/** Tiles in drawing order (ascending z, file order for equal z). */
export function byZ(layout) {
  return layout.tiles
    .map((tile, index) => ({ tile, index }))
    .sort((a, b) => a.tile.z - b.tile.z || a.index - b.index)
    .map((e) => e.tile);
}

/** Moves a tile one step up or down in the drawing order and renumbers z from 0. */
export function restack(layout, tile, step) {
  const order = byZ(layout);
  const at = order.indexOf(tile);
  const to = Math.max(0, Math.min(order.length - 1, at + step));
  order.splice(at, 1);
  order.splice(to, 0, tile);
  order.forEach((t, i) => (t.z = i));
}
