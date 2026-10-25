// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vyges

#include "shim/vyges_slang.h"

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/ASTSerializer.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/driver/Driver.h"
#include "slang/text/Json.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

struct vyges_slang_compilation {
  slang::driver::Driver driver;
  std::unique_ptr<slang::ast::Compilation> compilation;
  int errors = 0;
  std::string json;
};

namespace {

// Standard output and standard error into one file for the life of the object.
class Capture {
 public:
  explicit Capture(const char* path) {
    if (!path) {
      ok_ = true;
      return;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    out_ = dup(STDOUT_FILENO);
    err_ = dup(STDERR_FILENO);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_ < 0 || err_ < 0 || fd < 0) {
      if (fd >= 0) close(fd);
      return;
    }
    ok_ = dup2(fd, STDOUT_FILENO) >= 0 && dup2(fd, STDERR_FILENO) >= 0;
    close(fd);
  }
  ~Capture() {
    std::fflush(stdout);
    std::fflush(stderr);
    if (out_ >= 0) {
      dup2(out_, STDOUT_FILENO);
      close(out_);
    }
    if (err_ >= 0) {
      dup2(err_, STDERR_FILENO);
      close(err_);
    }
  }
  bool ok() const { return ok_; }

 private:
  int out_ = -1;
  int err_ = -1;
  bool ok_ = false;
};

}  // namespace

vyges_slang_compilation* vyges_slang_compile(int argc, const char* const* argv, const char* report_path,
                                             int* status) {
  try {
    Capture capture(report_path);
    if (!capture.ok()) return nullptr;
    auto c = std::make_unique<vyges_slang_compilation>();
    auto& d = c->driver;
    d.addStandardArgs();
    if (!d.parseCommandLine(argc, argv) || !d.processOptions()) {
      *status = 1;
      return c.release();
    }
    // The `slang` program's stages, in its order: parse, elaborate and report the design, analyse,
    // report the diagnostics.
    const bool parsed = d.parseAllSources();
    c->compilation = d.createCompilation();
    d.reportCompilation(*c->compilation, /* quiet */ false);
    d.runAnalysis(*c->compilation);
    const bool ok = d.reportDiagnostics(/* quiet */ false) && parsed;
    c->errors = static_cast<int>(d.diagEngine.getNumErrors());
    *status = !parsed ? 2 : ok ? 0 : 3;
    return c.release();
  } catch (...) {
    return nullptr;
  }
}

int vyges_slang_error_count(const vyges_slang_compilation* c) { return c ? c->errors : 0; }

long vyges_slang_ast_json(vyges_slang_compilation* c, int pretty, char* buf, long len) {
  try {
    if (!c || !c->compilation) return -1;
    slang::JsonWriter writer;
    writer.setPrettyPrint(pretty != 0);
    // `slang --ast-json`'s document: the design, then every definition.
    slang::ast::ASTSerializer serializer(*c->compilation, writer);
    serializer.setTryConstantFold(false);
    serializer.startObject();
    serializer.writeProperty("design");
    serializer.serialize(c->compilation->getRoot());
    serializer.writeProperty("definitions");
    serializer.startArray();
    for (auto def : c->compilation->getDefinitions()) serializer.serialize(*def);
    serializer.endArray();
    serializer.endObject();
    c->json = std::string(writer.view());
    if (buf && len > 0) {
      const long n = std::min<long>(len - 1, static_cast<long>(c->json.size()));
      std::memcpy(buf, c->json.data(), static_cast<size_t>(n));
      buf[n] = '\0';
    }
    return static_cast<long>(c->json.size());
  } catch (...) {
    return -1;
  }
}

void vyges_slang_free(vyges_slang_compilation* c) {
  try {
    delete c;
  } catch (...) {
  }
}
