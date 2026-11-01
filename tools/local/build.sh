#!/bin/bash
# Saints Row PS5 build in WSL (Ubuntu 24.04), keeping what it can between builds:
#
#   wsl -d Ubuntu-24.04 -u root -- bash /mnt/c/.../sr-ps5/tools/local/build.sh [repo] [out app folder]
#
# - tools/local/toolchain.sh builds the PS5 toolchain once ($W/tri).
# - The framework is cloned and set up once ($W/fw-pristine); each build patches
#   a fresh copy and syncs it into $W/ws/fw by content, so ninja and ccache only
#   rebuild what changed.
# - Codegen runs again only when its inputs change (cached in $W/gen/<hash>).
# - The games' code comes from your own files: SR1_XEX / SR1_XEXP (Saints Row's default.xex and
#   Title Update 1's default.xexp) and SR2_XEX / SR2_XEXP (Saints Row 2's, Title Update 3's),
#   checked by their SHA-1s; on this PC, Extras/sr1/ and Extras/sr2/ next to the repo by default.
#   Nothing leaves the PC.
set -eo pipefail

repo=$(realpath "${1:-$(dirname "$0")/../..}")
app_out=${2:-}
W=${SR_WORK:-/root/srbuild}
TITLE_ID=${TITLE_ID:-PPSA54107}
GTA4_PS5_REPO=https://github.com/elripalda/gta4-ps5
CD_PC_REPO=https://github.com/saulob/crackdown-xbox360-recompiled
CD_PC_COMMIT=33ab1b632e38cb5c1c39a3d56c479537b9e0d185
# the discs' default.xex (Saints Row 545107D1, Saints Row 2 545107FC) and their title updates'
# default.xexp (Saints Row TU1, Saints Row 2 TU3)
XEX1_SHA1=c2646093ff3926141fcf4fd630e876873d1caece
XEX2_SHA1=56cba9189d6ded13a60fe4b9ac4c29b2ab34b0f8
XEXP1_SHA1=b7bc1675d36340a2f931a85e088f2b2010b3ea41
XEXP2_SHA1=cd08ab77b19f24303853f6fa860c6d4feee9621a
SR1_XEX=${SR1_XEX:-$repo/../sr1/default.xex}
SR2_XEX=${SR2_XEX:-$repo/../sr2/default.xex}
SR1_XEXP=${SR1_XEXP:-$repo/../sr1/default.xexp}
SR2_XEXP=${SR2_XEXP:-$repo/../sr2/default.xexp}
for g in 1 2; do
  for k in XEX XEXP; do
    x=SR${g}_$k; h=${k}${g}_SHA1
    [ -f "${!x}" ] || { echo "set $x to your own game's default.${k,,}" >&2; exit 2; }
    echo "${!h}  ${!x}" | sha1sum -c --quiet || { echo "${!x} isn't Saints Row $g's default.${k,,}" >&2; exit 2; }
  done
done
export CCACHE_DIR=$W/ccache
step() { echo "==> $(date +%T) $*"; }

[ -d "$W/tri/.deps/native/ps5-payload-sdk" ] || { echo "run tools/local/toolchain.sh first" >&2; exit 2; }
mkdir -p "$W/ws" "$W/gen" "$CCACHE_DIR"
ccache -M 30G > /dev/null

# ---- the workspace: the repo's files (not .git), with tri, fw and out kept ----
step "workspace"
rsync -a --delete --exclude .git --exclude /tri --exclude /fw --exclude /out --exclude /cd \
  --exclude /build-rexglue --exclude /private "$repo/" "$W/ws/"
cd "$W/ws"
ln -sfn "$W/tri" tri

# ---- framework: pristine once, then patched into fw-new, synced into fw ----
if [ ! -f "$W/fw-pristine/.setup-done" ]; then
  step "framework clone and setup (once)"
  rm -rf "$W/fw-pristine"
  git clone --depth 1 "$GTA4_PS5_REPO" "$W/fw-pristine"
  (cd "$W/fw-pristine" && bash ./setup.sh)
  touch "$W/fw-pristine/.setup-done"
