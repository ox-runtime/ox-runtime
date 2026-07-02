# ox — Streaming & Driver Architecture Guide

Decisions to make now that prevent major pain later, particularly when adding Vision Pro or other non-Android targets. Includes planned `ox_driver.h` API changes.

---

# Part 1: Streaming Protocol

## 1. Never hardcode Android assumptions into the protocol

The biggest one. The wire protocol should feel platform-neutral from day one.

**Good:**
```
codec          = H264 | HEVC | AV1
color_format   = NV12 | P010
tracking_space = local | stage
```

**Bad:**
```
quest_foveation_level
mediacodec_surface_mode
```

Put all platform quirks behind capability negotiation. The host-side driver should never need to know whether it's talking to a Quest or a Vision Pro.

---

## 2. Separate "stream session" from "XR runtime"

Do not let the networking layer know about OpenXR.

```
XR runtime
    ↓
frame submission API
    ↓
streaming core
    ↓
encoder → packetizer → UDP socket   (video)
                     → UDP socket   (tracking/input)
                     → TCP socket   (control plane)
```

Not: OpenXR runtime talking directly to sockets.

Vision Pro may not map cleanly to current runtime assumptions. The streaming core should be ignorant of both ends.

---

## 3. Make timestamps first-class citizens

Every major packet must carry timing metadata:

```
VideoFrame:
    frame_id
    capture_ts
    encode_start_ts
    encode_end_ts
    predicted_display_ts
    pose_id
```

Apple platforms are significantly more timing-sensitive than Android. Retrofitting timestamp infrastructure later is painful.

**Clock sync — clocks drift, one-time sync is not enough.** Crystal oscillators drift at 10–100 ppm — at the bad end that's 6ms per minute. A single handshake offset is stale within seconds at VR timescales.

Run a lightweight NTP-style sync over the TCP control plane every ~5 seconds:

```
Host sends:    { T1: host_ts }
Headset:       receives at T2, replies with { T1, T2, T3: headset_ts }
Host receives: at T4

offset = ((T2 - T1) + (T3 - T4)) / 2
```

Filter with EWMA to reject outliers. At 100 ppm drift, resyncing every 5 seconds keeps clock error well under 1ms. No extra UDP packets — runs entirely on the existing TCP control connection.

---

## 4. Treat tracking poses independently from frames

There are two distinct pose flows, opposite directions, different purposes — do not conflate them.

**Tracking stream (headset → host), high rate:**
Head pose, controller poses, and input state are sent from the headset to the host on their own UDP channel at display rate or faster (90Hz+). This stream is never gated on video cadence. If tracking only flows when a frame is submitted, the host is starved of pose data during encode and transmit latency — exactly when it needs it most. This is what feeds `update_view()` and `update_devices()` in the driver.

Never:
- Send tracking only once per frame
- Tie controller updates to video cadence

**Render pose (host → headset), embedded in each video packet:**
When the host renders a frame, it records the head pose it used for that specific eye. This pose is embedded in the video packet (`render_pose`). When the frame arrives on the headset, it is compared against the current live head pose to compute the ATW warp — "the world was rendered from *here*, my head is now *there*, rotate accordingly." This is not tracking data; it is a rendering artifact used purely for warp correction.

These two pieces of pose data serve completely different purposes and flow in opposite directions. The tracking stream is about feeding the host with fresh state. The render pose is about correcting for the latency that already happened.

---

## 5. Abstract encoders immediately

Even if only NVENC exists today.

```cpp
class VideoEncoder {
    void Encode(Frame frame, FrameMetadata metadata);
    EncoderCapabilities GetCapabilities();
};
```

The streaming core must not know about CUDA, D3D11 textures, Media Foundation, or VideoToolbox. Adding AMF, QuickSync, or VideoToolbox later should be a new implementation, not surgery on the core.

---

## 6. Don't assume RGB everywhere

Hardware encoders work in YUV. Internally support:
- NV12 (most common hardware encoder input)
- P010 (HDR / 10-bit)
- YUV420
- HDR metadata (eventually)

**The ox boundary:** The runtime already copies the submitted frame from the app (to unblock the app and prevent tearing). That copy is the natural place to convert to whatever format the driver declared it wants via `get_capabilities()`. The source is whatever the app submitted — RGBA8, BGRA8, etc. The runtime owns the conversion; the driver never sees the app's original format.

Apple in particular expects a proper color pipeline. Assuming sRGB RGBA everywhere will cause problems on Vision Pro.

---

## 7. Plan for hardware decoder weirdness

Different decoders behave wildly differently:

- Buffering behavior
- Max slice count per frame
- SPS/PPS handling expectations
- IDR requirements on connect/reconnect
- Timestamp ordering quirks
- Specific H.265 profile restrictions

Design for: capability negotiation, runtime probing, and reconnect-safe codec reset. See also: Quirk Flags section.

---

## 8. Separate "stream config" from "session config"

**Session** (established at connect, stable for session lifetime):
- Runtime info (name, version)
- Tracking spaces supported
- Device capabilities

