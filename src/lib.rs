// SPDX-License-Identifier: Apache-2.0
//! slang (SystemVerilog compiler), pinned and statically linked.
//!
//! 🔑 **One call runs slang's own driver.** [`compile`] takes the words of a `slang` command line
//! (files, `-D`, `-I`, `--top`, ...) and hands them to slang's driver unchanged: the same option
//! parsing, source loading, elaboration and diagnostics the `slang` program does. Nothing here
//! re-interprets an option or reorders a source.
//!
//! What comes back is what slang reported ([`Compilation::report`], its text as it prints it) and
//! the elaborated design as slang serialises it to JSON ([`Compilation::ast_json`]).

use std::ffi::{c_char, CString};
use std::sync::Mutex;

mod ffi {
    use std::ffi::c_char;

    #[repr(C)]
    pub struct Compilation {
        _private: [u8; 0],
    }

    extern "C" {
        pub fn vyges_slang_compile(argc: i32, argv: *const *const c_char, report_path: *const c_char, status: *mut i32) -> *mut Compilation;
        pub fn vyges_slang_error_count(c: *const Compilation) -> i32;
        pub fn vyges_slang_ast_json(c: *mut Compilation, pretty: i32, buf: *mut c_char, len: i64) -> i64;
        pub fn vyges_slang_free(c: *mut Compilation);
    }
}

/// The report is captured by redirecting the process's standard output and error, so captured
/// compilations take turns.
static CAPTURE: Mutex<()> = Mutex::new(());

/// Why a call failed before slang produced a compilation.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// An argument contains a NUL byte and cannot be passed to slang.
    Nul(String),
    /// The call threw, or standard output could not be redirected to the report file.
    Failed,
    /// The report file could not be read back.
    Capture(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Nul(a) => write!(f, "argument contains a NUL byte: {a:?}"),
            Error::Failed => write!(f, "slang did not run"),
            Error::Capture(e) => write!(f, "capturing slang's report: {e}"),
        }
    }
}

impl std::error::Error for Error {}

/// How far the compilation got.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    /// Elaborated without errors.
    Ok,
    /// The command line was rejected.
    Options,
    /// A source could not be loaded or parsed.
    Parse,
    /// Elaborated, with errors.
    Errors,
}

/// One compilation: what slang reported and the elaborated design.
pub struct Compilation {
    raw: *mut ffi::Compilation,
    status: Status,
    report: String,
}

// The compilation is owned here and only reached through `&mut self` / `&self`.
unsafe impl Send for Compilation {}

impl Compilation {
    /// How far it got.
    pub fn status(&self) -> Status {
        self.status
    }

    /// Everything slang printed (to standard output and error), in its order.
    pub fn report(&self) -> &str {
        &self.report
    }

    /// The number of error diagnostics slang reported.
    pub fn error_count(&self) -> i32 {
        unsafe { ffi::vyges_slang_error_count(self.raw) }
    }

    /// The elaborated design as slang serialises it to JSON (`--ast-json`'s form); `None` when the
    /// compilation did not get as far as elaborating.
    pub fn ast_json(&mut self, pretty: bool) -> Option<String> {
        let n = unsafe { ffi::vyges_slang_ast_json(self.raw, i32::from(pretty), std::ptr::null_mut(), 0) };
        if n < 0 {
            return None;
        }
        let mut buf = vec![0u8; n as usize + 1];
        let m = unsafe { ffi::vyges_slang_ast_json(self.raw, i32::from(pretty), buf.as_mut_ptr() as *mut c_char, buf.len() as i64) };
        if m != n {
            return None;
        }
        buf.truncate(n as usize);
        String::from_utf8(buf).ok()
    }
}

impl Drop for Compilation {
    fn drop(&mut self) {
        unsafe { ffi::vyges_slang_free(self.raw) }
    }
}

/// Runs slang's driver on `args` — the words after `slang` on its command line — with what it
/// prints captured into [`Compilation::report`].
pub fn compile<S: AsRef<str>>(args: &[S]) -> Result<Compilation, Error> {
    use std::sync::atomic::{AtomicU64, Ordering};
    static N: AtomicU64 = AtomicU64::new(0);
    let words: Vec<CString> = std::iter::once("slang")
        .chain(args.iter().map(|a| a.as_ref()))
        .map(|a| CString::new(a).map_err(|_| Error::Nul(a.to_string())))
        .collect::<Result<_, _>>()?;
    let argv: Vec<*const c_char> = words.iter().map(|w| w.as_ptr()).collect();
    let path = std::env::temp_dir().join(format!("vyges-slang-{}-{}.out", std::process::id(), N.fetch_add(1, Ordering::Relaxed)));
    let cpath = CString::new(path.to_string_lossy().as_bytes()).map_err(|_| Error::Capture(path.display().to_string()))?;
    let _turn = CAPTURE.lock().unwrap_or_else(|e| e.into_inner());
    {
        use std::io::Write;
        let _ = std::io::stdout().flush();
        let _ = std::io::stderr().flush();
    }
    let mut status = -1;
    let raw = unsafe { ffi::vyges_slang_compile(argv.len() as i32, argv.as_ptr(), cpath.as_ptr(), &mut status) };
    let report = std::fs::read_to_string(&path);
    let _ = std::fs::remove_file(&path);
    if raw.is_null() {
        return Err(Error::Failed);
    }
    let status = match status {
        0 => Status::Ok,
        1 => Status::Options,
        2 => Status::Parse,
        _ => Status::Errors,
    };
    let report = match report {
        Ok(r) => r,
        Err(e) => {
            unsafe { ffi::vyges_slang_free(raw) };
            return Err(Error::Capture(e.to_string()));
        }
    };
    Ok(Compilation { raw, status, report })
}
