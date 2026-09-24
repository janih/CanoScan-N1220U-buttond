#!/bin/sh
# Wrapper for launchd: primes the CanoScan N1220U via SANE (see README.md
# for why this is required) and then execs the button-poll daemon.
set -e

export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:$PATH"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/canoscan-n1220u-buttond"
ACTION_SCRIPT="${1:-$SCRIPT_DIR/scan.script}"

# Prime the scanner's firmware/state. Without this, the button-status query
# always reads back 0x00 (see plustek.c's scanbtnd_get_button() comments).
scanimage -L >/dev/null 2>&1 || true

exec "$BINARY" "$ACTION_SCRIPT"