fi
if [ ! -d "$W/cd/patches" ]; then
  step "Crackdown PC recomp patches (once)"
  rm -rf "$W/cd" && git init -q "$W/cd" && git -C "$W/cd" remote add origin "$CD_PC_REPO"
  git -C "$W/cd" fetch -q --depth 1 origin "$CD_PC_COMMIT" && git -C "$W/cd" checkout -q FETCH_HEAD
fi
step "framework patches"
rm -rf "$W/fw-new"
mkdir -p "$W/fw-new"
cp -a "$W/fw-pristine/." "$W/fw-new/"  # (the contents: fw-pristine may be a link to a shared one)
python3 ci/pc_patches.py "$W/fw-new" "$W/cd" > "$W/pc_patches.txt"
python3 ci/fw_perf_fixes.py "$W/fw-new" "$W/cd" > "$W/fw_perf_fixes.txt"
python3 ci/patch_framework.py "$W/fw-new" > "$W/patch_framework.txt"
M=glue/rexglue-sdk-main/sr-recomp
rm -rf "$W/fw-new/$M"
cp -r sr-recomp "$W/fw-new/$M"

# ---- codegen, per game, only when its inputs changed ----
# (patch_framework.py only touches the codegen to dump the image: not an input)
python3 ci/sr_codegen.py "$W/fw-new" > "$W/sr_codegen.txt"
REXGLUE=
for g in 1 2; do
  xex=SR${g}_XEX; xexp=SR${g}_XEXP; xsha=XEX${g}_SHA1; psha=XEXP${g}_SHA1
  gen_key=$( (git -C "$W/fw-pristine" rev-parse HEAD; cat ci/pc_patches.py ci/fw_perf_fixes.py ci/sr_codegen.py     ci/mark_share_registers.py sr-recomp/p$g/sr${g}_config.toml sr-recomp/p$g/sr${g}_manifest.toml; echo "${!xsha} ${!psha}") |
    sha256sum | cut -c1-16)
  if [ ! -d "$W/gen/$gen_key/generated" ]; then
    if [ -z "$REXGLUE" ]; then
      step "rexglue CLI (host)"
      rsync -a --checksum --delete "$W/fw-new/" "$W/fw-host/"
      cmake -S "$W/fw-host/glue/rexglue-sdk-main" -B "$W/build-rexglue" -G Ninja         -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19         -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache         -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3         -DCMAKE_CXX_STANDARD=23 -DSR_NO_GTA4_NATIVE=ON         -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_GTA4_RECOMP=OFF         -DREXGLUE_BUILD_TESTS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "$W/rexglue-configure.txt"
      ninja -C "$W/build-rexglue" rexglue > "$W/rexglue-build.txt"
      REXGLUE=$(find "$W/build-rexglue" "$W/fw-host/glue/rexglue-sdk-main/out" -name rexglue -type f -perm -u+x 2>/dev/null | head -1)
    fi
    step "codegen (Saints Row $g)"
    H=$W/fw-host/$M/p$g
    rm -rf "$H/generated" "$H/assets" && mkdir -p "$H/assets" "$W/private"
    cp "${!xex}" "$H/assets/default.xex" && cp "${!xexp}" "$H/assets/default.xexp"
    REX_DUMP_IMAGE=$W/private/image$g.bin "$REXGLUE" --log-level info -f codegen "$H/sr${g}_manifest.toml" > "$W/codegen$g.txt" 2>&1 ||
      { tail -80 "$W/codegen$g.txt"; exit 1; }
    RBW=$W/fw-host/glue/rexglue-sdk-main/gta4-recomp/tools/read_before_write.py
    sed "s/gta4_recomp\.\*\.cpp/sr${g}_recomp.*.cpp/" "$RBW" > "$W/read_before_write$g.py"
    python3 "$W/read_before_write$g.py" "$H/generated" > "$W/read_before_write$g.txt" 2> "$W/rbw-total$g.txt"
    python3 ci/mark_share_registers.py "$W/read_before_write$g.txt" "$H/sr${g}_config.toml" "$W/private/image$g.bin"
    rm -rf "$H/generated"
    "$REXGLUE" --log-level info -f codegen "$H/sr${g}_manifest.toml" > "$W/codegen$g-pass2.txt" 2>&1 ||
      { tail -80 "$W/codegen$g-pass2.txt"; exit 1; }
    rm -rf "$H/assets"
    mkdir -p "$W/gen/$gen_key"
    mv "$H/generated" "$W/gen/$gen_key/generated"
    cp "$H/sr${g}_config.toml" "$W/gen/$gen_key/sr${g}_config.toml"
    # (only the latest few)
    ls -1t "$W/gen" | tail -n +7 | while read -r old; do rm -rf "$W/gen/$old"; done
  fi
  cp -r "$W/gen/$gen_key/generated" "$W/fw-new/$M/p$g/generated"
  cp "$W/gen/$gen_key/sr${g}_config.toml" "$W/fw-new/$M/p$g/sr${g}_config.toml"
