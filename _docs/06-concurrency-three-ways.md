---
title: "Concurrency, three ways"
order: 6
part: "Foundations"
description: "The second thread that runs the whole book — how each language shares work over the same kernel threads. C++23 jthreads and stop tokens, Go's runtime and channels, Rust's compiler-refereed sharing — one parallel checksummer, byte-identical output, and a SIGINT drain you can watch happen."
duration: 50 minutes
---

Chapter 5 pinned down the first of this book's two permanent threads: how each
language turns an `errno` into something a caller can act on. This chapter
pins down the second — how each language *shares work* — and it does it the
only way that is fair: one program, `parhash`, written three times with each
language's native concurrency toolkit, required to produce **byte-identical
stdout**. Underneath, all three are the same thing — kernel threads created
with `clone(2)`, parked and woken with `futex(2)` — so what you are really
comparing is vocabulary: what C++ hands you as parts, what Go's runtime takes
away from you, and what Rust's compiler refuses to let you get wrong. One
scope note up front: everything here is *thread* concurrency. Async — C++
coroutines, tokio, and what the Go scheduler looks like from below — is
deliberately deferred to Chapter 27, after you have seen the event loops that
motivate it.

The code is in `examples/06-concurrency-three-ways/`. `./demo.sh` builds and
runs all three implementations; its `README.md` covers the shared contract and
how `verify.lua` checks it.

{% include excalidraw.html
   file="06-three-concurrency-models"
   alt="Three columns — C++23, Go, Rust — each showing the same producer, queue, four-worker, cancellation shape: main thread into a mutex/deque/condition_variable_any queue into four jthreads with a sigtimedwait watcher; walker goroutine into an unbuffered channel into four goroutines with signal.NotifyContext; main thread into an Arc-Mutex-wrapped mpsc receiver into four scoped threads with a signal-hook AtomicBool."
   caption="Figure 6.1 — one program shape, three vocabularies; same kernel threads underneath." %}

> **Tools used** — `cmp`, `truncate`, `python3` (host); bcc-tools
> (`offcputime`, `exitsnoop`) (lab VM, exercised in Part 8). Everything here is
> checked by `scripts/check-host.sh`, ships with Fedora, or is preinstalled in
> the lab VMs.

## Three models over one primitive

`parhash DIR` walks every regular file under `DIR` (symlinks are never
followed), streams each through an inline FNV-1a 64 hash with a pool of
exactly **4 workers**, then prints one `<16-hex>  <relpath>` line per file,
sorted, plus a summary. The fixed worker count is part of the contract — the
summary line says `4 workers` in all three languages — and the sort at the end
is what makes byte-identical output possible at all, because the three
languages do not even *walk* the tree in the same order, let alone finish
hashing in one.

**C++23** gives you parts, not a machine. `std::jthread` improves on
`std::thread` in two ways that this example leans on: it joins in its
destructor (a `std::thread` you forget to join calls `std::terminate`), and it
carries a `std::stop_token` — a cooperative cancellation flag that
`condition_variable_any::wait` understands natively. What the standard does
*not* give you is everything else: there is no work queue, no channel, no
select, and — critically for this chapter — no signal story. Signals are POSIX
territory, and mixing them with threads requires a discipline the standard
never mentions: block the signal in every thread, then let exactly one thread
consume it synchronously. The C++ source is real work: about 230 lines, of
which maybe 60 are concurrency plumbing you had to design yourself.

**Go** took the opposite bet: the runtime owns the threads and you never see
them. In the GMP scheduler, *goroutines* (G) — cheap, growable-stack tasks —
are multiplexed by the runtime onto a small set of kernel threads (M), each of
which needs a *processor* slot (P, capped by `GOMAXPROCS`, 16 on this host) to
run Go code; when a goroutine blocks in a syscall its M can hand the P to
another thread, and idle Ps steal runnable Gs from busy ones. That is the
entire mental model this book needs until Chapter 27. What you program against
is not the scheduler but the idiom on top: goroutines communicate over
channels, `select` races multiple channel operations, and cancellation is a
`context.Context` whose `Done()` channel closes — one convention so universal
that the standard library will hand you a context pre-wired to SIGINT.

