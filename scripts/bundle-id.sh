#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Prints the bundle id: 12 hex digits of a SHA-256 over every file that decides what
# `scripts/bundle.sh` builds, in a fixed order, name and content both.
#
# 🔑 The prebuilt bundle is published once per id (release `bundle-<id>`) and fetched by id, so a
# consumer always links the archive built from the inputs it checked out, and a change to the
# Rust crate alone (src/, tests/, docs) does not ask for a long slang rebuild.
#
# ⛔ Add a file here when the build starts reading it. A build input missing from this list is a
# bundle that changes without its id changing, so a consumer keeps fetching the old archive.
set -euo pipefail
cd "$(dirname "$0")/.."
inputs=(.bazelrc .bazelversion BUILD.bazel MODULE.bazel scripts/bundle.sh)
while IFS= read -r f; do inputs+=("$f"); done < <(find bazel shim -type f | LC_ALL=C sort)
for f in "${inputs[@]}"; do
  printf '%s\n' "$f"
  sha256sum < "$f" 2>/dev/null || shasum -a 256 < "$f"
done | { sha256sum 2>/dev/null || shasum -a 256; } | cut -c1-12
