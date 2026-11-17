#!/bin/bash
# Flash build/tankg_app_mypedal.bin into the Tank-G app area (0x4000..0x7B000) with full verification. NOT TESTED ON HARDWARE.
#   tools/flash_tankg.sh check              # offline checks only, touches no device (safe anywhere)
#   tools/flash_tankg.sh write              # write + read back whole 1 MiB + compare (run in a REAL terminal: needs sudo)
#   tools/flash_tankg.sh restore            # put the original app area (from your backup) back + verify
# The pedal must already be in UBOOT (disk "WL82 UBOOT1.00"). Nothing outside 0x4000..0x7B000 is ever written.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
. "$HERE/tools/env.sh"   # JL_UBOOT_TOOL / JL_PYTHON from the environment or tools/local.env
JL=$JL_UBOOT_TOOL; PY=$JL_PYTHON
UR=$HERE/tools/jlfs       # JLFS helpers (decrypt / CRC check) used by the consistency checks
BK=${BK:-$HOME/backup_tankg}
IMG=$HERE/build/tankg_app_mypedal.bin
ORIG=$BK/app_area_0x4000_0x7b000.bin
OUT=${OUT:-$BK/flash_run_$(date +%Y%m%d_%H%M%S)}
APP_OFF=0x4000; APP_LEN=$((0x77000)); FLASH_LEN=$((0x100000))
die() { echo "ABORT: $*" >&2; exit 1; }

offline_checks() {
  echo "== offline checks"
  [ -f "$IMG" ] || die "missing $IMG (run tools/build.sh + tools/pack.py)"
  [ "$(stat -c %s "$IMG")" -eq $APP_LEN ] || die "image size != 0x77000"
  (cd "$HERE/build" && sha256sum -c tankg_app_mypedal.bin.sha256 >/dev/null) || die "sha256 of image does not match build/*.sha256"
  for f in backup_a.bin backup_b.bin; do [ "$(stat -c %s "$BK/$f")" -eq $FLASH_LEN ] || die "$BK/$f is not 1 MiB"; done
  cmp -s "$BK/backup_a.bin" "$BK/backup_b.bin" || die "backup_a != backup_b"
  "$PY" -I - "$BK" "$IMG" "$UR" <<'PY' || die "image/backup consistency check failed"
import sys,os; bk,img,ur=sys.argv[1:]; sys.path.insert(0,ur)
from decrypt_app import decrypt
from repack_app import check_dir
a=open(f'{bk}/backup_a.bin','rb').read(); o=open(img,'rb').read(); orig=open(f'{bk}/app_area_0x4000_0x7b000.bin','rb').read()
assert orig==a[0x4000:0x7b000], 'saved original app area != backup_a[0x4000:0x7B000]'
assert len(o)==0x77000
by,p=check_dir(decrypt(o,0x980F,0)); assert not p, p
by0,p0=check_dir(decrypt(orig,0x980F,0)); assert not p0, p0
d=decrypt(o,0x980F,0); e=by['app.bin']; assert d[e['off']:e['off']+4]==bytes.fromhex('04818000'), 'app.bin entry'
print('   image JLFS CRCs ok; original app area == backup_a[0x4000:0x7B000]; entry starts goto+4;rts')
PY
  echo "   image sha256 $(cut -d' ' -f1 "$HERE/build/tankg_app_mypedal.bin.sha256")"
  echo "   backup_a sha256 $(sha256sum "$BK/backup_a.bin" | cut -d' ' -f1)"
  echo "== offline checks OK"
}

