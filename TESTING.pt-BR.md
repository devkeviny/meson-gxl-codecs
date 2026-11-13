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

## Controles do driver e presets (12 nov 2026, testsrc2 1080p, 150 frames)

Encode V4L2, feed decodificado por software. O PSNR é medido contra o
arquivo de entrada (ele mesmo testsrc2 em libx264-ultrafast), então os
números comparam as variantes entre si, não contra vídeo perfeito.

| Configuração (ffmpeg) | fps | Tamanho | Bitrate real | PSNR médio |
|---|---|---|---|---|
| padrões (4 Mbps, GOP 60) | FALHOU (78 fr, 2 fps) | 1,3 MB parcial | — | — |
| `-b:v 8M` | 4,4 (timeouts EIO) | 5,5 MB | 8,9 Mbps | 34,81 |
| `-b:v 2M` | 29 | 3,3 MB | 5,25 Mbps (!) | 32,74 |
| `-g 30` | 31 | 2,7 MB | 4,39 Mbps | 32,93 |
| `-g 120` | 30 | 2,1 MB | 3,44 Mbps | 33,48 |
| `-qmin 18 -qmax 30` | FALHOU (17 fr) | 48 B | — | — |

Descobertas:

- **GOP 120 é a melhor configuração**: menor arquivo, menor bitrate,
  melhor PSNR do grupo ~4 Mbps, 30 fps cheios. Menos IDRs = mais bits
  para os P-frames.
- **8 Mbps dá a melhor qualidade (PSNR 34,81)** mas rodou a 4,4 fps
  (timeouts EIO no conteúdo detalhado) e 8,9 Mbps é pesado para
  streaming.
- **O alvo de 2 Mbps NÃO é respeitado**: real 5,25 Mbps. O controle de
  taxa do driver não consegue ir abaixo de ~5 Mbps neste conteúdo
  detalhado com QP 26/28 (piso do QP). Não dá para simplesmente baixar
  o bitrate para um upload limitado.
- **Não mexa em qmin/qmax**: `-qmin 18 -qmax 30` emperrou o
  codificador desde o frame 0 (provavelmente frames excedendo o buffer
  de bitstream de 1 MB).
- **A execução com padrões FALHOU desta vez** (78 frames, timeouts
  EIO) — o codificador é instável mesmo no caminho de feed por software
  antes confiável. As falhas EIO atingem intermitentemente todas as
  configurações.

## Por que uma execução que funcionou antes falha agora (12 nov 2026, ~17:52)

Mesmo comando, mesma entrada, mesma box, 5 execuções seguidas:

| Execução | Resultado |
|---|---|
| 1 | 150/150, 28 fps, 3225 KiB |
| 2 | 150/150, 28 fps, 3225 KiB |
| 3 | 150/150, 28 fps, 3225 KiB |
| 4 | **FALHOU no frame 17** (EIO, 0,4 fps) |
| 5 | 150/150, 28 fps, 3225 KiB |

Descartados por medição: temperatura (54 °C, CPU a 1,2 GHz cheios, sem
throttling), pressão de memória (512 MB disponíveis, sem OOM),
fragmentação de CMA (198 MB livres, 19 blocos livres de 4 MB),
configurações (a execução que falhou usava os padrões).

**Conclusão: uma corrida de inicialização por execução.** Cada execução
do ffmpeg ganha um reset completo do HCodec + recarga do firmware
(`avc_poweron` + `reload_mc`), então um travamento nunca sobrevive para
a próxima execução — mas dentro de uma execução, uma vez que um frame
estoura o tempo, o HCodec nunca se recupera. Todas as falhas observadas
são nos primeiros ~20 frames (frames 12–23); nenhuma execução jamais
falhou depois do frame 23. As execuções bem-sucedidas são
bit-idênticas (mesmo tamanho, mesmo bitrate), então o codificador é
determinístico quando sobrevive à inicialização. O gatilho provável é
uma conclusão de mailbox/IRQ perdida durante a rajada inicial de
frames, antes do estado driver/firmware estar assentado.

Consequência prática para um serviço de live: ~20–25% das partidas do
codificador travam no primeiro segundo. Mitigue com um watchdog que
reinicia o ffmpeg quando nenhum frame é produzido por alguns segundos
(cada reinício é uma nova moeda; 2–3 tentativas dão >99% de sucesso).
Um fix no nível do driver (ordem da configuração da IRQ, ou
reset-on-EIO em `avc_k_frame`) é depuração real de driver e não foi
feito.

## Depuração do travamento (12 nov 2026, ~18:00–18:40)

O que o log verboso do driver (`encode_print_level=2`) mostrou na
falha:

```
avc encode light reset ---
frame type: IDR, size: 1920x1080
mb info: 0x802a0769, encode status: 0x5, dct status: 0x0
vlc status: 0x7, me status: 0x0, risc pc:0x911, debug:0x0
```

`encode status 0x5` é `ENCODER_MB_HEADER`: o firmware começa o
primeiro IDR, entra na fase de cabeçalho de macrobloco e trava ali
(RISC parado no PC 0x911, VLC no estado 7). O frame que falha é o
próprio IDR, não um P-frame após um reset.

