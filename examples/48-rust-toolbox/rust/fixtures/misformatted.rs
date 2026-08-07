// Deliberately misformatted fixture for ch48's rustfmt gate.
//
// This file lives OUTSIDE the crate's module tree on purpose: nothing
// `mod`-declares it, so `cargo build`, `cargo test`, and `cargo clippy`
// never see it, and `cargo fmt --check` (which walks the module tree from
// the crate roots) leaves it alone. Only an explicit, per-file
// `rustfmt --edition 2024 --check fixtures/misformatted.rs` reads it -- and
// must exit nonzero, printing a diff.
//
// Do not "fix" the formatting below. The wrong spacing, the 4-into-2 indent,
// the missing trailing comma, and the crammed `if` are the assertion.

pub struct Sample{ pub id:u32, pub label:&'static str }

pub fn classify( n : i64 )->&'static str{
    if n<0 {"negative"} else if n==0 {"zero"}else{"positive"}
}

pub fn sum_all(xs:&[i64])->i64{
        let mut total=0;
        for x in xs { total+=x; }
        total
}