done

# ---- the PS5 program ----
step "sync framework (by content)"
rsync -a --checksum --delete "$W/fw-new/" "$W/ws/fw/"
rm -rf "$W/fw-new"
SDK=$W/tri/.deps/native/ps5-payload-sdk
if [ ! -f out/ps5/build.ninja ]; then
  step "configure (PS5)"
  cmake -S fw/ps5 -B out/ps5 -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$PWD/fw/ps5/cmake/ps5-toolchain.cmake -DPS5_PAYLOAD_SDK=$SDK \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DSR_NO_GTA4_NATIVE=ON -DREXGLUE_ENABLE_FIDELITYFX=OFF \
    -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF \
    -DSDL_OSS=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF \
    -DSDL_SNDIO=OFF -DSDL_HIDAPI=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF \
    -DSDL_VULKAN=OFF -DSDL_DUMMYVIDEO=ON -DSDL_DUMMYAUDIO=ON -DSDL_OFFSCREEN=OFF > "$W/configure.txt" 2>&1
fi
step "build (PS5)"
mkdir -p out
if ! ninja -C out/ps5 sr1 sr2 > out/build.txt 2>&1; then
  grep -E "error:|FAILED" out/build.txt | head -40
  exit 1
fi
tail -1 out/build.txt
step "package"
bash ci/ps5_package_sr.sh out/ps5 tri fw "$TITLE_ID" > out/package.txt 2>&1 || { tail -40 out/package.txt; exit 1; }
for g in 1 2; do
  llvm-nm-19 -n -C --defined-only out/ps5/ps5-link/build/sr$g.elf | grep -i ' t ' | gzip -9 > out/symbols$g.txt.gz || true
done
app=out/ps5/ps5-app/$TITLE_ID
# The user's own art and music (kept out of git): Extras/app-assets/ next to the repo, laid out as the
# app folder (sce_sys/icon0.png, pic0.dds, pic1.dds, snd0.at9; hub/background.jpg, hub/music/).
assets=${SR_APP_ASSETS:-$repo/../app-assets}
[ -d "$assets" ] || assets=$repo/package/app-assets
if [ -d "$assets" ]; then
  cp -r "$assets/." "$app/"
  step "app assets from $assets"
fi
ls -l "$app/eboot.bin"
if [ -n "$app_out" ]; then
  # the app's own files over the folder (a new one, or an installed one: its game, DLC, saves,
  # UserData and menu.ini are left as they are)
  mkdir -p "$app_out"
  cp -r "$app/." "$app_out/"
  mkdir -p "$repo/../build-logs" && cp out/symbols?.txt.gz "$repo/../build-logs/" 2>/dev/null || true
  step "app copied to $app_out"
fi
step "done"
