# Linux Systems Programming

A chaptered tutorial site: **Linux systems programming in modern C++23, Go, and
Rust** for intermediate-to-advanced developers. Every hands-on chapter ships the
same runnable example in all three languages (switchable code tabs), and every
example doubles as a standalone demo.

- **Site**: https://patterncatalyst.github.io/linux-systems-programming/
- **Stack**: Jekyll (GitHub Pages via Actions), examples in `examples/NN-slug/{cpp,go,rust}/`
- **Environment**: Fedora 44 host; disposable KVM lab (`systems-target`,
  `systems-peer`) for anything privileged, kernel-observing, or two-host;
  Podman LGTM stack (Grafana/Loki/Tempo/Mimir) + Performance Co-Pilot for
  observability chapters.
- **eBPF scope**: tooling only (bcc-tools, bpftrace, bpftool) — we observe our
  userspace programs; we do not write kernel eBPF.

## Local development

```bash
bundle install
bundle exec jekyll serve --baseurl ""     # http://127.0.0.1:4000/
```

## Verification model

CI validates site integrity and that examples *build*. It cannot run VM, root,
or observability demos — the authoritative gate is a host run of
`scripts/test-all-examples.py --mode all` with the lab up. A demo is marked
**verified** only when it produced its claimed observable effect, never merely
because it compiled.

## License

This project is dual-licensed to separate the code from the writing:

- **Code** — everything under `examples/`, plus build and helper scripts and
  inline code snippets — is licensed under the **Apache License 2.0**
  ([`LICENSE`](LICENSE)).
- **Content** — the tutorial and book prose (chapters under `_docs/`),
  diagrams and images under `assets/`, and the slide decks under
  `presentation/` — is licensed under **Creative Commons Attribution 4.0
  International (CC BY 4.0)** ([`LICENSE-docs.md`](LICENSE-docs.md)).

Copyright 2026 PatternCatalyst (Robert Sedor).
