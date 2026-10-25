// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vyges
//
// The synthesis frontend behind the C API in vyges_slang_elab.h: the hooks the frontend expects
// from its embedder (logging, "unimplemented" errors, attributes), the backend that forwards every
// netlist request to the caller, the library blackboxes, and the elaboration driver.

#include "shim/vyges_slang_elab.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backend_builder.h"
#include "log_stubs.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Expression.h"
#include "slang/ast/SemanticFacts.h"
#include "slang/ast/Statement.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/driver/Driver.h"
#include "slang/numeric/SVInt.h"
#include "slang/parsing/Token.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxPrinter.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/BumpAllocator.h"
#include "slang/util/SmallVector.h"
#include "slang_frontend.h"

namespace {

// The elaboration in progress on this thread: where the frontend's logging and errors go, and the
// sources that place an AST node.
struct Current {
  const slang_frontend::ElabBridge* bridge = nullptr;
  const slang::SourceManager* sources = nullptr;
};
thread_local Current current;

// Message codes of the frontend's own logging.
constexpr int kLogErrorCode = 79;
constexpr int kLogWarningCode = 80;

// printf-formatted, with trailing line ends removed.
std::string format(const char* fmt, va_list ap) {
  va_list again;
  va_copy(again, ap);
  const int n = std::vsnprintf(nullptr, 0, fmt, again);
  va_end(again);
  if (n <= 0) return {};
  std::string s(n, '\0');
  std::vsnprintf(s.data(), n + 1, fmt, ap);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

// Hands one message to the embedder, after what slang has printed so far.
void emit(int level, int code, const std::string& msg) {
  std::fflush(stdout);
  if (current.bridge && current.bridge->log) {
    current.bridge->log(current.bridge->ctx, level, code, msg.data(), msg.size());
  } else {
    std::fprintf(stderr, level == 1 ? "Warning: %s\n" : "%s\n", msg.c_str());
  }
}

[[noreturn]] void fail(int code, std::string msg) { throw slang_frontend::ElabError(code, std::move(msg)); }

}  // namespace

// ---- logging the frontend does through the Yosys names ----

namespace Yosys {

void log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const std::string msg = format(fmt, ap);
  va_end(ap);
  emit(0, 0, msg);
}

void log_warning(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const std::string msg = format(fmt, ap);
  va_end(ap);
  emit(1, kLogWarningCode, msg);
}

void log_error(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::string msg = format(fmt, ap);
  va_end(ap);
  fail(kLogErrorCode, std::move(msg));
}

}  // namespace Yosys

namespace slang_frontend {

// ---- constructs the frontend does not elaborate ----

namespace {

// `file:line:column` of a location in a loaded source; empty for anything else.
std::string where(slang::SourceLocation loc) {
  const slang::SourceManager* sm = current.sources;
  if (!sm || !sm->isFileLoc(loc)) return {};
  return std::string(sm->getFileName(loc)) + ":" + std::to_string(sm->getLineNumber(loc)) + ":" +
         std::to_string(sm->getColumnNumber(loc));
}

// The source text of a node on one line: comments and directives dropped, every run of
// whitespace one space (none leading), cut at 120 characters with "..." after.
std::string one_line(const slang::syntax::SyntaxNode& node) {
  slang::syntax::SyntaxPrinter printer;
  printer.setIncludeComments(false).setIncludeDirectives(false).print(node);
  std::string out;
  bool gap = false;
  for (char c : printer.str()) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      gap = true;
      continue;
    }
    if (gap && !out.empty()) out += ' ';
    gap = false;
    out += c;
  }
  constexpr size_t kMax = 120;
  if (out.size() > kMax) {
    out.resize(kMax);
    out += "...";
  }
  return out;
}

