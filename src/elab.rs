// SPDX-License-Identifier: Apache-2.0
//! The SystemVerilog synthesis frontend: slang compiles the design and the frontend walks it,
//! asking a [`Backend`] for every netlist node in the order it needs them.
//!
//! 🔑 **The netlist is the caller's.** The frontend holds only net ids: [`Backend::call`] gets each
//! request ([`Call`]) and returns the nets it built. Ids 0, 1 and 2 are the constants 0, 1 and x;
//! every other id is one the backend handed out. Values are nets, least significant first.
//!
//! Requests carry a graph number: 0 is the top instance's netlist; [`Call::NewGraph`] starts
//! another, for an instance the frontend elaborates on its own.

use std::any::Any;
use std::ffi::{c_char, c_int, c_void, CString};
use std::panic::{catch_unwind, resume_unwind, AssertUnwindSafe};

/// The constant nets.
pub const ZERO: u32 = 0;
pub const ONE: u32 = 1;
pub const UNDEF: u32 = 2;

/// slang's unary operators, in slang's order (the code a [`Call::Unop`] carries).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum UnaryOp {
    Plus,
    Minus,
    BitwiseNot,
    BitwiseAnd,
    BitwiseOr,
    BitwiseXor,
    BitwiseNand,
    BitwiseNor,
    BitwiseXnor,
    LogicalNot,
    Preincrement,
    Predecrement,
    Postincrement,
    Postdecrement,
}

/// slang's binary operators, in slang's order (the code a [`Call::Biop`] carries).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum BinaryOp {
    Add,
    Subtract,
    Multiply,
    Divide,
    Mod,
    BinaryAnd,
    BinaryOr,
    BinaryXor,
    BinaryXnor,
    Equality,
    Inequality,
    CaseEquality,
    CaseInequality,
    GreaterThanEqual,
    GreaterThan,
    LessThanEqual,
    LessThan,
    WildcardEquality,
    WildcardInequality,
    LogicalAnd,
    LogicalOr,
    LogicalImplication,
    LogicalEquivalence,
    LogicalShiftLeft,
    LogicalShiftRight,
    ArithmeticShiftLeft,
    ArithmeticShiftRight,
    Power,
}

const UNARY: [UnaryOp; 14] = {
    use UnaryOp::*;
    [Plus, Minus, BitwiseNot, BitwiseAnd, BitwiseOr, BitwiseXor, BitwiseNand, BitwiseNor, BitwiseXnor, LogicalNot,
     Preincrement, Predecrement, Postincrement, Postdecrement]
};

const BINARY: [BinaryOp; 28] = {
    use BinaryOp::*;
    [Add, Subtract, Multiply, Divide, Mod, BinaryAnd, BinaryOr, BinaryXor, BinaryXnor, Equality, Inequality,
     CaseEquality, CaseInequality, GreaterThanEqual, GreaterThan, LessThanEqual, LessThan, WildcardEquality,
     WildcardInequality, LogicalAnd, LogicalOr, LogicalImplication, LogicalEquivalence, LogicalShiftLeft,
     LogicalShiftRight, ArithmeticShiftLeft, ArithmeticShiftRight, Power]
};

/// A port direction.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dir {
    Input,
    Output,
    InOut,
}

impl Dir {
    fn from_code(c: c_int) -> Dir {
        match c {
            0 => Dir::Input,
            1 => Dir::Output,
            _ => Dir::InOut,
        }
    }

    fn code(self) -> c_int {
        match self {
            Dir::Input => 0,
            Dir::Output => 1,
            Dir::InOut => 2,
        }
    }
}

/// One connection of an instance.
#[derive(Debug)]
pub struct Port<'a> {
    pub name: &'a str,
    pub direction: Dir,
    pub value: &'a [u32],
}

