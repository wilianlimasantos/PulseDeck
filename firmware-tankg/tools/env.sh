# Source me:  . firmware-tankg/tools/env.sh
# Paths come from the environment or from tools/local.env (gitignored, per machine). Defaults: third_party/ in the repository root.
#   PI32V2_LLVM   pi32v2 LLVM release (jvitkauskas/pi32v2-llvm 23.1.5-5)
#   BSP           felucca-bsp checkout (MPL-2.0)
#   JL_UBOOT_TOOL jl-uboot-tool checkout (MIT), JL_PYTHON its Python (flashing only)
_TG_HERE=$(cd "$(dirname "${BASH_SOURCE:-$0}")/.." 2>/dev/null && pwd)
[ -f "$_TG_HERE/tools/env.sh" ] || _TG_HERE=$(pwd)   # sourced interactively by a shell without BASH_SOURCE: run it from firmware-tankg/
[ -f "$_TG_HERE/tools/local.env" ] && . "$_TG_HERE/tools/local.env"
_TG_TP="$_TG_HERE/../third_party"
export PI32V2_LLVM="${PI32V2_LLVM:-$_TG_TP/pi32v2-llvm}"
export BSP="${BSP:-$_TG_TP/felucca-bsp}"
export JL_UBOOT_TOOL="${JL_UBOOT_TOOL:-$_TG_TP/jl-uboot-tool}"
export JL_PYTHON="${JL_PYTHON:-python3}"
export PATH="$PI32V2_LLVM/bin:$PATH"
