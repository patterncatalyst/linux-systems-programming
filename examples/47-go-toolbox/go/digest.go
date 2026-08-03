package main

// Digest wraps a single 64-bit FNV-1a hash value. Small on purpose: it
// exists so the delve chapter section (go/.dlvinit) has a concrete,
// non-trivial type to break on and print instead of a bare integer.
type Digest struct {
	FNV uint64
}

const (
	fnvOffsetBasis uint64 = 0xcbf29ce484222325
	fnvPrime       uint64 = 0x100000001b3
)

// kPayload is ch46's exact 16-byte payload ("The quick brown."), reused
// byte-for-byte so the Go and C++ toolbox chapters land on the identical
// FNV-1a digest (0x481984990deee5ff) -- a cross-appendix easter egg, not a
// coincidence. FNV-1a is language-independent: any conforming
// implementation over the same bytes produces the same 64-bit value.
var kPayload = [16]byte{
	0x54, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6b,
	0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x2e,
}

// fnv1a hashes data with the 64-bit FNV-1a algorithm. Integer/string-only
// output, no floats, addresses, timing, or map iteration -- the same input
// bytes produce the same digest bit-for-bit on every conforming Go
// toolchain, on every architecture, every run.
func fnv1a(data []byte) Digest {
	h := fnvOffsetBasis
	for _, b := range data {
		h ^= uint64(b)
		h *= fnvPrime
	}
	return Digest{FNV: h}
}
