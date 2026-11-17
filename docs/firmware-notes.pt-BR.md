# firmware-tankg — M-VAVE Tank-G v1 (JieLi AC791N, pi32v2)

**Status: compila e LINKA para pi32v2 (build/tankg.bin); NÃO foi gravado e NÃO foi testado na Tank-G.**

Reaproveita `../esp32-mock/firmware-core` (C++17, sem STL) e o chip layer do `felucca-bsp` (MPL-2.0, escrito para o FM-1, mesma família de chip).

## Uso
    . tools/env.sh            # coloca o clang pi32v2 (release 23.1.5-5) no PATH
    tools/check_core.sh       # compila o firmware-core para pi32v2 (~22 KB)
    tools/build.sh            # compila + linka + gera build/tankg.bin (XIP 0x02000120, cabeçalho goto+4/rts)
    EXTRA="-DTG_FLASH_STORE -DTG_STORE_OFF=0x...u" tools/build.sh   # store em flash (região NÃO verificada!)
    g++ ... tests/test_flash_store.cpp   # 53 checks no host, com corte de energia simulado
    tools/try_build.sh        # compila bsp_glue.c (USB CDC) + main.cpp (PING->PONG) em .o

## Arquivos
- `src/pins.h` — mapa de pinos derivado do disassembly (com nível de confiança).
- `src/bsp_glue.c` — lado C: USB CDC via `usb.c` do felucca-bsp.
- `src/main.cpp` — lado C++: linhas -> `pd::Engine` (store em RAM/vazio por enquanto).
- `compat/` — headers libc mínimos só para compile-check.

## Comandos seriais (USB CDC, 115200, uma linha por comando, DTR ligado)
- `PING` → resposta do Engine. Todos os comandos do firmware-core continuam disponíveis.
- `LEDBIT <0..28>` acende **só** esse bit da palavra de 29 bits do registrador de deslocamento (PA2 CLK, PA3 DATA invertido, PA0 LATCH).
- `LEDRAW <hex>` envia a palavra inteira (máx. `1FFFFFFF`); `LEDRAW 0` zera todos os bits. Argumento inválido → `ERR …`, sem tocar nos pinos.
- `UBOOT` volta ao UBOOT da ROM (mesmo mecanismo `usb_update_mode` que o firmware original usa); o SysEx `F0 22 24 35 7D F7` também. **Caminho de recuperação** (não testado na Tank-G).
Só PA0..PA3 são tocados. Nada disso foi testado no pedal; a hipótese de que esses pinos são LED/display vem só do firmware original.

## Testes no host (sem hardware)
    tools/build.sh                       # imagem padrão: SEM código de escrita em flash (verificado: sem fl_* no ELF)
    g++ … tests/test_led_cmds.cpp …      # 44 checks: LEDBIT 0..28, LEDRAW, entradas inválidas, UBOOT
    g++ … tests/test_led_sr.cpp          # driver contra simulador de pinos
    python3 -I tools/pack.py --template ~/backup_tankg/app_area_0x4000_0x7b000.bin --out build/tankg_app_mypedal.bin

## Segurança
- `fm1_cstart` copia a proteção do hwtest do felucca-bsp: 2 boots seguidos que não chegam ao laço saudável caem no UBOOT da ROM.
- Sem `-DTG_FLASH_STORE` o firmware NUNCA escreve na flash (regiões de escrita vazias).
- A imagem ainda não está no formato de pacote da Tank-G (cabeçalho/CRC do container JieLi): não grave `tankg.bin` cru.

## Pendências
1. Empacotar `tankg.bin` no container da Tank-G (usar repack_app.py/jl-uboot-tool) e provar que o layout de flash não colide.
2. `TG_STORE_OFF`: **nenhuma região comprovadamente livre** (as faixas apagadas ficam dentro de VM/USR do firmware original — ver analysis/HW_MAP_TANKG_M548.md). Store em flash permanece desligado.
3. LEDs (driver não identificado), codec/áudio (+0x50DC), grupo de pinos do ALNK, função de PA10 e PD0.
4. SDK AC79 (Gitee) — clone a ~7 KiB/s; alternativa: felucca-bsp/fm1-base (em analysis/sdk).

