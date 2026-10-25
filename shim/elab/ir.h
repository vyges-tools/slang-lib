// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vyges
//
// The `ir` value types the SystemVerilog frontend computes with, for a build without Yosys.
//
// A net is a plain 32-bit id owned by the graph on the far side of the C API. Ids 0, 1 and 2 are
// the constants 0, 1 and x; every other id is a graph net. A value is a vector of nets, least
// significant first. Nothing here touches a graph: the frontend builds values out of nets the
// backend hands back, and constants.
#ifndef VYGES_SLANG_ELAB_IR_H
#define VYGES_SLANG_ELAB_IR_H

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <vector>

namespace ir {

using NetId = uint32_t;

inline constexpr NetId kZero = 0;
inline constexpr NetId kOne = 1;
inline constexpr NetId kUndef = 2;

// One constant bit: 0, 1 or x. Default-constructed it is x.
struct Trit {
  enum State : uint8_t { kS0 = 0, kS1 = 1, kSx = 2 };
  State raw_;

  constexpr Trit() : raw_(kSx) {}
  constexpr Trit(State s) : raw_(s) {}

  constexpr bool operator==(Trit o) const { return raw_ == o.raw_; }
  constexpr bool operator!=(Trit o) const { return raw_ != o.raw_; }

  // The constant net for this bit (the state is the id).
  constexpr NetId to_net() const { return static_cast<NetId>(raw_); }

  // The bit a net stands for; a graph net reads as x.
  static constexpr Trit from_net(NetId n) { return n <= kUndef ? Trit(static_cast<State>(n)) : Trit(kSx); }
};

inline constexpr Trit S0{Trit::kS0};
inline constexpr Trit S1{Trit::kS1};
inline constexpr Trit Sx{Trit::kSx};

// A constant of any width, least significant bit first.
class Const {
  std::vector<Trit> bits_;

 public:
  using const_iterator = std::vector<Trit>::const_iterator;

  Const() = default;
  // The low `width` bits of `val` sign-extended to 64 bits. A position at 64 or above reads the
  // bit at that position modulo 64, which is what a 64-bit shift does on the targets we build for.
  explicit Const(int val, uint64_t width) {
    const uint64_t v = static_cast<uint64_t>(static_cast<int64_t>(val));
    bits_.reserve(width);
    for (uint64_t i = 0; i < width; i++) bits_.push_back((v >> (i & 63)) & 1 ? S1 : S0);
  }
  explicit Const(Trit t, uint64_t width) : bits_(width, t) {}
  Const(const std::vector<Trit>& bits) : bits_(bits) {}

  int size() const { return static_cast<int>(bits_.size()); }
  bool empty() const { return bits_.empty(); }

  Const extract(int offset, int length) const {
    return Const(std::vector<Trit>(bits_.begin() + offset, bits_.begin() + offset + length));
  }
  void append(Const other) { bits_.insert(bits_.end(), other.bits_.begin(), other.bits_.end()); }
  void append(Trit t) { bits_.push_back(t); }

  // The low 32 bits as an integer, x reading as 0. Signed: a constant of at most 32 bits whose top
  // bit is 1 fills the bits above it with ones.
  int as_int(bool is_signed = false) const {
    uint32_t r = 0;
    const int n = size();
    for (int i = 0; i < n && i < 32; i++)
      if (bits_[i] == S1) r |= uint32_t{1} << i;
    if (is_signed && n > 0 && n <= 32 && bits_[n - 1] == S1)
      for (int i = n; i < 32; i++) r |= uint32_t{1} << i;
    return static_cast<int>(r);
  }

  bool is_fully_undef() const {
    for (Trit b : bits_)
      if (b != Sx) return false;
    return true;
  }

  void set(int i, Trit t) { bits_[i] = t; }
  Trit operator[](int i) const { return bits_[i]; }
  const_iterator begin() const { return bits_.begin(); }
  const_iterator end() const { return bits_.end(); }
  const std::vector<Trit>& bits() const { return bits_; }
};

struct Value;

// One bit of a signal: a net id. Default-constructed it is the constant x.
struct Net {
  NetId raw_;

  Net() : raw_(kUndef) {}
  Net(NetId n) : raw_(n) {}
  Net(Trit t) : raw_(t.to_net()) {}

  NetId raw() const { return raw_; }
  Trit trit() const { return Trit::from_net(raw_); }
  Value repeat(uint64_t width);

  bool operator==(Net o) const { return raw_ == o.raw_; }
  bool operator!=(Net o) const { return raw_ != o.raw_; }
  bool operator==(Trit t) const { return raw_ == t.to_net(); }
  bool operator!=(Trit t) const { return raw_ != t.to_net(); }

  // A constant, x included.
  bool is_const() const { return raw_ <= kUndef; }
  // A constant 0 or 1.
  bool is_def() const { return raw_ == kZero || raw_ == kOne; }
};

// A writable reference to one bit of a value.
class NetProxy {
  std::vector<NetId>& nets_;
  size_t idx_;

 public:
  NetProxy(std::vector<NetId>& nets, size_t i) : nets_(nets), idx_(i) {}
  operator Net() const { return Net(nets_[idx_]); }
  NetProxy& operator=(Net n) {
    nets_[idx_] = n.raw_;
    return *this;
  }
  NetProxy& operator=(const NetProxy& o) {
    nets_[idx_] = o.nets_[o.idx_];
    return *this;
  }
  bool operator==(Net n) const { return Net(*this) == n; }
  bool operator!=(Net n) const { return Net(*this) != n; }
  bool operator==(Trit t) const { return Net(*this) == t; }
  bool operator!=(Trit t) const { return Net(*this) != t; }
  Trit trit() const { return Net(*this).trit(); }
};

// A signal of any width: nets, least significant first.
struct Value {
  std::vector<NetId> nets_;

