// parhash: parallel FNV-1a 64 checksummer — Go flavor.
//
// Concurrency shape: 4 worker goroutines receive relative paths from an
// unbuffered channel fed by a walker goroutine; results flow back on a second
// channel that main drains. signal.NotifyContext turns SIGINT into context
// cancellation: the walker stops sending, workers finish the file they are on
// (select prefers Done over the next path), and main prints whatever
// completed plus "parhash: interrupted" (exit 130).
package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/signal"
	"path/filepath"
	"slices"
	"strings"
	"sync"
)

const workers = 4

const (
	fnvOffset = 0xcbf29ce484222325
	fnvPrime  = 0x00000100000001b3
)

type result struct {
	rel string
	sum uint64
}

// hashFile streams path through FNV-1a 64 in 64 KiB chunks.
func hashFile(path string) (uint64, error) {
	f, err := os.Open(path)
	if err != nil {
		return 0, fmt.Errorf("open %s: %w", path, err)
	}
	defer f.Close()

	hash := uint64(fnvOffset)
	buf := make([]byte, 64*1024)
	for {
		n, err := f.Read(buf)
		for _, b := range buf[:n] {
			hash ^= uint64(b)
			hash *= fnvPrime
		}
		if errors.Is(err, io.EOF) {
			return hash, nil
		}
		if err != nil {
			return 0, fmt.Errorf("read %s: %w", path, err)
		}
	}
}

// walk streams the relative path of every regular file under root into paths,
// stopping early once ctx is cancelled.
func walk(ctx context.Context, root string, paths chan<- string) {
	defer close(paths)
	_ = filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if ctx.Err() != nil {
			return filepath.SkipAll // interrupted: stop accepting work
		}
		if err != nil || !d.Type().IsRegular() {
			return nil // unreadable subtree or non-file: skip, keep walking
		}
		rel, err := filepath.Rel(root, p)
		if err != nil {
			return nil
		}
		select {
		case paths <- rel:
		case <-ctx.Done():
			return filepath.SkipAll
		}
		return nil
	})
}

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

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: parhash DIR")
		os.Exit(2)
	}
	root := os.Args[1]
	if info, err := os.Stat(root); err != nil || !info.IsDir() {
		fmt.Fprintf(os.Stderr, "parhash: cannot walk %s\n", root)
		os.Exit(1)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	paths := make(chan string)
	results := make(chan result)

	var wg sync.WaitGroup
	for range workers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			worker(ctx, root, paths, results)
		}()
	}
	go walk(ctx, root, paths)
	go func() {
		wg.Wait() // all in-flight hashes drained
		close(results)
	}()

	var out []result
	for r := range results {
		out = append(out, r)
	}

	slices.SortFunc(out, func(a, b result) int { return strings.Compare(a.rel, b.rel) })
	for _, r := range out {
		fmt.Printf("%016x  %s\n", r.sum, r.rel)
	}
	if ctx.Err() != nil {
		fmt.Fprintln(os.Stderr, "parhash: interrupted")
		os.Exit(130)
	}
	fmt.Printf("parhash: %d files, %d workers\n", len(out), workers)
}
