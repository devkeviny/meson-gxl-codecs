# Testing notes

[Leia em português](TESTING.pt-BR.md)

All tests on an Aquario STV-2000 (S905W, P281), Armbian 26.11 / Debian 13,
kernel 6.12.114-ophub, ffmpeg 7.1.5 (Debian), 12 Nov 2026. Outputs were
validated with `ffprobe` (codec, resolution, frame count).

## What works

- **Module build + load**: `amvenc_avc.ko` builds against the ophub
  headers with gcc 14.2 (kernel was built with gcc 15.3.1 — no problem).
  `insmod` creates `/dev/amvenc_avc` and `/dev/videoN`; dmesg reports
  `dos [mem 0xc8820000-0xc882ffff] irq <n> canvas 4..15`.
- **Staged char-device test** (`venc-stage`, logs each stage to disk with
  fsync before running it): open, CONFIG_INIT (power-on + firmware load),
  GET_BUFFINFO + mmap, SEQUENCE (SPS/PPS, 21 bytes), IDR frame, 5 P-frames
  — all pass; output is valid H.264 720p (6/6 frames).
- **Load test**: 90 frames back-to-back (no sleeps), 90/90 frames out.
- **V4L2 encode via ffmpeg** (`-c:v h264_v4l2m2m -pix_fmt nv12`):
  - 720p30, testsrc2 input, SW decode: **61 fps (2.02x)**.
  - 1080p30, light source (`color`): **50 fps (1.64x)**.
  - 1080p30, testsrc2 input, SW decode: **28 fps (0.93x)** — the
    software H.264 decode is the bottleneck, not the encoder.
  - 720p30, SW decode + libx264 ultrafast (comparison): 34 fps.
  - 1080p30, SW decode + libx264 ultrafast (comparison): 9.5 fps.
- **V4L2 decode alone** (in-kernel meson-vdec, `h264_v4l2m2m`): 720p
  ~100 fps, 1080p **121 fps (4.02x)**.
- **Boot install**: module in
  `/lib/modules/<kver>/updates/`, `depmod`, `modules-load.d`, udev rule
  for `/dev/amvenc_avc` (video group, 0660). Verified across a reboot:
  the module loads by itself and both devices appear.

## Driver controls and presets (12 Nov 2026, 1080p testsrc2, 150 frames)

V4L2 encode, software-decoded feed. PSNR is measured against the input
file (itself libx264-ultrafast testsrc2), so the numbers compare the
variants against each other, not against pristine video.

| Setting (ffmpeg) | fps | Size | Actual bitrate | PSNR avg |
|---|---|---|---|---|
| defaults (4 Mbps, GOP 60) | FAILED (78 fr, 2 fps) | 1.3 MB partial | — | — |
| `-b:v 8M` | 4.4 (EIO timeouts) | 5.5 MB | 8.9 Mbps | 34.81 |
| `-b:v 2M` | 29 | 3.3 MB | 5.25 Mbps (!) | 32.74 |
| `-g 30` | 31 | 2.7 MB | 4.39 Mbps | 32.93 |
| `-g 120` | 30 | 2.1 MB | 3.44 Mbps | 33.48 |
| `-qmin 18 -qmax 30` | FAILED (17 fr) | 48 B | — | — |

Findings:

- **GOP 120 is the best setting**: smallest file, lowest bitrate, best
  PSNR of the ~4 Mbps group, full 30 fps. Fewer IDRs = more bits for
  P-frames.
- **8 Mbps gives the best quality (PSNR 34.81)** but ran at 4.4 fps
  (EIO timeouts on detailed content) and 8.9 Mbps is heavy for
  streaming.
- **The 2 Mbps target is NOT respected**: actual 5.25 Mbps. The
  driver's rate control cannot go below ~5 Mbps for this detailed
  content at QP 26/28 (QP floor). You cannot just dial the bitrate down
  for a limited upload.
- **Do not touch qmin/qmax**: `-qmin 18 -qmax 30` wedged the encoder
  from frame 0 (probably frames exceeding the 1 MB bitstream buffer).
