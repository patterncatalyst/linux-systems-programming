//! Deliberate, feature-gated clippy bait (ch48's clippy gate). Only compiled
//! in with `--features lintbait`; the default build (and the default
//! `cargo clippy --all-targets -- -D warnings` gate) never sees this file,
//! so the crate stays lint-clean out of the box.
//!
//! `trip_needless_return` trips the stable `clippy::needless_return` lint:
//! an explicit `return x;` as a function's last statement, where a bare
//! trailing expression `x` would do. Left un-`#[allow]`-ed on purpose --
//! allowing the lint would defeat the point of the gate.

pub fn trip_needless_return(x: i64) -> i64 {
    let y = x * 2;
    return y;
}
