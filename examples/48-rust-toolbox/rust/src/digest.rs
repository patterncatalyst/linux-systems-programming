//! `Digest` wraps a single 64-bit FNV-1a hash value. Small on purpose: it
//! gives the chapter's `cargo test` gate a concrete, non-trivial type to
//! assert on instead of a bare integer.

const FNV_OFFSET_BASIS: u64 = 0xcbf29ce484222325;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Digest {
    pub fnv: u64,
}

/// ch46/ch47's exact 16-byte payload (`"The quick brown."`), reused
/// byte-for-byte so the C++, Go, and Rust toolbox chapters land on the
/// identical FNV-1a digest (`0x481984990deee5ff`) -- a cross-appendix
/// easter egg, not a coincidence. FNV-1a is language-independent: any
/// conforming implementation over the same bytes produces the same 64-bit
/// value.
pub const PAYLOAD: [u8; 16] = [
    0x54, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6b, 0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x2e,
];

/// FNV-1a over `data`. Integer-only, no floats, addresses, timing, or
/// hash-map iteration -- the same input bytes produce the same digest
/// bit-for-bit on every conforming Rust toolchain, every architecture,
/// every run.
pub fn fnv1a(data: &[u8]) -> Digest {
    let mut h = FNV_OFFSET_BASIS;
    for &b in data {
        h ^= b as u64;
        h = h.wrapping_mul(FNV_PRIME);
    }
    Digest { fnv: h }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fnv1a_matches_cross_language_digest() {
        let d = fnv1a(&PAYLOAD);
        assert_eq!(
            d,
            Digest {
                fnv: 0x481984990deee5ff
            },
            "digest must match the C++/Go toolbox chapters' literal"
        );
    }
}