Descartados como gatilho:

- **Endereços dos buffers**: o bloco CMA de 20 MB do codificador é
  sempre o mesmo endereço físico (0x27900000), e o buffer de entrada
  V4L2 do ffmpeg também é sempre o mesmo (0x24500000) — nas execuções
  boas e nas ruins. Os endereços não determinam o resultado.
- **Conteúdo dos buffers**: uma execução falha não envenena o próximo
  processo (uma execução boa rotineiramente segue uma ruim com os
  mesmos buffers).
- **Temperatura / memória / CMA**: 54 °C, sem throttling, 512 MB
  livres, sem OOM, CMA sem fragmentação (ver acima).

O que NÃO recupera uma instância travada (tudo medido):

- O **light reset** do fornecedor (`amvenc_avc_light_reset`:
  poweroff + recarga do firmware + nova requisição da IRQ, a cada
  timeout): 50 resets seguidos nunca recuperaram uma execução falha.
- **Recriar a instância do codificador no driver** (destruir o wq
  após 2 erros consecutivos, reabrir do zero): 0 sucessos em 31
  recriações na primeira bateria (12 nov). Uma bateria posterior
  (13 nov, 20 runs) viu 2 recuperações via recriação em 4 runs
  travadas — ou seja, a recriação também é uma moeda, não um fix
  confiável. Só um processo ffmpeg *novo* ganha uma nova moeda de
  forma confiável.

O travamento é uma corrida de inicialização dentro do blob fechado do
firmware. Seu gatilho interno exato agora é parcialmente entendido —
ver "Engenharia reversa do firmware (AMRISC / Project Vicigol)"
abaixo.

Mudanças feitas no driver mesmo assim (nesta árvore, no ar na box de
teste):

- Timeout por frame de 2000 ms → **1000 ms** (`a[8]` em
  `avc_k_frame`): um frame saudável leva ~20 ms, então 1 s é 50x de
  margem e um frame travado atrasa a live pela metade do tempo.
- Em erro de `avc_k_frame`, o wrapper V4L2 força o próximo frame a
  ser um IDR (`force_idr`: o estado de referência se foi após o light
  reset) e, após 2 erros consecutivos, recria a instância. Os buffers
  falhos são completados com `VB2_BUF_STATE_ERROR`, então o trabalho
  M2M continua rodando e o ffmpeg vê frames descartados em vez de um
  travamento. (A recriação não corrige o travamento — ver acima — mas
  o comportamento de descartar-e-continuar é o que permite o watchdog
  funcionar.)
- Uma linha `dev_dbg` em `venc_work` registra os endereços DMA de
  entrada de cada frame (ative com dynamic debug: `echo 'file
  venc_v4l2.c +p' > /sys/kernel/debug/dynamic_debug/control`).
- Uma linha `dev_info` registra `recovered after N consecutive
  errors` quando um frame funciona após erros (sinal de recuperação
  para os experimentos abaixo).
- Parâmetro de módulo `reset_delay_ms` (padrão 30, o valor do
  fornecedor): delay de power-off no light reset, ajustável em
  tempo real via `/sys/module/amvenc_avc/parameters/reset_delay_ms`.

## Engenharia reversa do firmware (AMRISC / Project Vicigol)

