# vyges-slang-lib

[slang](https://github.com/MikePopoloski/slang), the SystemVerilog compiler and language services
library, built with **Bazel** at a pinned version into one static archive (`libvyges_slang.a`),
behind a small C shim and a safe Rust API.

> Part of Vyges Loom. Used where an engine reads SystemVerilog and must elaborate it exactly as
> the pinned slang does, on every run and every machine.

## What it gives you

```rust
use vyges_slang_lib::{compile, Status};

let mut c = compile(&["rtl/top.sv", "rtl/counter.sv", "--top", "top", "-DWIDTH=8"])?;
assert_eq!(c.status(), Status::Ok);
println!("{}", c.report());                 // what slang printed: diagnostics, "Build succeeded"
let design = c.ast_json(true).unwrap();      // the elaborated design, as slang serialises it
```

- `compile` takes the words of a `slang` command line and hands them to slang's own driver:
  the same options (`-D`, `-I`, `-f`, `--top`, `--std`, ...), source loading, elaboration and
  diagnostics as the `slang` program.
- `report` is everything slang printed, in its order; `error_count` counts its errors.
- `ast_json` is the elaborated design (the root symbol) in slang's JSON form.

Compilations are independent. The report is captured by redirecting the process's standard output
and error for the call, so captured compilations take turns.

## Pin

- **slang `10.0.1-20260316-f04e8156`**: Bazel Central Registry module `sv-lang` (`MODULE.bazel`),
  slang at source commit `f04e8156`.
- the **hermetic LLVM** toolchain (Bazel module `llvm`), so the compiler is not the host's;
- `-c opt -O3 -ffp-contract=off -std=c++20` (`.bazelrc`).

`slang-pin.yaml` records the set. Bump on purpose, never to "latest".

## One object, one C API

`scripts/bundle.sh` prelinks slang, its dependencies and (on Linux) the toolchain's C++ runtime into
ONE object with only the `vyges_slang_*` functions global, so a program that links another fmt or
another C++ runtime cannot collide with this one.

## Getting the archive

- **Prebuilt** (no Bazel): `scripts/fetch-bundle.sh <dir>` downloads the bundle published for this
  checkout's build inputs (`scripts/bundle-id.sh`) and prints `VYGES_SLANG_PREBUILT_DIR=<dir>`.
  Linux x86_64 and arm64, macOS arm64.
- **From source**: `cargo build` runs `scripts/bundle.sh` (needs `bazelisk` or `bazel`).

## Licence

Apache-2.0 for the files in this repository. The archive also carries slang (MIT, Michael
Popoloski) and its dependencies; see `NOTICE`.
