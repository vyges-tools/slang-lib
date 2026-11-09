#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Downloads the prebuilt bundle for THIS checkout's bundle id (scripts/bundle-id.sh) into <dir>,
# checks its SHA-256, and prints the line to export:
#   VYGES_SLANG_PREBUILT_DIR=<dir>
#
# Published by .github/workflows/ci.yml as release `bundle-<id>`, one asset per target: Linux
# x86_64 and arm64, macOS arm64. Exit 1 when no bundle exists for this id yet: its inputs changed and the workflow has not
# published the new one. Nothing falls back to an older bundle.
#
# Usage: scripts/fetch-bundle.sh <dir>
set -euo pipefail
dir=${1:?usage: scripts/fetch-bundle.sh <dir>}
here=$(cd "$(dirname "$0")/.." && pwd)
id=$(bash "$here/scripts/bundle-id.sh")
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) target=x86_64-unknown-linux-gnu ;;
  Linux-aarch64) target=aarch64-unknown-linux-gnu ;;
  Darwin-arm64) target=aarch64-apple-darwin ;;
  *) echo "fetch-bundle.sh: prebuilt for Linux x86_64/arm64 and macOS arm64 only" >&2; exit 1 ;;
esac
name="vyges-slang-$id-$target.tar.gz"
url="https://github.com/vyges-tools/slang-lib/releases/download/bundle-$id"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# One asset into $work. With a token (CI passes GH_TOKEN) and gh on PATH the download is an
# authenticated API call: anonymous release downloads are rate-limited and time out first when
# GitHub is under load. Otherwise curl, retrying timeouts and 5xx (not 404: a missing bundle
# still fails at once, which is what the message below is for).
token=${GH_TOKEN:-${GITHUB_TOKEN:-}}
fetch() {
  if [ -n "$token" ] && command -v gh >/dev/null 2>&1; then
    GH_TOKEN=$token gh release download "bundle-$id" -R "vyges-tools/slang-lib" -p "$1" -O "$work/$1" --clobber \
      && return 0
    echo "fetch-bundle.sh: gh download of $1 failed, trying curl" >&2
  fi
  curl -fsSL --retry 5 --retry-delay 5 --retry-max-time 300 --connect-timeout 30 -o "$work/$1" "$url/$1"
}
if ! fetch "$name" || ! fetch "$name.sha256"; then
  echo "fetch-bundle.sh: no published bundle-$id ($url) — build it with scripts/bundle.sh, or push the input change to main so ci publishes it" >&2
  exit 1
fi
( cd "$work" && { sha256sum -c "$name.sha256" >/dev/null 2>&1 || shasum -a 256 -c "$name.sha256" >/dev/null; } ) \
  || { echo "fetch-bundle.sh: checksum mismatch for $name" >&2; exit 1; }
mkdir -p "$dir"
tar -xzf "$work/$name" -C "$dir" --strip-components=1
[ -s "$dir/lib/libvyges_slang.a" ] || { echo "fetch-bundle.sh: $name has no lib/libvyges_slang.a" >&2; exit 1; }
echo "VYGES_SLANG_PREBUILT_DIR=$(cd "$dir" && pwd)"