- **The defaults run FAILED this time** (78 frames, EIO timeouts) —
  the encoder is flaky even on the previously reliable software-decoded
  feed. The EIO failures strike intermittently in every configuration.

## Why a run that worked before fails now (12 Nov 2026, ~17:52)

Same command, same input, same box, 5 back-to-back runs:

| Run | Result |
|---|---|
| 1 | 150/150, 28 fps, 3225 KiB |
| 2 | 150/150, 28 fps, 3225 KiB |
| 3 | 150/150, 28 fps, 3225 KiB |
| 4 | **FAILED at frame 17** (EIO, 0.4 fps) |
| 5 | 150/150, 28 fps, 3225 KiB |

Ruled out by measurement: temperature (54 °C, CPU at full 1.2 GHz, no
throttling), memory pressure (512 MB available, no OOM), CMA
fragmentation (198 MB free, 19 free 4 MB blocks), settings (the failing
run used the defaults).

**Conclusion: a per-run startup race.** Each ffmpeg run gets a full
HCodec reset + firmware reload (`avc_poweron` + `reload_mc`), so a
wedge never survives into the next run — but within a run, once a frame
times out, the HCodec never recovers. Every observed failure is in the
first ~20 frames (frames 12–23); no run has ever failed after frame 23.
The successful runs are bit-identical (same size, same bitrate), so the
encoder is deterministic when it survives startup. The likely trigger
is a lost mailbox/IRQ completion during the initial frame burst, before
the driver/firmware state is settled.

Practical consequence for a live service: ~20–25% of encoder starts
wedge in the first second. Mitigate with a watchdog that restarts
ffmpeg when no frame is produced for a few seconds (each restart is a
fresh coin flip; 2–3 retries give >99% success). A driver-level fix
(IRQ setup ordering, or reset-on-EIO in `avc_k_frame`) is real driver
debugging and is not done.

## Debugging the wedge (12 Nov 2026, ~18:00–18:40)

What the verbose driver log (`encode_print_level=2`) showed on failure:

```
avc encode light reset ---
frame type: IDR, size: 1920x1080
mb info: 0x802a0769, encode status: 0x5, dct status: 0x0
vlc status: 0x7, me status: 0x0, risc pc:0x911, debug:0x0
```

`encode status 0x5` is `ENCODER_MB_HEADER`: the firmware starts the
first IDR, gets into the macroblock-header phase, and wedges there
(RISC stuck at PC 0x911, VLC in state 7). The failing frame is the
IDR itself, not a P-frame after a reset.

Ruled out as the trigger:

- **Buffer addresses**: the encoder's 20 MB CMA block is always the
  same physical address (0x27900000), and ffmpeg's V4L2 input buffer
  is always the same address too (0x24500000) — in good runs and bad
  runs alike. The addresses do not determine the outcome.
- **Buffer contents**: a failed run does not poison the next process
  (a good run routinely follows a bad one with the same buffers).
- **Temperature / memory / CMA**: 54 °C, no throttling, 512 MB free,
  no OOM, CMA unfragmented (see above).

What does NOT recover a wedged instance (all measured):

- The vendor's **light reset** (`amvenc_avc_light_reset`: poweroff +
  firmware reload + IRQ re-request, on every timeout): 50 resets in a
  row never recovered a failed run.
- **Recreating the encoder instance in the driver** (destroy the wq
  after 2 consecutive errors, re-open fresh): 0 successes in 31
  recreates. The recreated instance's SPS/PPS succeeds but its first
  IDR wedges again. Only a fresh ffmpeg *process* gets a new coin flip.

The wedge is a startup race inside the closed firmware blob; its exact
internal trigger is not diagnosable from the driver source.

Driver changes made anyway (in this tree, live on the test box):

- Per-frame timeout 2000 ms → **1000 ms** (`a[8]` in `avc_k_frame`):
  a healthy frame takes ~20 ms, so 1 s is 50x margin and a stuck frame
  stalls a live half as long.