// "Feature unimplemented at FILE:LINE[: CONDITION] (WHAT kind: KIND)[ at LOC][: SOURCE]"
[[noreturn]] void report_unimplemented(int code, const char* file, int line, const char* condition,
                                std::string_view what, const std::string& kind, slang::SourceLocation loc,
                                const slang::syntax::SyntaxNode* syntax) {
  std::string msg = "Feature unimplemented at " + std::string(file) + ":" + std::to_string(line);
  if (condition) msg += std::string(": ") + condition;
  msg += " (" + std::string(what) + " kind: " + kind + ")";
  const std::string at = where(loc);
  if (!at.empty()) msg += " at " + at;
  if (syntax) msg += ": " + one_line(*syntax);
  fail(code, std::move(msg));
}

}  // namespace

void unimplemented_(const ast::Symbol& obj, const char* file, int line, const char* condition) {
  // A symbol is shown with its name when it has one.
  std::string kind(slang::ast::toString(obj.kind));
  if (!obj.name.empty()) kind += " '" + std::string(obj.name) + "'";
  report_unimplemented(73, file, line, condition, "symbol", kind, obj.location, obj.getSyntax());
}

void unimplemented_(const ast::Expression& obj, const char* file, int line, const char* condition) {
  report_unimplemented(74, file, line, condition, "expression", std::string(slang::ast::toString(obj.kind)),
                obj.sourceRange.start(), obj.syntax);
}

void unimplemented_(const ast::Statement& obj, const char* file, int line, const char* condition) {
  report_unimplemented(75, file, line, condition, "statement", std::string(slang::ast::toString(obj.kind)),
                obj.sourceRange.start(), obj.syntax);
}

void unimplemented_(const ast::TimingControl& obj, const char* file, int line, const char* condition) {
  report_unimplemented(76, file, line, condition, "timing control", std::string(slang::ast::toString(obj.kind)),
                obj.sourceRange.start(), obj.syntax);
}

void wire_missing_(NetlistContext&, const ast::Symbol&, const char* file, int line) {
  fail(77, "Wire missing at " + std::string(file) + ":" + std::to_string(line));
}

void transfer_attrs(NetlistContext&, const ast::Symbol&, AttributeGuard&) {}
void transfer_attrs(NetlistContext&, const ast::Statement&, AttributeGuard&) {}
void transfer_attrs(NetlistContext&, const ast::Expression&, AttributeGuard&) {}

// ---- the backend: every request to the embedder ----

