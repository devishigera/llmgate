#!/usr/bin/env bash
# Runs the two LLMGate benchmarks (cache hit, cache miss) with wrk.
#
# Usage:   tools/loadtest.sh
# Options (environment variables):
#   BIN=build-linux-release/llmgate   which binary to test
#   CONFIG=config/bench.example.json  which config to use
#   PORT=8080                         port to use
#   DURATION=30s                      length of each run
#   THREADS=2  CONNECTIONS=50         wrk settings
set -u

BIN="${BIN:-build-linux-release/llmgate}"
CONFIG="${CONFIG:-config/bench.example.json}"
PORT="${PORT:-8080}"
DURATION="${DURATION:-30s}"
THREADS="${THREADS:-2}"
CONNECTIONS="${CONNECTIONS:-50}"
URL="http://127.0.0.1:${PORT}/v1/chat"

if ! command -v wrk >/dev/null 2>&1; then
  echo "error: wrk is not installed (sudo apt install -y wrk)" >&2
  exit 1
fi
if [ ! -x "$BIN" ]; then
  echo "error: $BIN not found. Build it first:" >&2
  echo "  cmake -B build-linux-release -DCMAKE_BUILD_TYPE=Release" >&2
  echo "  cmake --build build-linux-release -j\"\$(nproc)\"" >&2
  exit 1
fi

mkdir -p bench-results
STAMP="$(date +%Y%m%d-%H%M%S)"

LLMGATE_LOG_LEVEL=off LLMGATE_PORT="$PORT" "$BIN" --config "$CONFIG" > "bench-results/$STAMP-server.log" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null' EXIT

# Wait up to 10 seconds for the server to answer /health.
ready=0
for _ in $(seq 1 50); do
  if curl -s -o /dev/null "http://127.0.0.1:${PORT}/health"; then ready=1; break; fi
  sleep 0.2
done
if [ "$ready" -ne 1 ]; then
  echo "error: server did not start; see bench-results/$STAMP-server.log" >&2
  exit 1
fi

echo "commit:  $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "binary:  $BIN"
echo "config:  $CONFIG"
echo "cpus:    $(nproc)"
echo

run() {
  local name="$1" script="$2"
  echo "=== $name ==="
  wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" --latency -s "$script" "$URL" \
    | tee "bench-results/$STAMP-$name.txt"
  if grep -q "Non-2xx or 3xx" "bench-results/$STAMP-$name.txt"; then
    echo "WARNING: $name had non-2xx responses, so these numbers are NOT valid." >&2
  fi
  echo
}

run cache-hit  tools/post.lua
run cache-miss tools/post_unique.lua

echo "Raw output saved in bench-results/ (prefix $STAMP)."