O blob do firmware é código para o núcleo RISC proprietário da
Amlogic, **AMRISC**: 32 bits, 64 registradores de uso geral, um
branch delay slot, **instruções de 28 bits guardadas em palavras de
32 bits** (4 bits de padding). Foi documentado pelo
[Project Vicigol](https://codeberg.org/neuschaefer/vicigol)
(J. Neuschäfer, GPN23 2025), que publica um manual da arquitetura, um
emulador e um desmontador (`vici-dasm`). O disassembly completo do
`gxl_h264_enc.bin` tem 6.438 linhas de código coerente (chamadas,
branches, loops, registradores com nome como `MCPU_INTR_MSK` e
`LMEM_DMA_*`). O blob tem 24 KiB; só os primeiros ~10,4 KiB contêm
código (até o endereço 0x0a23), o resto é zero.

Achados relevantes para o travamento:

- O "risc pc" do dump do driver lê `HCODEC_MPC_E` — o program
  counter do estágio de execução do M-core (baseado em palavras). O
  PC do travamento **0x911** é um `sub r0, r1, r0` dentro de uma
  função de decisão de controle de taxa (chamada do loop principal
  em 0x0a08, entrada por `blti r0, 0` quando a entrada é negativa).
  Um `sub` não trava sozinho: o núcleo está parado ali, não em loop.
- Loop principal (0x0a05): se a flag DMEM[0x36] estiver armada,
  chama a função de controle de taxa com r0 = DMEM[0x28]; depois
  despacha o comando do mailbox (registrador 0x0ac0): 1/2 = ACK,
  3/4 = setup do frame (0x08c9) + arma a flag 0x36.
- O setup do frame (0x08c9) zera DMEM[0x24/0x26/0x27/0x28] a cada
  frame. Um handler de interrupção (0x03e4, vetor 0x0022) calcula
  DMEM[0x27] = erro de bitrate com clamp (bits reais menos meta,
  ±32767) e decrementa DMEM[0x28].
- O caminho do travamento (0x911) é tomado **sse DMEM[0x28] == 0 E
  DMEM[0x27] < 0**. Hipótese forte (encaixa em todas as
  observações): o caminho é armado nos primeiros frames (0x28 é
  zero até a função de controle de taxa preenchê-lo), disparado
  quando o erro de bitrate fica negativo (poucos bits produzidos no
  início do stream), e a corrida é a escrita de DMEM do handler de
  interrupção contra a chamada de controle de taxa do loop
  principal — o timing varia por execução, daí a taxa de falha de
  ~20–40%. O stall no `sub` é provavelmente um hazard de pipeline
  no r0, recém-carregado pelo `ldas` no delay slot.
- O carregador do firmware (`amvenc_loadmc`) carrega só os
  primeiros 16 KiB para a IMEM via DMA e **não toca a DMEM** — ou
  seja, o estado da DMEM sobrevive à recarga do firmware.

## Experimento do delay de power-off maior — REFUTADO (13 nov 2026)

Hipótese: os 30 ms de power-off do fornecedor no light reset são
curtos demais para o trilho de energia do HCodec descarregar abaixo
do limiar de retenção de estado da SRAM/VLC, então o estado travado
sobrevive ao reset. Teste: `reset_delay_ms=200` (6,7x maior),
20 runs x 30 frames:

- 17/20 runs limpos (30/30 frames).
- 2/20 runs totalmente travados (0 frames, 30 erros cada) — sem
  recuperação.
- 2/20 runs falharam 2 e 4 frames, depois se recuperaram via
  recriação da instância V4L2 (não via light reset).
- **Recuperações por light reset: 0.** Os 200 ms não trouxeram
  melhora sobre o baseline de 30 ms (2/10 totalmente travados lá).

Conclusão: o tempo de descarga do trilho não é o mecanismo. O padrão
continua nos 30 ms do fornecedor; `reset_delay_ms` fica como
parâmetro ajustável para experimentos futuros. O watchdog (reiniciar
o processo ffmpeg) continua sendo a única recuperação confiável.

## Watchdog da live — PROVADO (12 nov 2026, ~18:34–18:37)

`scripts/live-watchdog.sh` + `scripts/live-pipe.sh`: o pipeline da
live HLS (decodificação HW → overlay → pipe → codificação HW 1080p,
GOP 120) sob um watchdog que reinicia o pipeline quando o arquivo
`-progress` do codificador não mostra frame novo por 6 s. Teste de
150 s, fonte HLS de 3 min, saída para FLV local:

- Tentativa 1: travou na partida (frame parado em 0) — detectado em
  6 s, reiniciado.
- Tentativa 2: rodou limpo pelos 140 s restantes: **4.053 frames a
  29,36 fps** (conteúdo de cor sólida; testsrc2 detalhado roda
  22–25 fps).
- Temperatura da box após o teste: 60 °C, CPU ainda a 1,2 GHz cheios
  (sem throttling). O processo de decodificação+overlay roda quente
  (~285% de CPU — a cópia de CPU + overlay é o gargalo, não os
  codecs).

Para uma live de verdade, configure `LIVE_OUTPUT` com a URL RTMP (ex.
`rtmp://a.rtmp.youtube.com/live2/KEY`) e `INPUT_URL` com a fonte da
câmera / HLS. Um travamento então custa ~10 s de stream (6 s de
detecção + reinício); o YouTube trata uma reconexão rápida com a
mesma chave como o mesmo evento ao vivo.

Software (libx264, CRF 23 padrão) para comparação:

| Preset | 1080p fps | 1080p tamanho | 720p fps |
|---|---|---|---|
| ultrafast | 9,5 | — | 34 (transcode) |
| superfast | 13 | 6,3 MB (10,2 Mbps) | 28 |
| veryfast | 10 | 3,1 MB (5,1 Mbps) | — |

Nenhum preset de software chega a 1080p30 nesta box. Em 720p, o
superfast chega a 28 fps.

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

### Reboots sem explicação (5x: 4x em 12 nov, 1x em 13 nov 2026)

- ~12:20: reboot durante o primeiríssimo teste de encode (CLI venc).
- ~15:38: reboot durante um teste de decode HW + encode HW.
- ~16:35: reboot durante o teste de regressão do NV12M.
- ~16:47: reboot durante um teste só de encode (encode HW de um
  arquivo de 30 s).
- ~03:15 em 13 nov: reboot durante a run 1 da bateria de 200 ms
  (primeiro encode com o novo build do driver). Nenhum log de crash
  apesar do journald persistente — o journal termina só com ruído
  de USB. Se o novo build contribuiu, não se sabe; a box depois
  rodou a bateria completa de 20 runs e mais encodes sem rebootar.
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