// Operator codes are slang's enumerators in slang's order; a reordering upstream stops the build.
#define VYG_SAME(slang_name, code) \
  static_assert(static_cast<int>(slang_name) == (code), #slang_name " moved");
VYG_SAME(ast::UnaryOperator::Plus, VYGES_SLANG_U_PLUS)
VYG_SAME(ast::UnaryOperator::LogicalNot, VYGES_SLANG_U_LOGICAL_NOT)
VYG_SAME(ast::UnaryOperator::Postdecrement, VYGES_SLANG_U_POSTDECREMENT)
VYG_SAME(ast::BinaryOperator::Add, VYGES_SLANG_B_ADD)
VYG_SAME(ast::BinaryOperator::BinaryXnor, VYGES_SLANG_B_BINARY_XNOR)
VYG_SAME(ast::BinaryOperator::LessThan, VYGES_SLANG_B_LESS_THAN)
VYG_SAME(ast::BinaryOperator::LogicalEquivalence, VYGES_SLANG_B_LOGICAL_EQUIVALENCE)
VYG_SAME(ast::BinaryOperator::ArithmeticShiftRight, VYGES_SLANG_B_ARITHMETIC_SHIFT_RIGHT)
VYG_SAME(ast::BinaryOperator::Power, VYGES_SLANG_B_POWER)
#undef VYG_SAME

namespace {

vyges_slang_value view(const ir::Value& v) { return {v.nets_.data(), v.nets_.size()}; }

// A constant as nets 0/1/2, kept alive for the request that carries it.
std::vector<uint32_t> nets_of(const ir::Const& c) {
  std::vector<uint32_t> nets;
  nets.reserve(c.size());
  for (ir::Trit t : c) nets.push_back(t.to_net());
  return nets;
}

vyges_slang_call request(int method) {
  vyges_slang_call c;
  std::memset(&c, 0, sizeof c);
  c.method = method;
  return c;
}

void set_name(vyges_slang_call& c, std::string_view name) {
  c.name = name.data();
  c.name_len = name.size();
}

}  // namespace

std::string BackendGraphBuilder::new_id(const std::string& base) {
  const std::string n = std::to_string(next_id_++);
  return base.empty() ? "$" + n : "$" + base + "$" + n;
}

std::unique_ptr<BackendGraphBuilder> BackendGraphBuilder::start_new_graph(std::string_view module_name) {
  const int graph = ++*bridge_.graphs;
  auto builder = std::make_unique<BackendGraphBuilder>(bridge_, graph);
  vyges_slang_call c = request(VYGES_SLANG_NEW_GRAPH);
  set_name(c, module_name);
  builder->send(c);
  return builder;
}

ir::Value BackendGraphBuilder::send(vyges_slang_call& call, const vyges_slang_port* ports, size_t nports) {
  call.graph = graph_;
  call.ports = ports;
  call.nports = nports;
  std::fflush(stdout);
  vyges_slang_value out{nullptr, 0};
  const char* err = nullptr;
  size_t err_len = 0;
  const int rc = bridge_.backend(bridge_.ctx, &call, &out, &err, &err_len);
  if (rc != 0) fail(rc, err ? std::string(err, err_len) : std::string());
  ir::Value v;
  if (out.len) v.nets_.assign(out.nets, out.nets + out.len);
  return v;
}

ir::Value BackendGraphBuilder::Unop(ast::UnaryOperator op, ir::Value a, bool a_signed, uint64_t y_width) {
  vyges_slang_call c = request(VYGES_SLANG_UNOP);
  c.op = static_cast<int>(op);
  c.v[0] = view(a);
  c.flag[0] = a_signed;
  c.width = y_width;
  return send(c);
}

ir::Value BackendGraphBuilder::Biop(ast::BinaryOperator op, ir::Value a, ir::Value b, bool a_signed,
                                    bool b_signed, uint64_t y_width) {
  vyges_slang_call c = request(VYGES_SLANG_BIOP);
  c.op = static_cast<int>(op);
  c.v[0] = view(a);
  c.v[1] = view(b);
  c.flag[0] = a_signed;
  c.flag[1] = b_signed;
  c.width = y_width;
  return send(c);
}

ir::Value BackendGraphBuilder::Demux(ir::Value a, ir::Value s) {
  vyges_slang_call c = request(VYGES_SLANG_DEMUX);
  c.v[0] = view(a);
  c.v[1] = view(s);
  return send(c);
}

ir::Value BackendGraphBuilder::Bwmux(ir::Value a, ir::Value b, ir::Value s) {
  vyges_slang_call c = request(VYGES_SLANG_BWMUX);
  c.v[0] = view(a);
  c.v[1] = view(b);
  c.v[2] = view(s);
  return send(c);
}

ir::Value BackendGraphBuilder::Bmux(ir::Value a, ir::Value s) {
  vyges_slang_call c = request(VYGES_SLANG_BMUX);
  c.v[0] = view(a);
  c.v[1] = view(s);
  return send(c);
}

ir::Value BackendGraphBuilder::Shift(ir::Value a, ir::Value s, bool s_signed, uint64_t result_width) {
  vyges_slang_call c = request(VYGES_SLANG_SHIFT);
  c.v[0] = view(a);
  c.v[1] = view(s);
  c.flag[0] = s_signed;
  c.width = result_width;
  return send(c);
}

ir::Value BackendGraphBuilder::Shiftx(ir::Value a, ir::Value s, bool s_signed, uint64_t result_width) {
  vyges_slang_call c = request(VYGES_SLANG_SHIFTX);
  c.v[0] = view(a);
  c.v[1] = view(s);
  c.flag[0] = s_signed;
  c.width = result_width;
  return send(c);
}

ir::Value BackendGraphBuilder::Mux(ir::Value a, ir::Value b, ir::Net s) {
  const ir::Value sel(s);
  vyges_slang_call c = request(VYGES_SLANG_MUX);
  c.v[0] = view(a);
  c.v[1] = view(b);
  c.v[2] = view(sel);
  return send(c);
}

ir::Value BackendGraphBuilder::add_placeholder_signal(uint64_t width, std::string_view name_suggestion,
                                                      bool public_name) {
  vyges_slang_call c = request(VYGES_SLANG_PLACEHOLDER);
  c.width = width;
  set_name(c, name_suggestion);
  c.flag[0] = public_name;
  return send(c);
}

void BackendGraphBuilder::connect(ir::Value target, ir::Value source) {
  vyges_slang_call c = request(VYGES_SLANG_CONNECT);
  c.v[0] = view(target);
  c.v[1] = view(source);
  send(c);
}

void BackendGraphBuilder::set_initialization(ir::Value signal, ir::Const init_value) {
  const std::vector<uint32_t> init = nets_of(init_value);
  vyges_slang_call c = request(VYGES_SLANG_SET_INIT);
  c.v[0] = view(signal);
  c.v[1] = {init.data(), init.size()};
  send(c);
}

void BackendGraphBuilder::add_memory_init(std::string_view name, uint64_t bit_offset, bool big_endian,
                                          ir::Const data) {
  const std::vector<uint32_t> bits = nets_of(data);
  vyges_slang_call c = request(VYGES_SLANG_MEMORY_INIT);
  set_name(c, name);
  c.width = bit_offset;
  c.flag[0] = big_endian;
  c.v[0] = {bits.data(), bits.size()};
  send(c);
}

void BackendGraphBuilder::add_input(std::string_view name, ir::Value signal) {
  vyges_slang_call c = request(VYGES_SLANG_INPUT);
  set_name(c, name);
  c.v[0] = view(signal);
  send(c);
}

void BackendGraphBuilder::add_output(std::string_view name, ir::Value signal) {
  vyges_slang_call c = request(VYGES_SLANG_OUTPUT);
  set_name(c, name);
  c.v[0] = view(signal);
  send(c);
}

void BackendGraphBuilder::add_instance(std::string_view cell_type, std::vector<PortConnection> ports) {
  std::vector<vyges_slang_port> wire(ports.size());
  for (size_t i = 0; i < ports.size(); i++) {
    const PortConnection& p = ports[i];
    wire[i].name = p.name.data();
    wire[i].name_len = p.name.size();
    wire[i].direction = p.direction == PortConnection::kInput    ? VYGES_SLANG_DIR_INPUT
                        : p.direction == PortConnection::kOutput ? VYGES_SLANG_DIR_OUTPUT
                                                                 : VYGES_SLANG_DIR_INOUT;
    wire[i].value = view(p.value);
  }
  vyges_slang_call c = request(VYGES_SLANG_INSTANCE);
  set_name(c, cell_type);
  send(c, wire.data(), wire.size());
}

void BackendGraphBuilder::add_dual_edge_aldff(const std::string& base_name, ir::Value clk, ir::Value aload,
                                              ir::Value d, ir::Value q, ir::Value ad, bool aload_polarity) {
  vyges_slang_call c = request(VYGES_SLANG_DUAL_EDGE_ALDFF);
  set_name(c, base_name);
  c.v[0] = view(clk);
  c.v[1] = view(aload);
  c.v[2] = view(d);
  c.v[3] = view(q);
  c.v[4] = view(ad);
  c.flag[0] = aload_polarity;
  send(c);
}

void BackendGraphBuilder::add_dff(std::string_view name, const ir::Value& clk, const ir::Value& d,
                                  const ir::Value& q, bool clk_polarity) {
  vyges_slang_call c = request(VYGES_SLANG_DFF);
  set_name(c, name);
  c.v[0] = view(clk);
  c.v[1] = view(d);
  c.v[2] = view(q);
  c.flag[0] = clk_polarity;
  send(c);
}

void BackendGraphBuilder::add_dffe(std::string_view name, const ir::Value& clk, const ir::Value& en,
                                   const ir::Value& d, const ir::Value& q, bool clk_polarity,
                                   bool en_polarity) {
  vyges_slang_call c = request(VYGES_SLANG_DFFE);
  set_name(c, name);
  c.v[0] = view(clk);
  c.v[1] = view(en);
  c.v[2] = view(d);
  c.v[3] = view(q);
  c.flag[0] = clk_polarity;
  c.flag[1] = en_polarity;
  send(c);
}

void BackendGraphBuilder::add_aldff(std::string_view name, const ir::Value& clk, const ir::Value& aload,
                                    const ir::Value& d, const ir::Value& q, const ir::Value& ad,
                                    bool clk_polarity, bool aload_polarity) {
  vyges_slang_call c = request(VYGES_SLANG_ALDFF);
  set_name(c, name);
  c.v[0] = view(clk);
  c.v[1] = view(aload);
  c.v[2] = view(d);
  c.v[3] = view(q);
  c.v[4] = view(ad);
  c.flag[0] = clk_polarity;
  c.flag[1] = aload_polarity;
  send(c);
}

}  // namespace slang_frontend

