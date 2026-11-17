# Building and flashing

## Requirements
- `pi32v2` LLVM/clang release (Apache-2.0) and **felucca-bsp** (MPL-2.0). Point the scripts at them:
  ```sh
  export PI32V2_LLVM=/path/to/pi32v2-llvm-<version>
  export BSP=/path/to/felucca-bsp
  ```
- Python 3, `g++` (host tests), Rust + Node (PulseDeck).
- [jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool) (MIT) for flashing. It downloads JieLi's `wl82loader.bin`, which is
  not redistributed here.

## Firmware
```sh
cd firmware-tankg
. tools/env.sh
tools/build.sh                      # -> build/tankg.bin (raw application)
python3 -I tools/pack.py --template <your app-area backup> --out build/tankg_app_mypedal.bin
tools/flash_tankg.sh check          # offline checks only
tools/flash_tankg.sh write          # real terminal, sudo, asks you to type GRAVAR
tools/flash_tankg.sh restore        # put your original application back
```
**Make a full read-back of your own pedal first** (both copies must match). It contains data unique to your unit: keep it
private and never commit it. `pack.py` builds the container from *your* backup, so the result is also personal: do not
publish built images.

Recovery: the pedal re-enters the ROM loader (`WL82 UBOOT` disk) after two boots that never reach the main loop, or with the
`UBOOT` command; then `tools/flash_tankg.sh restore`.

## Tests (no hardware)
```sh
make -C esp32-mock/firmware-core test
firmware-tankg/tools/host_pedal.sh      # the whole firmware as a host process
cargo test --workspace                  # includes the editor-against-firmware end-to-end test
```

## PulseDeck
```sh
npm install
npm run tauri dev                    # with the pedal
PULSEDECK_SIM=1 npm run tauri dev    # simulated pedal
npm run build && cargo build --release -p pulsedeck
```
Linux: `webkit2gtk-4.1`, `libayatana-appindicator`, and membership of the serial group (`uucp` / `dialout`).