**Stream** (can be renegotiated without reconnecting):
- Codec
- Bitrate
- Resolution
- Color format
- Foveation mode

This allows dynamic switching — H.265 ↔ AV1, bitrate adjustment, resolution scaling — without tearing down the session. Auth is deliberately excluded: ox is a local-network tool and premature auth design tends to produce bad auth.

---

## 9. Build a stats/telemetry pipeline early

You will need this for debugging, especially across platforms:

- Encode latency
- Network jitter
- Decode latency
- Pose-to-render latency (delta between tracking pose capture on headset and frame encode on host)
- Dropped frames (encoder-side and decoder-side separately)
- Motion-to-photon estimate (requires clock sync from section 3)

These should flow back to the host over the TCP control plane so the driver can expose them through the runtime. Debugging reprojection issues or Apple-specific timing problems without this data is effectively impossible.

---

## 10. Avoid OpenXR assumptions in the client protocol

Do not expose raw OpenXR concepts over the wire.

**Instead of:**
```
xrSpace, xrView, xrAction
```

**Use:**
```
head_pose, controller_pose, hand_skeleton, view_projection
```

The protocol should be XR-runtime-neutral. Vision Pro's tracking comes from ARKit, not OpenXR — if the protocol is OpenXR-native, the Vision Pro viewer has to fake OpenXR concepts that don't exist on that platform.

---

## 11. Build for asymmetric frame pacing

Quest tolerates:
- Unstable frame delivery
- Jitter spikes

Apple platforms expect:
- Smooth, consistent cadence
- Bounded latency
- Stable frame intervals

Design the delivery pipeline around consistent pacing and bounded latency, not just minimizing average latency. This matters for the packetizer and the decoder-side jitter buffer design.

---

## 12. Leave room for foveation

Don't implement it now, but don't design it out either.

Leave space in the packet format for:
- Eye gaze direction (headset → host, feeds the host-side render decision)
- Foveated region hints
- Dynamic bitrate regions
- Multi-resolution tiles

Foveation requires eye tracking data flowing from the viewer back to the host — design the tracking packet with room for it.

---

## 13. Use codec-agnostic frame metadata

```
Frame {
    codec           // H264 | HEVC | AV1
    frame_type      // IDR | P | B
    timestamp
    color_format
    alpha_mode
    payloads[]
}
```

Not H.264-specific NAL assumptions in the core. AV1 and future codecs should slot in without touching frame handling logic.

---

## 14. Keep platform implementations "cleanly hacky"

Platform-specific hacks will be necessary. That's fine. The rule is isolation:

- Codec quirks → target's encoder/decoder module
- Packet pacing quirks → target's transport module
- Quest-specific optimizations → Android viewer
- Vision Pro-specific behavior → visionOS viewer

Core protocol and streaming logic stay clean. Platform dirt stays in platform modules.

---

## Quirk Flags

Some decoder behaviors cannot be expressed as clean capability negotiation — they are bugs or undocumented constraints in specific hardware or firmware. Quirk flags handle these without polluting the core protocol.

**Design:**
- Declared by the viewer during the TCP handshake (the viewer knows its own quirks)
- Honored by the host driver when configuring the encoder
- Bit flags in a `uint64_t` — unknown flags are ignored by older drivers
- Never added to the core protocol; always a workaround for a specific known issue

```c
typedef uint64_t OxQuirkFlags;

// Bitstream format
#define OX_QUIRK_ANNEXB_REQUIRED        (1ULL << 0)  // need start codes, not AVCC length-prefixed
#define OX_QUIRK_SPS_PPS_INLINE         (1ULL << 1)  // SPS/PPS must be inline per-IDR, not out-of-band
#define OX_QUIRK_FORCE_IDR_ON_CONNECT   (1ULL << 2)  // request IDR immediately on session start

// Slice / NAL constraints
#define OX_QUIRK_SINGLE_SLICE_ONLY      (1ULL << 3)  // decoder breaks with >1 slice per frame
#define OX_QUIRK_NO_CABAC               (1ULL << 4)  // CABAC entropy coding unsupported, use CAVLC

// Timestamp handling
#define OX_QUIRK_MONOTONIC_DTS_REQUIRED (1ULL << 5)  // decoder requires strictly monotonic DTS

// Color format
#define OX_QUIRK_NO_P010                (1ULL << 6)  // P010 unsupported, force NV12 even for HDR

// Pacing
#define OX_QUIRK_STABLE_CADENCE         (1ULL << 7)  // decoder sensitive to jitter, enforce paced delivery
```

**In the handshake:**
```c
typedef struct {
    // ... codec list, resolution, refresh rate, etc.
    OxQuirkFlags quirk_flags;
} OxSessionCapabilities; // viewer → host, sent over TCP at connect
```

The host driver reads `quirk_flags` once at session start and adjusts encoder + packetizer configuration accordingly. No quirk logic touches the core streaming path.

---