// ---- library cells as blackbox modules ----

namespace {

using namespace slang;
using namespace slang::syntax;
using namespace slang::parsing;

// Builds module declarations as syntax (no source text, no locations) and adds them to a
// compilation as one library compilation unit.
class BlackboxBuilder {
 public:
  explicit BlackboxBuilder(ast::Compilation& comp) : comp_(comp) {}

  void add(const vyges_slang_bb_cell& cell) {
    const std::string_view name(cell.name);
    if (comp_.tryGetDefinition(name, comp_.getRootNoFinalize()).definition) return;
    modules_.push_back(module(cell));
  }

  void finish(SourceManager& sources) {
    if (modules_.empty()) return;
    auto* unit = alloc_.emplace<CompilationUnitSyntax>(
        *comp_.emplace<SyntaxList<MemberSyntax>>(modules_.copy(comp_)), tok(TokenKind::EndOfFile));
    auto tree = std::make_shared<SyntaxTree>(unit, sources, std::move(alloc_), &comp_.getDefaultLibrary(),
                                             nullptr);
    tree->isLibraryUnit = true;
    comp_.addSyntaxTree(tree);
  }

 private:
  std::string_view keep(std::string_view s) {
    char* p = reinterpret_cast<char*>(alloc_.allocate(s.size(), 1));
    if (!s.empty()) std::memcpy(p, s.data(), s.size());
    return {p, s.size()};
  }

