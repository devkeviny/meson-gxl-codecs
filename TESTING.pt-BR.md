# Notas de teste

[Read in English](TESTING.md)

Todos os testes numa Aquario STV-2000 (S905W, P281), Armbian 26.11 /
Debian 13, kernel 6.12.114-ophub, ffmpeg 7.1.5 (Debian), 12 nov 2026. As
saídas foram validadas com `ffprobe` (codec, resolução, contagem de
frames).

## O que funciona

- **Build + carga do módulo**: `amvenc_avc.ko` compila com os headers
  ophub e gcc 14.2 (o kernel foi compilado com gcc 15.3.1 — sem
  problema). O `insmod` cria `/dev/amvenc_avc` e `/dev/videoN`; o
  dmesg reporta `dos [mem 0xc8820000-0xc882ffff] irq <n> canvas 4..15`.
- **Teste por etapas no dispositivo de caracteres** (`venc-stage`,
  registra cada etapa em disco com fsync antes de executá-la): open,
  CONFIG_INIT (liga o hardware + carrega o firmware), GET_BUFFINFO +
  mmap, SEQUENCE (SPS/PPS, 21 bytes), frame IDR, 5 P-frames — tudo
  passa; a saída é H.264 720p válido (6/6 frames).
- **Teste de carga**: 90 frames seguidos (sem pausas), 90/90 frames.
- **Encode V4L2 via ffmpeg** (`-c:v h264_v4l2m2m -pix_fmt nv12`):
  - 720p30, entrada testsrc2, decode por software: **61 fps (2,02x)**.
  - 1080p30, fonte leve (`color`): **50 fps (1,64x)**.
  - 1080p30, entrada testsrc2, decode por software: **28 fps (0,93x)**
    — o decode H.264 por software é o gargalo, não o codificador.
  - 720p30, decode por software + libx264 ultrafast (comparação):
    34 fps.
  - 1080p30, decode por software + libx264 ultrafast (comparação):
    9,5 fps.
- **Decode V4L2 sozinho** (meson-vdec no kernel, `h264_v4l2m2m`): 720p
  ~100 fps, 1080p **121 fps (4,02x)**.
- **Instalação no boot**: módulo em
  `/lib/modules/<kver>/updates/`, `depmod`, `modules-load.d`, regra
  udev para `/dev/amvenc_avc` (grupo video, 0660). Verificado após um
  reboot: o módulo carrega sozinho e os dois dispositivos aparecem.

## O que não funciona

### Decode HW + encode HW num só processo ffmpeg: trava ou emperra

```
ffmpeg -c:v h264_v4l2m2m -i in1080.mp4 -c:v h264_v4l2m2m out.mp4
```

Dois modos de falha, ambos reproduzidos várias vezes (12 nov 2026):

1. **Trava antes do primeiro frame.** A thread do decoder fica em
   `poll()` para sempre e o codificador nunca abre (só `/dev/video1`,
   o decoder, está aberto). Nenhuma saída no dmesg de nenhum dos dois
   drivers. A CMA drena de ~180 MB livres para ~15 MB enquanto trava.
   `timeout`/`SIGTERM` não matam o ffmpeg; precisa `kill -9`. Depois de
   matar, o decoder fica emperrado: uma nova execução só de decoder
   também trava, e recarregar o módulo `meson_vdec` não desemperra —
   só um reboot restaura o decoder. Alimentar a entrada em tempo real
   (`-re`) evita este modo (os frames começam a fluir), então é uma
   corrida de tempo: quando o arquivo inteiro + EOF chega no decoder
   antes dele decodificar o frame 1, o pipeline trava. Uma fonte ao
   vivo (tempo real) não cai nisso.
2. **Timeouts do codificador no meio do stream.** Com `-re`, os frames
   0..~14 codificam, depois o codificador falha com
   `amvenc_avc: frame N: -5` (EIO, timeout de 2 s por frame) e nunca se
   recupera. O frame da falha varia (12, 15, 16, 17, 23 entre execuções)
   e não depende da contagem de buffers de captura do decoder (8, 20,
   32 falham no mesmo ponto). Conteúdo de cor sólida em 1080p completa
   limpo (149/150 frames); conteúdo detalhado em 720p completa em sua
   maior parte (144/150, dois frames com EIO, depois se recupera). Ou
   seja, a falha depende do conteúdo e da resolução.

O que os logs estabeleceram:

