// SPDX-License-Identifier: Apache-2.0
//! The frontend drives a backend: a recording backend sees each request in order.

use vyges_slang_lib::elab::{elaborate, Backend, BlackboxCell, BlackboxPort, Call, Dir, ElabError, Failure, LogLevel};

#[derive(Default)]
struct Recorder {
    next: u32,
    calls: Vec<String>,
    logs: Vec<String>,
}

impl Recorder {
    fn fresh(&mut self, width: usize) -> Vec<u32> {
        (0..width)
            .map(|_| {
                self.next += 1;
                2 + self.next
            })
            .collect()
    }
}

impl Backend for Recorder {
    fn call(&mut self, graph: i32, call: Call<'_>) -> Result<Vec<u32>, ElabError> {
        self.calls.push(format!("g{graph} {call:?}"));
        Ok(match call {
            Call::Placeholder { width, .. } => self.fresh(width as usize),
            Call::Biop { y_width, .. } | Call::Unop { y_width, .. } => self.fresh(y_width as usize),
            Call::Mux { a, .. } => self.fresh(a.len()),
            Call::Instance { cell_type: "FAIL", .. } => return Err(ElabError { code: 91, message: "refused".into() }),
            _ => Vec::new(),
        })
    }

    fn log(&mut self, level: LogLevel, code: i32, message: &str) {
        self.logs.push(format!("{level:?} {code} {message}"));
    }
}

#[test]
fn adder_requests_in_order() {
    let mut r = Recorder::default();
    let top = elaborate(&[], Some("module top(input [1:0] a, b, output [1:0] y); assign y = a + b; endmodule\n"), &[], &mut r)
        .expect("elaborates");
    assert_eq!(top, "top");
    let text = r.calls.join("\n");
    assert!(text.contains("Placeholder { width: 2, name: \"a\""), "{text}");
    assert!(text.contains("Biop { op: Add"), "{text}");
    assert!(text.contains("Output { name: \"y\""), "{text}");
    assert!(r.calls.iter().all(|c| c.starts_with("g0 ")), "{text}");
}

#[test]
fn blackbox_cell_is_instantiable_and_a_backend_error_ends_it() {
    let cell = BlackboxCell {
        name: "FAIL".into(),
        ports: vec![
            BlackboxPort { name: "A".into(), direction: Dir::Input, width: 2 },
            BlackboxPort { name: "Y".into(), direction: Dir::Output, width: 1 },
        ],
    };
    let mut r = Recorder::default();
    let got = elaborate(
        &[],
        Some("module top(input [1:0] a, output y); FAIL u(.A(a), .Y(y)); endmodule\n"),
        &[cell],
        &mut r,
    );
    assert_eq!(got, Err(Failure::Error(ElabError { code: 91, message: "refused".into() })));
    assert!(r.calls.iter().any(|c| c.contains("Instance { cell_type: \"FAIL\"")), "{:?}", r.calls);
}

#[test]
fn two_tops_is_error_78() {
    let mut r = Recorder::default();
    let got = elaborate(&[], Some("module a; endmodule\nmodule b; endmodule\n"), &[], &mut r);
    assert_eq!(
        got,
        Err(Failure::Error(ElabError { code: 78, message: "Expected exactly one top-level module, got 2".into() }))
    );
}

#[test]
fn syntax_error_fails_with_report() {
    let mut r = Recorder::default();
    assert_eq!(elaborate(&[], Some("module top(; endmodule\n"), &[], &mut r), Err(Failure::Failed));
}