## Gravação com verificação (`tools/flash_tankg.sh`) — NÃO testada em hardware
    tools/flash_tankg.sh check     # só offline: hash da imagem, CRCs do JLFS, backup_a == backup_b, original == backup_a[0x4000:0x7B000]
    tools/flash_tankg.sh write     # terminal real (sudo): confere identidade do pedal, pede "GRAVAR", escreve 0x4000..0x7B000,
                                   # lê 1 MiB de volta e compara: app == imagem; 0..0x3FFF e 0x7B000..0xFFFFF == backup_a
    tools/flash_tankg.sh restore   # devolve o app original do backup e verifica (flash == backup_a)
Dispositivo: o `jluboottool` precisa do nó SCSI-generic `/dev/sgN` do disco WL82 (não do `/dev/sdX`). O script mapeia sdX→sgN pelo sysfs, roda `modprobe sg` se faltar, cria o nó com `mknod /dev/sgN c 21 N` se não existir e confere que é um char device de major 21. Chamadas ao tool têm `timeout` (60 s identidade, 600 s leitura, 900 s escrita) para não ficar preso.
Pré-requisito: pedal em UBOOT (disco "WL82 UBOOT1.00"). O `write` do jluboottool só apaga dentro de 0x4000..0x7B000 (setores 4 KiB / blocos 64 KiB alinhados, sem passar do fim).
A lógica de verificação foi testada contra um "pedal" simulado (arquivo): gravação boa, corrupção do SPL / da cauda / do app, pedal estranho, sem confirmação, restore.

## Varredura dos 29 bits (`tools/led_sweep.py`)
    ~/.local/share/pipx/venvs/platformio/bin/python tools/led_sweep.py      # terminal real; --check = só PING/LEDBIT 0/LEDRAW 0
Acende um bit por vez (`LEDBIT n`) e você digita o que vê: `fs1 r`/`fs2 verde`/`fs3 azul` (footswitch + cor), `seg a`..`seg g`, `dp`, `n` (nada) ou texto livre. Comandos: `r` repetir, `b` voltar, `s` pular, `q` salvar/sair. Resultados em `build/led_sweep.json` (retoma de onde parou; `--fresh` recomeça). Ao final, grava `kFsColorBit`, `kSegBit`, `kDpBit` e `kBitLabel` no bloco gerado de `src/led_sr.h` (backup em `led_sr.h.bak`). Teste sem hardware: `tests/test_led_sweep.py`.

## Descoberta dos footswitches (`PORTS`, `tools/port_probe.py`)
`FSREAD` com PH6..9/PA10 não reagiu no pedal real. `PORTS` mostra o registrador de entrada das portas A..H; `PORTS PU` liga DIE+pull-up temporariamente em todos os pinos (exceto PA0..3) e restaura em seguida. `tools/port_probe.py` pede para pisar cada footswitch e lista o bit que mudou. Teste sem hardware verificado só contra um pedal simulado.

## Footswitches e LEDs RGB (medido / rastreado)
`FSREAD`: FS1=PA10, FS2=PH9, FS3=PH8, FS4=PH6 (medidos com `port_probe.py`, ativos em nível baixo); `aux` = PH7 (não é footswitch). Resposta inclui `rawA`/`rawH` para diagnóstico.
LEDs RGB: cadeia WS2812 de 12 pixels no pino **PC10** via SPI+DMA do periférico `0x11D00` (rastreado em `+0x12ED8`, **ainda não testado no pedal**). Comandos: `RGB <0..11|ALL> <RRGGBB>` (envia o quadro inteiro, mantém os outros pixels), `RGBOFF`, `RGBDIV <1..255>` (stock 9). Comece com valores baixos (`RGB ALL 100000`).

## Calibração e mapa dos 12 pixels (`tools/rgb_sweep.py`)
`RGBPAT <one> <zero>` ajusta o byte SPI de bit 1/0 (stock 7E 60). `tools/rgb_sweep.py` (terminal real): fase 1 testa divisores/padrões acendendo tudo em vermelho; fase 2 acende um pixel por vez e você diz `fs2 b`; fase 3 testa a ordem dos canais; grava `src/rgb_map.h`. Teste sem hardware: `tests/test_rgb_sweep.py`.
**Validado no pedal (2026-11-15):** FSREAD (os 4 footswitches, PH7 não é usado). `RGB ALL 100000` acendeu os 4 anéis, mas com cores erradas (1 verde, 3 laranja) com div 9 / 7E 60.