find_dev() {
  local d; d=$(lsblk -dno PATH,VENDOR,MODEL 2>/dev/null | awk '/WL82/ && /UBOOT/ {print $1}')
  [ -n "$d" ] || die "no 'WL82 UBOOT' disk found. Put the pedal in UBOOT first (serial 'UBOOT' / SysEx F0 22 24 35 7D F7)."
  [ "$(echo "$d" | wc -l)" -eq 1 ] || die "more than one WL82 UBOOT disk: $d"
  echo "$d"
}
SYSROOT=${SYSROOT:-/sys}; DEVROOT=${DEVROOT:-/dev}
# jluboottool talks SCSI-generic: it needs the /dev/sgN node of the WL82 UBOOT disk, not /dev/sdX.
# 1) map sdX -> sgN through sysfs (loading the 'sg' module if needed), 2) create the node if missing (mknod c 21 N).
sg_name_for() {  # $1 = /dev/sdX ; prints sgN or fails
  local base d v try; base=$(basename "$1")
  for try in 1 2; do
    for d in "$SYSROOT"/block/"$base"/device/scsi_generic/sg*; do [ -e "$d" ] && { basename "$d"; return 0; }; done
    for d in "$SYSROOT"/class/scsi_generic/sg*; do
      [ -e "$d" ] || continue; v=$(cat "$d/device/vendor" 2>/dev/null)
      case "$v" in *WL82*) basename "$d"; return 0;; esac
    done
    [ "$try" = 1 ] && { sudo modprobe sg 2>/dev/null; sleep 1; }
  done
  return 1
}
find_sg_dev() {  # $1 = /dev/sdX ; prints /dev/sgN (node verified/created)
  local sg
  if ! sg=$(sg_name_for "$1"); then
    sg=sg0; echo "   WARNING: could not map $1 to an sg node via sysfs; falling back to /dev/sg0 (the identity gate below still protects the write)" >&2
  fi
  local node="$DEVROOT/$sg" n=${sg#sg}
  if [ ! -e "$node" ]; then echo "   creating $node (mknod c 21 $n)" >&2; sudo mknod "$node" c 21 "$n" || die "mknod $node failed"; fi
  [ -c "$node" ] || die "$node exists but is not a character device"
  [ "$(stat -c %t "$node")" = "15" ] || die "$node is not major 21 (sg)"
  echo "$node"
}
# jl <timeout_s> "cmd" ... ; a timeout stops a hung tool instead of waiting forever
jl() { local t=$1; shift; sudo timeout "$t" "$PY" "$JL/jluboottool.py" --chip wl82 --device "$DEV" "$@"; }

# identity gate: the pedal in UBOOT must be the one we backed up (first 4 KiB of flash identical)
identity_gate() {
  jl 60 "read 0 0x1000 $OUT/id.bin" "exit" >/dev/null 2>&1 || die "cannot read the pedal through $DEV within 60 s (sg node ok? pedal in UBOOT? sudo ok?)"
  sudo chown "$USER:$USER" "$OUT/id.bin" 2>/dev/null
  cmp -s "$OUT/id.bin" <(head -c 4096 "$BK/backup_a.bin") || die "first 4 KiB differ from your backup: this is NOT the backed-up pedal (or its flash changed). Stop."
  echo "   identity ok: first 4 KiB of flash == backup_a"
}

verify_full() {  # $1 = expected app-area file ; reads whole flash and compares
  jl 600 "read 0 0x100000 $OUT/after.bin" "exit" || die "readback failed or timed out"
  sudo chown "$USER:$USER" "$OUT/after.bin" 2>/dev/null
  [ "$(stat -c %s "$OUT/after.bin")" -eq $FLASH_LEN ] || die "readback is not 1 MiB"
  cmp -s <(dd if="$OUT/after.bin" bs=4096 skip=4 count=$((APP_LEN/4096)) status=none) "$1" || { echo "MISMATCH in app area 0x4000..0x7B000"; return 1; }
  cmp -s <(head -c $((0x4000)) "$OUT/after.bin") <(head -c $((0x4000)) "$BK/backup_a.bin") || { echo "MISMATCH: bytes 0..0x3FFF (SPL) changed!"; return 1; }
  cmp -s <(tail -c +$((0x7B000+1)) "$OUT/after.bin") <(tail -c +$((0x7B000+1)) "$BK/backup_a.bin") || { echo "MISMATCH: bytes 0x7B000..0xFFFFF changed!"; return 1; }
  echo "   VERIFIED: app area == $(basename "$1"); 0..0x3FFF and 0x7B000..0xFFFFF identical to backup_a"
}

confirm() { [ "${YES:-}" = "GRAVAR" ] && return 0; read -r -p "$1  Type GRAVAR to continue: " a; [ "$a" = "GRAVAR" ] || die "not confirmed"; }

case "${1:-}" in
  check) offline_checks ;;
  write)
    offline_checks; SD=$(find_dev) || exit 1; DEV=$(find_sg_dev "$SD") || exit 1; mkdir -p "$OUT"; echo "== disk $SD  ->  tool device $DEV   logs in $OUT"
    identity_gate
    confirm "This ERASES and rewrites flash 0x4000..0x7B000 of the pedal on $DEV with tankg_app_mypedal.bin (stock app is replaced; the pedal stops being a pedal until restored)."
    jl 900 "write $APP_OFF $IMG" "exit" || die "write failed or timed out (flash may be half-written!) -> run: tools/flash_tankg.sh restore"
    if verify_full "$IMG"; then
      echo "== WRITE VERIFIED. Not rebooted. Power-cycle the pedal (or: sudo $PY $JL/jluboottool.py --chip wl82 --device $DEV 'reset')."
      echo "   Recovery if it does not enumerate: serial/SysEx UBOOT, or tools/flash_tankg.sh restore"
    else
      echo "== VERIFY FAILED. Do NOT reboot. Run: tools/flash_tankg.sh restore"; exit 2
    fi ;;
  restore)
    offline_checks; SD=$(find_dev) || exit 1; DEV=$(find_sg_dev "$SD") || exit 1; mkdir -p "$OUT"; echo "== disk $SD  ->  tool device $DEV   logs in $OUT"
    identity_gate
    confirm "Restoring the ORIGINAL app area (from backup) to 0x4000..0x7B000 on $DEV."
    jl 900 "write $APP_OFF $ORIG" "exit" || die "restore write failed or timed out (do not power off; retry restore)"
    verify_full "$ORIG" && echo "== RESTORE VERIFIED (flash identical to backup_a)." || { echo "== restore verify FAILED; retry restore"; exit 2; } ;;
  *) sed -n 2,7p "$0"; exit 1 ;;
esac