**Rust** keeps the threads visible — `std::thread::scope` spawns real kernel
threads, one each — and moves the refereeing into the compiler. `Send` and
`Sync` are marker traits computed from a type's contents: `Send` means a value
may move to another thread, `Sync` means `&T` may be shared across threads.
The race you would have written in C++ — four workers pushing into one shared
`Vec` because you forgot the mutex — simply does not compile in Rust: `&mut
Vec` cannot be aliased across threads, and the compiler says so at the exact
line you tried. This example hits a subtler case of the same rule:
`mpsc::Receiver` is not `Sync` — sharing one receiver among four workers is
only legal behind a lock — so the type `Arc<Mutex<mpsc::Receiver<String>>>` in
the source is not a style choice; it is the shortest thing the compiler would
accept. Scoped threads add the ergonomic half: because `scope` guarantees
every spawned thread joins before the scope returns, workers may borrow plain
stack data (the root path, the interrupt flag) without `'static` bounds or
reference counting.

## How the code works

All three sources — `examples/06-concurrency-three-ways/cpp/src/main.cpp`,
`go/main.go`, and `rust/src/main.rs` — share one shape: a producer walks the
tree and feeds relative paths into a queue; four workers pull paths, hash
files, and emit `(relpath, hash)` results; main collects, sorts by path, and
prints. The interesting 30 lines of each are the worker/queue mechanics:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// Mutex-guarded FIFO. pop() blocks with a stop_token-aware wait, so a
// request_stop() wakes idle workers immediately and makes them refuse
// queued-but-unstarted items; a file a worker is mid-hash on always finishes.
class TaskQueue {
public:
    void push(Task task) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] std::optional<Task> pop(std::stop_token st) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, st, [this] { return closed_ || !queue_.empty(); });
        if (st.stop_requested() || queue_.empty()) {
            return std::nullopt;
        }
        Task task = std::move(queue_.front());
        queue_.pop_front();
        return task;
    }

private:
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<Task> queue_;
    bool closed_ = false;
};
```

```go
func worker(ctx context.Context, root string, paths <-chan string, results chan<- result) {
	for {
		// Done first: once cancelled, refuse queued-but-unstarted work.
		select {
		case <-ctx.Done():
			return
		default:
		}
		select {
		case <-ctx.Done():
			return
		case rel, ok := <-paths:
			if !ok {
				return
			}
			sum, err := hashFile(filepath.Join(root, rel))
			if err != nil {
				fmt.Fprintf(os.Stderr, "parhash: skipping %s\n", rel)
				continue
			}
			results <- result{rel: rel, sum: sum}
		}
	}
}
```

```rust
    std::thread::scope(|scope| {
        for _ in 0..WORKERS {
            let path_rx = Arc::clone(&path_rx);
            let result_tx = result_tx.clone();
            let interrupted = Arc::clone(&interrupted);
            let root = &root;
            scope.spawn(move || {
                loop {
                    if interrupted.load(Ordering::Relaxed) {
                        return; // refuse queued-but-unstarted work
                    }
                    // Lock only around recv so dequeues serialize but hashing
                    // runs in parallel across all four workers.
                    let Ok(rel) = path_rx.lock().expect("receiver lock").recv() else {
                        return; // walker done and queue drained
                    };
                    match hash_file(&root.join(&rel)) {
                        Ok(sum) => {
                            let _ = result_tx.send((rel, sum));
                        }
                        Err(_) => eprintln!("parhash: skipping {rel}"),
                    }
                }
            });
        }
        drop(result_tx); // main keeps no sender: result_rx ends when workers do

        walk(&root, Path::new(""), &interrupted, &path_tx);
        drop(path_tx); // walker done: idle workers see the channel close

        for entry in result_rx {
            results.push(entry); // drains until every in-flight hash lands
        }
    });
