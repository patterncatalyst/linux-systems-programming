# Diagrams

Each diagram is a paired `name.svg` (committed, embedded via the `excalidraw.html`
include) + `name.excalidraw` (editable source). Generate both with
`scripts/generate_diagram.py` (see `references/diagram-engine.md`). Catalogue them
here as you add them:

| Diagram | Chapter | What it shows |
|---|---|---|
| `00-course-map` | 0 | The four journey stages, fourteen parts, and six recurring artifacts |
| `01-toolchain-landscape` | 1 | Three language toolchain stacks converging on the shared demo.sh contract |
| `02-lab-topology` | 2 | Host, libvirt NAT network, and the systems-target/systems-peer guests |
| `03-telemetry-pipeline` | 3 | Demos exporting OTLP into the lsp-lgtm container and out to Grafana |
