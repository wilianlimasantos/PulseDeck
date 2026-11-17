# Serial protocol

USB CDC-ACM (the open firmware enumerates as `1209:0001`), nominal 115200 baud, one ASCII command per line, one reply line
(or a multi-line reply ending in a known terminator). Lines starting with `@` are unsolicited and never a reply. The full,
machine-readable description (timeouts, ranges, upload formats) is `protocol.json`; this page lists what the open firmware does.

## State
| Command | Reply |
|---|---|
| `PING` | `PONG tankg_fw ...` (identifies the pedal) |
| `PLIST` | `PRE <slot> <blocks> ... '<name>' ...` lines, then `PLIST end current=<slot>` |
| `PLOAD <slot>` / `PSAVE <slot> <name>` / `SAVE` | load / store a preset (`SAVE` = current slot) |
| `FXS` | the chain: `FX <P|Q><pos> id=<n> <type> on=<0|1> p=<v,...> ...`, then `FXS amp=`, `FXS cpu=`, `FXS peak=` |
| `FXT` | effect catalogue with parameter names, ranges and defaults |
| `LVL` | levels and state; `c=<generation>` changes on every edit (the editor polls it every 250 ms) |
| `CAPS` | `CAPS a2l_macs=<n> ...`: capture-player limits **measured on the chip** (the editor compacts captures to fit) |

## Editing
`FXA <P|Q> <pos> <type>`, `FXR <id>`, `FXM <id> <P|Q> <pos>`, `FXE <id> <0|1>`, `FXP <id> <param> <value>`,
`AMP <0|1>`, `AMPEQ <vol> <bass> <mid> <treble> <presence> <gain>` (dB x 10), `FXIR <id> <slot>`.

## Uploads (hex chunks, stop-and-wait, CRC32 at the end)
- Capture: `LDBEGIN <bytes>`, `LDB <offset> <hex>`..., `LDEND <crc32>` -> `LOAD rc=<0|-4|-100> ...`, then `CSAVE <slot> <name>`.
  `rc=-100` = too heavy for real time on this chip (see `CAPS`). `CLIST`, `CLOAD`, `CDEL`.
- Impulse response: `IRBEGIN <samples> <gain_mdB> 48`, `IRB ...`, `IREND <crc32>`, `ISAVE <slot> <name>`. `ILIST` also lists the
  8 built-in cabinets (size 0). `IDEL`.

## Diagnostics (open firmware only)
`AUDIO OFF|A2L|TONE|STAT|CFG|SET`, `A2LBENCH`, `CPUBENCH`, `KNOBS`, `FSREAD`, `PORTS`, `LEDBIT`, `LEDRAW`, `RGB`, `PANEL MON 1`,
`UBOOT` (back to the ROM loader).
`AUDIO STAT` reports `missed=` (lost audio blocks), `load=<us>/<period> max=<us>` (DSP time per block) and `cpu1=` (second core).
