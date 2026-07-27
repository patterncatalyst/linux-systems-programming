# 44 — go-runtime-for-systems

A **single-language** example (Go only): a program that observes its own Go
runtime across four themes, printing self-computed verdict lines so the
behaviour is checkable, not just describable.

- **`sched`** — the GMP scheduler: a rendezvous barrier proves N goroutines are
  concurrently alive, a worker pool proves work distribution, and a busy-loop
  under `GOMAXPROCS=1` proves signal-based **async preemption** (a starved
  goroutine still makes progress).
- **`gc`** — GC pacing: with auto-GC off, one `runtime.GC()` advances `NumGC` by
  exactly 1; then the same churn under `GOGC=100` vs `800`, and under a tight
  `GOMEMLIMIT`, shows the pacer's CPU/memory trade-off in collection counts.
- **`netpoll`** — 2000 goroutines block on pipe reads while the OS **thread**
  count stays bounded (`<= 4*GOMAXPROCS+16`): goroutines are not threads.
- **`knobs`** — echoes the effective `GOMAXPROCS`/`NumCPU`/`GOGC`/`GOMEMLIMIT`/
  `GODEBUG`.

On **Go 1.26** this runs with the Green Tea GC on by default and container-aware
`GOMAXPROCS` (both since 1.25/1.26) — see the chapter.

## Run it

```bash
./demo.sh go run all        # all four themes + final ALL: verdict (what CI verifies)
./demo.sh go run sched      # one theme at a time
./demo.sh go run gc
./demo.sh go run netpoll
./demo.sh go run knobs

# the chapter's live traces (noisy, not asserted by verify.lua):
GODEBUG=gctrace=1 ./go/bin/app gc
GODEBUG=schedtrace=1000,scheddetail=1 ./go/bin/app sched
GODEBUG=asyncpreemptoff=1 ./go/bin/app sched   # the preempt check flips to FAIL
```

## Verify

`verify.lua` (Go only; skips other langs) matches each theme's fixed verdict
label ending in the literal `ok`, plus the `ALL: PASS` roll-up — behaviour the
program asserts about itself, run on the pinned Go 1.26.5 toolchain.

```bash
LSP_LANG=go lua verify.lua
```

Mode: `local`. Stdlib only (`runtime`, `runtime/debug`, `os`, `sync`,
`sync/atomic`, `bufio`) — no external modules.
