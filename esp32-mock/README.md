# esp32-mock — bancada de testes do firmware PulseDeck

ESP32 usado como cobaia para desenvolver e validar a arquitetura do firmware open-source antes de portar para o JieLi AC791N (pi32v2). Fala o protocolo de texto do `PROTOCOL_SPEC.md` pela USB serial e **guarda presets, cenas e ajustes na Flash real**.

```
firmware-core/   C++17 puro, sem Arduino/ESP-IDF (é o que será portado)
  include/pd/      crc, store (IBlobStore), model, catalog, codec, footswitch, engine
  src/             parser de comandos, presets/cenas, FSM de footswitch/LED, CRC32 de upload, orçamento CPU/RAM
  tests/           testes no host (store em RAM com injeção de falha)
hal-esp32/src/   camada de hardware: LittleFS (presets+cenas), NVS (GSAVE), botões, serial, LED
tools/           bench_serial.py: valida a persistência na placa real (reset de hardware no meio)
legacy/          mock monolítico anterior (referência)
```

## Uso

```sh
make -C firmware-core test                       # 131 checks no host, com ASan/UBSan, sem placa
pio run -e esp32dev -t upload                    # grava na placa (/dev/ttyUSB0)
~/.local/share/pipx/venvs/platformio/bin/python tools/bench_serial.py   # grava, reseta e confere
```
Atenção: `bench_serial.py` começa com `FACTORYRESET` (apaga os presets da placa de bancada).

## Persistência

| Dado | Onde | Quando grava |
|---|---|---|
| Preset (cadeia + 4 cenas), 1 registro por slot `p00..p31` | LittleFS, escrita em `pNN.tmp` + `rename` (atômica) | `PSAVE`, `PNAME`, `SNAPSAVE`, `SNAPNAME`, `SNAPDEL` |
| Ajustes globais (`gset`): HPF, GEQ, layout, FSA, FSPRESS, LEDCFG, FSLED, tabela de capturas/IRs, preset atual | NVS | `GSAVE`, `CSAVE`/`ISAVE`/`CDEL`/`IDEL` |

Cada registro tem magic, versão e **CRC32**; um registro corrompido é ignorado (o slot some do `PLIST`, `PLOAD` falha com erro) sem derrubar o boot. Primeiro boot (sem `gset`) semeia "Clean Demo"/"Crunch Demo".
Semântica (PROTOCOL_SPEC §5): `PSAVE` grava a cadeia; `SNAPSAVE` faz leitura-modificação-escrita do registro e **só** substitui a cena (edições não salvas da cadeia não vão para a Flash).

## Footswitches

BOOT (GPIO0) = A; GPIO 32/33/25 para GND = B/C/D (pull-up, debounce 25 ms; pinos sem fio ficam "solto"). Gestos: curto (ao soltar), longo (≥1 s), duplo (2º toque em 300 ms, só se houver ação de duplo). Layout `factory`: toque carrega o preset do banco (toque no ativo = bypass), segurar salva, A+B / C+D troca de banco, B+C segurado: 1 s afinador, 3 s BLE pareamento, 6 s BLE off (só eventos). Layouts `scenes`/`custom`: tabela `FSA` (ações 1..10 do spec). Sem fios, use `FSDO <pedal 0..3> <gesto 0..2>`. Eventos saem como linhas `EVT ...` (enfileiradas entre comandos). O LED da placa (GPIO2) pisca a cada evento.

Comandos só da bancada: `FSDO`, `FSST`, `STORE`, `FACTORYRESET`.

## Limites conhecidos

- Testado de verdade: host (131 checks) e placa (persistência após reset de hardware, EVT via `FSDO`, upload com CRC). **Não testado**: botões físicos, queda de energia no meio de uma escrita (só simulada no host com `failWrites`; a atomicidade depende do `rename` do LittleFS).
- Formatos de resposta seguem o simulador Rust do PulseDeck; campos que o spec marca como não documentados (`PSTAT`, `LVLSUB`, `s=` do `PLIST` como bitmask) são suposições.
- O payload de captura/IR é validado por CRC32 em fluxo e **não é armazenado** (só nome/tamanho no slot).
- Sem BLE, sem áudio/DSP. Custo/memória dos efeitos são números inventados para exercitar o orçamento (CPU 1000 ticks, arena 48 KiB).
- Para o AC791N: reimplementar só `IBlobStore` (área VM do flash), entrada de botões e serial; o core não usa heap por comando além de buffers curtos e mantém os structs grandes fora da pilha.

## Atualização: core sem STL
`firmware-core` agora é livre de STL/exceções/RTTI (`tokens.h`), com 135 checks no host (`make test`, `make nostl`) e compila para pi32v2 (ver `../firmware-tankg`). Esta versão ainda precisa ser regravada no ESP32 e re-testada com `tools/bench_serial.py` (a porta estava ocupada pelo PulseDeck).

## Catálogo de efeitos
`firmware-core/src/catalog.cpp` espelha `protocol.json` (`effects.table`): 37 tipos, ids 1..39 (6 e 24 são ids aposentados), nomes/faixas/defaults do protocolo. `cost`/`mem` NÃO estão no protocolo: gate, boost, overdrive, ir, delay e reverb2 mantêm os valores históricos da bancada; os demais são **estimativas** até medir no pedal. `tools/check_catalog.py` (rodado por `make test`) falha se o catálogo divergir do `protocol.json`.
