// pmon: a tiny process supervisor whose restart/backoff policy is not
// compiled in -- it is a Lua script (policy.lua), loaded into a sandboxed
// gopher-lua state and re-loaded on demand with a real SIGHUP. See the
// chapter for why this beats a JSON config: the policy can compute (Lua
// closures, exponential backoff, crash-loop detection) instead of merely
// describing.
//
// Two subcommands:
//
//	app run   --policy PATH   run the two-phase demo scenario, reload on SIGHUP
//	app check --policy PATH   load PATH, probe on_exit once, report ok/error
package main

import (
	"errors"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	lua "github.com/yuin/gopher-lua"
)

// event describes one supervised-child exit, handed to the policy's on_exit
// as a plain Lua table of primitives. exitCode/signal are pointers because
// the Lua contract treats "field absent" and "field nil" as the same thing:
// a signal death has no exit_code, a normal exit has no signal.
type event struct {
	name                string
	exitCode            *int64
	signal              *string
	restarts            int64
	consecutiveFailures int64
	uptimeMs            int64
	lastBackoffMs       int64
}

// decision is what the policy's on_exit returned: what to do about the
// child, when, and why. reason is Lua's string, verbatim -- the host never
// constructs decision text itself.
type decision struct {
	action  string
	delayMs int64
	reason  string
}