**Mapa dos pixels medido (2026-11-15, `rgb_sweep.py`):** pixels 0..2 = FS1, 3..5 = FS2, 6..8 = FS3, 9..11 = FS4 (posições a,b,c em sequência); ordem de canais RGB correta; calibração ok com os valores do original (div 9, 7E/60). `FSRGB <1..4> <RRGGBB>` colore os 3 pixels de um footswitch. O primeiro quadro após o boot saía com cor errada; o firmware agora manda um quadro apagado antes do primeiro (correção por hipótese, ainda não verificada no pedal).

## Knobs (`KNOBS`)
`KNOBS [ch]` varre os 8 endereços do mux (PC6..8) e imprime 16 valores brutos de 10 bits: `ch3(PB1)` nos endereços 0..7 e depois o 2º mux (canal 8 por padrão; `KNOBS <n>` testa outro canal SARADC 0..15). `x` = timeout do ADC. Rastreado em `+0xBD8E` do original; **não testado no pedal**; qual slot é qual knob é descoberto girando um por vez e comparando as leituras. PB1 e PC9 só são configurados como entrada analógica (sem pull, DIE desligado).

## Mapa dos knobs (`tools/knob_sweep.py`)
`~/.local/share/pipx/venvs/platformio/bin/python tools/knob_sweep.py` (terminal real, PulseDeck fechado): mede a linha de base dos 16 slots do `KNOBS`, pede para girar cada knob (MASTER → NOISE GATE) e atribui o slot que variar mais de 200 unidades (`--threshold`); depois espera estabilizar e passa ao próximo. Teclas: `s` pular, `b` voltar, `r` nova base, `q` salvar e sair. Grava `build/knob_sweep.json` e `src/knob_map.h` (`kKnobSlot[15]`, `kSlotKnob[16]`). Teste sem hardware: `tests/test_knob_sweep.py`.

## Integração painel ↔ Engine (`src/panel.h`) — NÃO TESTADA NO HARDWARE
Loop em `tg_main`: `panel->poll()` a cada volta. Footswitches (debounce 3×5 ms) → `Engine::fsEvent`; `Engine::tick` a cada 5 ms; anéis RGB = `Engine::ledColor` limitado a 25 % (`PANEL CAP n`); display = último dígito de slot+1 (ponto = bypass, `-` = sem preset); LED do knob mexido acende 0,8 s.
Knobs (zona morta 8/1023, a cada 25 ms): VOLUME/BASS/MIDDLE/TREBLE/GAIN → `AMPEQ` (mapeamento por nome, **inferido**; unidade dB×10 "não verificada" no PROTOCOL_SPEC). Os outros 10 só geram `KNOBEV <idx> <raw>` com `PANEL MON 1` (o Engine não tem API de knob; o firmware-core não foi alterado).
Comandos: `PANEL`, `PANEL MON 0|1`, `PANEL CAP n`, `PANEL AUTO`. Qualquer `LEDBIT/LEDRAW/RGB*/FSRGB` pausa o render automático até `PANEL AUTO`.
Testes: `tests/test_panel.cpp` (compilar como `test_led_cmds`).