  // A token, optionally preceded by one space and/or followed by a line end (as trivia).
  Token tok(TokenKind kind, std::string_view text = {}, bool space = false, bool eol = false) {
    SmallVector<Trivia, 2> trivia;
    if (space) trivia.push_back(Trivia(TriviaKind::Whitespace, " "sv));
    if (eol) trivia.push_back(Trivia(TriviaKind::EndOfLine, "\n"sv));
    return Token(comp_, kind, trivia.copy(comp_), keep(text), SourceLocation::NoLocation);
  }

  Token bare(TokenKind kind) { return Token(comp_, kind, {}, "", SourceLocation::NoLocation); }

  template <typename T>
  SyntaxList<T>& none() {
    return *alloc_.emplace<SyntaxList<T>>(nullptr);
  }

  ExpressionSyntax& literal(int value) {
    const std::string text = std::to_string(value);
    return *alloc_.emplace<LiteralExpressionSyntax>(
        SyntaxKind::IntegerLiteralExpression,
        Token(comp_, TokenKind::IntegerLiteral, {}, keep(text), SourceLocation::NoLocation, SVInt(value)));
  }

  // `input|output|inout [width-1:0] name`; no range for one bit.
  MemberSyntax* port(const vyges_slang_bb_port& p) {
    const TokenKind dir = p.direction == VYGES_SLANG_DIR_INPUT    ? TokenKind::InputKeyword
                          : p.direction == VYGES_SLANG_DIR_OUTPUT ? TokenKind::OutputKeyword
                                                                  : TokenKind::InOutKeyword;
    SmallVector<VariableDimensionSyntax*, 2> dims;
    if (p.width > 1) {
      auto* range = alloc_.emplace<RangeSelectSyntax>(SyntaxKind::SimpleRangeSelect, literal(p.width - 1),
                                                      bare(TokenKind::Colon), literal(0));
      dims.push_back(alloc_.emplace<VariableDimensionSyntax>(
          tok(TokenKind::OpenBracket, {}, true), alloc_.emplace<RangeDimensionSpecifierSyntax>(*range),
          bare(TokenKind::CloseBracket)));
    }
    auto* type = alloc_.emplace<ImplicitTypeSyntax>(
        Token(), *alloc_.emplace<SyntaxList<VariableDimensionSyntax>>(dims.copy(comp_)), Token());
    auto* header = alloc_.emplace<VariablePortHeaderSyntax>(Token(), tok(dir, {}, false, true), Token(), *type);
    auto* decl = alloc_.emplace<DeclaratorSyntax>(tok(TokenKind::Identifier, p.name, true),
                                                  none<VariableDimensionSyntax>(), nullptr);
    return alloc_.emplace<ImplicitAnsiPortSyntax>(none<AttributeInstanceSyntax>(), *header, *decl);
  }