/// One request from the frontend. Constants (an initialization, memory data) are nets 0/1/2.
#[derive(Debug)]
pub enum Call<'a> {
    Unop { op: UnaryOp, a: &'a [u32], a_signed: bool, y_width: u64 },
    Biop { op: BinaryOp, a: &'a [u32], b: &'a [u32], a_signed: bool, b_signed: bool, y_width: u64 },
    Demux { a: &'a [u32], s: &'a [u32] },
    Bwmux { a: &'a [u32], b: &'a [u32], s: &'a [u32] },
    Bmux { a: &'a [u32], s: &'a [u32] },
    Shift { a: &'a [u32], s: &'a [u32], s_signed: bool, width: u64 },
    Shiftx { a: &'a [u32], s: &'a [u32], s_signed: bool, width: u64 },
    /// `s ? a : b`, as the frontend names its operands (`s` is one net).
    Mux { a: &'a [u32], b: &'a [u32], s: u32 },
    Placeholder { width: u64, name: &'a str, public_name: bool },
    Connect { target: &'a [u32], source: &'a [u32] },
    SetInit { signal: &'a [u32], init: &'a [u32] },
    MemoryInit { name: &'a str, bit_offset: u64, big_endian: bool, data: &'a [u32] },
    Input { name: &'a str, signal: &'a [u32] },
    Output { name: &'a str, signal: &'a [u32] },
    Instance { cell_type: &'a str, ports: Vec<Port<'a>> },
    DualEdgeAldff { base_name: &'a str, clk: &'a [u32], aload: &'a [u32], d: &'a [u32], q: &'a [u32], ad: &'a [u32], aload_polarity: bool },
    Dff { name: &'a str, clk: &'a [u32], d: &'a [u32], q: &'a [u32], clk_polarity: bool },
    Dffe { name: &'a str, clk: &'a [u32], en: &'a [u32], d: &'a [u32], q: &'a [u32], clk_polarity: bool, en_polarity: bool },
    Aldff { name: &'a str, clk: &'a [u32], aload: &'a [u32], d: &'a [u32], q: &'a [u32], ad: &'a [u32], clk_polarity: bool, aload_polarity: bool },
    NewGraph { module: &'a str },
}

/// An error that ends the elaboration: a message code and its text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ElabError {
    pub code: i32,
    pub message: String,
}

/// What the frontend logs on the way.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LogLevel {
    Info,
    Warning,
}

/// The netlist side.
pub trait Backend {
    /// Builds what `call` asks for in `graph`: the nets of its value (empty for a request without
    /// one), or an error (code > 0) that ends the elaboration.
    fn call(&mut self, graph: i32, call: Call<'_>) -> Result<Vec<u32>, ElabError>;

    /// A message from the frontend (a warning has a code). Called after everything slang printed
    /// before it has been flushed.
    fn log(&mut self, level: LogLevel, code: i32, message: &str);
}

/// A library cell offered to the design as a blackbox module, when the design defines no module
/// of that name.
#[derive(Clone, Debug)]
pub struct BlackboxCell {
    pub name: String,
    pub ports: Vec<BlackboxPort>,
}

#[derive(Clone, Debug)]
pub struct BlackboxPort {
    pub name: String,
    pub direction: Dir,
    pub width: i32,
}

/// Why an elaboration produced no netlist.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Failure {
    /// Options, sources or diagnostics stopped it; slang reported why.
    Failed,
    /// It ended with an error.
    Error(ElabError),
    /// The frontend threw something else.
    Exception(String),
    /// An argument, the source text or a cell name holds a NUL byte.
    Nul(String),
}

mod ffi {
    use std::ffi::{c_char, c_int, c_void};

    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct Value {
        pub nets: *const u32,
        pub len: usize,
    }

    #[repr(C)]
    pub struct Port {
        pub name: *const c_char,
        pub name_len: usize,
        pub direction: c_int,
        pub value: Value,
    }

    #[repr(C)]
    pub struct Call {
        pub graph: c_int,
        pub method: c_int,
        pub op: c_int,
        pub v: [Value; 5],
        pub flag: [c_int; 2],
        pub width: u64,
        pub name: *const c_char,
        pub name_len: usize,
        pub ports: *const Port,
        pub nports: usize,
    }

    #[repr(C)]
    pub struct BbPort {
        pub name: *const c_char,
        pub direction: c_int,
        pub width: c_int,
    }

    #[repr(C)]
    pub struct BbCell {
        pub name: *const c_char,
        pub ports: *const BbPort,
        pub nports: usize,
    }

    pub type BackendFn =
        unsafe extern "C" fn(*mut c_void, *const Call, *mut Value, *mut *const c_char, *mut usize) -> c_int;
    pub type LogFn = unsafe extern "C" fn(*mut c_void, c_int, c_int, *const c_char, usize);

    extern "C" {
        #[allow(clippy::too_many_arguments)]
        pub fn vyges_slang_elaborate(
            argc: c_int,
            argv: *const *const c_char,
            inline_text: *const c_char,
            cells: *const BbCell,
            ncells: usize,
            backend: BackendFn,
            log: LogFn,
            ctx: *mut c_void,
            top: *mut c_char,
            top_len: usize,
            err_code: *mut c_int,
            err_msg: *mut c_char,
            err_len: usize,
        ) -> c_int;
    }
}

struct Ctx<'b> {
    backend: &'b mut dyn Backend,
    out: Vec<u32>,
    err: String,
    panic: Option<Box<dyn Any + Send>>,
}

unsafe fn slice<'a>(v: ffi::Value) -> &'a [u32] {
    if v.len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(v.nets, v.len)
    }
}

