// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vyges
//
// C API over slang's driver: one call runs a compilation the way the `slang` command line does
// (the same options, parsed by slang's own driver), and hands back what it reports and the
// elaborated design as JSON. Nothing is reordered or re-interpreted here.
//
// Every function catches any C++ exception and reports it as a failure; none crosses this
// boundary. A compilation is independent of every other: no process-wide state is kept here
// (slang's own output still goes to the process's standard streams unless captured).
#ifndef VYGES_SLANG_H
#define VYGES_SLANG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vyges_slang_compilation vyges_slang_compilation;

// Runs slang's driver on `argc` command-line words (`argv[0]` is the program name, as for
// `slang`): standard options, every source parsed, the design elaborated, every diagnostic
// reported. While it runs, standard output and standard error are redirected to the file
// `report_path` (created or truncated) when it is not NULL, then restored.
// The stages are the `slang` program's, in its order: parse every source, elaborate and report
// the design, analyse it, report the diagnostics.
// Returns the compilation (free it with vyges_slang_free), or NULL if the call threw or the
// redirection could not be set up. `*status`: 0 compiled without errors, 1 the options were
// rejected, 2 a source could not be loaded or parsed, 3 the design has errors.
vyges_slang_compilation* vyges_slang_compile(int argc, const char* const* argv, const char* report_path,
                                             int* status);

// The number of error diagnostics the compilation reported.
int vyges_slang_error_count(const vyges_slang_compilation* c);

// The elaborated design as `slang --ast-json` writes it ({"design": the root, "definitions": every
// definition}), pretty-printed or not:
// copies it NUL-terminated into `buf` (truncated to len - 1 bytes) and returns its full length,
// or -1 if there is no elaborated design or the call threw. Call with len 0 to size `buf`.
long vyges_slang_ast_json(vyges_slang_compilation* c, int pretty, char* buf, long len);

void vyges_slang_free(vyges_slang_compilation* c);

#ifdef __cplusplus
}
#endif

#endif  // VYGES_SLANG_H
