#!/bin/bash
# Build the virglrenderer fork and this QEMU into one self-contained folder.
# Runs in MSYS2 UCRT64 (MSYSTEM=UCRT64 bash -lc ...). Debug info is kept so a
# hang can be inspected with gdb (thread apply all bt).
#   VIRGL_SRC=/e/Development/winq-emu-virglrenderer OUT=/e/AgentData/winq-build JOBS=6 scripts-winq/build-all.sh
set -euo pipefail
QEMU_SRC=$(cd "$(dirname "$0")/.." && pwd)
VIRGL_SRC=${VIRGL_SRC:?path to the virglrenderer fork}
OUT=${OUT:?output folder}
JOBS=${JOBS:-6}
PREFIX=$OUT/prefix
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig

echo "== virglrenderer"
meson setup "$OUT/virgl" "$VIRGL_SRC" --prefix="$PREFIX" --buildtype=debugoptimized \
    -Dvenus=true -Dvideo=true -Dtests=false --reconfigure >/dev/null
ninja -C "$OUT/virgl" -j "$JOBS"
ninja -C "$OUT/virgl" install >/dev/null

echo "== qemu"
mkdir -p "$OUT/qemu"
cd "$OUT/qemu"
[[ -f build.ninja ]] || "$QEMU_SRC/configure" --target-list=x86_64-softmmu --prefix="$PREFIX" \
    --enable-whpx --enable-opengl --enable-virglrenderer --enable-sdl --enable-slirp --enable-png \
    --disable-docs --disable-plugins --enable-debug-info --disable-werror
ninja -j "$JOBS" qemu-system-x86_64.exe qemu-img.exe

echo "== bundle"
BIN=$OUT/bin
mkdir -p "$BIN/share"
cp qemu-system-x86_64.exe qemu-img.exe "$BIN/"
cp "$PREFIX"/bin/*.dll "$BIN/" 2>/dev/null || true
# Keymaps (VNC needs them): the ready-made ones from the source tree; the
# build folder only has generated ones when xkbcommon is present.
mkdir -p "$BIN/share/keymaps"
cp "$QEMU_SRC"/pc-bios/keymaps/[a-z]* "$BIN/share/keymaps/"
rm -f "$BIN/share/keymaps/meson.build"
for f in bios-256k.bin kvmvapic.bin linuxboot_dma.bin vgabios-virtio.bin vgabios-stdvga.bin efi-virtio.rom efi-e1000e.rom; do
    cp "$QEMU_SRC/pc-bios/$f" "$BIN/share/" 2>/dev/null || true
done
# UEFI firmware (OVMF), so a build is self-contained: code, Secure Boot code,
# the vars template (copy it per VM) and the licences.
for f in edk2-x86_64-code.fd edk2-x86_64-secure-code.fd edk2-i386-vars.fd; do
    bzip2 -dc "$QEMU_SRC/pc-bios/$f.bz2" > "$BIN/share/$f"
done
cp "$QEMU_SRC/pc-bios/edk2-licenses.txt" "$BIN/share/"
# EGL for the windowless GL displays (egl-headless, dbus gl=on): libepoxy
# loads it at run time, so ldd cannot see it. ANGLE comes from the
# mingw-w64-ucrt-x86_64-angleproject package; the ldd pass below then
# picks up its own dependencies (libstdc++ and friends).
cp /ucrt64/bin/libEGL.dll /ucrt64/bin/libGLESv2.dll "$BIN/"
# Every MinGW DLL the binaries need, found with ldd (system DLLs excluded).
for exe in "$BIN"/qemu-system-x86_64.exe "$BIN"/*.dll; do
    ldd "$exe" | awk '/ucrt64/ {print $3}'
done | sort -u | while read -r dll; do cp -n "$dll" "$BIN/"; done
echo "built: $BIN"