## Áudio (`src/audio.h` + `tg_audio_*` em `src/bsp_glue.c`) — NÃO TESTADO NO HARDWARE
- Hardware: ALNK0 no grupo da porta A (IOMAP_CON5[7:6]=01): MCLK PA4, SCLK PA5, LRCK PA6, lane 0 TX = PA7, lane 1 RX = PA8, PA9 = **LOW como no original** (o original nunca sobe PA9; a 1ª tentativa subia PA9 e não deu som), PC0/PC1/PC5 = saídas em 1 como no original (função desconhecida); `AUDIO PIN <A..H><n> <0|1>` testa pinos ao vivo. PA10 (FS1) e PA0..3 não são tocados. Sequência de registradores copiada de `gx_audio_init` do Galaxia (verificada no Galaxia, **só inferida para a Tank-G**). Codec/barramento de controle: não identificado.
- Liga com `AUDIO THRU` (ou `AUDIO TONE` = triângulo 440 Hz); `AUDIO OFF`; `AUDIO STAT` mostra irqs/missed/PLL. **Inicia sozinho no boot (AUDIO THRU)**; `-DTG_AUDIO_NO_AUTOSTART` volta ao início manual; `-DTG_AUDIO_STOCK_PINS=0` não mexe em PC0/PC1/PC5/PA9; `-DTG_AUDIO_PA9_HIGH=1` volta à polaridade do Galaxia).
- DSP (inteiro, na ISR, 128 frames/meia-buffer): entrada = canal ESQUERDO [suposição], saída nos dois; gain in/out do preset; blocos implementados (ids do protocol.json): gate(1), boost(2), overdrive(3), ts9(9), ds1(10), rat(17), muff(18), bd2(20); os demais passam direto. Os demais tipos passam direto. O `firmware-core` não tem `processBlock`/DSP, então o DSP vive aqui.
- Boot: presets padrão em RAM (perdem-se ao desligar): 0 Clean, 1 Boost, 2 Overdrive (gate DESLIGADO + od), 3 Distortion (gate DESLIGADO + ds1); o knob NOISE GATE liga o gate (threshold = posição) e no mínimo desliga; sem bloco gate no preset, ele é adicionado na 1ª vez; carrega o slot 0 (some o `-`). FS1..4 = slots 0..3 (fábrica); apertar de novo o slot atual alterna bypass.
- Testes: `tests/test_audio.cpp`, e `test_led_cmds.cpp` cobre boot e comandos AUDIO.

### Ajuste do ALNK (round 3) — `AUDIO CFG` / `AUDIO SET` / `tools/audio_sweep.py`
Padrões agora = estado final do original (CLK_CON2[11:8]=1, CON0=0x180, CON1=0x00D5, CON3=0x83). Mude ao vivo: `AUDIO SET CLK 0..f`, `CON0`, `CON1` (ex. `11d5`), `CON3`, `LEN` (reinicia o áudio sozinho); `AUDIO CFG` mostra. `tools/audio_sweep.py` percorre combinações e pergunta o que você ouviu.


## Modo Preset / Live, knobs e persistência (Fases 2 e 3) — NÃO TESTADO NO PEDAL

- **Modo Preset** (boot): FS A..D carregam os presets 0..3 (de novo no preset atual = bypass); segurar um FS grava a cadeia atual nele.
- **Modo Live**: A = reverb, B = delay, C = modulação, D = amp (primeiro bloco da categoria; anel apagado se não existe). Segurar um FS grava no preset ATUAL.
- **B+C** pisados juntos e soltos em <1 s alternam Preset/Live (segurar mais: afinador/BLE como antes). Display: ponto decimal aceso = Live (em Preset o ponto = bypass).
- **Knobs** (`Engine::knob`): MASTER age direto em qualquer modo (curva quadrática, não é salvo). NOISE GATE segue a **posição absoluta**: ≤2 % (raw ≤20) desliga, >2,3 % liga e mantém ligado depois de soltar. Em Live os demais knobs editam o primeiro bloco ativo da categoria (RVB MIX/DECAY, DLY TIME/MIX, MOD SPEED/FX, amp VOLUME/BASS/MIDDLE/TREBLE/GAIN, IR CAB = IR 0..8) com **soft takeover**: após trocar preset/cena ou entrar em Live o knob fica desengatado e o LED pisca até cruzar o valor salvo. TYPE não faz nada.
- **Salvar**: segurar footswitch ou `SAVE` na serial (grava no preset atual); display mostra `5.` (ok) ou `E` (erro) por 1,5 s.
- **Flash**: `tg::LogStore` (anexação com CRC32 e commit por último) em 0x75000–0x7B000; ver `analysis/HW_MAP_TANKG_M548.md`. Primeiro boot com store vazio grava os 4 presets de fábrica; depois valem os salvos. Build só-RAM: `EXTRA=" " tools/build.sh`.


## Capturas, IR enviados, oitavador e medição de CPU — NÃO TESTADO NO PEDAL

