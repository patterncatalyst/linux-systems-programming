import sys
sys.path.insert(0, "scripts")
import generate_diagram as g
g.OUT = "assets/diagrams"

# ── Figure 56.1 — one workload, seven models, two uniform instruments ──────
bands = [
  dict(x=24, y=40, w=1112, h=96, label="ONE workload, shared verbatim by every model (workload.hpp)", fill="#fbfbfb"),
  dict(x=24, y=156, w=1112, h=164, label="one OS thread — concurrent, not parallel (ch49). No lock anywhere, because there is nothing to race with.", fill="#f7faf7"),
  dict(x=24, y=340, w=1112, h=176, label="many OS threads + a LOCK — a thread that loses the race is parked, and has to be woken", fill="#faf7f7"),
  dict(x=24, y=536, w=1112, h=132, label="many OS threads + a STRAND — the handler is deferred, so no thread ever blocks", fill="#fff8ef"),
]
nodes = [
  dict(x=180, y=70, w=800, h=54, style="ink", lines=[
      "8 tasks x 5000 rounds = 40000 folds into one accumulator",
      "every model below produced total = 0xb75768f6610642a0  (gate B, checked before any comparison is drawn)"]),

  dict(x=45,  y=196, w=330, h=100, lines=["sequential  (the control)", "distinct_tids = 1", "futex calls = 1", "8 tasks, one after another"]),
  dict(x=415, y=196, w=330, h=100, lines=["coroutine  (ch53)", "distinct_tids = 1", "futex calls = 1", "8 in flight, 32-byte frames"]),
  dict(x=785, y=196, w=330, h=100, lines=["fiber  (ch54)", "distinct_tids = 1", "futex calls = 1", "8 in flight, 128 KiB stacks"]),

  dict(x=44,  y=380, w=250, h=112, lines=["pthreads  (ch50)", "distinct_tids = 8", "futex calls = 230", "pthread_mutex_t"]),
  dict(x=318, y=380, w=250, h=112, lines=["std::jthread  (ch51)", "distinct_tids = 8", "futex calls = 427", "std::mutex"]),
  dict(x=592, y=380, w=250, h=112, lines=["Boost.Thread  (ch52)", "distinct_tids = 8", "futex calls = 451", "boost::mutex + when_all"]),
  dict(x=866, y=380, w=250, h=112, style="ghost", lines=["P2300 senders  (opt-in)", "distinct_tids = 8", "futex calls = 562", "NVIDIA stdexec, NOT the stdlib"]),

  dict(x=330, y=568, w=500, h=88, style="accent", lines=["Boost.Asio strand  (ch55)", "distinct_tids = 8   ·   futex calls = 51", "same 8 threads as the row above, serialized without a lock"]),
]
edges = [
  dict(x1=717, y1=496, x2=717, y2=566, amber=True, label="1/8 the futex traffic of the row above", lx=0, ly=-8),
]
notes = [
  dict(x=24, y=692, text="Instrument 1: gettid(), ch50's. Instrument 2: strace -f -c -e trace=futex, ch51's. Both applied identically to all seven models — which is the only thing that makes this a comparison rather than a table of quotations."),
  dict(x=24, y=710, text="Futex magnitudes move on every run and are never gated; the three TIERS are. Measured on the Fedora 44 reference host, Boost 1.90.0, GCC 16.1.1.", color="#555555"),
]
g.emit("56-one-workload-seven-models", 1160, 726, bands, nodes, edges, notes)

# ── Figure 56.2 — the two assembled axes ──────────────────────────────────
bands2 = [
  dict(x=24,  y=40, w=540, h=506, label="what a paused computation costs", fill="#fbfbfb"),
  dict(x=596, y=40, w=540, h=506, label="how cancellation arrives", fill="#fbfbfb"),
]
nodes2 = [
  dict(x=64,  y=96,  w=460, h=92, lines=["thread — 8388608 bytes", "ch50, pthread_getattr_np", "the kernel schedules it, so it costs kernel state"]),
  dict(x=64,  y=246, w=460, h=92, style="accent", lines=["fiber — 131072 bytes", "ch54, stack_traits::default_size()", "user-scheduled, but it keeps a real stack"]),
  dict(x=64,  y=396, w=460, h=92, lines=["coroutine frame — 32 bytes", "ch53, operator new in promise_type", "no stack: only what crosses a suspension"]),

  dict(x=636, y=96,  w=460, h=92, lines=["ch50  pthread_cancel", "a FORCED UNWIND", "swallow it without rethrowing and the process aborts"]),
  dict(x=636, y=206, w=460, h=92, lines=["ch51  std::stop_token", "a FLAG the target polls", "nothing is thrown; there is nothing to swallow"]),
  dict(x=636, y=316, w=460, h=92, lines=["ch52  thread::interrupt()", "an ORDINARY EXCEPTION", "thrown at defined interruption points"]),
  dict(x=636, y=426, w=460, h=92, style="accent", lines=["ch55  cancellation_signal", "a COMPLETION carrying operation_aborted", "the handler still runs exactly once, so ownership never changes"]),
]
edges2 = [
  dict(x1=294, y1=188, x2=294, y2=244, amber=True, label="divide by 64", lx=0, ly=-4),
  dict(x1=294, y1=338, x2=294, y2=394, amber=True, label="divide by 4096", lx=0, ly=-4),
]
notes2 = [
  dict(x=24, y=578, text="NEITHER band is measured in ch56. Both are assembled from earlier chapters' own measurements, carried into the table arm as named constants beside the instrument that produced them — the pattern ch54 established."),
  dict(x=24, y=596, text="Cost and capability move together and in opposite directions: the more a paused computation costs, the fewer restrictions there are on where it may pause.", color="#555555"),
  dict(x=24, y=614, text="P2300 senders are absent from both bands on purpose: __cpp_lib_senders is still undefined here, so ch56's seventh arm links NVIDIA's reference implementation, not the standard library.", color="#555555"),
]
g.emit("56-two-assembled-axes", 1160, 636, bands2, nodes2, edges2, notes2)