- A configuração do decoder é idêntica no caso que funciona (só decode)
  e no caso que trava (mesmo dispositivo, formatos, contagem de buffers
  — comparados os logs `-v debug` linha a linha). O decoder recebe a
  entrada completa + EOF e produz zero frames no caso que trava.
- O `vdec_1_start` do meson-vdec no kernel (mainline 6.12,
  `drivers/staging/media/meson/vdec/vdec_1.c:201`) faz uma escrita
  completa `DOS_GCLK_EN0 = 0x3ff`, que apaga os gates de relógio do
  HCodec [26:12]. O projeto SM1 corrigiu isso no vdec modificado deles;
  este repo reafirma os gates antes de cada frame em `avc_k_frame()`
  (`WRITE_VREG_BITS(DOS_GCLK_EN0, 0x7fff, 12, 15)`). Essa correção está
  no driver, mas **não** resolveu nenhum dos dois modos de falha.
- O decoder entrega dmabufs NM12 de 2 planos; o codificador v4l2m2m do
  ffmpeg negocia NV12 de 1 plano para sua fila OUTPUT (o ffmpeg prefere
  1 plano para `AV_PIX_FMT_NV12`), e o ffmpeg passa os frames do
  decoder sem conversão (mesmo formato de pixel AV dos dois lados).
  Restringir o codificador a só multi-plano (NV12M/NV21M) foi tentado e
  REVERTIDO: quebrou o caminho de decode por software que funcionava
  (mesmos timeouts -EIO a partir de ~frame 10) e a box reiniciou uma
  vez durante esse teste de regressão. Os formatos de 1 plano ficam.

**Regra que funciona (provada): os dmabufs do decoder não podem chegar
no codificador sem cópia. Qualquer cópia de CPU no meio resolve:**

| Pipeline (1080p30, testsrc2) | Resultado |
|---|---|
| Decode HW + encode por software (libx264) | trava (mesma trava pré-frame-1) |
| Decode HW + overlay + null (sem codificador) | funciona, 79 fps |
| Decode HW -> `format=yuv420p` -> encode HW (1 processo) | funciona, 150/150 frames, 27 fps |
| Decode HW -> pipe -> encode HW (2 processos) | funciona (veja o teste HLS abaixo) |
| Decode HW + overlay -> pipe -> encode HW (2 processos) | funciona, 150/150 frames, 25 fps |
| **HLS (M3U8) -> decode HW -> overlay -> pipe -> encode HW** | **funciona, 448/450 frames, 22 fps** |

O teste HLS é o pipeline de live: um stream HLS local (segmentos de 2 s
por HTTP) decodificado por hardware, com overlay na CPU, recodificado
por hardware para H.264 1080p. Os 2 primeiros frames falharam com EIO e
o codificador se recuperou; o resto completou. Taxa de 22 fps no
testsrc2 (o conteúdo mais difícil de codificar — um feed de câmera real
é mais leve); isso é marginal para live 1080p30, confortável para
720p30 ou 1080p25. O gargalo é a cópia de CPU + overlay + ida e volta
pelo pipe nos núcleos A53, não os codecs (só decode: 121 fps; só
encode: 50 fps).

O ffmpeg 7.1.5 (Debian) não tem hwaccel `v4l2m2m`
(`Unrecognized hwaccel: v4l2m2m`), então o caminho `-hwaccel v4l2m2m`
não existe; só o decoder explícito `-c:v h264_v4l2m2m`.

### Reboots sem explicação (4x em 12 nov 2026)

- ~12:20: reboot durante o primeiríssimo teste de encode (CLI venc).
- ~15:38: reboot durante um teste de decode HW + encode HW.
- ~16:35: reboot durante o teste de regressão do NV12M.
- ~16:47: reboot durante um teste só de encode (encode HW de um
  arquivo de 30 s).
- Nenhum log de crash sobreviveu a nenhum deles (o ramlog do Armbian
  só sincroniza periodicamente; `kernel.panic = 0`, então um panic
  travaria, não rebootaria — os reboots são compatíveis com o watchdog
  de hardware (`/dev/watchdog` existe) disparando após um hang do
  kernel).
- O `systemd-journald` agora é `Storage=persistent` para o próximo
  crash deixar logs. Até um teste de horas passar, não confie neste
  driver para uma live longa sem supervisão.

### Erros de USB (não relacionados)

O `dmesg` mostra repetidos `usb 1-1: device descriptor read/64,
error -71` de um dispositivo USB que nunca enumera. Presente antes e
depois de todo o trabalho com o codificador; sem relação com ele.
