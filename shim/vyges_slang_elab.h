// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vyges
//
// C API over the SystemVerilog synthesis frontend: slang compiles the design, the frontend walks
// it and asks a BACKEND for every node of the netlist. The backend is the caller's: one callback
// that receives each request in the order the frontend makes it and returns the nets it built.
// The netlist lives entirely on the caller's side; this side holds only net ids.
//
// A net is a 32-bit id. Ids 0, 1 and 2 are the constants 0, 1 and x and need no backend; every
// other id is one the backend handed back. A value is a vector of nets, least significant first.
//
// No C++ exception crosses this boundary.
#ifndef VYGES_SLANG_ELAB_H
#define VYGES_SLANG_ELAB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const uint32_t* nets;
  size_t len;
} vyges_slang_value;

// What the frontend asks for. Operands are in `v[]` in the order listed; `flag[]`, `width` and
// `name` as listed. A constant operand (an initialization, memory data) is a value of ids 0/1/2.
enum {
  VYGES_SLANG_UNOP = 0,         // op (unary code); v[0] a; flag[0] a signed; width = y width -> value
  VYGES_SLANG_BIOP = 1,         // op (binary code); v[0] a, v[1] b; flag[0] a signed, flag[1] b signed;
                                //   width = y width -> value
  VYGES_SLANG_DEMUX = 2,        // v[0] a, v[1] s -> value
  VYGES_SLANG_BWMUX = 3,        // v[0] a, v[1] b, v[2] s -> value
  VYGES_SLANG_BMUX = 4,         // v[0] a, v[1] s -> value
  VYGES_SLANG_SHIFT = 5,        // v[0] a, v[1] s; flag[0] s signed; width = result width -> value
  VYGES_SLANG_SHIFTX = 6,       // as SHIFT, padding with x
  VYGES_SLANG_MUX = 7,          // v[0] a, v[1] b, v[2] s (one net) -> value
  VYGES_SLANG_PLACEHOLDER = 8,  // width; name = suggestion; flag[0] public name -> value
  VYGES_SLANG_CONNECT = 9,      // v[0] target, v[1] source
  VYGES_SLANG_SET_INIT = 10,    // v[0] signal, v[1] init (constant)
  VYGES_SLANG_MEMORY_INIT = 11, // name; width = bit offset; flag[0] big endian; v[0] data (constant)
  VYGES_SLANG_INPUT = 12,       // name; v[0] signal
  VYGES_SLANG_OUTPUT = 13,      // name; v[0] signal
  VYGES_SLANG_INSTANCE = 14,    // name = cell type; ports[nports]
  VYGES_SLANG_DUAL_EDGE_ALDFF = 15, // name = base name; v[0] clk, v[1] aload, v[2] d, v[3] q, v[4] ad;
                                    //   flag[0] aload polarity
  VYGES_SLANG_DFF = 16,         // name; v[0] clk, v[1] d, v[2] q; flag[0] clk polarity
  VYGES_SLANG_DFFE = 17,        // name; v[0] clk, v[1] en, v[2] d, v[3] q; flag[0] clk polarity,
                                //   flag[1] en polarity
  VYGES_SLANG_ALDFF = 18,       // name; v[0] clk, v[1] aload, v[2] d, v[3] q, v[4] ad;
                                //   flag[0] clk polarity, flag[1] aload polarity
  VYGES_SLANG_NEW_GRAPH = 19,   // `graph` is a new graph, for an instance of module `name`
};

// Unary operator codes: slang's UnaryOperator, in its order.
enum {
  VYGES_SLANG_U_PLUS, VYGES_SLANG_U_MINUS, VYGES_SLANG_U_BITWISE_NOT, VYGES_SLANG_U_BITWISE_AND,
  VYGES_SLANG_U_BITWISE_OR, VYGES_SLANG_U_BITWISE_XOR, VYGES_SLANG_U_BITWISE_NAND,
  VYGES_SLANG_U_BITWISE_NOR, VYGES_SLANG_U_BITWISE_XNOR, VYGES_SLANG_U_LOGICAL_NOT,
  VYGES_SLANG_U_PREINCREMENT, VYGES_SLANG_U_PREDECREMENT, VYGES_SLANG_U_POSTINCREMENT,
  VYGES_SLANG_U_POSTDECREMENT,
};