# Part 2: ox_driver.h API Changes

Changes required to support the streaming driver cleanly without making simple drivers harder to write.

---

## Problems with the current API

**`submit_frame_pixels` has no `render_pose`.**
The streaming driver must embed the render pose in every video packet for ATW. Currently there's no way to know what pose the runtime used to render a given frame without storing it externally and guessing the correlation via `frame_time`. Making it explicit eliminates the guesswork.

**`format` is graphics-API-specific.**
The current `format` parameter accepts values like `GL_RGBA8` or `VK_FORMAT_R8G8B8A8_UNORM`. This leaks the graphics API into the driver interface. A driver author shouldn't need to know which graphics API the runtime is using.

**Flat parameter list doesn't extend cleanly.**
Adding any new per-frame metadata requires changing the callback signature and breaking all existing drivers.

**Driver has no way to declare format preference.**
The runtime always does a GPU readback to RGBA before calling `submit_frame_pixels`, even if the driver would prefer NV12 — which hardware encoders want natively and avoids a redundant color conversion.

---

## Changes

### 1. Add `OxColorFormat`

Platform-neutral replacement for the graphics-API-specific `format` parameter.

```c
typedef enum {
    OX_COLOR_FORMAT_RGBA8 = 0,  // default — always supported, easy to start
    OX_COLOR_FORMAT_NV12  = 1,  // hardware encoder native format
    OX_COLOR_FORMAT_P010  = 2,  // 10-bit / HDR
} OxColorFormat;
```

### 2. Add `OxDriverCapabilities`

Returned by the optional `get_capabilities` callback after `initialize()`. Lets the driver declare what format it wants the runtime to deliver. If the callback is NULL, the runtime defaults to `OX_COLOR_FORMAT_RGBA8`.

```c
typedef struct {
    OxColorFormat preferred_frame_format;
} OxDriverCapabilities;
```

The runtime already copies the submitted frame from the app (to unblock the app and prevent tearing). That copy step is where the conversion happens — from whatever the app submitted (RGBA8, BGRA8, etc.) into whatever format the driver declared. The driver never sees the app's original format.

### 3. Replace flat `submit_frame_pixels` params with `OxFrameInfo`

```c
typedef struct {
    XrTime        frame_time;    // predicted display time (unchanged)
    uint32_t      eye_index;     // 0 = left, 1 = right (unchanged)
    uint32_t      width;         // (unchanged)
    uint32_t      height;        // (unchanged)
    OxColorFormat color_format;  // replaces graphics-API-specific format
    XrPosef       render_pose;   // NEW: head pose used to render this eye
    const void*   pixel_data;    // (unchanged)
    uint32_t      data_size;     // (unchanged)
} OxFrameInfo;
```

`render_pose` is always populated by the runtime — it is the head pose returned from `update_view()` for this eye and frame. The streaming driver forwards it in the video packet so the headset can use it for ATW. Drivers that don't do streaming can ignore it.

### 4. Add `get_capabilities` and update `submit_frame_pixels` in `OxDriver`

```c
typedef struct OxDriver {
    // ... all existing callbacks unchanged ...

    // Optional. Called once after initialize() succeeds.
    // Driver declares its preferred pixel format here.
    // If NULL, runtime delivers OX_COLOR_FORMAT_RGBA8.
    void (*get_capabilities)(OxDriverCapabilities* out_caps);

    // Replaces the old submit_frame_pixels.
    // Signature changes from flat params to OxFrameInfo struct.
    // color_format matches what the driver declared in get_capabilities.
    // render_pose is always valid — the head pose used to render this eye.
    void (*submit_frame_pixels)(const OxFrameInfo* frame);

} OxDriver;
```

---

## What a simple driver looks like

No new concepts required. Ignore everything added:

```c
void my_submit_frame(const OxFrameInfo* frame) {
    // frame->pixel_data is RGBA8 by default
    // frame->render_pose is available but can be ignored
    display(frame->pixel_data, frame->width, frame->height);
}

// get_capabilities not implemented — runtime uses RGBA8 default
```

## What the streaming driver looks like

```c
void my_get_capabilities(OxDriverCapabilities* caps) {
    caps->preferred_frame_format = OX_COLOR_FORMAT_NV12;
    // runtime delivers NV12 — fed directly to NVENC, no conversion step
}

void my_submit_frame(const OxFrameInfo* frame) {
    OxVideoPacket pkt = {
        .frame_time  = frame->frame_time,
        .eye         = frame->eye_index,
        .render_pose = frame->render_pose,  // embedded in packet for headset ATW
    };
    encode_and_send(frame->pixel_data, frame->data_size, &pkt);
}
```

---

## Not in scope now

**`submit_frame_texture`** — GPU texture handle path for zero-copy encoding (runtime texture → NVENC, no CPU readback). Deferred. When added, it will be a separate optional callback declared via `get_capabilities`, not a replacement for `submit_frame_pixels`. NV12 via `submit_frame_pixels` is already a significant win and keeps the API understandable.
