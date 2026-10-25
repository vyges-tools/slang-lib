// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vyges
//
// The backend the SystemVerilog frontend builds its netlist through, for a build without Yosys.
//
// Every request is forwarded, unchanged and in the order made, to the embedder's backend callback
// (vyges_slang_elab.h); the nets it returns come back as the request's value. An error from the
// callback ends the elaboration (ElabError, caught at the C API). Only `new_id` is answered here.
#ifndef VYGES_SLANG_ELAB_BACKEND_BUILDER_H
#define VYGES_SLANG_ELAB_BACKEND_BUILDER_H

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ir.h"
#include "shim/vyges_slang_elab.h"
#include "slang_frontend.h"

namespace slang_frontend {

// An error that ends the elaboration: a code and its message.
struct ElabError : std::runtime_error {
  int code;
  ElabError(int c, const std::string& message) : std::runtime_error(message), code(c) {}
};

// The embedder's callbacks for one elaboration, and its count of graphs started.
struct ElabBridge {
  vyges_slang_backend_fn backend;
  vyges_slang_log_fn log;
  void* ctx;
  int* graphs;
};

class BackendGraphBuilder : public BackendGraphBuilderBase {
 public:
  BackendGraphBuilder(const ElabBridge& bridge, int graph) : bridge_(bridge), graph_(graph) {}

  // A builder for a new graph, announced to the embedder; names count from 0 again there.
  std::unique_ptr<BackendGraphBuilder> start_new_graph(std::string_view module_name);
  // Nothing to do when a graph is done.
  void finalize() {}

  // A fresh internal name: `$N`, or `$base$N`, N counting up from 0 across both forms.
  std::string new_id(const std::string& base = std::string());

  ir::Value Unop(ast::UnaryOperator op, ir::Value a, bool a_signed, uint64_t y_width) override;
  ir::Value Biop(ast::BinaryOperator op, ir::Value a, ir::Value b, bool a_signed, bool b_signed,
                 uint64_t y_width) override;
  ir::Value Demux(ir::Value a, ir::Value s) override;
  ir::Value Bwmux(ir::Value a, ir::Value b, ir::Value s) override;
  ir::Value Bmux(ir::Value a, ir::Value s) override;
  ir::Value Shift(ir::Value a, ir::Value s, bool s_signed, uint64_t result_width) override;
  ir::Value Shiftx(ir::Value a, ir::Value s, bool s_signed, uint64_t result_width) override;
  ir::Value Mux(ir::Value a, ir::Value b, ir::Net s) override;
  ir::Value add_placeholder_signal(uint64_t width, std::string_view name_suggestion,
                                   bool public_name) override;
  void connect(ir::Value target, ir::Value source) override;
  void set_initialization(ir::Value signal, ir::Const init_value) override;
  void add_memory_init(std::string_view name, uint64_t bit_offset, bool big_endian,
                       ir::Const data) override;
  void add_input(std::string_view name, ir::Value signal) override;
  void add_output(std::string_view name, ir::Value signal) override;
  void add_instance(std::string_view cell_type, std::vector<PortConnection> ports) override;
  void add_dual_edge_aldff(const std::string& base_name, ir::Value clk, ir::Value aload, ir::Value d,
                           ir::Value q, ir::Value ad, bool aload_polarity) override;
  void add_dff(std::string_view name, const ir::Value& clk, const ir::Value& d, const ir::Value& q,
               bool clk_polarity) override;
  void add_dffe(std::string_view name, const ir::Value& clk, const ir::Value& en, const ir::Value& d,
                const ir::Value& q, bool clk_polarity, bool en_polarity) override;
  void add_aldff(std::string_view name, const ir::Value& clk, const ir::Value& aload, const ir::Value& d,
                 const ir::Value& q, const ir::Value& ad, bool clk_polarity, bool aload_polarity) override;

 private:
  // Sends one request; the value it returns (empty for requests without one).
  ir::Value send(vyges_slang_call& call, const vyges_slang_port* ports = nullptr, size_t nports = 0);

  ElabBridge bridge_;
  int graph_;
  unsigned next_id_ = 0;
};

// Attributes are not carried into the netlist: setting one does nothing.
class AttributeGuard {
 public:
  explicit AttributeGuard(GraphBuilder&) {}

  template <typename Id, typename Val>
  void set(Id, Val) {}
};

}  // namespace slang_frontend

#endif  // VYGES_SLANG_ELAB_BACKEND_BUILDER_H
