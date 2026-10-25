# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Vyges
#
# Our build of the synthesis frontend's sources (ISC, see LICENSE in this archive). The library
# that compiles them is `//:elab` in the main repository: the frontend includes headers only the
# embedder provides (`ir.h`, `backend_builder.h`, `log_stubs.h`), so it cannot be a library here.

load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

exports_files(["LICENSE"])

filegroup(
    name = "frontend_srcs",
    srcs = glob(["src/*.cc"]),
)

cc_library(
    name = "frontend_hdrs",
    hdrs = glob(["src/*.h"]),
    includes = ["src"],
)

# Header only: its one definition (the hasher's `fudge`) is also made by the frontend when built
# without Yosys, and two would collide in one archive.
cc_library(
    name = "hashlib",
    hdrs = ["third_party/hashlib/hashlib.h"],
    includes = ["third_party/hashlib"],
)
