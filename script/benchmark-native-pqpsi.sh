#!/usr/bin/env bash

set -euo pipefail

export LC_ALL=C
export LANG=C

usage() {
    cat <<'EOF'
Usage
  bash script/benchmark-native-pqpsi.sh [out.md|-]

Purpose
  Native Linux two-process PQ-PSI runner. This does not use Docker. It can
  apply tc shaping on lo when the user has permission.

Examples
  SIZES=128 ROUNDS=1 WARMUPS=0 bash script/benchmark-native-pqpsi.sh -
  SIZES=128 ROUNDS=5 WARMUPS=1 KEM=eckem PI=xoodoo THREADS=4 bash script/benchmark-native-pqpsi.sh native-smoke.md
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" || "${1:-}" == "help" ]]; then
    usage
    exit 0
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

if [[ ! -x "$REPO_ROOT/bin/pqpsi_party_bench" ]]; then
    echo "Missing $REPO_ROOT/bin/pqpsi_party_bench. Build the native binaries first." >&2
    exit 1
fi

BENCH_DIR="${BENCH_DIR:-$REPO_ROOT/build-docker/benchmarks/rbokvs-pqpsi}"
OUTPUT_FILE="${1:-$BENCH_DIR/pqpsi-native-$(date +%Y%m%d-%H%M%S).md}"
TERM_ONLY=0
if [[ "$OUTPUT_FILE" == "-" || "$OUTPUT_FILE" == "stdout" || "$OUTPUT_FILE" == "terminal" ]]; then
    TERM_ONLY=1
    OUTPUT_FILE="$(mktemp "${TMPDIR:-/tmp}/pqpsi-native-report.XXXXXX")"
elif [[ "$OUTPUT_FILE" != */* ]]; then
    OUTPUT_FILE="$BENCH_DIR/$OUTPUT_FILE"
fi

SIZES="${SIZES:-128}"
WARMUPS="${WARMUPS:-1}"
ROUNDS="${ROUNDS:-5}"
RATE="${RATE:-10gbit}"
RTT="${RTT:-}"
DELAY="${DELAY:-}"
THREAD_MODE="${THREAD_MODE:-multi}"
THREADS="${THREADS:-4}"
CHANNELS="${CHANNELS:-}"
KEM="${KEM:-obf-mlkem}"
PI="${PI:-hctr}"
PI_LAMBDA="${PI_LAMBDA:-128}"
PI_ROUNDS="${PI_ROUNDS:-8}"
BOB_PI="${BOB_PI:-0}"
HITS="${HITS:-}"
RB_LAMBDA="${RB_LAMBDA:-40}"
RB_EPS="${RB_EPS:-}"
RB_W="${RB_W:-}"
RB_COLS="${RB_COLS:-}"
PORT_BASE_START="${PORT_BASE_START:-43000}"
RUN_TIMEOUT="${RUN_TIMEOUT:-900}"
STARTUP_SLEEP="${STARTUP_SLEEP:-1}"


parse_delay_ms() {
    python3 - "$1" <<'PY'
import re
import sys
value = sys.argv[1].strip().lower()
m = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?)(ms|s)?", value)
if not m:
    raise SystemExit(f"unsupported delay format: {sys.argv[1]}")
x = float(m.group(1))
unit = m.group(2) or "ms"
if unit == "s":
    x *= 1000.0
print(f"{x:.3f}")
PY
}

if [[ "$RATE" == "none" || "$RATE" == "off" || "$RATE" == "no" ]]; then
    RATE=""
fi
if [[ -n "$RTT" && -n "$DELAY" ]]; then
    echo "Specify at most one of RTT or DELAY" >&2
    exit 1
fi

applied_delay=""
target_rtt=""
if [[ -n "$RTT" ]]; then
    target_rtt="$(parse_delay_ms "$RTT")"
    applied_delay="$(python3 - "$target_rtt" <<'PY'
import sys
print(f"{float(sys.argv[1]) / 2.0:.3f}")
PY
)"
elif [[ -n "$DELAY" ]]; then
    applied_delay="$(parse_delay_ms "$DELAY")"
fi

tc_shape_cmd=(true)
if [[ -n "$RATE" || -n "$applied_delay" ]]; then
    if ! command -v tc >/dev/null 2>&1; then
        echo "tc is required for RATE/RTT/DELAY shaping. Use RATE=none for an unshaped native run." >&2
        exit 1
    fi
    tc_shape_cmd=(tc qdisc replace dev lo root netem)
    if [[ -n "$applied_delay" && "$applied_delay" != "0.000" ]]; then
        tc_shape_cmd+=(delay "${applied_delay}ms")
    fi
    if [[ -n "$RATE" ]]; then
        tc_shape_cmd+=(rate "$RATE")
    fi
fi

cleanup_tc() {
    if [[ "${#tc_shape_cmd[@]}" -gt 1 ]]; then
        tc qdisc del dev lo root >/dev/null 2>&1 || true
    fi
}
trap cleanup_tc EXIT
cleanup_tc
if ! "${tc_shape_cmd[@]}"; then
    echo "failed to apply tc shaping on lo. Use RATE=none, or run with permission to configure tc." >&2
    exit 1
fi

if [[ "$THREAD_MODE" == "single" ]]; then
    THREADS=1
    CHANNELS=1
elif [[ -z "$CHANNELS" ]]; then
    CHANNELS="$THREADS"
fi

RUN_ARGS=()
if [[ -n "$RB_EPS" ]]; then
    RUN_ARGS+=(--rb-eps "$RB_EPS")
fi
if [[ -n "$RB_W" ]]; then
    RUN_ARGS+=(--rb-w "$RB_W")
fi
if [[ -n "$RB_COLS" ]]; then
    RUN_ARGS+=(--rb-cols "$RB_COLS")
fi
RUN_ARGS+=(--rb-lambda "$RB_LAMBDA")
RUN_ARGS+=(--kem "$KEM" --pi "$PI" --pi-lambda "$PI_LAMBDA" --pi-rounds "$PI_ROUNDS")
if [[ -n "$HITS" ]]; then
    RUN_ARGS+=(--hits "$HITS")
fi
if [[ "$BOB_PI" == "0" || "$BOB_PI" == "false" || "$BOB_PI" == "off" || "$BOB_PI" == "no" ]]; then
    RUN_ARGS+=(--no-bob-pi)
else
    RUN_ARGS+=(--bob-pi)
fi
if [[ "$THREAD_MODE" == "single" ]]; then
    RUN_ARGS+=(--single-thread)
else
    RUN_ARGS+=(--multi-thread --threads "$THREADS" --channels "$CHANNELS")
fi

extract_metric() {
    local key="$1"
    local file="$2"
    awk -v key="$key" '$1 == key { print $2 }' "$file" | tail -n 1
}

mean_file() {
    awk 'NF { sum += $1; n += 1 } END { if (n) printf "%.2f", sum / n; else print "-" }' "$1"
}

max_ms() {
    python3 - "$1" "$2" <<'PY'
import sys
print(f"{max(float(sys.argv[1]), float(sys.argv[2])):.3f}")
PY
}

if (( TERM_ONLY == 0 )); then
    mkdir -p "$(dirname "$OUTPUT_FILE")"
fi
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/pqpsi-native.XXXXXX")"
trap 'rm -rf "$tmp_root"; if [[ "${TERM_ONLY:-0}" == "1" ]]; then rm -f "$OUTPUT_FILE"; fi; cleanup_tc' EXIT

{
    printf "# PQ-PSI Native Two-Process Benchmark\n\n"
    printf "This report runs receiver and sender as two native Linux processes. It does not use Docker.\n\n"
    printf "| item | value |\n"
    printf "| --- | --- |\n"
    printf "| measured_rounds | \`%s\` |\n" "$ROUNDS"
    printf "| warmups | \`%s\` |\n" "$WARMUPS"
    printf "| rate | \`%s\` |\n" "${RATE:-none}"
    if [[ -n "$target_rtt" ]]; then
        printf "| target_rtt_ms | \`%s\` |\n" "$target_rtt"
        printf "| tc_delay_ms | \`%s\` |\n" "$applied_delay"
    elif [[ -n "$applied_delay" ]]; then
        printf "| tc_delay_ms | \`%s\` |\n" "$applied_delay"
    else
        printf "| tc_delay_ms | \`none\` |\n"
    fi
    printf "| tc_backend | \`native-lo\` |\n"
    printf "| thread_mode | \`%s\` |\n" "$THREAD_MODE"
    printf "| threads_per_party | \`%s\` |\n" "$THREADS"
    printf "| net_channels | \`%s\` |\n" "$CHANNELS"
    printf "| kem | \`%s\` |\n" "$KEM"
    printf "| pi | \`%s\` |\n" "$PI"
    printf "| bob_pi | \`%s\` |\n\n" "$BOB_PI"
    printf "| Size | Wall Mean (ms) | Comm (KB) | Measured rounds |\n"
    printf "| --- | ---: | ---: | ---: |\n"
} > "$OUTPUT_FILE"

for size in $SIZES; do
    times_file="$tmp_root/time-${size}.txt"
    comm_file="$tmp_root/comm-${size}.txt"
    : > "$times_file"
    : > "$comm_file"

    total_trials=$(( WARMUPS + ROUNDS ))
    for run_idx in $(seq 1 "$total_trials"); do
        phase="measure"
        if (( run_idx <= WARMUPS )); then
            phase="warmup"
        fi

        port=$(( PORT_BASE_START + size + run_idx ))
        tag="native_${size}_${run_idx}_${port}_$$"
        recv_log="$tmp_root/recv-${size}-${run_idx}.log"
        sender_log="$tmp_root/sender-${size}-${run_idx}.log"

        recv_rc=0
        timeout "$RUN_TIMEOUT" ./bin/pqpsi_party_bench 0 "$size" "$port" --tag "$tag" "${RUN_ARGS[@]}" >"$recv_log" 2>&1 &
        recv_pid=$!
        sleep "$STARTUP_SLEEP"

        sender_rc=0
        timeout "$RUN_TIMEOUT" ./bin/pqpsi_party_bench 1 "$size" "$port" --tag "$tag" "${RUN_ARGS[@]}" >"$sender_log" 2>&1 || sender_rc=$?
        if [[ "$sender_rc" != "0" ]]; then
            kill "$recv_pid" >/dev/null 2>&1 || true
        fi
        wait "$recv_pid" || recv_rc=$?

        sender_time_ms="$(extract_metric result_time_ms "$sender_log")"
        receiver_time_ms="$(extract_metric result_time_ms "$recv_log")"
        sender_bytes_sent="$(extract_metric result_bytes_sent "$sender_log")"
        receiver_bytes_sent="$(extract_metric result_bytes_sent "$recv_log")"

        if [[ "$sender_rc" != "0" || "$recv_rc" != "0" || -z "$sender_time_ms" || -z "$receiver_time_ms" ]]; then
            echo "Native PQ-PSI run failed for size=$size run=$run_idx phase=$phase" >&2
            echo "--- receiver log ---" >&2
            cat "$recv_log" >&2
            echo "--- sender log ---" >&2
            cat "$sender_log" >&2
            exit 1
        fi

        if [[ "$phase" == "measure" ]]; then
            round_time_ms="$(max_ms "$sender_time_ms" "$receiver_time_ms")"
            round_comm_bytes=$(( ${sender_bytes_sent:-0} + ${receiver_bytes_sent:-0} ))
            printf '%s\n' "$round_time_ms" >> "$times_file"
            printf '%s\n' "$round_comm_bytes" >> "$comm_file"
        fi
    done

    mean_ms="$(mean_file "$times_file")"
    comm_kb="$(awk 'NF { sum += $1; n += 1 } END { if (n) printf "%.2f", (sum / n) / 1024.0; else print "-" }' "$comm_file")"
    printf "| \`%s\` | \`%s\` | \`%s\` | \`%s\` |\n" "$size" "$mean_ms" "$comm_kb" "$ROUNDS" >> "$OUTPUT_FILE"

    if (( TERM_ONLY == 1 )); then
        printf "\nNative two-process PQ-PSI, size=%s\n" "$size"
        printf "setting: kem=%s pi=%s bob_pi=%s threads=%s channels=%s rounds=%s warmups=%s\n" \
            "$KEM" "$PI" "$BOB_PI" "$THREADS" "$CHANNELS" "$ROUNDS" "$WARMUPS"
        printf "summary: wall=%sms comm=%sKB\n" "$mean_ms" "$comm_kb"
    fi
done

if (( TERM_ONLY == 0 )); then
    printf "\nWrote %s\n" "$OUTPUT_FILE"
fi