- On `avc_k_frame` error the V4L2 wrapper forces the next frame to be
  an IDR (`force_idr`: the reference state is gone after the light
  reset) and, after 2 consecutive errors, recreates the instance.
  The failed buffers are completed with `VB2_BUF_STATE_ERROR`, so the
  M2M job keeps running and ffmpeg sees dropped frames instead of a
  hang. (The recreate does not fix the wedge — see above — but the
  drop-and-continue behavior is what lets the watchdog work.)
- A `dev_dbg` line in `venc_work` logs each frame's input DMA
  addresses (enable with dynamic debug: `echo 'file venc_v4l2.c +p' >
  /sys/kernel/debug/dynamic_debug/control`).

## Live watchdog — PROVEN (12 Nov 2026, ~18:34–18:37)

`scripts/live-watchdog.sh` + `scripts/live-pipe.sh`: the HLS live
pipeline (HW decode → overlay → pipe → HW encode 1080p, GOP 120)
under a watchdog that restarts the pipeline when the encoder's
`-progress` file shows no new frame for 6 s. 150 s test, 3 min HLS
source, output to local FLV:

- Attempt 1: wedged at startup (frame stuck at 0) — detected in 6 s,
  restarted.
- Attempt 2: ran clean for the remaining 140 s: **4,053 frames at
  29.36 fps** (solid-colour content; detailed testsrc2 runs 22–25 fps).
- Box temperature after the test: 60 °C, CPU still at full 1.2 GHz
  (no throttling). The decode+overlay process runs hot (~285% CPU —
  the CPU copy + overlay is the bottleneck, not the codecs).

For a real live, set `LIVE_OUTPUT` to the RTMP URL (e.g.
`rtmp://a.rtmp.youtube.com/live2/KEY`) and `INPUT_URL` to the camera /
HLS source. A wedge then costs ~10 s of stream (6 s detection +
restart); YouTube treats a quick reconnect with the same stream key
as the same live event.

Software (libx264, CRF 23 default) for comparison:

| Preset | 1080p fps | 1080p size | 720p fps |
|---|---|---|---|
| ultrafast | 9.5 | — | 34 (transcode) |
| superfast | 13 | 6.3 MB (10.2 Mbps) | 28 |
| veryfast | 10 | 3.1 MB (5.1 Mbps) | — |

No software preset reaches 1080p30 on this box. At 720p, superfast
reaches 28 fps.

## What does not work

### HW decode + HW encode in one ffmpeg process: hangs or wedges

```
ffmpeg -c:v h264_v4l2m2m -i in1080.mp4 -c:v h264_v4l2m2m out.mp4
```

Two failure modes, both reproduced multiple times (12 Nov 2026):

1. **Pre-frame-1 hang.** The decoder thread sits in `poll()` forever and
   the encoder never opens (only `/dev/video1`, the decoder, is open).
   No dmesg output from either driver. CMA drains from ~180 MB free to
   ~15 MB while stalled. `timeout`/`SIGTERM` cannot kill the ffmpeg;
   `kill -9` is needed. After killing it, the decoder is wedged: a fresh
   decoder-only run hangs too, and reloading `meson_vdec` does not
   unwedge it — only a reboot restores the decoder. Feeding the input in
   realtime (`-re`) avoids this mode (frames start flowing), so it is a
   timing race: when the whole file + EOF reaches the decoder before it
   decodes frame 1, the pipeline stalls. A live (realtime) source does
   not hit it.
2. **Mid-stream encoder timeouts.** With `-re`, frames 0..~14 encode,
   then the encoder fails with `amvenc_avc: frame N: -5` (EIO, 2 s
   timeout per frame) and never recovers. The failure frame varies
   (12, 15, 16, 17, 23 across runs) and does not depend on the decoder's
   capture buffer count (8, 20, 32 all fail at the same point). Solid
   colour content at 1080p completes cleanly (149/150 frames); detailed
   content at 720p mostly completes (144/150, two EIO frames, then
   recovers). So the failure is content- and resolution-dependent.

What the logs established:

