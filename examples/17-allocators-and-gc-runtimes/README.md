# 17-allocators-and-gc-runtimes — allocbench

Chapter 17 puts the three languages' memory stories side by side with one
allocation-heavy workload: build a string→string index (1000 distinct keys,
overwritten round-robin) out of `N` iterations of short-lived intermediate
strings, then query every key back.

```
allocbench [--allocs N] [--variant default|arena]
```

- `--allocs N` — iterations of churn (default 200000; must be a positive
  integer).
- `--variant default` — every intermediate goes through the language's
  general-purpose allocation path (glibc malloc, the Go heap + GC, the Rust
  global allocator).
- `--variant arena` — the same logical work, but the short-lived
  intermediates are batch-allocated and reclaimed wholesale (or recycled)
  instead of being freed one by one.

## Behavior (identical across C++, Go, and Rust)

- On success, exactly one stdout line:
  `allocbench: variant=<v> allocs=<n> peak_rss=<kb>KB ms=<t>`
  where `peak_rss` is `getrusage(2)` `ru_maxrss` (the process high-water
  mark, `VmHWM`, in KB) and `ms` is the wall time of the build+query phase.
- **Go only** appends ` gc_cycles=<c>`: the delta of
  `runtime.MemStats.NumGC` across the workload, read via
  `runtime.ReadMemStats`. C++ and Rust have no collector, so the field would
  be a lie there — its absence is part of the lesson.
- Bad usage (unknown flag, non-positive `--allocs`, unknown variant) prints
  a diagnostic plus `usage: allocbench [--allocs N] [--variant default|arena]`
  on stderr and exits 2. Runtime failures print `allocbench: ...` on stderr
  and exit 1.
- The query phase is a real check: if any of the distinct keys is missing
  from the index or the summed value bytes are zero, the run fails — the
  benchmark cannot "pass" by optimizing the work away.

## The two variants, per language

| | default | arena |
|---|---|---|
| **C++** | `std::string` intermediates from the global heap, freed one by one when each batch's scratch `std::vector` is destroyed | per-batch `std::pmr::monotonic_buffer_resource` over one reusable 256 KiB slab; intermediates are `std::pmr::string`s held in a `std::pmr::vector`, released wholesale when the resource is destroyed |
| **Go** | fresh strings every iteration (`strconv.Itoa`, `+` concatenation) — all of it garbage for the collector | `sync.Pool` of scratch objects (three `bytes.Buffer`s plus a `strconv.AppendInt` byte slice): churn buffers are recycled, not collected |
| **Rust** | `String`/`format!` intermediates from the global allocator, dropped individually | `bumpalo::Bump` arena: `bumpalo::collections::String`/`Vec` intermediates, reclaimed in O(1) by `bump.reset()` after each 1000-iteration batch |

The long-lived index itself is an ordinary hash map in all six builds; only
the final key/value pairs are copied into it. That is the arena discipline
in one sentence: bulk-free the churn, copy out what survives.

Go deliberately is not "arena" in the C++/Rust sense — the language offers
no stable arena API (the `arena` experiment stalled), so the idiomatic
answer to allocation churn is object reuse via `sync.Pool`, which is what
the variant demonstrates.

## Knobs worth playing with (Go)

The Go runtime exposes its memory policy as environment variables — no
rebuild required:

```console
$ ./demo.sh go run
allocbench: variant=default allocs=200000 peak_rss=8196KB ms=52 gc_cycles=9
$ GOGC=25 ./demo.sh go run                    # GC 4x as eager
allocbench: variant=default allocs=200000 peak_rss=5908KB ms=73 gc_cycles=33
$ GOGC=off GOMEMLIMIT=8MiB ./demo.sh go run   # pacing off; the soft memory
                                              # limit alone drives collection
allocbench: variant=default allocs=200000 peak_rss=5724KB ms=61 gc_cycles=34
$ GOGC=off GOMEMLIMIT=64MiB ./demo.sh go run  # limit never approached...
allocbench: variant=default allocs=200000 peak_rss=37292KB ms=51 gc_cycles=0
```

`GOMEMLIMIT` (Go 1.19+) is a soft cap on the runtime's total memory: as the
heap approaches it, the collector runs more aggressively to stay under —
and if the limit is never approached (last run above), the collector simply
never fires and the garbage piles up as RSS. Watch `gc_cycles` and
`peak_rss` move in opposite directions as you trade one for the other.
`GOGC` scales how much new garbage may accumulate between cycles (default
100 = the heap may double).

## Try it

```console
$ ./demo.sh cpp run                       # default variant, 200000 allocs
allocbench: variant=default allocs=200000 peak_rss=3908KB ms=21
$ ./demo.sh cpp run --variant arena
allocbench: variant=arena allocs=200000 peak_rss=4296KB ms=22
$ ./demo.sh go run --allocs 500000
allocbench: variant=default allocs=500000 peak_rss=8132KB ms=124 gc_cycles=24
$ ./demo.sh rust run
allocbench: variant=default allocs=200000 peak_rss=2536KB ms=57
$ ./demo.sh rust run --variant arena
allocbench: variant=arena allocs=200000 peak_rss=2576KB ms=31
```

(Numbers vary by host and run; only the line shape is stable.) Note what
the C++ pair shows: at this workload size the arena's peak RSS is
*comparable*, not lower — glibc malloc recycles the churn through the same
pages, while the arena's always-touched 256 KiB slab is a fixed cost. The
arena's win here is deterministic batch reclamation (no per-object frees),
not a smaller high-water mark; crank `--allocs` or the value sizes and the
curves separate.

## Demo contract

Each language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary (builds first if needed)
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh` instead of running locally.

The top-level `demo.sh` dispatches: `./demo.sh go run --variant arena` execs
`go/demo.sh run --variant arena`. The binary is always named `app`.

## Verification

`verify.lua` (run per language with `LSP_LANG` set) asserts behavior, not
just exit codes: bad usage exits 2 with the usage line; both variants emit
the exact single-line report; `variant`/`allocs` echo the request;
`peak_rss`/`ms` (and `gc_cycles` for Go — which must be absent for C++ and
Rust) parse to sane numbers; the bare run defaults to
`variant=default allocs=200000`; and a 7-alloc run still answers its
queries. The default-vs-arena RSS delta is printed to the log but not
asserted — ordering can flake on a loaded host, and the chapter text, not
CI, owns that claim.
