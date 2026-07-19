#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $(basename "$0") NN-slug    e.g. $(basename "$0") 02-hello-syscall" >&2
  exit 2
}

[[ $# -eq 1 ]] || usage
name="$1"
[[ "$name" =~ ^[0-9]{2}-[a-z0-9][a-z0-9-]*$ ]] || usage

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
src="$repo_root/examples/_template"
dst="$repo_root/examples/$name"

[[ -d "$src" ]] || { echo "error: template not found: $src" >&2; exit 1; }
[[ -e "$dst" ]] && { echo "error: refusing to overwrite existing $dst" >&2; exit 1; }

cp -r "$src" "$dst"

# Strip build outputs the template may have on disk (gitignored, but a copied
# CMake cache poisons the new example's out-of-source build).
rm -rf "$dst/cpp/build" "$dst/go/bin" "$dst/rust/target"

if [[ -f "$dst/go/go.mod" ]]; then
  sed -i "s|_template|$name|g" "$dst/go/go.mod"
else
  echo "warning: $dst/go/go.mod not found; go module path not updated" >&2
fi

echo "Created examples/$name"
echo "Next steps:"
echo "  - add an entry for '$name' to examples/manifest.yaml"
echo "  - update the matching chapter in _docs/"
