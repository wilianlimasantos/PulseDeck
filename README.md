# PulseDeck

Editor desktop open-source (Linux e Windows) para a pedaleira UltraRig (firmware `M5.x`, USB VID `0x3654`).
Tauri 2 + Rust (`serialport`) no backend, React + TypeScript + Tailwind no frontend.

## Estrutura
- `protocol.json` — definição do protocolo; é embutida no binário e lida em tempo de execução (VID, baud, limites de linha, timeouts, catálogo de efeitos). O catálogo real vem do `FXT` da pedaleira.
- `crates/pulsedeck-core` — protocolo, sessão serial (1 comando em voo, linhas `@` roteadas), parsers, conversor `.nam` → `A2L1`, preparo de IR `.wav`, upload com CRC32, simulador de pedaleira.
- `src-tauri` — comandos Tauri e sincronização de estado.
- `src` — interface React.

## Desenvolvimento
```sh
npm install
npm run tauri dev            # com a pedaleira
PULSEDECK_SIM=1 npm run tauri dev   # modo demo com a pedaleira simulada
cargo test --workspace && npm test
```
Linux precisa de `webkit2gtk-4.1`, `libayatana-appindicator` e acesso à porta serial (grupo `uucp`/`dialout`).

## Limitações conhecidas
- O protocolo foi derivado por leitura estática; nada foi validado contra uma pedaleira real. O conversor A2L1 reproduz byte a byte a referência em modelos sintéticos, mas não foi testado com `.nam` reais nem com o firmware.
- Não há edição de ramos paralelos (`FXL`/`FXMIX`) na interface.
- Backup: presets são lidos/reconstruídos via `PLOAD`/`FXS`/`SNAP` e `PNEW`/`FXA`/`FXP`/`PSAVE`/`SNAPSAVE`; capturas e IRs via `CREAD`/`IREAD`. As configurações globais (GEQ, FSA, LEDCFG...) são gravadas como o texto devolvido pela pedaleira e reenviadas na restauração em melhor esforço (o protocolo não documenta o formato de leitura de todas); a restauração lista avisos por passo. Nada disso foi validado em hardware real.
- Exportar backup carrega cada preset na pedaleira (descarta edições não salvas) e restaura o preset/cena atuais no fim.
- Atalhos: `1`–`4` trocam de cena, `←`/`→` navegam entre presets (ignorados em campos de texto e seletores).

## Idiomas / Languages

- Interface em **português (pt-BR)** e **inglês**: `src/i18n/pt-BR.ts` (referência, define as chaves) e `src/i18n/en.ts` (tipado: faltar uma chave não compila). Use `useT()` nos componentes e `t()` fora deles.
- Primeira abertura: usa o idioma do sistema (`navigator.language` começando com `pt` → pt-BR, senão en). A escolha é salva em `localStorage` (`pulsedeck.lang`).
- Troca instantânea em **≡ Gerenciar → Idioma / Language**, sem recarregar nem desconectar a pedaleira.
- Termos de áudio universais ficam em inglês nos dois idiomas (Bypass, Drive, Delay, Reverb, Full Rig, Amp Only, IR...). Os nomes dos parâmetros do firmware (`umbral_db`, `mezcla_pct`...) têm tradução em `p.*`; os nomes dos efeitos são os do firmware.
- Mensagens vindas do Rust (etapas de progresso, erros, avisos) são escritas em português; em inglês são traduzidas por padrões em `src/i18n/backend.ts`. Mensagens que não casam com nenhum padrão aparecem em português. Textos já exibidos (um toast, uma etapa de progresso) não mudam de idioma depois de mostrados; o resto da interface muda na hora.

## Configurações Globais

Menu **≡ Gerenciar → Configurações Globais** (EQ / Pedais / LEDs). Os valores são lidos da pedaleira ao abrir e cada ajuste é aplicado na hora (USB ou Bluetooth); **Salvar na Memória** envia `GSAVE`, sem o qual tudo se perde ao desligar.

- **GEQ:** 5 bandas (100 Hz – 7 kHz, ±12 dB) em sliders verticais, com *Resetar EQ* e HPF de entrada. O firmware **não tem bypass do EQ**: o botão zera as bandas e guarda os valores neste computador para voltar.
- **FSA / FSMODE:** modos prontos (Fábrica, Modo Cena, Modo Preset, Stompbox) definidos em `protocol.json` (`commands.global_settings.host_ui`). As 12 ações `FSA` são enviadas antes de `FSMODE layout=`. Edição ação por ação não existe nesta versão.
- **LEDCFG / FSLED:** brilho, afinador, tap, medidor, progresso, animação e respiração; por footswitch, cor automática ou personalizada (ativo/inativo) com pré-visualização do anel.
- Não existe comando de volume master ou cab global no protocolo. SPILL, GLIDE, SPUR e LIM continuam só no backup.
- O backup completo já guardava os globais; agora GEQ (uma linha com 5 bandas), FSA (sem `id=`), FSMODE e FSLED são convertidos nos comandos de escrita corretos ao restaurar.
- Formatos de resposta vêm do protocolo documentado e do simulador; **não foram verificados em hardware**.

## Bluetooth LE

- A pílula de conexão (canto superior esquerdo) abre um painel **USB / Bluetooth**. No modo Bluetooth: **Procurar Pedaleira**, lista com intensidade de sinal (RSSI) e **Conectar**; o ícone de Bluetooth aparece na pílula quando conectado. Perda de conexão não trava o app: a pílula volta a "Bluetooth desconectado" com o motivo.
- Transporte (`crates/pulsedeck-core/src/ble.rs`, `btleplug`): mesmo protocolo de linhas e telemetria `@` do USB sobre o Nordic UART Service (`6e400001-…`, documentado em `protocol.json`). A busca filtra por nome (UltraRig, Tank-G, Tank-B, M-VAVE) ou pelos serviços `6e400001`, `0000ae00` e `0000fff0`; as características de escrita e de notificação são descobertas automaticamente. Escritas são fatiadas em 20 bytes (ou MTU−3, lido do comando `BLE`) com pausa de 12 ms entre pacotes.
- **Capturas (.nam), IRs (.wav), backup/restauração, biblioteca e renomear slot são bloqueados por Bluetooth** com aviso para usar o cabo USB: são muitos KB em linhas confirmadas uma a uma e uma queda no meio deixaria o slot incompleto.
- **Não verificado em hardware.** Só o Nordic UART é documentado para esta pedaleira; `0000ae00`/`0000fff0` e os nomes extras são palpites genéricos. Sem pareamento, a pedaleira responde `ERR <cmd> pair` (só leitura); o app lê a característica de pareamento para pedir ao sistema o enlace cifrado. No Linux é preciso o BlueZ ativo (D-Bus).

## Afinador

- Atalho `T` (fora de campos de texto) ou botão "Afinador" na barra superior; `Esc` fecha.
- **Pedaleira:** `TUNER <on> <mute> <ref>` liga o modo afinador (e silencia a saída) e `TUNE` é consultado em loop (um comando em voo por vez) para nota, Hz e cents. O formato de `TUNE` foi derivado do analisador da página de referência, não verificado em hardware (documentado em `protocol.json`). Se a pedaleira sair do modo sozinha (`TUNE off`), o overlay fecha.
- **Áudio do sistema:** detector YIN (`src/pitch.ts`) sobre `getUserMedia`, com escolha da entrada de áudio e referência A4 de 432 a 445 Hz (a pedaleira aceita referência inteira). Depende de o webview liberar o microfone: no Linux (WebKitGTK) isso não foi verificado.
- Sem pedaleira conectada, o afinador funciona só com o áudio do sistema.
