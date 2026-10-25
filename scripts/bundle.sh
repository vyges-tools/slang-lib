#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Builds slang with Bazel and makes the bundle a Rust (or C) program links:
#   <out>/lib/libvyges_slang.a   one relocatable object, only the `vyges_slang_*` C API global
#   <out>/include/vyges_slang.h, vyges_slang_elab.h
#
# 🔑 WHY ONE PRELINKED, HIDDEN OBJECT. The hermetic toolchain builds slang against its own libc++ —
# which is part of the pin: the C++ library decides sorts inside slang. The program linking this
# crate may already carry another C++ runtime (libstdc++, through
# another `-lib` crate). Two exception runtimes resolving each other's `__cxa_*` would corrupt each
# other's exceptions. So the archive is `ld -r`-linked WITH its libc++/libc++abi/libunwind into one
# object, and every symbol except the C API is made local: the C++ runtime and slang's own symbols
# (and the fmt it carries; a program may link another fmt) are private to it, and no C++ type or
# exception crosses the C boundary (the shim catches everything).
#
# `--whole-archive`, not archive semantics: the archive is kept whole, as a linked slang has it.
#
# 🔑 CROSS: `VYGES_BUNDLE_TARGET=aarch64-unknown-linux-gnu` on an x86_64 host builds for arm64 — the
# same pinned compiler and flags, targeting `@llvm//platforms:linux_aarch64`, and prelinks with the
# `aarch64-linux-gnu-` binutils. Default: the host's own target.
#
# 🔑 MACOS (Apple Silicon): the toolchain compiles against the pinned macOS SDK's libc++ and has
# no static C++ runtime to embed, so the bundle is slang alone, linked against the system
# `libc++` by the final link. It is still ONE object with only the C API global: `ld -r -all_load`
# (the `--whole-archive` of Mach-O) with `-exported_symbols_list`, which in a `-r` link turns every
# other global — the weak inline and template definitions included, Mach-O's COMDATs — into a
# local symbol that no other object can coalesce with.
#
# Usage: [VYGES_BUNDLE_TARGET=<triple>] scripts/bundle.sh <out-dir>   (Linux x86_64/arm64, macOS arm64)
set -euo pipefail

out=${1:?usage: scripts/bundle.sh <out-dir>}
here=$(cd "$(dirname "$0")/.." && pwd)
bazel=${VYGES_BAZEL:-$(command -v bazelisk || command -v bazel || true)}
[ -n "$bazel" ] || { echo "bundle.sh: no bazelisk/bazel on PATH (set VYGES_BAZEL)" >&2; exit 1; }

if [ "$(uname -s)" = Darwin ]; then
  [ "$(uname -m)" = arm64 ] || { echo "bundle.sh: macOS arm64 only" >&2; exit 1; }
  [ -z "${VYGES_BUNDLE_TARGET:-}" ] || [ "$VYGES_BUNDLE_TARGET" = aarch64-apple-darwin ] \
    || { echo "bundle.sh: no cross build for $VYGES_BUNDLE_TARGET from macOS" >&2; exit 1; }
  cd "$here"
  "$bazel" build //:vyges_slang
  archive=$("$bazel" cquery --output=files //:vyges_slang 2>/dev/null | grep '\.a$' | grep -v -- '-exec')
  [ -s "$archive" ] || { echo "bundle.sh: missing or empty archive: $archive" >&2; exit 1; }
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  nm "$archive" 2>/dev/null | awk '$2 == "T" && $3 ~ /^_vyges_slang_/ {print $3}' | sort -u > "$work/keep.txt"
  [ -s "$work/keep.txt" ] || { echo "bundle.sh: no vyges_slang_* symbols in $archive" >&2; exit 1; }
  # The deployment target is .bazelrc's `--macos_minimum_os`.
  ld -r -arch arm64 -platform_version macos 13.3 13.3 -o "$work/vyges_slang.o" \
     -all_load "$archive" -exported_symbols_list "$work/keep.txt"
  extra=$(nm -g --defined-only "$work/vyges_slang.o" | awk '{print $3}' | grep -v '^_vyges_slang_' || true)
  [ -z "$extra" ] || { echo "bundle.sh: symbols left global: $extra" >&2; exit 1; }
  [ "$(lipo -archs "$work/vyges_slang.o")" = arm64 ] || { echo "bundle.sh: not an arm64 object" >&2; exit 1; }
  mkdir -p "$out/lib" "$out/include"
  rm -f "$out/lib/libvyges_slang.a"
  ZERO_AR_DATE=1 libtool -static -o "$out/lib/libvyges_slang.a" "$work/vyges_slang.o"   # no timestamp: reproducible
  cp shim/vyges_slang.h shim/vyges_slang_elab.h "$out/include/"
  cp slang-pin.yaml LICENSE NOTICE "$out/"
  echo "bundle.sh: $out/lib/libvyges_slang.a for aarch64-apple-darwin ($(wc -l < "$work/keep.txt" | tr -d ' ') C entry points)"
  exit 0
fi
[ "$(uname -s)" = Linux ] || { echo "bundle.sh: Linux and macOS only" >&2; exit 1; }

host=$(uname -m)-unknown-linux-gnu
target=${VYGES_BUNDLE_TARGET:-$host}
plat=(); tool=
if [ "$target" != "$host" ]; then
  case "$target" in
    aarch64-unknown-linux-gnu) plat=(--platforms=@llvm//platforms:linux_aarch64); tool=aarch64-linux-gnu- ;;
    *) echo "bundle.sh: no cross build for $target from $host" >&2; exit 1 ;;
  esac
