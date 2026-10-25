// SPDX-License-Identifier: Apache-2.0
//! The pinned slang through the crate's API: a clean design elaborates and serialises, a broken
//! one reports its error, an unknown option is rejected.

use std::path::PathBuf;

use vyges_slang_lib::{compile, Status};

fn write(name: &str, text: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("vyges-slang-test-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let p = dir.join(name);
    std::fs::write(&p, text).unwrap();
    p
}

#[test]
fn a_clean_design_elaborates_to_json() {
    let f = write("counter.sv", "module counter #(parameter W = 4) (input logic clk, output logic [W-1:0] q);\n  always_ff @(posedge clk) q <= q + 1'b1;\nendmodule\n\nmodule top(input logic clk, output logic [7:0] q);\n  counter #(.W(8)) u(.clk, .q);\nendmodule\n");
    let mut c = compile(&[f.to_str().unwrap(), "--top", "top"]).unwrap();
    assert_eq!(c.status(), Status::Ok, "{}", c.report());
    assert_eq!(c.error_count(), 0);
    let json = c.ast_json(false).expect("an elaborated design");
    // The top instance and the parameter value elaboration gave the child.
    assert!(json.starts_with("{\"design\":"), "{json}");
    assert!(json.contains("\"name\":\"top\""), "{json}");
    assert!(json.contains("\"name\":\"W\""), "{json}");
    assert!(json.contains("\"value\":\"8\""), "{json}");
    assert!(c.report().contains("Build succeeded"), "{}", c.report());
}

#[test]
fn an_undeclared_name_is_an_error() {
    let f = write("bad.sv", "module bad(output logic y);\n  assign y = nope;\nendmodule\n");
    let c = compile(&[f.to_str().unwrap()]).unwrap();
    assert_eq!(c.status(), Status::Errors);
    assert_eq!(c.error_count(), 1);
    assert!(c.report().contains("use of undeclared identifier 'nope'"), "{}", c.report());
}

#[test]
fn an_unknown_option_is_rejected() {
    let c = compile(&["--no-such-option"]).unwrap();
    assert_eq!(c.status(), Status::Options, "{}", c.report());
}