unsafe fn text<'a>(p: *const c_char, len: usize) -> &'a str {
    if len == 0 {
        return "";
    }
    let bytes = std::slice::from_raw_parts(p as *const u8, len);
    // Names are UTF-8 (slang decoded the sources); one that is not reads as empty.
    std::str::from_utf8(bytes).unwrap_or("")
}

unsafe fn decode<'a>(c: &'a ffi::Call) -> Result<Call<'a>, ElabError> {
    let v = |i: usize| slice(c.v[i]);
    let f = |i: usize| c.flag[i] != 0;
    let name = text(c.name, c.name_len);
    Ok(match c.method {
        0 => Call::Unop {
            op: *UNARY.get(c.op as usize).ok_or_else(|| bad(c))?,
            a: v(0),
            a_signed: f(0),
            y_width: c.width,
        },
        1 => Call::Biop {
            op: *BINARY.get(c.op as usize).ok_or_else(|| bad(c))?,
            a: v(0),
            b: v(1),
            a_signed: f(0),
            b_signed: f(1),
            y_width: c.width,
        },
        2 => Call::Demux { a: v(0), s: v(1) },
        3 => Call::Bwmux { a: v(0), b: v(1), s: v(2) },
        4 => Call::Bmux { a: v(0), s: v(1) },
        5 => Call::Shift { a: v(0), s: v(1), s_signed: f(0), width: c.width },
        6 => Call::Shiftx { a: v(0), s: v(1), s_signed: f(0), width: c.width },
        7 => Call::Mux { a: v(0), b: v(1), s: *v(2).first().ok_or_else(|| bad(c))? },
        8 => Call::Placeholder { width: c.width, name, public_name: f(0) },
        9 => Call::Connect { target: v(0), source: v(1) },
        10 => Call::SetInit { signal: v(0), init: v(1) },
        11 => Call::MemoryInit { name, bit_offset: c.width, big_endian: f(0), data: v(0) },
        12 => Call::Input { name, signal: v(0) },
        13 => Call::Output { name, signal: v(0) },
        14 => {
            let raw = if c.nports == 0 { &[][..] } else { std::slice::from_raw_parts(c.ports, c.nports) };
            let ports = raw
                .iter()
                .map(|p| Port { name: text(p.name, p.name_len), direction: Dir::from_code(p.direction), value: slice(p.value) })
                .collect();
            Call::Instance { cell_type: name, ports }
        }
        15 => Call::DualEdgeAldff { base_name: name, clk: v(0), aload: v(1), d: v(2), q: v(3), ad: v(4), aload_polarity: f(0) },
        16 => Call::Dff { name, clk: v(0), d: v(1), q: v(2), clk_polarity: f(0) },
        17 => Call::Dffe { name, clk: v(0), en: v(1), d: v(2), q: v(3), clk_polarity: f(0), en_polarity: f(1) },
        18 => Call::Aldff { name, clk: v(0), aload: v(1), d: v(2), q: v(3), ad: v(4), clk_polarity: f(0), aload_polarity: f(1) },
        19 => Call::NewGraph { module: name },
        _ => return Err(bad(c)),
    })
}

fn bad(c: &ffi::Call) -> ElabError {
    ElabError { code: 1, message: format!("unknown frontend request (method {}, op {})", c.method, c.op) }
}

unsafe extern "C" fn backend_tramp(
    ctx: *mut c_void,
    call: *const ffi::Call,
    out: *mut ffi::Value,
    err: *mut *const c_char,
    err_len: *mut usize,
) -> c_int {
    let ctx = &mut *(ctx as *mut Ctx);
    if ctx.panic.is_some() {
        return fail(ctx, err, err_len, 1, "backend panicked".into());
    }
    let call = &*call;
    let r = catch_unwind(AssertUnwindSafe(|| match decode(call) {
        Ok(c) => ctx.backend.call(call.graph, c),
        Err(e) => Err(e),
    }));
    match r {
        Ok(Ok(nets)) => {
            ctx.out = nets;
            *out = ffi::Value { nets: ctx.out.as_ptr(), len: ctx.out.len() };
            0
        }
        Ok(Err(e)) => fail(ctx, err, err_len, e.code.max(1), e.message),
        Err(p) => {
            ctx.panic = Some(p);
            fail(ctx, err, err_len, 1, "backend panicked".into())
        }
    }
}