fi
for t in ld objcopy nm readelf ar; do
  command -v "$tool$t" >/dev/null || { echo "bundle.sh: no $tool$t on PATH" >&2; exit 1; }
done

cd "$here"
llvm=@@llvm++llvm+llvm-project
runtimes=("$llvm//libcxx:libcxx.static" "$llvm//libcxxabi:libcxxabi.static" "$llvm//libunwind:libunwind.static")
"$bazel" build "${plat[@]}" //:vyges_slang "${runtimes[@]}"

# The TARGET configuration's archive. Once something is built for the exec (tool) configuration
# too, cquery lists that copy as well — a second line that is not what the program runs.
files() { "$bazel" cquery "${plat[@]}" --output=files "$1" 2>/dev/null | grep '\.a$' | grep -v -- '-exec'; }
archive=$(files //:vyges_slang)
cxx=$(files "${runtimes[0]}"); cxxabi=$(files "${runtimes[1]}"); unwind=$(files "${runtimes[2]}")
# The builtins sit beside the archive in its configuration's output tree. ⚠️ That tree is named after
# `--cpu`, not the platform, so a cross build writes the same `k8-opt` the host build does (Bazel
# rebuilds on the next host build): the archive's ELF machine is checked after the link below.
bin=${archive%/libvyges_slang.a}
builtins=$(find -L "$bin/external/llvm+/runtimes/resource_directory_default" \
             -name libclang_rt.builtins.a | head -1)
for f in "$archive" "$cxx" "$cxxabi" "$unwind" "$builtins"; do
  [ -s "$f" ] || { echo "bundle.sh: missing or empty archive: $f" >&2; exit 1; }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"${tool}nm" "$archive" 2>/dev/null | awk '$2 == "T" && $3 ~ /^vyges_slang_/ {print $3}' | sort -u > "$work/keep.txt"
[ -s "$work/keep.txt" ] || { echo "bundle.sh: no vyges_slang_* symbols in $archive" >&2; exit 1; }

"${tool}ld" -r -o "$work/whole.o" --whole-archive "$archive" --no-whole-archive \
   --start-group "$cxx" "$cxxabi" "$unwind" "$builtins" --end-group
"${tool}objcopy" --keep-global-symbols="$work/keep.txt" "$work/whole.o" "$work/local.o"
# ⛔ **Local symbols are not enough: COMDAT GROUPS are merged by their SIGNATURE NAME**, which
# localising a symbol does not change. An inline or template instance the program also has (a
# boost exception vtable, built against the other C++ runtime) shared one group signature with the
# program's copy; the linker kept the first group it saw and DISCARDED the other — leaving the
# program's references to a symbol in a dropped section (lld: "relocation R_X86_64_PC32 cannot be
# used against symbol 'vtable for boost::exception'"), or, linked the other way round, binding the
# program to this runtime's copy. Dropping the group sections makes every member an ordinary
# section of this object: private, like everything else in it.
"${tool}objcopy" --remove-section=.group "$work/local.o" "$work/vyges_slang.o"

machine=$("${tool}readelf" -h "$work/whole.o" | awk -F: '/Machine:/ {gsub(/^ +/, "", $2); print $2}')
case "$target:$machine" in
  x86_64-*:"Advanced Micro Devices X86-64"|aarch64-*:AArch64) ;;
  *) echo "bundle.sh: built for '$machine', not $target" >&2; exit 1 ;;
esac

# Nothing but the C API may stay global and defined, and no group may remain to merge.
extra=$("${tool}nm" -g --defined-only "$work/vyges_slang.o" | awk '{print $3}' | grep -v '^vyges_slang_' || true)
[ -z "$extra" ] || { echo "bundle.sh: symbols left global: $extra" >&2; exit 1; }
groups=$("${tool}readelf" -g --wide "$work/vyges_slang.o" | grep -c 'COMDAT group' || true)
[ "$groups" = 0 ] || { echo "bundle.sh: $groups COMDAT group(s) left" >&2; exit 1; }

mkdir -p "$out/lib" "$out/include"
rm -f "$out/lib/libvyges_slang.a"
"${tool}ar" rcsD "$out/lib/libvyges_slang.a" "$work/vyges_slang.o"
cp shim/vyges_slang.h shim/vyges_slang_elab.h "$out/include/"
cp slang-pin.yaml LICENSE NOTICE "$out/"
echo "bundle.sh: $out/lib/libvyges_slang.a for $target ($(wc -l < "$work/keep.txt") C entry points)"
