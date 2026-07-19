// bugfarm — chapter 29: valgrind, sanitizers, and miri.
//
//	app <leak|uaf|uninit|overflow|race>
//
// Go is memory-safe by construction: there's no dangling pointer to chase
// after free (the GC never reclaims memory a live reference still points
// at), no read of uninitialised memory (every value starts zeroed), and no
// out-of-bounds write that silently corrupts a neighbor (a slice/array
// access past its bound panics immediately -- a runtime check, not
// something a background tool has to catch after the fact). So uaf,
// uninit, and overflow print a "prevented by the runtime" note and exit
// 64: that absence of a bug *is* the language's story here.
//
// What Go can still get wrong:
//
//	leak -- a goroutine that blocks forever. There's no valgrind
//	        equivalent watching the Go heap for this; the way you notice
//	        is runtime.NumGoroutine() not going back down.
//	race -- unsynchronized access to a shared variable from goroutines.
//	        Build with `go build -race` and the runtime's race detector
//	        prints "WARNING: DATA RACE" and the process exits 66.
package main

import (
	"fmt"
	"os"
	"runtime"
	"sync"
	"time"
)

const usage = "usage: app <leak|uaf|uninit|overflow|race>"
const prevented = "go: prevented by the runtime — see chapter"

func parseBug(arg string) (string, error) {
	switch arg {
	case "leak", "uaf", "uninit", "overflow", "race":
		return arg, nil
	default:
		return "", fmt.Errorf("unknown bug: %s", arg)
	}
}

// leak: spawns a goroutine that blocks forever on a channel receive nobody
// will ever send to. Nothing external is watching for this; we notice it
// the same way a real operator would, by the goroutine count never coming
// back down.
func leak() int {
	before := runtime.NumGoroutine()
	block := make(chan struct{})
	go func() {
		<-block // never sent to: this goroutine (and its stack) leaks forever
	}()
	time.Sleep(50 * time.Millisecond) // let the scheduler actually start it
	after := runtime.NumGoroutine()
	leaked := after - before
	fmt.Printf("bugfarm: leak: goroutines before=%d after=%d leaked=%d\n", before, after, leaked)
	if leaked < 1 {
		fmt.Fprintln(os.Stderr, "bugfarm: leak: expected goroutine leak was not observed")
		return 1
	}
	return 0
}

var counter int // race: shared, unsynchronized on purpose

func bump(wg *sync.WaitGroup) {
	defer wg.Done()
	for i := 0; i < 100_000; i++ {
		counter++
	}
}

// race: two goroutines increment a shared int with no lock or atomic. Under
// a plain build this just risks a wrong final count; built with -race, the
// runtime instruments every access and reports the conflicting pair.
func race() int {
	var wg sync.WaitGroup
	wg.Add(2)
	go bump(&wg)
	go bump(&wg)
	wg.Wait()
	fmt.Printf("bugfarm: race: counter=%d (expected 200000; a wrong value is the benign symptom)\n",
		counter)
	return 0
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, usage)
		os.Exit(2)
	}
	bug, err := parseBug(os.Args[1])
	if err != nil {
		fmt.Fprintf(os.Stderr, "app: %v\n%s\n", err, usage)
		os.Exit(2)
	}
	switch bug {
	case "leak":
		os.Exit(leak())
	case "race":
		os.Exit(race())
	default: // uaf, uninit, overflow: not expressible in safe Go
		fmt.Fprintln(os.Stderr, prevented)
		os.Exit(64)
	}
}