unsafe fn fail(ctx: &mut Ctx, err: *mut *const c_char, err_len: *mut usize, code: i32, message: String) -> c_int {
    ctx.err = message;
    *err = ctx.err.as_ptr() as *const c_char;
    *err_len = ctx.err.len();
    code
}

unsafe extern "C" fn log_tramp(ctx: *mut c_void, level: c_int, code: c_int, msg: *const c_char, len: usize) {
    let ctx = &mut *(ctx as *mut Ctx);
    if ctx.panic.is_some() {
        return;
    }
    let message = String::from_utf8_lossy(if len == 0 { &[] } else { std::slice::from_raw_parts(msg as *const u8, len) }).into_owned();
    let level = if level == 1 { LogLevel::Warning } else { LogLevel::Info };
    if let Err(p) = catch_unwind(AssertUnwindSafe(|| ctx.backend.log(level, code, &message))) {
        ctx.panic = Some(p);
    }
}

fn cstring(s: &str) -> Result<CString, Failure> {
    CString::new(s).map_err(|_| Failure::Nul(s.to_string()))
}

/// Elaborates a design: the words of a slang command line (files, `-D`, `-I`, `--top`, ...; no
/// program name), optionally a source text compiled as the file `<inline>`, and the library cells
/// to offer as blackboxes. Returns the top instance's name; the netlist is what `backend` built.
///
/// slang's own reports go to standard output; flush what this process printed before calling.
pub fn elaborate(args: &[&str], inline_text: Option<&str>, cells: &[BlackboxCell], backend: &mut dyn Backend) -> Result<String, Failure> {
    let args: Vec<CString> = args.iter().map(|a| cstring(a)).collect::<Result<_, _>>()?;
    let argv: Vec<*const c_char> = args.iter().map(|a| a.as_ptr()).collect();
    let inline = inline_text.map(cstring).transpose()?;

    let names: Vec<(CString, Vec<CString>)> = cells
        .iter()
        .map(|c| Ok((cstring(&c.name)?, c.ports.iter().map(|p| cstring(&p.name)).collect::<Result<_, _>>()?)))
        .collect::<Result<_, Failure>>()?;
    let ports: Vec<Vec<ffi::BbPort>> = cells
        .iter()
        .zip(&names)
        .map(|(c, (_, pn))| {
            c.ports
                .iter()
                .zip(pn)
                .map(|(p, n)| ffi::BbPort { name: n.as_ptr(), direction: p.direction.code(), width: p.width })
                .collect()
        })
        .collect();
    let bb: Vec<ffi::BbCell> = names
        .iter()
        .zip(&ports)
        .map(|((n, _), p)| ffi::BbCell { name: n.as_ptr(), ports: p.as_ptr(), nports: p.len() })
        .collect();

    let mut ctx = Ctx { backend, out: Vec::new(), err: String::new(), panic: None };
    let mut top = vec![0u8; 4096];
    let mut err_msg = vec![0u8; 1 << 16];
    let mut err_code: c_int = 0;
    let rc = unsafe {
        ffi::vyges_slang_elaborate(
            argv.len() as c_int,
            argv.as_ptr(),
            inline.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            bb.as_ptr(),
            bb.len(),
            backend_tramp,
            log_tramp,
            &mut ctx as *mut Ctx as *mut c_void,
            top.as_mut_ptr() as *mut c_char,
            top.len(),
            &mut err_code,
            err_msg.as_mut_ptr() as *mut c_char,
            err_msg.len(),
        )
    };
    if let Some(p) = ctx.panic.take() {
        resume_unwind(p);
    }
    let until_nul = |b: &[u8]| String::from_utf8_lossy(&b[..b.iter().position(|&x| x == 0).unwrap_or(b.len())]).into_owned();
    match rc {
        0 => Ok(until_nul(&top)),
        1 => Err(Failure::Failed),
        2 => Err(Failure::Error(ElabError { code: err_code, message: until_nul(&err_msg) })),
        _ => Err(Failure::Exception(until_nul(&err_msg))),
    }
}
