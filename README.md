# meson-gxl-codecs

[Leia em português](README.pt-BR.md)

Hardware H.264 encoder for Amlogic **S905W (GXL)** on mainline Linux, as a
V4L2 mem2mem device (`h264_v4l2m2m` in ffmpeg). Ported from
[ray38080289/meson-sm1-codecs](https://github.com/ray38080289/meson-sm1-codecs)
(SM1: S905X3/S905D3) to GXL.

Tested on an Aquario STV-2000 TV box (S905W, P281 board), Armbian 26.11
(Debian 13), kernel 6.12.114-ophub. The S905W has an H.264 encoder in
silicon; mainline has no driver for it. This port makes it work.

## What it is

- `venc-h264/` — the vendor HCodec driver (`encoder.c`, 5,444 lines)
  adapted to mainline: a platform glue layer (`compat.c`), a V4L2 mem2mem
  wrapper (`venc_v4l2.c`, `/dev/videoN`), and two test CLIs (`venc.c`,
  `venc-stage.c`) that drive the char device (`/dev/amvenc_avc`) directly.
- `firmware/get-firmware.py` — downloads the GXL encoder ucode
  (`gxl_h264_enc.bin`, 24,576 bytes, md5 `548df08288d7cf48d4bb1255bca45ca7`)
  from CoreELEC's `media_modules-aml` at a pinned commit and installs it
  under `/lib/firmware/meson/venc/`. Nothing is redistributed: the ucode
  is a closed Amlogic blob; only the driver (GPL) is in this repo.
- `scripts/build.sh`, `scripts/install.sh` — build the module and install
  it to load at boot.
- `scripts/live-pipe.sh`, `scripts/live-watchdog.sh` — the HLS live
  pipeline (HW decode → overlay → HW encode) and its restart watchdog;
  see TESTING.md.

GXL has an H.264 encoder only — there is no HEVC encoder on this chip
(the WAVE420L is G12A/SM1 hardware), so there is no `venc-hevc` here.

## Quick start (on the box)

```sh
git clone https://github.com/devkeviny/meson-gxl-codecs.git
cd meson-gxl-codecs
sudo apt install build-essential linux-headers-$(uname -r)
sh scripts/build.sh          # module + venc + venc-stage
sudo sh scripts/install.sh   # firmware + module + boot + udev
sudo modprobe amvenc_avc
ffmpeg -hide_banner -encoders 2>/dev/null | grep h264_v4l2m2m
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 5 \
    -c:v h264_v4l2m2m -pix_fmt nv12 out.mp4
```

## Measured performance (S905W, ffmpeg 7.1.5)

| Path | 720p30 | 1080p30 |
|---|---|---|
| V4L2 encode, light source | 61 fps (2.0x) | 50 fps (1.6x) |
| SW decode + V4L2 encode | — | 28 fps (0.93x, decode-bound) |
| libx264 ultrafast (software, for comparison) | 34 fps (1.14x) | 9.5 fps (0.32x) |
| V4L2 decode alone (meson-vdec) | ~100 fps | 121 fps (4.0x) |

1080p30 live encoding works from a camera (raw/MJPEG) feed. See
[TESTING.md](TESTING.md) for the full results and method.

## GXL adaptations (vs the SM1 tree)

1. `compat.h`: `get_cpu_type()` / `get_cpu_major_id()` return
   `MESON_CPU_MAJOR_ID_GXL` (drives every CPU-type branch in `encoder.c`).
2. `compat.c`: the VDEC node lookup tries `amlogic,gxl-vdec`, then
   `amlogic,gx-vdec`, then `amlogic,sm1-vdec`. The mailbox IRQ is SPI 45
   on GXL too (vendor P281 device tree).
3. `encoder.c` `select_ucode()`: restored the vendor CPU-based selection
   (the SM1 tree hardcodes the G12A ucode); on GXL it picks `gxl_h264_enc`.
4. `encoder.c` `hcodec_clk_prepare()`: tolerates a missing
   `cts_hcodec_aclk` on pre-SC2 chips. Mainline has no such clock; the
   HCodec core clock is programmed by the driver itself via
   `HHI_VDEC_CLK_CNTL` (level 6 = fclk_div3, 666 MHz) plus the
   `DOS_GCLK_EN0` gate bits [26:12].
5. `encoder.c` `avc_k_frame()`: re-asserts the HCodec clock gates
   (`DOS_GCLK_EN0` bits [26:12]) before every frame. The in-kernel
   meson-vdec's `vdec_1_start` does a full write of `DOS_GCLK_EN0 =
   0x3ff`, which clears those gates; without the re-assert, a running
   decoder can hang the encoder mid-stream.
6. `venc_v4l2.c`: kernel 6.12 shims for the V4L2 API the SM1 tree expects
   from 6.18 (`file_to_v4l2_fh`, one-arg `v4l2_fh_add`/`v4l2_fh_del`,
   3-arg `v4l2_m2m_buf_copy_metadata`), guarded by `LINUX_VERSION_CODE`.
7. `compat.c`: `MODULE_IMPORT_NS(DMA_BUF)` unquoted (6.12 stringifies the
   argument; the quoted form breaks modpost).

## Known issues

- **HW decode + HW encode in one ffmpeg process hangs or wedges.**
  Decoder alone and encoder alone both work, but `ffmpeg -c:v
  h264_v4l2m2m -i in.mp4 -c:v h264_v4l2m2m out.mp4` stalls before the
  first frame, or the encoder times out (`frame N: -5`) a dozen frames
  in. A wedged decoder only recovers with a reboot. **Proven
  workaround: put a CPU copy between the decoder and the encoder** —
  either two ffmpeg processes joined by a rawvideo pipe, or a
  `format=yuv420p` filter in a single process. The live pipeline
  (HLS -> HW decode -> overlay -> pipe -> HW encode, 1080p) works:
  448/450 frames, 22 fps on the hardest test content. See
  [TESTING.md](TESTING.md) for the full diagnosis.
- **The box rebooted 4x in one day of encoder testing** (12 Nov 2026),
  cause unproven (no logs survived; consistent with the hardware
  watchdog firing after a kernel hang). `systemd-journald` is now
  persistent so the next one leaves logs. Do not trust this driver for
  an unattended multi-hour live stream until a soak test passes.
- After a reboot the encoder registers as `/dev/video0`
  (`meson-venc-h264`) and the decoder as `/dev/video1`
  (`meson-video-decoder`). ffmpeg finds each by capability, so this only
  matters to scripts that hardcode device numbers — match on
  `/sys/class/video4linux/*/name` instead.

## Credits

- [ray38080289/meson-sm1-codecs](https://github.com/ray38080289/meson-sm1-codecs)
  — the SM1 port this is derived from (driver, V4L2 wrapper, firmware
  tooling, and the `DOS_GCLK_EN0` pitfall note).
- CoreELEC `media_modules-aml` — the public firmware pack the GXL ucode
  comes from.
- Amlogic — the vendor HCodec driver this is based on (GPL).

## License

GPL-2.0 (see `LICENSE`). The firmware blob is Amlogic ucode, fetched from
upstream at build time; see `firmware/get-firmware.py`.
