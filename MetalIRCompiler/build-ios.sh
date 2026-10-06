#!/bin/sh
set -eu
if [ "${1:-}" = "--link-frameworks" ]; then
  shift
  exec python3 "$(dirname "$0")/link_frameworks.py" "$@"
fi
exec python3 "$(dirname "$0")/build_ios_dependencies.py" "$@"
