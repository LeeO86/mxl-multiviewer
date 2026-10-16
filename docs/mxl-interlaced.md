# MXL interlaced video: shared convention v1

Date: 2026-10-16. MXL pin: `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`.

This is the one rule for interlaced video that every LeeO86 MXL app follows, as writer or reader. MXL itself leaves the field phase open (§2), so apps that write and read the same flow agree only through this document. The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

## 1. What MXL does

The bare line numbers in §1 are in `lib/internal/src/FlowParser.cpp` at 218ddaa.

- `interlace_mode` in the flow definition is `progressive`, `interlaced_tff` or `interlaced_bff`. A missing key means `progressive` (274-284). Any other value is refused with "Invalid interlace_mode" (286-292); that includes `interlaced_psf`, the fourth NMOS value.
- An interlaced video flow must declare `grain_rate` 25/1 or 30000/1001 (294-301). Its `frame_height` must be even, for `video/v210` (337, 345) and `video/v210a` (351, 368).
- For an interlaced flow MXL doubles the rate: `_grainRate.numerator *= 2` (303-305), inside the video-only branch (263). The ring runs at the field rate, 50/1 or 60000/1001. Only video flows are doubled; a data or audio flow keeps the rate it declares.
- A grain is one field of `frame_height / 2` lines: the v210 line length times `height / 2` (338-341; for v210a fill plus key, 352-364). A 1920×1080 v210 grain is 5120 × 540 = 2 764 800 bytes.
- The grain has `height / 2` slices, one per field line (478-483).

## 2. What MXL does not define

- No per-grain field marker. A grain does not say whether it holds the top or the bottom field.
- No rule for field order in time, and no rule for which grain index carries the first field.
- No documentation of interlaced flows in `docs/`.
- Its own tools do not support interlaced video. `mxl-gst` sink and testsrc refuse a flow that is not progressive (`tools/mxl-gst/sink.cpp:933-935`, `testsrc.cpp:865-867`). `looping_filesrc` has a TODO, treats interlaced input as progressive (`tools/mxl-gst/looping_filesrc.cpp:356-359`) and always declares `progressive` (379); its unused non-progressive branch would write only `interlaced_tff` (567).

## 3. The convention

### 3.1 Grain index and field

- Let t be the frame's origin (capture) time in TAI, and k = `mxlTimestampToIndex(frame rate, t)` the TAI frame index at the declared frame rate (25/1 or 30000/1001).
- Grain 2k MUST hold the first field in time of frame k. Grain 2k+1 MUST hold the second field.
- This matches the timestamps: grain 2k at the field rate starts at the same TAI time as frame k.
- `interlaced_tff`: the first field is the top field, the even frame lines 0, 2, 4, … The second field holds the odd lines 1, 3, 5, …
- `interlaced_bff`: the first field is the bottom field, the odd frame lines. The second field holds the even lines.
- Field line i of grain `idx` is frame line `2 * i + p`, with p = `(idx & 1) ^ bff` (bff = 1 for `interlaced_bff`, 0 for `interlaced_tff`). A reader MUST weave with this parity.

### 3.2 Flow definition and NMOS

- The flow definition and the NMOS flow MUST declare `grain_rate` = the frame rate (25/1 or 30000/1001), not the field rate. MXL doubles it (§1).
- They MUST carry the real `interlace_mode` (`interlaced_tff` or `interlaced_bff`), the same in both.

### 3.3 Ancillary data

- The ANC flow (`video/smpte291` data flow) of an interlaced video runs at the field rate. It MUST be declared at twice the frame rate: 50/1 for 1080i50, 60000/1001 for 1080i59.94. MXL does not double a data flow (§1).
- ANC grain 2k MUST carry the packets of the first field, grain 2k+1 those of the second field. The index is the same as that of the video field.
- The MXL-side NMOS ANC flow, source and receiver caps MUST also declare the field rate; they follow the flow definition. Only the video flow keeps the frame rate (§3.2).
- ANC grains MUST carry the RFC 8331 F bits: 0b10 for grain 2k, 0b11 for grain 2k+1, 0b00 for a progressive video.
- A field without ANC packets gets an empty grain only when the other field of the same frame has packets. A frame without any ANC packets writes no ANC grain.

### 3.4 Writers

- A writer MUST put the first field on an even index.
- It MUST keep that after drops, late input and resyncs. When it skips a frame it skips both fields. It MUST NOT shift the index by one field.
- On a hold or repeat (freeze, missing input) it SHOULD repeat the last whole pair: the first field again on the even index, the second field on the odd index. It MUST NOT write one field into both slots.
- A writer that derives the index from a per-field capture time with `mxlTimestampToIndex(field rate, t)` is compliant only while each field's t lies within ± half a field (a quarter frame) of that field's nominal time, because MXL rounds to the nearest index. It SHOULD derive k at the frame rate instead.

### 3.5 Readers

- A reader cannot detect a violation. MXL has no field marker, and a field with the wrong phase has the right size. A wrong phase shows as a one-line shift and, on motion, combing and judder.
- A reader that builds whole frames at the frame rate MUST pair grains 2k and 2k+1. A reader that shows every field (bob, or a weave with the field before) MAY pair neighbours across frames, as long as each field's lines follow the parity rule.
- A reader that does not support interlaced flows MUST refuse them visibly (error state, NMOS caps). It MUST NOT read `frame_height` lines from a field grain.

### 3.6 SMPTE ST 2110-20 and ST 2110-40

