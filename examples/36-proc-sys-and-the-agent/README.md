# 36-proc-sys-and-the-agent — sysagent v0

Chapter 36 opens Part 9 (observability). `sysagent` is a small USE-method
metrics collector — the same tool the rest of the Part builds on (chapter 37's
saturation drills, chapter 41's capstone fleet) — reading nothing but
`/proc`, `/proc/net`, `/proc/diskstats`, and cgroup PSI. No root, no
`/dev/kmsg`, no eBPF: everything here is a plain file a normal user can open.

```
sysagent sample [--json] [--interval-ms N]   # one snapshot to stdout
sysagent serve  --port P [--interval-ms N]   # the same snapshot over HTTP

opts: --interval-ms N (default 200)  cpu utilization sampling window
      --json                        machine-readable output for `sample`
```

## Behavior (identical across C++, Go, and Rust)

- **`sample`** takes exactly one snapshot: it reads `/proc/stat` once, sleeps
  `--interval-ms`, reads it again, and reports the CPU delta over that
  window — alongside `/proc/loadavg`, `/proc/meminfo`, `/proc/diskstats`,
  `/proc/net/dev`, and cgroup PSI, all gathered **concurrently** with the
  sleep (C++ `std::jthread`, Go goroutines fanned in over a channel, Rust
  scoped threads — three languages, one idea: don't serialize five
  independent file reads behind a timer). Default text output is
  `key=value` lines; `--json` emits the single-line JSON object below.
- **`serve --port P`** exposes the identical snapshot over a hand-rolled
  HTTP/1.1 `GET /metrics` endpoint (Go uses `net/http`; C++ and Rust hand-roll
  the same tiny surface on raw sockets + `poll(2)`, in keeping with the
  book's earlier chatterd examples) — one fresh sample per request. Any
  other path 404s. `SIGINT`/`SIGTERM` prints a shutdown line and exits 0.
- No/bad arguments print a usage line on stderr and exit 2.

## The deterministic field schema

Every field name below is byte-identical across `--json` and `/metrics` in
all three languages — that's the contract this example exists to prove out:
you can point one Grafana dashboard or one test suite at any of the three.

| field                 | type    | source                                          |
|-----------------------|---------|--------------------------------------------------|
| `cpu_util_pct`        | float   | `/proc/stat` delta: 100 × busy⁄total             |
| `cpu_user_pct`        | float   | `/proc/stat` delta: 100 × user⁄total             |
| `cpu_system_pct`      | float   | `/proc/stat` delta: 100 × system⁄total           |
| `load1/5/15`          | float   | `/proc/loadavg` fields 1–3                       |
| `runnable`            | int     | `/proc/loadavg` "runnable" of runnable/total      |
| `total_threads`       | int     | `/proc/loadavg` "total" of runnable/total         |
| `mem_total_kb`        | int     | `/proc/meminfo` `MemTotal`                        |
| `mem_available_kb`    | int     | `/proc/meminfo` `MemAvailable`                    |
| `mem_used_kb`         | int     | `mem_total_kb - mem_available_kb`                 |
| `disks[]`             | array   | `/proc/diskstats`, one entry per non-loop/ram device: `name`, `reads`, `writes`, `read_sectors`, `write_sectors` |
| `net[]`               | array   | `/proc/net/dev`, one entry per interface: `iface`, `rx_bytes`, `tx_bytes` |
| `psi_available`       | bool    | whether any PSI file below was readable           |
| `psi_cpu_some_avg10`  | float   | cgroup/system `cpu.pressure` "some avg10"         |
| `psi_mem_some_avg10`  | float   | cgroup/system `memory.pressure` "some avg10"      |
| `psi_io_some_avg10`   | float   | cgroup/system `io.pressure` "some avg10"          |

CPU percentages are computed the standard `/proc/stat` way: `busy` is
`user + nice + system + irq + softirq + steal`; `idle` is `idle + iowait`;
`total` is their sum, all as **deltas** between the two `--interval-ms`-apart
readings (a single reading of `/proc/stat` is a cumulative counter since
boot, not a percentage).

PSI (Pressure Stall Information) is read from **this process's own cgroup**
first — resolved via the single `0::<path>` line in `/proc/self/cgroup`
(cgroup v2 unified hierarchy) joined onto `/sys/fs/cgroup<path>/{cpu,memory,io}.pressure`
— falling back to the system-wide `/proc/pressure/{cpu,memory,io}` files if
the cgroup-scoped ones aren't there. If neither is readable (PSI disabled in
the kernel, or a container without a cgroup-v2 delegate), `psi_available` is
`false` and the three `avg10` fields are `0.0` — that's the "if available"
in the chapter spec: a missing PSI source is not an error.

## Example output

```
$ ./demo.sh cpp run sample --json
{"cpu_util_pct":10.69,"cpu_user_pct":9.12,"cpu_system_pct":1.26,"load1":1.21,
 "load5":1.20,"load15":0.91,"runnable":3,"total_threads":3523,
 "mem_total_kb":65545860,"mem_available_kb":36847108,"mem_used_kb":28698752,
 "disks":[{"name":"nvme0n1","reads":698032,"writes":19501729,
           "read_sectors":55243425,"write_sectors":738027946}, ...],
 "net":[{"iface":"lo","rx_bytes":65086669,"tx_bytes":65086669}, ...],
 "psi_available":true,"psi_cpu_some_avg10":0.09,"psi_mem_some_avg10":0.00,
 "psi_io_some_avg10":0.00}

$ ./demo.sh go run serve --port 9100 &
sysagent: listening on 0.0.0.0:9100
$ curl -s http://127.0.0.1:9100/metrics | head -c 120
{"cpu_util_pct":8.18,"cpu_user_pct":4.09,"cpu_system_pct":3.46,"load1":1.27,"load5":1.21,"load15":0.92,"runnable":3,"total_threads
$ kill -INT %1
sysagent: shutting down
```

(That `sample` line reflects real load on the box this was written on — a
concurrent `cc1plus`-heavy build was running in the background, which is
exactly the kind of noise the verification below is built to tolerate.)

## One subject, three concurrency styles

The `/proc` parsing itself is deliberately unglamorous — `open`, read lines,
split fields — so the interesting difference is how each language overlaps
the five independent reads with the CPU-delta sampling window instead of
doing five sequential blocking calls:

- **C++** — `std::jthread` per source (`loadavg`, `meminfo`, `diskstats`,
  `netdev`, `psi`), an `std::atomic<int>` success counter, and a small
  `ProcFile` RAII wrapper around `std::ifstream` so every `/proc` handle
  closes itself. Errors flow through `std::expected<T, std::string>`.
- **Go** — one goroutine per source, fanned in over a single buffered
  `chan namedErr` — the idiomatic Go answer to "wait for N concurrent
  results and keep the first error." Errors are wrapped with `%w` throughout.
- **Rust** — `std::thread::scope` with one scoped closure per source, so the
  borrows into the final `Snapshot` don't need `Arc`/`Mutex`; each closure
  returns a `Result<T, String>` collected via `JoinHandle::join()`. Errors
  flow through `Result`/`?`.

The HTTP layer follows the same three-languages-one-idea pattern as the
book's chatterd examples: Go leans on its stdlib `net/http`; C++ and Rust
hand-roll the request line + `poll(2)` + `signalfd`/`SignalFd` shutdown
doorbell that chapter 24 already established, because there's no reason to
pull in a framework for one endpoint.

## Layout

```
36-proc-sys-and-the-agent/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated behavioral check (schema, cpu-busy delta, serve)
├── cpp/         # procfs.{hpp,cpp} + httpd.{hpp,cpp} + main.cpp, demo.sh
├── go/          # procfs.go + httpd.go + main.go, demo.sh
└── rust/        # src/procfs.rs + src/httpd.rs + src/main.rs, demo.sh
```

## The demo contract

Each language directory has a `demo.sh` with the identical interface used
book-wide: `./demo.sh build`, `./demo.sh run [args]`, and a bare `./demo.sh`
that builds then runs. With `TARGET` set, `run` deploys to that lab VM
instead of running locally — unused here since this chapter is local-only
(`mode: local` in the manifest); it reads the current host's `/proc`, not a
VM's. `run` execs straight into the built binary so signals (`SIGINT` on
`serve`) reach it directly rather than a wrapping shell.

