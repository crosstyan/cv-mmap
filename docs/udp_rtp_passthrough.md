# `udp_rtp` Backend And ABI v2.1 Encoded Passthrough

## Summary

The `udp_rtp` backend ingests an H.264 or H.265 RTP stream, parses it once, and publishes:

- the decoded left image plane as raw BGR
- the parsed encoded access unit as an optional ABI v2.1 plane

Both are written into the same `cvmmap://...` shared-memory snapshot. Consumers that only understand v1 or v2.0 should reject the stream; updated parsers accept the extended v2.1 layout.

## Backend behavior

`udp_rtp` is implemented with GStreamer and builds a pipeline equivalent to one of these graphs:

```text
udpsrc -> rtph264depay -> h264parse -> tee
  tee -> parsed encoded AU appsink
  tee -> decoder -> videoconvert -> BGR appsink

udpsrc -> rtph265depay -> h265parse -> tee
  tee -> parsed encoded AU appsink
  tee -> decoder -> videoconvert -> BGR appsink
```

Behavior:

- Input scope is RTP video only.
- Codec is selected by config: H.264 or H.265.
- The parser is configured so keyframes carry codec parameter sets in-band.
- The backend pairs encoded and raw samples by GStreamer PTS before publishing.
- Encoded access-unit callbacks are emitted before the matching raw-frame callback.
- Unmatched raw or encoded samples are dropped instead of publishing mixed snapshots.

## Config

Use `video.backend = "udp_rtp"` and add an `[udp_rtp]` section:

```toml
[video]
backend = "udp_rtp"

[udp_rtp]
address = "224.0.0.123"
port = 5602
payload_type = 96
auto_multicast = true
codec = "h265"
decoder = "auto"
```

`address` is optional. When it is omitted, `udpsrc` uses its default bind address (`0.0.0.0`).
Use a multicast group address here for multicast RTP, or a local interface address for unicast RTP.
The older `multicast_group` key is still accepted as a compatibility alias.

`codec` accepts:

- `h264`
- `h265`

`decoder` accepts:

- for `codec = "h264"`: `auto`, `nvh264dec`, `avdec_h264`
- for `codec = "h265"`: `auto`, `nvh265dec`, `avdec_h265`

## ABI v2.1 layout

The wire major version stays `2`. When the encoded plane is present, `versions_minor = 1`.

Plane slots are fixed:

- slot `0`: `LEFT`
- slot `1`: `DEPTH` optional
- slot `2`: `CONFIDENCE` optional
- slot `3`: `ENCODED_ACCESS_UNIT` optional

For v2.1:

- `plane_presence_mask` is sparse
- `plane_count` is `popcount(plane_presence_mask)`
- inactive descriptors must be empty

The encoded metadata lives in the v2 header `reserved_0[19]` bytes:

- `encoded_codec`
- `encoded_bitstream_format`
- `encoded_flags`
- `encoded_frame_rate_num`
- `encoded_frame_rate_den`
- `encoded_stream_pts_ns`

Current encoded-plane semantics:

- codec: `H264` or `H265`
- bitstream format: `AnnexB`
- flags: `KEYFRAME` when the AU is a keyframe
- plane payload: one access unit, already AU-aligned

The encoded descriptor itself is a byte payload descriptor, not an image plane:

- `pixel_format = GRAY`
- `depth = U8`
- `height = 1`
- `width = stride_bytes = size_bytes = encoded AU byte length`

Because H.264/H.265 access units vary in size, `payload_size_bytes` also varies by frame when the encoded plane is present.
It still records the exact payload byte count for the current snapshot.
The shared-memory mapping is allocated in binary capacity buckets so normal encoded-size variation does not force consumers to remap on every slightly larger access unit.

## Consumer expectations

Updated consumers can use the same snapshot in two ways:

- raw path: read the left plane and ignore the encoded plane
- passthrough path: read the encoded access unit and mux or forward it directly

`cvmmap-streamer` uses this to publish RTMP or write MCAP without re-encoding the video.

## Compatibility

- Existing v1 and v2.0 producers are unchanged.
- Existing unmodified consumers that only understand the old contiguous v2 mask semantics will reject `udp_rtp` snapshots.
- The parser intentionally fails closed on unknown plane types or invalid sparse masks.