  // `(* blackbox *) module NAME(ports); endmodule`
  MemberSyntax* module(const vyges_slang_bb_cell& cell) {
    SmallVector<TokenOrSyntax, 16> ports;
    for (size_t i = 0; i < cell.nports; i++) {
      if (i) ports.push_back(tok(TokenKind::Comma));
      ports.push_back(port(cell.ports[i]));
    }
    auto* port_list = alloc_.emplace<AnsiPortListSyntax>(
        tok(TokenKind::OpenParenthesis), *alloc_.emplace<SeparatedSyntaxList<MemberSyntax>>(ports.copy(comp_)),
        tok(TokenKind::CloseParenthesis));
    auto* header = alloc_.emplace<ModuleHeaderSyntax>(
        SyntaxKind::ModuleHeader, tok(TokenKind::ModuleKeyword, {}, false, true), Token(),
        tok(TokenKind::Identifier, cell.name, true), none<PackageImportDeclarationSyntax>(), nullptr, port_list,
        tok(TokenKind::Semicolon));

    SmallVector<TokenOrSyntax, 2> spec;
    spec.push_back(alloc_.emplace<AttributeSpecSyntax>(tok(TokenKind::Identifier, "blackbox", true), nullptr));
    SmallVector<AttributeInstanceSyntax*, 2> attrs;
    attrs.push_back(alloc_.emplace<AttributeInstanceSyntax>(
        tok(TokenKind::OpenParenthesis), tok(TokenKind::Star),
        *alloc_.emplace<SeparatedSyntaxList<AttributeSpecSyntax>>(spec.copy(comp_)),
        tok(TokenKind::Star, {}, true), tok(TokenKind::CloseParenthesis)));

    return alloc_.emplace<ModuleDeclarationSyntax>(
        SyntaxKind::ModuleDeclaration, *alloc_.emplace<SyntaxList<AttributeInstanceSyntax>>(attrs.copy(comp_)),
        *header, none<MemberSyntax>(), tok(TokenKind::EndModuleKeyword, {}, false, true), nullptr);
  }