  Value() = default;
  Value(Net bit) : nets_{bit.raw_} {}
  Value(Trit t) : nets_{t.to_net()} {}
  Value(Trit t, uint64_t width) : nets_(width, t.to_net()) {}
  Value(const Const& c) {
    nets_.reserve(c.size());
    for (Trit t : c) nets_.push_back(t.to_net());
  }
  // The low `width` bits of `val` as 32 bits. A position at 32 or above reads the bit at that
  // position modulo 32, which is what a 32-bit shift does on the targets we build for.
  explicit Value(int val, int width = 32) {
    const uint32_t v = static_cast<uint32_t>(val);
    nets_.reserve(width > 0 ? width : 0);
    for (int i = 0; i < width; i++) nets_.push_back((v >> (i & 31)) & 1 ? kOne : kZero);
  }
  // Concatenation written most significant part first, as in `{a, b}`.
  Value(std::initializer_list<Value> parts) {
    for (auto it = parts.end(); it != parts.begin();) {
      --it;
      nets_.insert(nets_.end(), it->nets_.begin(), it->nets_.end());
    }
  }

  uint64_t size() const { return nets_.size(); }
  uint64_t width() const { return nets_.size(); }
  bool empty() const { return nets_.empty(); }

  Net operator[](uint64_t i) const { return Net(nets_[i]); }
  NetProxy operator[](uint64_t i) { return NetProxy(nets_, i); }

  Value extract(uint64_t offset, uint64_t width) const {
    Value r;
    r.nets_.assign(nets_.begin() + offset, nets_.begin() + offset + width);
    return r;
  }
  Value extract_end(uint64_t offset) const { return extract(offset, nets_.size() - offset); }
  void append(Net bit) { nets_.push_back(bit.raw_); }
  void append(const NetProxy& p) { nets_.push_back(Net(p).raw_); }
  void append(Trit t) { nets_.push_back(t.to_net()); }
  void append(Value other) { nets_.insert(nets_.end(), other.nets_.begin(), other.nets_.end()); }
  void remove(uint64_t index, uint64_t width = 1) {
    nets_.erase(nets_.begin() + index, nets_.begin() + index + width);
  }
  Value repeat(uint64_t count) const {
    Value r;
    r.nets_.reserve(nets_.size() * count);
    for (uint64_t i = 0; i < count; i++) r.nets_.insert(r.nets_.end(), nets_.begin(), nets_.end());
    return r;
  }
  // Truncate to `width`, or widen with zeros (with the top bit when signed and not empty).
  void extend_u0(uint64_t width, bool is_signed = false) {
    if (width < nets_.size()) {
      nets_.resize(width);
      return;
    }
    const NetId fill = is_signed && !nets_.empty() ? nets_.back() : kZero;
    nets_.resize(width, fill);
  }
  Net msb() const { return Net(nets_.back()); }
  void reserve(uint64_t n) { nets_.reserve(n); }

  bool is_fully_const() const {
    for (NetId n : nets_)
      if (n > kUndef) return false;
    return true;
  }
  bool is_fully_def() const {
    for (NetId n : nets_)
      if (n != kZero && n != kOne) return false;
    return true;
  }
  bool is_fully_zero() const {
    for (NetId n : nets_)
      if (n != kZero) return false;
    return true;
  }
  bool is_fully_ones() const {
    for (NetId n : nets_)
      if (n != kOne) return false;
    return true;
  }

  Net as_net() const {
    assert(nets_.size() == 1);
    return Net(nets_[0]);
  }
  // Every net as a bit, graph nets reading as x.
  Const as_const() const {
    std::vector<Trit> bits;
    bits.reserve(nets_.size());
    for (NetId n : nets_) bits.push_back(Trit::from_net(n));
    return Const(bits);
  }
  // True when any bit is the constant 1.
  bool as_bool() const {
    for (NetId n : nets_)
      if (n == kOne) return true;
    return false;
  }
  int as_int(bool is_signed = false) const { return as_const().as_int(is_signed); }

  struct const_iterator {
    using iterator_category = std::input_iterator_tag;
    using value_type = Net;
    using difference_type = std::ptrdiff_t;
    using pointer = const Net*;
    using reference = Net;

    std::vector<NetId>::const_iterator it;
    Net operator*() const { return Net(*it); }
    const_iterator& operator++() {
      ++it;
      return *this;
    }
    const_iterator operator++(int) {
      const_iterator t = *this;
      ++it;
      return t;
    }
    bool operator==(const const_iterator& o) const { return it == o.it; }
    bool operator!=(const const_iterator& o) const { return it != o.it; }
  };
  const_iterator begin() const { return {nets_.begin()}; }
  const_iterator end() const { return {nets_.end()}; }

  bool operator==(const Value& o) const { return nets_ == o.nets_; }
  bool operator!=(const Value& o) const { return nets_ != o.nets_; }
};

inline Value Net::repeat(uint64_t width) { return Value(*this).repeat(width); }

}  // namespace ir

#endif  // VYGES_SLANG_ELAB_IR_H
