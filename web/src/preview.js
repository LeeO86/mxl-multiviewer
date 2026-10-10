// The WebRTC preview (SPECIFICATION.md §8.4): one WHEP connection per page and one MediaStream of
// the mosaic of every head. Each head view shows that same stream in a box of its head's aspect ratio
// with overflow hidden; a CSS transform from the tile map (/api/v1/preview/map) scales and moves the
// video so only the head's region is visible. That works in every browser (object-view-box is Chrome
// and Edge only).
import { reactive } from "vue";
import { api, live } from "./api.js";

const RETRY_MS = 2000;

export const preview = reactive({
  stream: null,
  state: "idle", // idle, connecting, playing, error
  error: "",
  map: null,
});

let users = 0;
let pc = null;
let retry = 0;

/** The WHEP URL as this page reaches it: a public URL as it is, else the own MediaMTX on the page's host. */
function whepUrl() {
  const p = live.info?.preview;
  if (!p?.whep) return "";
  if (p.public?.whep) return p.whep;
  try {
    const url = new URL(p.whep);
    url.hostname = location.hostname;
    return url.toString();
  } catch {
    return p.whep;
  }
}

function close() {
  if (pc) pc.close();
  pc = null;
  preview.stream = null;
}

function again(reason) {
  close();
  preview.state = "error";
  preview.error = reason || "";
  clearTimeout(retry);
  retry = setTimeout(connect, RETRY_MS);
}

async function connect() {
  clearTimeout(retry);
  const url = whepUrl();
  if (!url) {
    retry = setTimeout(connect, 1000);
    return;
  }
  close();
  preview.state = "connecting";
  const conn = new RTCPeerConnection();
  pc = conn;
  conn.addTransceiver("video", { direction: "recvonly" });
  conn.ontrack = (ev) => {
    if (pc === conn) preview.stream = ev.streams[0] || new MediaStream([ev.track]);
  };
  conn.onconnectionstatechange = () => {
    if (pc !== conn) return;
    if (conn.connectionState === "connected") {
      preview.state = "playing";
      preview.error = "";
    } else if (conn.connectionState === "failed" || conn.connectionState === "disconnected") {
      again(`WebRTC ${conn.connectionState}`);
    }
  };
  try {
    await conn.setLocalDescription(await conn.createOffer());
    // Send the offer with the candidates (no trickle): wait for gathering, at most 1 s.
    await new Promise((resolve) => {
      if (conn.iceGatheringState === "complete") return resolve();
      const timer = setTimeout(resolve, 1000);
      conn.addEventListener("icegatheringstatechange", () => {
        if (conn.iceGatheringState === "complete") {
          clearTimeout(timer);
          resolve();
        }
      });
    });
    if (pc !== conn) return;
    const response = await fetch(url, { method: "POST", headers: { "Content-Type": "application/sdp" }, body: conn.localDescription.sdp });
    if (!response.ok) throw new Error(`WHEP answered ${response.status}`);
    const answer = await response.text();
    if (pc !== conn) return;
    await conn.setRemoteDescription({ type: "answer", sdp: answer });
  } catch (e) {
    if (pc === conn) again(e.message);
  }
}

export async function loadMap() {
  try {
    preview.map = await api.get("/api/v1/preview/map");
  } catch {
    /* kept; the next format change loads it again */
  }
}

/** A head view starts using the stream; the first one connects. */
export function usePreview() {
  if (++users === 1) {
    loadMap();
    connect();
  }
}

/** A head view stops using the stream; the last one closes it. */
export function releasePreview() {
  if (--users > 0) return;
  users = 0;
  clearTimeout(retry);
  close();
  preview.state = "idle";
}

/**
 * How head `head` is shown: `box` for the element around its <video> (the region's aspect ratio, --ar) and
 * `video` for the <video> filling that box: the whole mosaic stretched to the box, moved so the region is at
 * the top left and scaled up so the region fills the box. Empty until the map is loaded (whole mosaic, 16:9).
 */
export function headCrop(head) {
  const map = preview.map;
  const r = map?.heads?.find((h) => h.head === head);
  if (!r) return { box: {}, video: {} };
  return {
    box: { "--ar": (r.w / r.h).toFixed(6) },
    video: {
      transform: `scale(${map.width / r.w}, ${map.height / r.h}) translate(${(-100 * r.x) / map.width}%, ${(-100 * r.y) / map.height}%)`,
    },
  };
}
