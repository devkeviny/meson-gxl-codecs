# meson-gxl-codecs

[Read in English](README.md)

Codificador H.264 por hardware para Amlogic **S905W (GXL)** no Linux
mainline, como dispositivo V4L2 mem2mem (`h264_v4l2m2m` no ffmpeg).
Portado de
[ray38080289/meson-sm1-codecs](https://github.com/ray38080289/meson-sm1-codecs)
(SM1: S905X3/S905D3) para o GXL.

Testado numa TV box Aquario STV-2000 (S905W, placa P281), Armbian 26.11
(Debian 13), kernel 6.12.114-ophub. O S905W tem codificador H.264 no
silício; o mainline não tem driver para ele. Este port faz ele funcionar.

## O que é

- `venc-h264/` — o driver HCodec do fornecedor (`encoder.c`, 5.444 linhas)
  adaptado ao mainline: uma camada de integração (`compat.c`), um
  encapsulamento V4L2 mem2mem (`venc_v4l2.c`, `/dev/videoN`) e dois CLIs
  de teste (`venc.c`, `venc-stage.c`) que acionam o dispositivo de
  caracteres (`/dev/amvenc_avc`) diretamente.
- `firmware/get-firmware.py` — baixa o ucode do codificador GXL
  (`gxl_h264_enc.bin`, 24.576 bytes, md5
  `548df08288d7cf48d4bb1255bca45ca7`) do `media_modules-aml` da CoreELEC
  num commit fixo e instala em `/lib/firmware/meson/venc/`. Nada é
  redistribuído: o ucode é um blob fechado da Amlogic; só o driver
  (GPL) está neste repositório.
- `scripts/build.sh`, `scripts/install.sh` — compilam o módulo e o
  instalam para carregar no boot.

O GXL só tem codificador H.264 — não existe codificador HEVC neste chip
(o WAVE420L é hardware G12A/SM1), por isso não há `venc-hevc` aqui.

## Início rápido (na box)

```sh
git clone https://github.com/devkeviny/meson-gxl-codecs.git
cd meson-gxl-codecs
sudo apt install build-essential linux-headers-$(uname -r)
sh scripts/build.sh          # módulo + venc + venc-stage
sudo sh scripts/install.sh   # firmware + módulo + boot + udev
sudo modprobe amvenc_avc
ffmpeg -hide_banner -encoders 2>/dev/null | grep h264_v4l2m2m
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 5 \
    -c:v h264_v4l2m2m -pix_fmt nv12 out.mp4
```

## Desempenho medido (S905W, ffmpeg 7.1.5)

| Caminho | 720p30 | 1080p30 |
|---|---|---|
| Encode V4L2, fonte leve | 61 fps (2,0x) | 50 fps (1,6x) |
| Decode por software + encode V4L2 | — | 28 fps (0,93x, limitado pelo decode) |
| libx264 ultrafast (software, p/ comparação) | 34 fps (1,14x) | 9,5 fps (0,32x) |
| Decode V4L2 sozinho (meson-vdec) | ~100 fps | 121 fps (4,0x) |

Encode ao vivo em 1080p30 funciona a partir de uma câmera (feed
cru/MJPEG). Veja [TESTING.pt-BR.md](TESTING.pt-BR.md) para os resultados
completos e o método.

## Adaptações para o GXL (em relação à árvore SM1)

1. `compat.h`: `get_cpu_type()` / `get_cpu_major_id()` retornam
   `MESON_CPU_MAJOR_ID_GXL` (controla todos os desvios por tipo de CPU
   em `encoder.c`).
2. `compat.c`: a busca do nó VDEC tenta `amlogic,gxl-vdec`, depois
   `amlogic,gx-vdec`, depois `amlogic,sm1-vdec`. A IRQ do mailbox é a
   SPI 45 no GXL também (device tree do fornecedor para a P281).
3. `encoder.c` `select_ucode()`: restaurada a seleção por CPU do
   fornecedor (a árvore SM1 fixa o ucode G12A); no GXL ela escolhe
   `gxl_h264_enc`.
4. `encoder.c` `hcodec_clk_prepare()`: tolera a ausência do
   `cts_hcodec_aclk` em chips pré-SC2. O mainline não tem esse relógio;
   o relógio do núcleo HCodec é programado pelo próprio driver via
   `HHI_VDEC_CLK_CNTL` (nível 6 = fclk_div3, 666 MHz) mais os bits de
   gate [26:12] de `DOS_GCLK_EN0`.
5. `encoder.c` `avc_k_frame()`: reafirma os gates de relógio do HCodec
   (bits [26:12] de `DOS_GCLK_EN0`) antes de cada frame. O
   `vdec_1_start` do meson-vdec no kernel faz uma escrita completa
   `DOS_GCLK_EN0 = 0x3ff`, que apaga esses gates; sem a reafirmação,
   um decoder rodando pode travar o codificador no meio do stream.
6. `venc_v4l2.c`: shims do kernel 6.12 para a API V4L2 que a árvore SM1
   espera do 6.18 (`file_to_v4l2_fh`, `v4l2_fh_add`/`v4l2_fh_del` com 1
   argumento, `v4l2_m2m_buf_copy_metadata` com 3 argumentos), guardados
   por `LINUX_VERSION_CODE`.
7. `compat.c`: `MODULE_IMPORT_NS(DMA_BUF)` sem aspas (o 6.12
   stringifica o argumento; a forma com aspas quebra o modpost).

## Problemas conhecidos

- **Decode por hardware + encode por hardware num só processo ffmpeg
  trava ou emperra.** Decoder sozinho e codificador sozinho funcionam,
  mas `ffmpeg -c:v h264_v4l2m2m -i in.mp4 -c:v h264_v4l2m2m out.mp4`
  trava antes do primeiro frame, ou o codificador dá timeout
  (`frame N: -5`) uma dúzia de frames depois. Um decoder emperrado só
  se recupera com reboot. **Solução comprovada: ponha uma cópia de CPU
  entre o decoder e o codificador** — dois processos ffmpeg ligados por
  um pipe de vídeo cru, ou um filtro `format=yuv420p` num só processo.
  O pipeline de live (HLS -> decode HW -> overlay -> pipe -> encode HW,
  1080p) funciona: 448/450 frames, 22 fps no conteúdo de teste mais
  difícil. Veja [TESTING.pt-BR.md](TESTING.pt-BR.md) para o diagnóstico
  completo.
- **A box reiniciou 4 vezes num dia de testes com o codificador**
  (12 nov 2026), causa não provada (nenhum log sobreviveu; compatível
  com o watchdog de hardware disparando após um hang do kernel). O
  `systemd-journald` agora é persistente para o próximo deixar logs.
  Não confie neste driver para uma live longa sem supervisão até um
  teste de horas passar.
- Depois de um reboot o codificador registra como `/dev/video0`
  (`meson-venc-h264`) e o decoder como `/dev/video1`
  (`meson-video-decoder`). O ffmpeg acha cada um pela capacidade, então
  isso só importa para scripts que fixam o número do dispositivo —
  combine por `/sys/class/video4linux/*/name`.

## Créditos

- [ray38080289/meson-sm1-codecs](https://github.com/ray38080289/meson-sm1-codecs)
  — o port SM1 do qual este deriva (driver, encapsulamento V4L2,
  ferramentas de firmware e a nota da armadilha do `DOS_GCLK_EN0`).
- CoreELEC `media_modules-aml` — o pacote público de firmware de onde
  vem o ucode GXL.
- Amlogic — o driver HCodec do fornecedor no qual isto se baseia (GPL).

## Licença

GPL-2.0 (veja `LICENSE`). O blob de firmware é ucode Amlogic, baixado
do upstream na hora do build; veja `firmware/get-firmware.py`.