  ast::Compilation& comp_;
  BumpAllocator alloc_;
  SmallVector<MemberSyntax*, 16> modules_;
};

void copy_out(const std::string& s, char* buf, size_t len) {
  if (!buf || len == 0) return;
  const size_t n = std::min(s.size(), len - 1);
  std::memcpy(buf, s.data(), n);
  buf[n] = '\0';
}

// The elaboration, in the frontend's order. Empty when it failed with everything reported.
std::optional<std::string> elaborate(int argc, const char* const* argv, const char* inline_text,
                                     const vyges_slang_bb_cell* cells, size_t ncells,
                                     const slang_frontend::ElabBridge& bridge) {
  using namespace slang_frontend;
  driver::Driver driver;
  driver.addStandardArgs();

  struct Scope {
    Current saved = current;
    ~Scope() { current = saved; }
  } scope;
  current.bridge = &bridge;
  current.sources = &driver.sourceManager;

  SynthesisSettings settings;
  settings.addOptions(driver.cmdLine);

  if (inline_text) {
    driver.sourceLoader.addBuffer(driver.sourceManager.assignText("<inline>", std::string_view(inline_text)));
  }

  std::vector<const char*> args;
  args.reserve(static_cast<size_t>(argc) + 1);
  args.push_back("syn_elaborate");
  for (int i = 0; i < argc; i++) args.push_back(argv[i]);
  if (!driver.parseCommandLine(static_cast<int>(args.size()), const_cast<char**>(args.data()))) return {};

  fixup_options(settings, driver);
  if (!driver.processOptions()) return {};
  catch_forbidden_options(driver);
  if (!driver.parseAllSources()) return {};

  auto compilation = driver.createCompilation();
  {
    BlackboxBuilder blackboxes(*compilation);
    for (size_t i = 0; i < ncells; i++) blackboxes.add(cells[i]);
    blackboxes.finish(driver.sourceManager);
  }

  driver.reportCompilation(*compilation, /*quiet=*/false);
  if (driver.diagEngine.getNumErrors() || settings.ast_compilation_only.value_or(false)) {
    (void)driver.reportDiagnostics(/*quiet=*/false);
    return {};
  }

  // Exactly one top instance; programs do not count.
  std::vector<const ast::InstanceSymbol*> tops;
  for (const ast::InstanceSymbol* inst : compilation->getRoot().topInstances) {
    if (inst->getDefinition().definitionKind != ast::DefinitionKind::Program) tops.push_back(inst);
  }
  if (tops.size() != 1) fail(78, "Expected exactly one top-level module, got " + std::to_string(tops.size()));

  HierarchyQueue hqueue;
  NetlistContext netlist(std::make_unique<BackendGraphBuilder>(bridge, 0), settings, *compilation, *tops[0]);
  populate_netlist(hqueue, netlist);

  // The frontend's own diagnostics: sorted, each once.
  Diagnostics diags;
  diags.append_range(netlist.issued_diagnostics);
  diags.sort(driver.sourceManager);
  for (size_t i = 0; i < diags.size(); i++) {
    if (i > 0 && diags[i] == diags[i - 1]) continue;
    driver.diagEngine.issue(diags[i]);
  }
  if (!driver.reportDiagnostics(/*quiet=*/false)) return {};
  return std::string(tops[0]->name);
}

}  // namespace

extern "C" int vyges_slang_elaborate(int argc, const char* const* argv, const char* inline_text,
                                     const vyges_slang_bb_cell* cells, size_t ncells,
                                     vyges_slang_backend_fn backend, vyges_slang_log_fn log, void* ctx,
                                     char* top, size_t top_len, int* err_code, char* err_msg, size_t err_len) {
  if (err_code) *err_code = 0;
  copy_out(std::string(), err_msg, err_len);
  copy_out(std::string(), top, top_len);
  int graphs = 0;
  const slang_frontend::ElabBridge bridge{backend, log, ctx, &graphs};
  int rc;
  try {
    const std::optional<std::string> name = elaborate(argc, argv, inline_text, cells, ncells, bridge);
    if (name) copy_out(*name, top, top_len);
    rc = name ? 0 : 1;
  } catch (const slang_frontend::ElabError& e) {
    if (err_code) *err_code = e.code;
    copy_out(e.what(), err_msg, err_len);
    rc = 2;
  } catch (const std::exception& e) {
    copy_out(e.what(), err_msg, err_len);
    rc = 3;
  } catch (...) {
    copy_out("unknown exception", err_msg, err_len);
    rc = 3;
  }
  std::fflush(stdout);
  return rc;
}