func main() {
	if len(os.Args) < 2 {
		usage()
	}
	switch os.Args[1] {
	case "run":
		policyPath, err := parsePolicyFlag(os.Args[2:])
		if err != nil {
			usage()
		}
		if err := cmdRun(policyPath); err != nil {
			die(err)
		}
	case "check":
		policyPath, err := parsePolicyFlag(os.Args[2:])
		if err != nil {
			usage()
		}
		os.Exit(cmdCheck(policyPath))
	default:
		usage()
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: app run --policy PATH | app check --policy PATH")
	os.Exit(2)
}

func parsePolicyFlag(args []string) (string, error) {
	if len(args) != 2 || args[0] != "--policy" || args[1] == "" {
		return "", fmt.Errorf("expected --policy PATH")
	}
	return args[1], nil
}

// die is the terminal-error path for `run`: print the fully wrapped error
// chain and exit 1. Every error reaching here was built with fmt.Errorf's
// %w, so the chain (CLI step -> Lua-load step -> Lua's own message) survives
// intact -- this is "errors, three ways" step one: wrapped errors.
func die(err error) {
	fmt.Fprintln(os.Stderr, "pmon:", err)
	os.Exit(1)
}

// luaErr is "errors, three ways" step two: unwrap gopher-lua's *lua.ApiError
// (via errors.As, not a bare type assertion) down to the Lua-side message,
// so a caller sees "attempt to index a nil value (global 'os')" rather than
// a Go-shaped wrapper string.
func luaErr(err error) error {
	var apiErr *lua.ApiError
	if errors.As(err, &apiErr) {
		return errors.New(strings.TrimSpace(apiErr.Object.String()))
	}
	return err
}

// newSandbox builds a fresh Lua state with only base/string/table/math
// opened -- no os, io, package, debug, coroutine -- and the loader globals
// that could pull in more code or touch the filesystem removed. Every
// (re)load, at startup and again on each SIGHUP, gets its own state built
// from scratch: a misbehaving policy can never leave residue for the next
// one to inherit.
func newSandbox() *lua.LState {
	L := lua.NewState(lua.Options{SkipOpenLibs: true})

	for _, lib := range []struct {
		name string
		open lua.LGFunction
	}{
		{lua.BaseLibName, lua.OpenBase},
		{lua.StringLibName, lua.OpenString},
		{lua.TabLibName, lua.OpenTable},
		{lua.MathLibName, lua.OpenMath},
	} {
		L.Push(L.NewFunction(lib.open))
		L.Push(lua.LString(lib.name))
		L.Call(1, 0)
	}

	for _, g := range []string{"load", "loadfile", "dofile", "require", "collectgarbage", "loadstring"} {
		L.SetGlobal(g, lua.LNil)
	}

	// The unopened libraries (os, io, package, debug, coroutine) are left as
	// plain nils by SkipOpenLibs, which gopher-lua's VM reports generically
	// ("attempt to index a non-table object(nil) with key 'execute'") -- it
	// never names the global the way PUC-Rio Lua does. Trade a nil for a
	// guard table whose __index raises a message that names the blocked
	// global explicitly: same sandboxing (the real library is still never
	// opened), a clearer error for whoever runs `check`.
	for _, blocked := range []string{"os", "io", "package", "debug", "coroutine"} {
		name := blocked
		guard := L.NewTable()
		mt := L.NewTable()
		mt.RawSetString("__index", L.NewFunction(func(L *lua.LState) int {
			key := L.CheckString(2)
			L.RaiseError("sandbox: global '%s' is not available (blocked field '%s')", name, key)
			return 0
		}))
		L.SetMetatable(guard, mt)
		L.SetGlobal(name, guard)
	}

	host := L.NewTable()
	L.SetFuncs(host, map[string]lua.LGFunction{
		"log": func(L *lua.LState) int {
			fmt.Printf("pmon: policy: %s\n", L.CheckString(1))
			return 0
		},
		"now_ms": func(L *lua.LState) int {
			L.Push(lua.LNumber(time.Now().UnixMilli()))
			return 1
		},
	})
	L.SetGlobal("host", host)

	return L
}

// loadPolicy compiles and executes the policy chunk under the sandbox (a
// protected load -- a syntax error becomes a returned error, never a panic),
// then calls on_load() if the policy defines one, also protected.
func loadPolicy(L *lua.LState, path string) error {
	if err := L.DoFile(path); err != nil {
		return fmt.Errorf("loading %s: %w", path, luaErr(err))
	}
	if fn, ok := L.GetGlobal("on_load").(*lua.LFunction); ok {
		if err := L.CallByParam(lua.P{Fn: fn, NRet: 0, Protect: true}); err != nil {
			return fmt.Errorf("on_load in %s: %w", path, luaErr(err))
		}
	}
	return nil
}

func i64ptr(n int64) *int64 { return &n }

// callOnExit builds the info table from ev, calls the policy's on_exit under
// a protected call, and reads back action/delay_ms/reason from the returned
// table. signal/exit_code are set on the table only when ev carries them,
// matching the "a nil field is simply absent" contract.
func callOnExit(L *lua.LState, ev event) (decision, error) {
	info := L.NewTable()
	info.RawSetString("name", lua.LString(ev.name))
	if ev.exitCode != nil {
		info.RawSetString("exit_code", lua.LNumber(*ev.exitCode))
	}
	if ev.signal != nil {
		info.RawSetString("signal", lua.LString(*ev.signal))
	}
	info.RawSetString("restarts", lua.LNumber(ev.restarts))
	info.RawSetString("consecutive_failures", lua.LNumber(ev.consecutiveFailures))
	info.RawSetString("uptime_ms", lua.LNumber(ev.uptimeMs))
	info.RawSetString("last_backoff_ms", lua.LNumber(ev.lastBackoffMs))

	fn, ok := L.GetGlobal("on_exit").(*lua.LFunction)
	if !ok {
		return decision{}, fmt.Errorf("policy defines no on_exit function")
	}
	if err := L.CallByParam(lua.P{Fn: fn, NRet: 1, Protect: true}, info); err != nil {
		return decision{}, fmt.Errorf("on_exit: %w", luaErr(err))
	}
	ret := L.Get(-1)
	L.Pop(1)
	tbl, ok := ret.(*lua.LTable)
	if !ok {
		return decision{}, fmt.Errorf("on_exit returned %s, want table", ret.Type())
	}
	return decision{
		action:  tbl.RawGetString("action").String(),
		delayMs: int64(lua.LVAsNumber(tbl.RawGetString("delay_ms"))),
		reason:  tbl.RawGetString("reason").String(),
	}, nil
}

func printDecision(name string, d decision) {
	fmt.Printf("pmon: decision child=%s action=%s delay_ms=%d reason=\"%s\"\n",
		name, d.action, d.delayMs, d.reason)
}

// phase1Events and phase2Events are the fixed scenario `run` replays: three
// exits against the v1 policy, a SIGHUP-triggered reload to v2, then two
// more exits against v2. Hardcoded here (not read from anywhere) because the
// point of this example is the policy engine, not a real process reaper.
func phase1Events() []event {
	return []event{
		{name: "web", exitCode: i64ptr(0), restarts: 0, consecutiveFailures: 0, uptimeMs: 120000, lastBackoffMs: 0},
		{name: "worker", exitCode: i64ptr(1), restarts: 0, consecutiveFailures: 0, uptimeMs: 9000, lastBackoffMs: 0},
		{name: "worker", exitCode: i64ptr(1), restarts: 1, consecutiveFailures: 1, uptimeMs: 9000, lastBackoffMs: 500},
	}
}

func phase2Events() []event {
	return []event{
		{name: "api", exitCode: i64ptr(1), restarts: 0, consecutiveFailures: 0, uptimeMs: 5000, lastBackoffMs: 0},
		{name: "worker", exitCode: i64ptr(1), restarts: 2, consecutiveFailures: 2, uptimeMs: 9000, lastBackoffMs: 1000},
	}
}

func runPhase(L *lua.LState, events []event) error {
	for _, ev := range events {
		d, err := callOnExit(L, ev)
		if err != nil {
			return err
		}
		printDecision(ev.name, d)
	}
	return nil
}

// cmdRun drives the two-phase scenario. The SIGHUP reload is the
// systems-programming content of this chapter (same pattern as ch12): pmon
// is a long-running process that picks up a new policy without restarting.
// signal.Notify + a blocking receive on a buffered channel is Go's idiomatic
// async-signal-safe equivalent of signalfd+poll -- the actual OS-level
// signal handling happens in the runtime's signal thread, never in
// user Go code, so there is nothing unsafe to do inside a handler here.
func cmdRun(policyPath string) error {
	L := newSandbox()
	if err := loadPolicy(L, policyPath); err != nil {
		L.Close()
		return err
	}
	if err := runPhase(L, phase1Events()); err != nil {
		L.Close()
		return err
	}

	fmt.Println("pmon: awaiting SIGHUP to reload policy")
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGHUP)
	<-sigCh
	fmt.Println("pmon: reload requested")

	// Re-read the same --policy path (its content has since changed) into a
	// brand-new sandboxed state; the old one is discarded, not mutated.
	L.Close()
	L = newSandbox()
	if err := loadPolicy(L, policyPath); err != nil {
		L.Close()
		return err
	}
	if err := runPhase(L, phase2Events()); err != nil {
		L.Close()
		return err
	}

	L.Close()
	return nil
}

// cmdCheck loads one policy under the sandbox and probes on_exit once. Any
// failure -- a syntax error at load, an on_load error, or a sandbox
// violation reached from on_exit -- is reported the same way: the operator
// deciding whether to trust this file doesn't need to know which step
// failed, only that it did.
func cmdCheck(policyPath string) int {
	L := newSandbox()
	defer L.Close()

	if err := loadPolicy(L, policyPath); err != nil {
		fmt.Printf("pmon: policy error: %s\n", err)
		return 1
	}

	probe := event{
		name: "probe", exitCode: i64ptr(1),
		restarts: 0, consecutiveFailures: 0, uptimeMs: 9000, lastBackoffMs: 0,
	}
	if _, err := callOnExit(L, probe); err != nil {
		fmt.Printf("pmon: policy error: %s\n", err)
		return 1
	}

	version := L.GetGlobal("POLICY_VERSION").String()
	fmt.Printf("pmon: policy ok version=%s\n", version)
	return 0
}