// Binary operator codes: slang's BinaryOperator, in its order.
enum {
  VYGES_SLANG_B_ADD, VYGES_SLANG_B_SUBTRACT, VYGES_SLANG_B_MULTIPLY, VYGES_SLANG_B_DIVIDE,
  VYGES_SLANG_B_MOD, VYGES_SLANG_B_BINARY_AND, VYGES_SLANG_B_BINARY_OR, VYGES_SLANG_B_BINARY_XOR,
  VYGES_SLANG_B_BINARY_XNOR, VYGES_SLANG_B_EQUALITY, VYGES_SLANG_B_INEQUALITY,
  VYGES_SLANG_B_CASE_EQUALITY, VYGES_SLANG_B_CASE_INEQUALITY, VYGES_SLANG_B_GREATER_THAN_EQUAL,
  VYGES_SLANG_B_GREATER_THAN, VYGES_SLANG_B_LESS_THAN_EQUAL, VYGES_SLANG_B_LESS_THAN,
  VYGES_SLANG_B_WILDCARD_EQUALITY, VYGES_SLANG_B_WILDCARD_INEQUALITY, VYGES_SLANG_B_LOGICAL_AND,
  VYGES_SLANG_B_LOGICAL_OR, VYGES_SLANG_B_LOGICAL_IMPLICATION, VYGES_SLANG_B_LOGICAL_EQUIVALENCE,
  VYGES_SLANG_B_LOGICAL_SHIFT_LEFT, VYGES_SLANG_B_LOGICAL_SHIFT_RIGHT,
  VYGES_SLANG_B_ARITHMETIC_SHIFT_LEFT, VYGES_SLANG_B_ARITHMETIC_SHIFT_RIGHT, VYGES_SLANG_B_POWER,
};

// Port directions, for instance ports and blackbox ports.
enum { VYGES_SLANG_DIR_INPUT = 0, VYGES_SLANG_DIR_OUTPUT = 1, VYGES_SLANG_DIR_INOUT = 2 };

typedef struct {
  const char* name;
  size_t name_len;
  int direction;
  vyges_slang_value value;
} vyges_slang_port;

// `graph` says which netlist a request builds: 0 is the top instance's; the frontend starts
// another (NEW_GRAPH, numbered 1, 2, ...) for each instance it elaborates on its own.
typedef struct {
  int graph;
  int method;
  int op;
  vyges_slang_value v[5];
  int flag[2];
  uint64_t width;
  const char* name;
  size_t name_len;
  const vyges_slang_port* ports;
  size_t nports;
} vyges_slang_call;

// The backend. Returns 0 and, for a request that yields a value, points `*out` at its nets (valid
// until the next call). Returns an error code (> 0) and points `*err`/`*err_len` at its message to
// end the elaboration with that error.
typedef int (*vyges_slang_backend_fn)(void* ctx, const vyges_slang_call* call, vyges_slang_value* out,
                                      const char** err, size_t* err_len);

// What the frontend logs: level 0 a plain message, 1 a warning (with its code); errors end the
// elaboration and come back from vyges_slang_elaborate instead.
typedef void (*vyges_slang_log_fn)(void* ctx, int level, int code, const char* msg, size_t len);

// A library cell offered to the design as a blackbox module (`(* blackbox *)`, ANSI ports
// `input|output|inout [width-1:0] name` in this order; no range when width is 1). A cell the
// design already defines is not offered.
typedef struct {
  const char* name;
  int direction;
  int width;
} vyges_slang_bb_port;

typedef struct {
  const char* name;
  const vyges_slang_bb_port* ports;
  size_t nports;
} vyges_slang_bb_cell;

// Elaborates a design: the words of a slang command line (files, -D, -I, --top, ...; no program
// name), optionally a source text compiled as the file `<inline>`, and the blackbox cells. The
// stages are the synthesis frontend's, in its order: options, sources parsed, the compilation
// created and the blackboxes added to it, its diagnostics reported, exactly one top instance
// (programs aside) elaborated through the backend, the frontend's own diagnostics reported.
// slang's reports go to standard output.
//
// Returns 0 elaborated (the top instance's name is copied NUL-terminated into `top`, truncated
// to `top_len` - 1); 1 elaboration failed (options, sources or diagnostics; reported already);
// 2 ended with an error: `*err_code` and the message, NUL-terminated, in `err_msg` (truncated to
// `err_len` - 1); 3 the frontend threw something else (its text in `err_msg`).
int vyges_slang_elaborate(int argc, const char* const* argv, const char* inline_text,
                          const vyges_slang_bb_cell* cells, size_t ncells, vyges_slang_backend_fn backend,
                          vyges_slang_log_fn log, void* ctx, char* top, size_t top_len, int* err_code,
                          char* err_msg, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif  // VYGES_SLANG_ELAB_H