```

**C++ — the queue you had to build.** `TaskQueue` is the channel C++ does not
ship: a `std::deque` guarded by a mutex, with a `condition_variable_any` for
blocking pops. The load-bearing call is the three-argument
`cv_.wait(lock, st, pred)`: it sleeps until the predicate holds *or the stop
token fires*, which is exactly why the type is `condition_variable_any` and
not plain `condition_variable` — only the `_any` variant accepts a
`stop_token`. Without it, a `request_stop()` could not wake an idle worker,
and Ctrl-C would hang until the next `push`. On waking, `pop` deliberately
checks `st.stop_requested()` *before* looking at the queue: a cancelled worker
refuses work that is queued but unstarted, while a file it is already mid-hash
on always finishes, because cancellation is only ever observed between pops.
`close()` is the producer's end-of-input signal — `notify_all`, because every
idle worker must wake to see `closed_` and exit. The cancellation source is a
watcher `jthread`: `main` blocks SIGINT process-wide with `pthread_sigmask`
*before creating any thread* (the mask is inherited, so no thread can be
interrupted asynchronously), and the watcher loops on `sigtimedwait` with a
20 ms timeout, consuming the signal synchronously and calling
`cancel.request_stop()` — no async-signal-safety puzzle, no handler, ever.

**Go — the idiom in eleven lines of select.** The worker's first `select` has
a `default` arm, making it a non-blocking poll of `ctx.Done()`; the second
races `Done()` against the next path. Both are needed: Go's `select` picks
*pseudorandomly* among ready cases, so if paths keep arriving after
cancellation, the second select alone could keep choosing work forever. The
poll first guarantees a cancelled worker takes no new task. `rel, ok :=
<-paths` with `ok == false` is Go's equivalent of `close()` above — the walker
`close(paths)`s in a `defer`, and every worker unblocks. The wiring in `main`
is the part worth memorizing: `signal.NotifyContext(context.Background(),
os.Interrupt)` gives a context that SIGINT cancels; a `sync.WaitGroup`
counts the four workers; and a fifth goroutine does `wg.Wait(); close(results)`
so that main's `for r := range results` terminates exactly when every
in-flight hash has landed. That closer goroutine *is* the drain.

**Rust — sharing only what the types allow.** The four `scope.spawn` closures
borrow `root` directly (`let root = &root;`) — legal only because scoped
threads provably join before `main`'s stack frame dies. Each worker locks the
shared receiver *only around* `recv()`, so dequeues serialize but hashing runs
four-wide; hold the lock across `hash_file` and you would have built a
one-worker pool with extra steps. The two `drop` calls are doing what `close()`
and the closer goroutine did in the other languages: dropping `path_tx` after
the walk makes every `recv()` return `Err`, which is how idle workers learn
there is no more work; dropping main's `result_tx` clone means `result_rx`'s
iterator ends precisely when the last worker (holding the last sender) exits.
Cancellation is the bluntest of the three: `signal_hook::flag::register`
points SIGINT at a shared `AtomicBool`, and walker and workers poll it with
`Ordering::Relaxed` — a plain flag needs no ordering guarantees, a point
Chapter 26 returns to properly.

### Errors, three ways

The per-file hash function carries this book's Chapter 5 policy into
concurrent code. C++'s `hash_file` returns
`std::expected<std::uint64_t, std::error_code>` and is the only one of the
three that must handle `EINTR` by hand — its raw `::read` loop retries on
`errno == EINTR`, while Go's `os.File.Read` and Rust's `File::read` retry
inside the runtime. Go wraps with `%w` (`fmt.Errorf("open %s: %w", path,
err)`) so callers can still `errors.Is` through the context; Rust does the
same with `anyhow::Context`. The *policy* is identical everywhere: an
unreadable file is not fatal — `parhash: skipping <relpath>` on stderr, keep
hashing; a missing argument is `usage: parhash DIR` and exit 2; and an
interrupt is exit **130**, the shell convention for 128 + SIGINT's number 2.
Exit codes are part of the error surface, and `verify.lua` checks all three.

### Concurrency lens

This chapter *is* the lens the rest of the book looks through, so here is the
first reading. The C++ model buys maximum control — you chose the queue, the
wake policy, the signal discipline — at the price of designing all of it. The
Go model is the least code by far and the only one where a runtime stands
between you and the kernel. The Rust model costs you a negotiation with the
compiler up front and repays it by making the forgotten-mutex bug
unrepresentable. From here on, each example states which model it leans on and
why; when the answer becomes "none of these — the problem wants an event
loop," that is Chapter 27's cue.

## Build, run, observe

```bash
[host]$ cd examples/06-concurrency-three-ways && ./demo.sh build
[host]$ ./demo.sh cpp run /some/tree
```

The Go build is special in one visible way: `go/demo.sh` compiles with
`go build -race`. This is the concurrency chapter, so the race detector
referees every Go run, including all of CI's verification runs. On the book's
five-file fixture (two text files, a nested pair, an empty file), every
language prints exactly:

```
bbd23ea491ed9813  a.txt
4d038ef62a7d9c0b  b.txt
f839d20567c0a911  sub/c.txt
51d88627df287325  sub/deep/d.bin
cbf29ce484222325  zz-empty.dat
parhash: 5 files, 4 workers
```

The empty file's hash is `cbf29ce484222325` — FNV-1a's offset basis, i.e. the
hash of zero bytes — which makes it a nice canary: if that line differs, the
hash loop itself is wrong, not the concurrency. On this host the full runner
pass (`python3 scripts/test-all-examples.py --only 06-concurrency-three-ways`)
reports PASS for all three languages, 8 checks each, Go under `-race`.

## Cross-check: cmp, /proc, and a live Ctrl-C

The contract says the three binaries are interchangeable. `cmp` — which
compares byte streams and says nothing only when they are identical — is the
right referee. On the fixture above:

```bash
[host]$ ./cpp/build/release/app "$FIX" > out-cpp.txt
[host]$ ./go/bin/app "$FIX" > out-go.txt
[host]$ ./rust/target/release/app "$FIX" > out-rust.txt
[host]$ cmp out-cpp.txt out-go.txt && echo identical
identical
[host]$ cmp out-cpp.txt out-rust.txt && echo identical
identical
```

Next, the claim that these are all just kernel threads. Give each binary a
tree big enough to keep it busy — `verify.lua`'s trick is 16 small files plus
96 files of 64 MiB created with `truncate`, ~6.1 GiB logical, 64 KiB on disk,
so the workers hash streams of zeros for seconds while your SSD does nothing —
and count entries in `/proc/<pid>/task`, one per thread, while it runs. On
this host: **C++ 6** (main, four workers, the sigtimedwait watcher), **Rust
5** (main plus four scoped threads — no watcher, since a flag needs none), and
**Go 10** — the runtime decides its own thread count, and under `-race` on a
16-CPU host it settled on ten kernel threads for a program whose source names
only six concurrent activities. Ask *what the threads are doing* and `/proc`
answers that too:

```bash
[host]$ for t in /proc/$pid/task/*; do echo "$(basename $t) $(cat $t/wchan)"; done
2558332 futex_do_wait
2558334 do_sigtimedwait.isra.0
2558335 0
2558336 0
2558337 0
2558338 0
```

That is the C++ binary mid-run: the main thread parked in `futex_do_wait`
(the mutex/CV machinery is futexes underneath), the watcher visibly sitting in
`do_sigtimedwait`, and four workers with `wchan` 0 — running, hashing.

Last, the drain — the behavior Figure 6.2 sequences and the one a validator
can only trust if it was watched happening. Start each binary on the sparse
tree, wait 400 ms, send SIGINT, and collect streams and exit codes. All three
exited **130** with `parhash: interrupted` on stderr, and each printed **20**
completed, well-formed hash lines with no summary line. Twenty is not a
coincidence to wave past: roughly 16 files were fully hashed when the signal
landed, and the **4 in-flight 64 MiB files — one per worker — all completed
after SIGINT**, exactly the finish-what-you-hold guarantee all three
implementations promise. The partial *sets* differ, and that is instructive:
Go's partial output starts `56750b99cb963a67  aa-01.txt` because
`filepath.WalkDir` sorts directory entries, so the small files went first;
C++ and Rust use raw `readdir` order and their partials were twenty
`805f256ad4222325  big-NN.bin` lines (64 MiB of zeros always hashes the same).
Identical *final* output despite different walk orders is precisely why the
program sorts at the end instead of trusting the walk.

{% include excalidraw.html
   file="06-cancellation-drain"
   alt="SIGINT fans out as one flipped flag to three parties — the walker stops enumerating, idle workers wake and exit, busy workers finish the file in hand — then the queue closes and results drain into a final box: partial sorted lines, parhash: interrupted, exit 130."
   caption="Figure 6.2 — the drain: stop accepting, finish what you hold, report what completed." %}

> **On the lab VM** — <span class="status status--unverified">unverified</span>:
> bcc-tools would let you watch the drain from the kernel's side —
> `offcputime` showing the idle workers parked in futex waits, `exitsnoop`
> catching the thread exits as they cascade. Those tools are not runnable on
> this host without root; Chapter 30 (Debugging part) builds that toolkit on
> the `systems-target` VM and points it at these exact binaries.

One asymmetry to name plainly: Go ran every check under its race detector,
but the C++ pool ran with no equivalent referee — this example's CMake presets
stop at `asan`, and no ThreadSanitizer build was exercised here.
<span class="status status--unverified">unverified</span> — the host carries
the `libtsan` runtime (`scripts/check-host.sh` checks for it) and a TSan probe
links and runs, but running *this* worker pool under TSan, and what its
reports look like when the mutex is deliberately removed, is Chapter 29's
job.

## What you learned

- All three models are `clone(2)` + `futex(2)` underneath — `/proc/<pid>/task`
  showed 6, 10, and 5 kernel threads for the same four-worker program, and
  `wchan` showed who was parked where.
- C++23 gives parts (`jthread`, `stop_token`, `condition_variable_any`), and
  you assemble the queue and the signal discipline — block SIGINT everywhere,
  consume it in one thread via `sigtimedwait`, `request_stop()` the rest.
- Go's cancellation idiom is structural: `signal.NotifyContext`, workers that
  `select` on `Done()` before work, and a `WaitGroup`-driven channel close
  that *is* the drain.
- Rust turns sharing mistakes into type errors — `mpsc::Receiver` is not
  `Sync`, so the compiler forces `Arc<Mutex<…>>`; scoped threads let workers
  borrow stack data because the join is guaranteed.
- Cancellation-as-drain is a contract you can verify: stop accepting, finish
  what you hold, report what completed, exit 130 — and all three languages
  honored it live.

Next, Part 2 begins with **file descriptors and the VFS** — the first chapter
of `fwatch`, where the question stops being "who runs the code" and becomes
"what exactly is that small integer the kernel keeps handing us."

---

<p><span class="status status--verified">verified</span> — every number above
comes from runs on the Fedora 44 reference host (kernel 7.1.3-200.fc44) this
session: the runner reported <code>06-concurrency-three-ways  PASS  PASS
PASS</code> (3 passed, 0 failed, Go built with <code>-race</code>); the
fixture outputs of the three binaries were <code>cmp</code>-identical,
including <code>cbf29ce484222325  zz-empty.dat</code>; <code>/proc/&lt;pid&gt;/task</code>
held 6 (C++), 10 (Go), and 5 (Rust) entries mid-run on the ~6.1 GiB sparse
tree, with the quoted <code>wchan</code> values; and SIGINT at ~400 ms
produced exit 130, <code>parhash: interrupted</code> on stderr, and 20
completed hash lines (the 4 in-flight 64 MiB files finishing after the
signal) in each language. The bcc-tools observation and the TSan run remain
unverified as marked above.</p>
