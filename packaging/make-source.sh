#!/usr/bin/env bash
# Source archive a distribution builds from: landline-<version>.tar.gz
# (engine/, extension/, system/, packaging/), from the working tree.
#   packaging/make-source.sh <version> <out dir>
set -euo pipefail
version="$1"; out="$2"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$out"
cd "$repo"
git ls-files -z --cached --others --exclude-standard -- engine extension system packaging NOTICE.md README.md \
    | tar --null -T - --transform "s|^|landline-$version/|" --owner=0 --group=0 -czf "$out/landline-$version.tar.gz"
ls -l "$out/landline-$version.tar.gz"
