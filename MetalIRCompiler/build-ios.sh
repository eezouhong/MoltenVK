#!/bin/sh
set -eu
exec python3 "$(dirname "$0")/build_ios_dependencies.py" "$@"