- Field F=0 (RFC 4175 §4.2) is the first field. It maps to grain 2k, F=1 to grain 2k+1.
- Ingest MUST align by the F bit while the sender sets F (it has seen F=1 within the last 4 fields): an F=0 field goes to an even index, an F=1 field to an odd index, whatever the RTP timestamp says.
- A sender that never sets F=1 is not RFC 4175 / ST 2110-20 compliant. Ingest SHOULD then keep its timestamp index and log it, rather than lose fields.
- Note: MTL v26.09 itself drops a second field that reuses the first field's RTP timestamp, so such a sender loses every second field before ingest sees it.
- Egress MUST set F = `idx & 1`. Its output delay MUST be a whole number of frames (an even number of fields), so the RTP timestamp of an F=0 field stays on the frame grid.
- ST 2110-40 ANC uses the RFC 8331 F field. On ingest, a packet with F=0b10 goes to grain 2k and one with F=0b11 to grain 2k+1. Egress sets F from the index parity: 0b10 for an even index, 0b11 for an odd one.

### 3.7 Field order

- `interlaced_bff` is allowed in MXL and readers MUST handle it.
- ST 2110 and SDI 1080i receivers assume tff. A gateway SHOULD NOT send a 1080-line bff flow; it MAY refuse one.

## 4. Compliance

Status at 2026-10-16: the audit of each repo's `main` on that day, plus the fixes made after it. A version is the first one that complies.

| repo | role | status | notes |
| --- | --- | --- | --- |
| mxl-srt-gateway | writer (ingest), reader (egress) | compliant | Index from frame k, both fields per frame; a drop or resync skips both fields. Egress pairs 2k and 2k+1, tff and bff. |
| mxl-test-player, video | writer | compliant (1.2.2) | `(idx & 1) ^ bff` field extraction. |
| mxl-test-player, ANC | writer | compliant (1.2.3, PR #14, released) | ANC at the field rate (§3.3). |
| mxl-multiviewer | reader | compliant (1.4.4) | Weaves grain idx with idx − 1, `(idx & 1) ^ bff`. |
| mxl-webrtc-monitor | reader | compliant (1.4.1) | Pairs 2k and 2k+1, tff and bff. NMOS caps list `interlaced_bff` since 1.4.2 (PR #17, released). |
| mxl-color-corrector | reader and writer | compliant (1.2.3) | Output index = input index; a hold keeps the parity. |
| mxl-scopes | reader | compliant | Pairs 2k and 2k+1. |
| mxl-decklink, capture | writer | compliant (1.0.6, first release with #23; #24 adds 29.97/59.94 audio) | Field lines of frame k to 2k and 2k+1. Field order from a mode table (NTSC bff), not yet checked on real cards. |
| mxl-decklink, playout | reader | compliant (1.0.6, as above) | Reads 2k and 2k+1 per frame and weaves them, at the frame rate. |
| mxl-decklink, ANC | writer | compliant (1.0.6, as above) | ANC at the field rate (§3.3). |
| mxl-st2110-gateway, ingest | writer | compliant (1.0.19, PR #28, released) | Aligns parity by the F bit while the sender sets F=1; a sender that never sets F=1 keeps its timestamp index. MTL v26.09 itself drops a second field that reuses the first field's timestamp. ANC at the field rate. |
| mxl-st2110-gateway, egress | reader | compliant (1.0.19, as above) | Output delay a whole number of frames; a repeat sends the last pair (§3.4). |
| mxl-replay | reader and writer, progressive only | compliant (1.7.1, PR #27, released) | Refuses interlaced inputs visibly (progressive-only NMOS caps, error state). |
| obs-mxl-plugin, input | reader | compliant (input 0.0.2-alpha, PR #1) | Refuses interlaced flows. |
| mxl-browser-source | writer, progressive only | N/A | Config refuses interlaced formats. |
| strom | progressive only | compliant (refuses: caps negotiation fails) | An interlaced flow fails caps negotiation. |

Not audited: mxl-audio-mixer, mxl-fabrics-agent, mxl-nmos.

## 5. Test checklist

For a writer, with a 1920×1080 `interlaced_tff` flow at 25/1:

- The flow definition and NMOS say `grain_rate` 25/1 and `interlaced_tff`. The MXL ring runs at 50 grains per second.
- Each grain is 5120 × 540 bytes (`grainSize` 2 764 800) with 540 slices.
- Both parities are valid: indexes 2k and 2k+1 are both written, complete, and not flagged invalid.
- Grain 2k holds the even frame lines. Use a source with distinct even and odd lines (for example a line-number pattern) and compare grain 2k row i with frame line 2i.
- Repeat with `interlaced_bff`: grain 2k holds the odd lines.
- The ANC flow is declared at 50/1 (flow definition and NMOS), and each ANC grain has the same index as the video field it belongs to, with F bits 0b10 on even and 0b11 on odd indexes.
- After a forced drop or resync, the first field is still on an even index.
- On a hold or repeat, the repeated pair keeps its parity: the even index still holds the even frame lines (tff).
- ST 2110 egress: F = `idx & 1` on every field. ST 2110 ingest: with a sender that sets F, F=0 fields land on even indexes; with a sender stuck at F=0, ingest keeps the timestamp index, logs it, and loses no fields.

For a reader:

- Feed a tff and a bff flow with distinct even and odd lines. The woven frame matches the source line for line.
- Each field grain is read as 540 lines, never 1080.
- A reader that does not support interlaced flows shows a visible error and does not crash. If it has NMOS receivers, their caps list only `progressive`.

## 6. Open points

- ANC grain byte layout: mxl-test-player to be aligned. MXL `docs/Architecture.md:353` says only the bytes from the RFC 8331 Length field on are stored, and mxl-decklink does that. mxl-test-player departs from MXL: it adds a 2-byte extended sequence number before Length.
- mxl-decklink's field-order table (NTSC bff) is not yet checked on real cards.