## Verification

`verify.lua` (run by the test harness with `LSP_LANG` set to one language at
a time) asserts observable behavior, not exit 0:

1. Usage errors — no arguments, an unknown subcommand, `serve` without
   `--port` — all exit 2 with a usage line.
2. `sample --json` carries every field in the schema above with a numeric
   (or boolean, for `psi_available`) value, `mem_total_kb` is positive,
   `cpu_util_pct` is in `[0, 100]`, the `disks` array has a named entry, and
   the `net` array includes `lo`.
3. **A cpu-busy child measurably raises `cpu_util_pct` versus an idle
   baseline.** The check spins up `nproc` tight busy loops for slightly
   longer than a `sample --interval-ms 500` window and compares against an
   immediately-prior idle sample, retried up to three times with a 12
   percentage-point margin (or an absolute ≥55% floor, for the case where the
   host is already saturated) — robust against running on a real, shared
   development box rather than an isolated benchmark rig.
4. `serve --port P` answers `GET /metrics` with the same JSON shape, 404s an
   unknown path, and its `mem_total_kb` agrees **exactly** with a direct
   `awk '/^MemTotal:/{print $2}' /proc/meminfo` read in the same process
   (a static fact, unlike load or disk counters, so an exact match is the
   right bar); `SIGINT` shuts it down cleanly (exit 0), logging the listen
   address on start and a shutdown line on the way out.

Exit 0 = pass, 1 = fail, 77 = skip.
