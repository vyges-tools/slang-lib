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
            Call::Mux { a, .. } | Call::Bwmux { a, .. } => self.fresh(a.len()),
            // Every request that yields a value gets one of its width: the frontend checks widths, and
            // a procedural block that fails a check part way aborts (a destructor raises).
            Call::Demux { a, s } => self.fresh(a.len() << s.len()),
            Call::Bmux { a, s } => self.fresh(a.len() >> s.len()),
            Call::Shift { width, .. } | Call::Shiftx { width, .. } => self.fresh(width as usize),
            Call::Blackbox { cell_type: "FAIL", .. } => return Err(ElabError { code: 91, message: "refused".into() }),
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
    // From sv-elab 2026-11-09 a library cell arrives as a blackbox instantiation (with its instance
    // name), not as a module instance.
    assert!(r.calls.iter().any(|c| c.contains("Blackbox { cell_type: \"FAIL\", name: \"u\"")), "{:?}", r.calls);
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

#[test]
fn clock_guard_with_async_load_is_one_aldffe_request() {
    // sv-elab 2026-11-09 adds `add_aldffe`: a flop with an asynchronous load whose clock event has an
    // `iff` guard is ONE request carrying clk, en (the guard) and aload, one net each. An enable written
    // as an `if` in the body stays an aldff behind a mux (the frontend only passes the event guard; a
    // constant guard falls back to `add_aldff`). Reference witness: OpenROAD f11b4276 `sv_elaborate`
    // elaborates this module without error.
    let mut r = Recorder::default();
    elaborate(
        &[],
        Some("module top(input clk, rst, en, d, output reg q); always @(posedge clk iff en or posedge rst) if (rst) q <= 1'b0; else q <= d; endmodule\n"),
        &[],
        &mut r,
    )
    .expect("elaborates");
    let text = r.calls.join("\n");
    assert!(text.contains("Aldffe { name: \"$driver$q\", clk: [3], en: [5], aload: [4]"), "{text}");
    assert!(!text.contains("Aldff {"), "{text}");
}

#[test]
fn unpacked_array_written_in_a_clocked_block_is_per_word_flops_not_a_memory() {
    // Witness (OpenROAD f11b4276 `sv_elaborate`, the reference refuses every memory request with
    // 92-95): this module elaborates without error there, so no memory request is made; here the
    // array becomes one flop per word.
    let mut r = Recorder::default();
    elaborate(
        &[],
        Some("module top(input clk, we, input [1:0] wa, ra, input [3:0] wd, output [3:0] rd); reg [3:0] m [0:3]; always @(posedge clk) if (we) m[wa] <= wd; assign rd = m[ra]; endmodule\n"),
        &[],
        &mut r,
    )
    .expect("elaborates");
    let text = r.calls.join("\n");
    assert!(!text.contains("Memory {") && !text.contains("WritePort {") && !text.contains("ReadPort {"), "{text}");
    assert_eq!(r.calls.iter().filter(|c| c.contains(" Dff { name: \"$driver$m[")).count(), 4, "{text}");
}
