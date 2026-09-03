#!/usr/bin/env bash
# Builds and runs the send/receive latency benchmarks (bare DPDK vs. this
# library, both access patterns) against a self-looping net_ring vdev, and
# prints a side-by-side comparison.
#
# Usage: benchmarks/run_benchmark.sh [extra EAL args...]
#   e.g. benchmarks/run_benchmark.sh --no-huge -m 512 --vdev=net_ring0
#
# With no arguments it defaults to --no-huge -m 512 --vdev=net_ring0,
# which needs no hugepages, no root, and no real NIC. If you have
# hugepages reserved and want a more realistic run, pass your own EAL
# args instead, e.g.:
#   benchmarks/run_benchmark.sh --vdev=net_ring0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
# A dedicated build dir, not the default ./build -- benchmark numbers
# are meaningless if that happens to be a Debug configuration left over
# from other work (unoptimized code makes the library look far slower
# than it is, since none of shared_ptr/span/packet_burst's machinery
# gets inlined away). This always configures fresh as Release.
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-bench2}"

EAL_ARGS=("$@")
if [ ${#EAL_ARGS[@]} -eq 0 ]; then
    EAL_ARGS=(--no-huge -m 512 --vdev=net_ring0)
fi

echo "Configuring and building benchmarks (Release, in $BUILD_DIR)..."
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR" --target bench_bare_dpdk bench_library -j"$(nproc)" >/dev/null

BARE_BIN="$BUILD_DIR/bench_bare_dpdk"
LIB_BIN="$BUILD_DIR/bench_library"

run_and_capture() {
    local bin="$1"
    local out
    if ! out=$("$bin" "${EAL_ARGS[@]}" 2>/tmp/bench_stderr.$$); then
        echo "error: $bin failed:" >&2
        cat /tmp/bench_stderr.$$ >&2
        rm -f /tmp/bench_stderr.$$
        exit 1
    fi
    rm -f /tmp/bench_stderr.$$
    echo "$out" | grep -E '^[a-zA-Z0-9_]+=' || true
}

echo "Running bare-DPDK baseline ($BARE_BIN)..."
BARE_OUT="$(run_and_capture "$BARE_BIN")"

echo "Running library benchmark ($LIB_BIN)..."
LIB_OUT="$(run_and_capture "$LIB_BIN")"

ALL_OUT="$BARE_OUT
$LIB_OUT"

get() {
    echo "$ALL_OUT" | awk -F'=' -v k="$1" '$1==k {print $2}'
}

printf "\n%-10s %14s %14s %14s\n" "metric" "bare_dpdk" "lib_direct" "lib_forwarding"
for metric in min p50 p99 mean max; do
    printf "%-10s %14s %14s %14s\n" "$metric" \
        "$(get "bare_dpdk_${metric}_ns")" \
        "$(get "library_direct_${metric}_ns")" \
        "$(get "library_port_forwarding_${metric}_ns")"
done

echo
echo "Overhead vs. bare DPDK:"
awk -v bare="$(get bare_dpdk_p50_ns)" \
    -v direct="$(get library_direct_p50_ns)" \
    -v fwd="$(get library_port_forwarding_p50_ns)" \
    'BEGIN {
        printf "  p50: direct-queue path   +%.1f ns (+%.0f%%)\n", direct-bare, (direct-bare)/bare*100
        printf "  p50: port-forwarding path +%.1f ns (+%.0f%%)\n", fwd-bare, (fwd-bare)/bare*100
    }'

cat <<'EOF'

Note: each "sample" above is the average round-trip time over a batch
of 100 iterations, not a single rdtsc-to-rdtsc measurement -- two
back-to-back rte_rdtsc() calls with nothing between them already cost
~10ns on typical hardware (RDTSC's own execution latency), which would
otherwise swamp a per-call signal this small. See bench_common.h.
EOF