- The decoder setup is identical in the working (decode-only) and the
  hanging case (same device, formats, buffer counts — compared the
  `-v debug` logs line by line). The decoder receives the full input +
  EOF and produces zero frames in the hanging case.
- The in-kernel meson-vdec's `vdec_1_start` (mainline 6.12,
  `drivers/staging/media/meson/vdec/vdec_1.c:201`) does a full write
  `DOS_GCLK_EN0 = 0x3ff`, which clears the HCodec clock gates [26:12].
  The SM1 project fixed this in their modified vdec; this repo instead
  re-asserts the gates before every frame in `avc_k_frame()`
  (`WRITE_VREG_BITS(DOS_GCLK_EN0, 0x7fff, 12, 15)`). That fix is in the
  driver, but it did **not** resolve either failure mode.
- The decoder outputs 2-plane NM12 dmabufs; ffmpeg's v4l2m2m encoder
  negotiates single-plane NV12 for its OUTPUT queue (ffmpeg prefers
  single-plane for `AV_PIX_FMT_NV12`), and ffmpeg passes the decoder
  frames through without conversion (same AV pixel format on both
  sides). Restricting the encoder to multi-plane only (NV12M/NV21M)
  was tried and REVERTED: it broke the previously working
  software-decode feed (same -EIO timeouts from ~frame 10) and the box
  rebooted once during that regression test. The single-plane formats
  stay.

**Working rule (proven): the decoder's dmabuf frames must not reach the
encoder zero-copy. Any CPU copy in between fixes it:**

| Pipeline (1080p30, testsrc2) | Result |
|---|---|
| HW decode + SW encode (libx264) | hangs (same pre-frame-1 stall) |
| HW decode + overlay + null (no encoder) | works, 79 fps |
| HW decode -> `format=yuv420p` -> HW encode (1 process) | works, 150/150 frames, 27 fps |
| HW decode -> pipe -> HW encode (2 processes) | works (see HLS test below) |
| HW decode + overlay -> pipe -> HW encode (2 processes) | works, 150/150 frames, 25 fps |
| **HLS (M3U8) -> HW decode -> overlay -> pipe -> HW encode** | **works, 448/450 frames, 22 fps** |

The HLS test is the live pipeline: a local HLS stream (2 s segments
over HTTP) decoded in hardware, overlaid on CPU, re-encoded in hardware
to H.264 1080p. The first 2 frames failed with EIO and the encoder
recovered; the rest completed. Throughput 22 fps on testsrc2 (the
hardest content to encode — a real camera feed is easier); that is
marginal for 1080p30 live, comfortable for 720p30 or 1080p25. The
bottleneck is the CPU copy + overlay + pipe round-trip on the A53
cores, not the codecs (decode alone: 121 fps; encode alone: 50 fps).

ffmpeg 7.1.5 (Debian) has no `v4l2m2m` hwaccel
(`Unrecognized hwaccel: v4l2m2m`), so the `-hwaccel v4l2m2m` path is
unavailable; only the explicit `-c:v h264_v4l2m2m` decoder exists.

### Unexplained reboots (4x on 12 Nov 2026)

- ~12:20: hard reboot during the very first encode test (venc CLI).
- ~15:38: reboot during a HW-decode+HW-encode test run.
- ~16:35: reboot during the NV12M regression test.
- ~16:47: reboot during an encoder-only test (HW encode of a 30 s file).
- No crash logs survived any of them (Armbian ramlog only syncs
  periodically; `kernel.panic = 0`, so a panic would hang, not reboot —
  the reboots are consistent with the hardware watchdog (`/dev/watchdog`
  exists) firing after a kernel hang in the codec drivers).
- `systemd-journald` is now `Storage=persistent` so the next crash
  leaves logs. Until a multi-hour soak test passes, do not trust this
  driver for an unattended long live stream.

### USB errors (unrelated)

`dmesg` shows repeated `usb 1-1: device descriptor read/64, error -71`
from a USB device that never enumerates. Present before and after all
encoder work; unrelated to it.
