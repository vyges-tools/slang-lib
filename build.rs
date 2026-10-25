// SPDX-License-Identifier: Apache-2.0
//! Links `libvyges_slang.a`: slang + fmt + its own C++ runtime, prelinked
//! into one object with only the `vyges_slang_*` C API global (see `scripts/bundle.sh` for why).
//!
//! Two ways to get it, as for the sibling `-lib` crates:
//! - **Prebuilt:** `VYGES_SLANG_PREBUILT_DIR=<dir>` holding `lib/libvyges_slang.a` (the bundle
//!   `scripts/fetch-bundle.sh` downloads). No Bazel, no compile.
//! - **From source (default):** `scripts/bundle.sh` into `OUT_DIR` — Bazel with the versions pinned
//!   in `MODULE.bazel`. `VYGES_BAZEL` names the launcher if it is not `bazelisk`/`bazel` on PATH.
//!
//! ⛔ The bundle is written into `OUT_DIR`, never linked out of `bazel-bin`: that tree is Bazel's
//! to rewrite, and a later Bazel run must not change what an already-built crate links.

use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-env-changed=VYGES_SLANG_PREBUILT_DIR");
    println!("cargo:rerun-if-env-changed=VYGES_BAZEL");
    for p in ["MODULE.bazel", "BUILD.bazel", ".bazelrc", ".bazelversion", "bazel", "shim", "scripts/bundle.sh"] {
        println!("cargo:rerun-if-changed={p}");
    }

    let lib = match env::var_os("VYGES_SLANG_PREBUILT_DIR") {
        Some(dir) => PathBuf::from(dir).join("lib"),
        None => bundle(),
    };
    if !lib.join("libvyges_slang.a").exists() {
        panic!("{} has no libvyges_slang.a", lib.display());
    }
    println!("cargo:rustc-link-search=native={}", lib.display());
    println!("cargo:rustc-link-lib=static=vyges_slang");
    // Linux: the C++ runtime is inside the bundle; only the C library's pieces come from the final
    // link. macOS: the toolchain builds against the SDK's libc++ and has no static runtime to embed,
    // so the system `libc++` is linked.
    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos") {
        println!("cargo:rustc-link-lib=c++");
    } else {
        for l in ["pthread", "m", "dl"] {
            println!("cargo:rustc-link-lib={l}");
        }
    }
}

fn bundle() -> PathBuf {
    let here = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bundle");
    let status = Command::new("bash")
        .arg(here.join("scripts/bundle.sh"))
        .arg(&out)
        .current_dir(&here)
        .status()
        .unwrap_or_else(|e| panic!("running scripts/bundle.sh: {e}"));
    if !status.success() {
        panic!("scripts/bundle.sh failed ({status}); or set VYGES_SLANG_PREBUILT_DIR to a prebuilt bundle");
    }
    out.join("lib")
}