- **RAM**: `tools/build.sh` alarga a região de dados do linker de 96 K para 320 K (0x01C08000–0x01C58000). A SRAM do AC79 vai de 0x01C00000 a 0x01C7FD50 (felucca `docs/BOARD_IO.md`) e as pilhas começam em 0x01C74000. `.bss` atual ≈ 230 KB. Nunca rodou no pedal: se não subir, a guarda de dois boots falhos cai no UBOOT → `flash_tankg.sh restore`.
- **Capturas (A2L1)**: `src/a2l.h` executa a rede NAM A2 Lite em ponto fixo (pesos int16, ativações int32, acumulador int64); `tests/test_a2l.cpp` compara com a mesma rede em float64 (≈100 dB de SNR). `LDBEGIN/LDB/LDEND` valida (CRC, formato, limites: ≤8192 B, ≤1400 MAC/amostra, ≤64 KB de histórico) e passa a soar na hora; `CSAVE`/`CLOAD`/`CDEL`; o preset guarda o slot. Com captura ativa o amp é: GAIN → rede → bloqueio de DC → BASS/MIDDLE/TREBLE → VOLUME. Sem captura, o estágio de amp aberto de antes. `CTRIM` é aceito mas o trim não é aplicado.
- **IR enviado (.wav)**: `IRBEGIN/IRB/IREND` → `tg::makeIr` reduz a ≤512 taps a 24 kHz (21 ms; meia-banda + pré-ênfase) → FIR no primeiro bloco `ir` ligado (±0,6 dB da convolução exata até 8 kHz em `tests/test_media.cpp`). `ISAVE s`, `FXIR bloco s` (= `ir_slot s+1`), `IDEL`. Slot sem IR enviado = caixa interna. `ILIST` mostra as internas com tamanho 0.
- **Flash**: capturas (`c00..c14`) e IRs (`i0..i7`) ficam no mesmo `LogStore` dos presets (0x75000–0x7B000, banco de 12 KiB para TUDO). Cabem poucas capturas (~1,5 KB cada) e IRs (~1 KB cada); sem espaço, `CSAVE/ISAVE` respondem `ERR ... flash write failed` e o item continua tocando até desligar. Tudo é espelhado em RAM no boot.
- **Efeitos**: `pitchpro` (oitavador/pitch −12..+12 semitons, deslocador por linha de atraso com duas leituras cruzadas), `rotary`, `envfilter`, `comp`; `klon`/`fuzzface` (clipper compartilhado), `sgate` (gate), `doubler` (chorus raso), `basseq` (bandas do GEQ). Só `hum` segue em bypass.
- **CPU**: `AUDIO STAT` mostra `load=<us do último bloco>/<período> max=<pior desde o último STAT>` medido no ISR com o TIMER4. É o número a conferir no pedal (junto com `missed`).
- **Simulador**: `tools/host_pedal.sh` gera `build/host_pedal` (o `tg_main` inteiro em processo host; serial em stdin/stdout, flash em `TG_FLASH_FILE`). O teste `crates/pulsedeck-core/tests/firmware_e2e.rs` roda o código real de upload/sincronia do PulseDeck contra ele.

## Capturas .nam reais: compactação no editor (2026-11-15)

- Os `.nam` atuais (SlimmableContainer A2) trazem uma rede "Lite" de 3 canais com **23 camadas e núcleos de 6/15**: ~1730 multiplicações por amostra, blob de ~4,9 kB, 76 kB de histórico. Pela contagem de instruções do laço interno compilado para pi32v2 (6 por multiplicação + ~80 por camada) isso dá ~12 mil instruções por amostra, contra 7500 ciclos disponíveis a 360 MHz: **não cabe em tempo real neste player em C num núcleo**. O firmware por isso recusa (LOAD rc=-100) acima de `kMaxMacs = 700`.
- O firmware informa seus limites com `CAPS a2l_macs=700 a2l_arena=16384 a2l_layers=32 a2l_blob=4096 ir_taps=512`.
- O PulseDeck (`crates/pulsedeck-core/src/a2l/distill.rs`) lê o `CAPS` e, se a captura for mais pesada, **treina uma rede menor** (3 canais, 10 camadas de núcleo 3, dilatações 1..512: 438 multiplicações, 1532 B) para imitar a original em sinais sintéticos de guitarra (destilação por Adam). É uma aproximação: o erro (ESR) contra a original é mostrado. No arquivo de teste `FNDR BFSR VB Edge WRM2 CAB FREE.nam`: ESR ≈ 0,09 com 12000 passos (≈ 1 min 50 s num núcleo), 0,16 com 3000.
- Linha de comando: `cargo run -p pulsedeck-core --example nam2a2l -- entrada.nam saida.a2l --macs 700 [--steps 8000] [--quality]`.
- O player foi reescrito com histórico espelhado e canais intercalados (passo constante, sem teste de volta), mantendo ≈100 dB de SNR contra float64.
