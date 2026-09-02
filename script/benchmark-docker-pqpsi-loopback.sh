#!/usr/bin/env bash

set -euo pipefail

export LC_ALL=C
export LANG=C
export PATH="/opt/homebrew/bin:/usr/local/bin:/Applications/Docker.app/Contents/Resources/bin:$PATH"

usage() {
    cat <<'EOF'
Usage
  bash script/benchmark-docker-pqpsi-loopback.sh [out.md|-]

Purpose
  MiniPSI/VOLE-PSI aligned loopback benchmark:
  - one Linux Docker container per round
  - receiver and sender are two separate processes
  - tc netem is applied to lo inside the container
  - reported time is max(sender protocol time, receiver protocol time)

Examples
  RATE=10gbit THREAD_MODE=single bash script/benchmark-docker-pqpsi-loopback.sh lan-single.md
  RATE=200mbit RTT=80ms THREAD_MODE=multi THREADS=4 bash script/benchmark-docker-pqpsi-loopback.sh wan-multi.md
  SIZES=128 ROUNDS=5 RATE=none KEM=eckem PI=xoodoo bash script/benchmark-docker-pqpsi-loopback.sh -
  KEM=eckem PI=xoodoo SIZES="128 256 512 1024" RATE=200mbit RTT=80ms bash script/benchmark-docker-pqpsi-loopback.sh eckem-wan.md

Defaults
  SIZES="128 256 512 1024"
  WARMUPS=3
  ROUNDS=60
  RATE=10gbit
  RTT=
  DELAY=
  THREAD_MODE=multi
  THREADS=4
  CHANNELS=
  PIN_CPUS=0
  RECV_CPUS=0-3
  SEND_CPUS=4-7
  KEM=obf-mlkem
  PI=hctr
  HITS=setSize-1
  RB_LAMBDA=40
  RB_EPS=
  RB_W=
  RB_COLS=

Notes
  RTT=80ms means tc gets delay 40ms on lo.
  DELAY=80ms means one-way tc delay 80ms on lo.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" || "${1:-}" == "help" ]]; then
    usage
    exit 0
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-docker}"
BENCH_DIR="${BENCH_DIR:-$BUILD_DIR/benchmarks/rbokvs-pqpsi}"
OUTPUT_FILE="${1:-$BENCH_DIR/pqpsi-loopback-$(date +%Y%m%d-%H%M%S).md}"
TERM_ONLY=0
if [[ "$OUTPUT_FILE" == "-" || "$OUTPUT_FILE" == "stdout" || "$OUTPUT_FILE" == "terminal" ]]; then
    TERM_ONLY=1
    OUTPUT_FILE="$(mktemp "${TMPDIR:-/tmp}/pqpsi-loopback-report.XXXXXX")"
elif [[ "$OUTPUT_FILE" != */* ]]; then
    OUTPUT_FILE="$BENCH_DIR/$OUTPUT_FILE"
fi

SIZES="${SIZES:-128 256 512 1024}"
WARMUPS="${WARMUPS:-3}"
ROUNDS="${ROUNDS:-60}"
TOTAL_TRIALS=$(( WARMUPS + ROUNDS ))
RATE="${RATE:-10gbit}"
if [[ "$RATE" == "none" || "$RATE" == "off" || "$RATE" == "no" ]]; then
    RATE=""
fi
RTT="${RTT:-}"
DELAY="${DELAY:-}"
THREAD_MODE="${THREAD_MODE:-multi}"
THREADS="${THREADS:-4}"
CHANNELS="${CHANNELS:-}"
PIN_CPUS="${PIN_CPUS:-0}"
RECV_CPUS="${RECV_CPUS:-0-3}"
SEND_CPUS="${SEND_CPUS:-4-7}"
APPEND_ROUNDS="${APPEND_ROUNDS:-0}"
SAMPLE_DIR="${SAMPLE_DIR:-}"
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
MAX_ATTEMPTS="${MAX_ATTEMPTS:-3}"
DOCKER_PLATFORM="${DOCKER_PLATFORM:-linux/amd64}"
DOCKER_IMAGE="${DOCKER_IMAGE:-ubuntu:22.04}"

if [[ "$THREAD_MODE" == "single" ]]; then
    THREADS=1
    CHANNELS=1
else
    THREAD_MODE="multi"
    if [[ -z "$CHANNELS" ]]; then
        CHANNELS="$THREADS"
    fi
fi

if [[ -n "$RTT" && -n "$DELAY" ]]; then
    echo "Specify at most one of RTT or DELAY" >&2
    exit 1
fi

if [[ ! -x "$REPO_ROOT/bin/pqpsi_party_bench" ]]; then
    echo "Missing $REPO_ROOT/bin/pqpsi_party_bench. Run script/build-docker-pqpsi-bench.sh first." >&2
    exit 1
fi

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

parse_rate_bps() {
    python3 - "$1" <<'PY'
import re
import sys

value = sys.argv[1].strip().lower()
m = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?)(bit|kbit|mbit|gbit|bps)?", value)
if not m:
    raise SystemExit(f"unsupported rate format: {sys.argv[1]}")
x = float(m.group(1))
unit = m.group(2) or "bit"
scale = {
    "bit": 1.0,
    "bps": 1.0,
    "kbit": 1_000.0,
    "mbit": 1_000_000.0,
    "gbit": 1_000_000_000.0,
}[unit]
print(f"{x * scale:.3f}")
PY
}

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

rate_bps=""
if [[ -n "$RATE" ]]; then
    rate_bps="$(parse_rate_bps "$RATE")"
fi

tc_shape_cmd="true"
if [[ -n "$RATE" || -n "$applied_delay" ]]; then
    tc_shape_cmd="tc qdisc replace dev lo root netem"
    if [[ -n "$applied_delay" && "$applied_delay" != "0.000" ]]; then
        tc_shape_cmd="${tc_shape_cmd} delay ${applied_delay}ms"
    fi
    if [[ -n "$RATE" ]]; then
        tc_shape_cmd="${tc_shape_cmd} rate ${RATE}"
    fi
fi

RB_ARGS=()
if [[ -n "$RB_EPS" ]]; then
    RB_ARGS+=(--rb-eps "$RB_EPS")
fi
if [[ -n "$RB_W" ]]; then
    RB_ARGS+=(--rb-w "$RB_W")
fi
if [[ -n "$RB_COLS" ]]; then
    RB_ARGS+=(--rb-cols "$RB_COLS")
fi
RB_ARGS+=(--rb-lambda "$RB_LAMBDA")
RB_ARGS+=(--pi "$PI" --pi-lambda "$PI_LAMBDA" --pi-rounds "$PI_ROUNDS" --kem "$KEM")
if [[ -n "$HITS" ]]; then
    RB_ARGS+=(--hits "$HITS")
fi
if [[ "$BOB_PI" == "0" || "$BOB_PI" == "false" || "$BOB_PI" == "off" || "$BOB_PI" == "no" ]]; then
    RB_ARGS+=(--no-bob-pi)
fi
if [[ "$THREAD_MODE" == "single" ]]; then
    RB_ARGS+=(--single-thread)
else
    RB_ARGS+=(--multi-thread --threads "$THREADS" --channels "$CHANNELS")
fi
RB_ARGS_STR="${RB_ARGS[*]}"

platform_args=()
if [[ -n "$DOCKER_PLATFORM" ]]; then
    platform_args+=(--platform "$DOCKER_PLATFORM")
fi

extract_metric() {
    local key="$1"
    local file="$2"
    awk -v key="$key" '$1 == key { print $2 }' "$file" | tail -n 1
}

extract_section() {
    local begin="$1"
    local end="$2"
    local file="$3"
    awk -v begin="$begin" -v end="$end" '
        $0 == begin { flag = 1; next }
        $0 == end { flag = 0; next }
        flag { print }
    ' "$file"
}

mean_file() {
    awk 'NF { sum += $1; n += 1 } END { if (n) printf "%.2f", sum / n; else print "-" }' "$1"
}

trim_mean_file() {
    python3 - "$1" <<'PY'
import math
import pathlib
import sys

vals = []
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    line = line.strip()
    if line:
        vals.append(float(line))
if not vals:
    print("-")
    raise SystemExit
vals.sort()
drop = int(math.floor(len(vals) * 0.05))
if drop > 0 and len(vals) > 2 * drop:
    vals = vals[drop:-drop]
print(f"{sum(vals) / len(vals):.2f}")
PY
}

comm_kb_file() {
    awk 'NF { sum += $1; n += 1 } END { if (n) printf "%.2f", (sum / n) / 1024.0; else print "-" }' "$1"
}

sum_local_compute() {
    python3 - "$@" <<'PY'
import sys

vals = []
for x in sys.argv[1:]:
    vals.append(float(x) if x else 0.0)
print(f"{sum(vals):.3f}")
PY
}

sub_ms() {
    python3 - "$1" "$2" <<'PY'
import sys

a = float(sys.argv[1]) if sys.argv[1] else 0.0
b = float(sys.argv[2]) if sys.argv[2] else 0.0
print(f"{max(a - b, 0.0):.3f}")
PY
}

round_network_lower_bound_ms() {
    python3 - "$1" "$2" "$3" "$4" "${applied_delay:-0}" <<'PY'
import sys

comm_bytes = float(sys.argv[1]) if sys.argv[1] else 0.0
sender_sent = float(sys.argv[2]) if sys.argv[2] else 0.0
receiver_sent = float(sys.argv[3]) if sys.argv[3] else 0.0
rate_bps = float(sys.argv[4]) if sys.argv[4] else 0.0
one_way_ms = float(sys.argv[5]) if sys.argv[5] else 0.0
msgs = (1 if sender_sent > 0 else 0) + (1 if receiver_sent > 0 else 0)
wire_ms = (comm_bytes * 8.0 / rate_bps * 1000.0) if rate_bps > 0 else 0.0
print(f"{wire_ms + msgs * one_way_ms:.3f}")
PY
}

one_msg_network_ms() {
    python3 - "$1" "$2" "${applied_delay:-0}" <<'PY'
import sys

msg_bytes = float(sys.argv[1]) if sys.argv[1] else 0.0
rate_bps = float(sys.argv[2]) if sys.argv[2] else 0.0
one_way_ms = float(sys.argv[3]) if sys.argv[3] else 0.0
wire_ms = (msg_bytes * 8.0 / rate_bps * 1000.0) if rate_bps > 0 else 0.0
delay_ms = one_way_ms if msg_bytes > 0 else 0.0
print(f"{wire_ms + delay_ms:.3f}")
PY
}

phase_model_ms() {
    python3 - "$@" <<'PY'
import sys

vals = [float(x) if x else 0.0 for x in sys.argv[1:]]
print(f"{sum(vals):.3f}")
PY
}

if (( TERM_ONLY == 0 )); then
    mkdir -p "$(dirname "$OUTPUT_FILE")"
fi
cleanup_samples=1
if [[ -n "$SAMPLE_DIR" ]]; then
    tmp_root="$SAMPLE_DIR"
    mkdir -p "$tmp_root"
    cleanup_samples=0
else
    tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/pqpsi-loopback.XXXXXX")"
fi
trap 'if (( cleanup_samples == 1 )); then rm -rf "$tmp_root"; fi; if [[ "${TERM_ONLY:-0}" == "1" ]]; then rm -f "$OUTPUT_FILE"; fi' EXIT

line_count_file() {
    if [[ -f "$1" ]]; then
        awk 'END { print NR + 0 }' "$1"
    else
        printf '0\n'
    fi
}

{
    printf "# PQ-PSI Loopback Benchmark\n\n"
    printf "This report uses the MiniPSI/VOLE-PSI loopback setting: one container per round, two party processes inside the container, and tc netem on \`lo\`.\n\n"
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
    printf "| tc_command | \`%s\` |\n" "$tc_shape_cmd"
    printf "| thread_mode | \`%s\` |\n" "$THREAD_MODE"
    printf "| threads_per_party | \`%s\` |\n" "$THREADS"
    printf "| net_channels | \`%s\` |\n" "$CHANNELS"
    printf "| cpu_pinning | \`%s\` |\n" "$PIN_CPUS"
    if [[ "$PIN_CPUS" == "1" || "$PIN_CPUS" == "true" || "$PIN_CPUS" == "on" || "$PIN_CPUS" == "yes" ]]; then
        printf "| receiver_cpus | \`%s\` |\n" "$RECV_CPUS"
        printf "| sender_cpus | \`%s\` |\n" "$SEND_CPUS"
    fi
    printf "| kem | \`%s\` |\n" "$KEM"
    printf "| pi | \`%s\` |\n" "$PI"
    printf "| hits | \`%s\` |\n" "${HITS:-setSize-1}"
    printf "| bob_pi | \`%s\` |\n" "$BOB_PI"
    printf "| rb_lambda | \`%s\` |\n" "$RB_LAMBDA"
    printf "| rb_args | \`%s\` |\n\n" "$RB_ARGS_STR"
    printf "The main time is wall-clock protocol time: \`max(receiver protocol, sender protocol)\`.\n\n"
    printf "| Size | Wall Mean (ms) | Comm (KB) | Measured rounds |\n"
    printf "| --- | ---: | ---: | ---: |\n"
} > "$OUTPUT_FILE"

for size in $SIZES; do
    times_file="$tmp_root/time-${size}.txt"
    comm_file="$tmp_root/comm-${size}.txt"
    sender_protocol_file="$tmp_root/sender-protocol-${size}.txt"
    receiver_protocol_file="$tmp_root/receiver-protocol-${size}.txt"
    sender_compute_file="$tmp_root/sender-compute-${size}.txt"
    receiver_compute_file="$tmp_root/receiver-compute-${size}.txt"
    sender_nonlocal_file="$tmp_root/sender-nonlocal-${size}.txt"
    receiver_nonlocal_file="$tmp_root/receiver-nonlocal-${size}.txt"
    network_lower_file="$tmp_root/network-lower-${size}.txt"
    raw_log="$tmp_root/raw-${size}.md"
    stage_names=(
        prep keygen mask permute perm_inv kem okvs_encode okvs_decode
        network_send network_recv protocol local_compute nonlocal
    )
    warmup_done_file="$tmp_root/warmup-done-${size}.flag"
    if [[ "$APPEND_ROUNDS" == "1" || "$APPEND_ROUNDS" == "true" || "$APPEND_ROUNDS" == "on" || "$APPEND_ROUNDS" == "yes" ]]; then
        touch "$times_file" "$comm_file" "$sender_protocol_file" "$receiver_protocol_file" \
            "$sender_compute_file" "$receiver_compute_file" "$sender_nonlocal_file" \
            "$receiver_nonlocal_file" "$network_lower_file" "$raw_log"
        for role in sender receiver; do
            for stage in "${stage_names[@]}"; do
                touch "$tmp_root/${role}-${stage}-${size}.txt"
            done
        done
    else
        : > "$times_file"
        : > "$comm_file"
        : > "$sender_protocol_file"
        : > "$receiver_protocol_file"
        : > "$sender_compute_file"
        : > "$receiver_compute_file"
        : > "$sender_nonlocal_file"
        : > "$receiver_nonlocal_file"
        : > "$network_lower_file"
        : > "$raw_log"
        rm -f "$warmup_done_file"
        for role in sender receiver; do
            for stage in "${stage_names[@]}"; do
                : > "$tmp_root/${role}-${stage}-${size}.txt"
            done
        done
    fi

    existing_measured_rounds="$(line_count_file "$times_file")"
    warmups_remaining=0
    if [[ ! -f "$warmup_done_file" ]]; then
        warmups_remaining="$WARMUPS"
    fi
    additional_measured_rounds=$(( ROUNDS - existing_measured_rounds ))
    if (( additional_measured_rounds < 0 )); then
        additional_measured_rounds=0
    fi
    total_trials_this_call=$(( warmups_remaining + additional_measured_rounds ))
    measured_so_far="$existing_measured_rounds"
    serial_base=$(( existing_measured_rounds + WARMUPS ))

    for run_idx in $(seq 1 "$total_trials_this_call"); do
        phase="measure"
        if (( run_idx <= warmups_remaining )); then
            phase="warmup"
        fi

        serial_idx=$(( serial_base + run_idx ))
        if [[ "$phase" == "measure" ]]; then
            measured_idx=$(( measured_so_far + run_idx - warmups_remaining ))
            serial_idx=$(( WARMUPS + measured_idx ))
        fi
        port=$(( PORT_BASE_START + size + serial_idx ))
        tag="pqpsi_${size}_${serial_idx}_${port}"
        sender_time_ms=""
        receiver_time_ms=""
        sender_bytes_sent=""
        receiver_bytes_sent=""
        sender_status=""
        receiver_status=""
        run_status=1
        attempt_combined_log=""

        for attempt_idx in $(seq 1 "$MAX_ATTEMPTS"); do
            combined_log="$(mktemp "$tmp_root/combined.XXXXXX")"
            sender_log="$(mktemp "$tmp_root/sender.XXXXXX")"
            recv_log="$(mktemp "$tmp_root/recv.XXXXXX")"

            container_script="$(cat <<EOF
set -euo pipefail
sender_log=\$(mktemp)
recv_log=\$(mktemp)
cleanup() {
    rm -f "\$sender_log" "\$recv_log"
}
trap cleanup EXIT
if ! command -v tc >/dev/null 2>&1 || ! command -v taskset >/dev/null 2>&1; then
    apt-get update >/dev/null
    apt-get install -y --no-install-recommends \
        iproute2 util-linux \
        libboost-system1.74.0 libboost-thread1.74.0 \
        libgmp10 libsodium23 >/dev/null
fi
${tc_shape_cmd}
export PQPSI_DISABLE_APP_NET_SIM=1
export PQPSI_NET_EMU=tc
export PQPSI_SIM_NET_DELAY_MS=0
export PQPSI_SIM_NET_BW_MBPS=0
recv_cmd=(./bin/pqpsi_party_bench 0 ${size} ${port} --tag ${tag} ${RB_ARGS_STR})
send_cmd=(./bin/pqpsi_party_bench 1 ${size} ${port} --tag ${tag} ${RB_ARGS_STR})
if [[ "${PIN_CPUS}" == "1" || "${PIN_CPUS}" == "true" || "${PIN_CPUS}" == "on" || "${PIN_CPUS}" == "yes" ]]; then
    recv_cmd=(taskset -c "${RECV_CPUS}" "\${recv_cmd[@]}")
    send_cmd=(taskset -c "${SEND_CPUS}" "\${send_cmd[@]}")
fi
recv_rc=0
timeout ${RUN_TIMEOUT} stdbuf -oL -eL "\${recv_cmd[@]}" >"\$recv_log" 2>&1 &
recv_pid=\$!
sleep 1
sender_rc=0
timeout ${RUN_TIMEOUT} stdbuf -oL -eL "\${send_cmd[@]}" >"\$sender_log" 2>&1 || sender_rc=\$?
if [[ "\$sender_rc" != "0" ]]; then
    kill "\$recv_pid" >/dev/null 2>&1 || true
fi
wait "\$recv_pid" || recv_rc=\$?
printf 'sender_status %s\n' "\$sender_rc"
printf 'receiver_status %s\n' "\$recv_rc"
printf '__SENDER_LOG_BEGIN__\n'
cat "\$sender_log"
printf '__SENDER_LOG_END__\n'
printf '__RECV_LOG_BEGIN__\n'
cat "\$recv_log"
printf '__RECV_LOG_END__\n'
if [[ "\$sender_rc" != "0" || "\$recv_rc" != "0" ]]; then
    exit 1
fi
EOF
)"

            set +e
            docker run --rm --cap-add NET_ADMIN "${platform_args[@]}" \
                -v "$REPO_ROOT:/work" \
                -w /work \
                "$DOCKER_IMAGE" \
                bash -lc "$container_script" > "$combined_log" 2>&1
            run_status=$?
            set -e

            extract_section "__SENDER_LOG_BEGIN__" "__SENDER_LOG_END__" "$combined_log" > "$sender_log"
            extract_section "__RECV_LOG_BEGIN__" "__RECV_LOG_END__" "$combined_log" > "$recv_log"
            sender_status="$(extract_metric sender_status "$combined_log")"
            receiver_status="$(extract_metric receiver_status "$combined_log")"
            sender_time_ms="$(extract_metric result_time_ms "$sender_log")"
            receiver_time_ms="$(extract_metric result_time_ms "$recv_log")"
            sender_bytes_sent="$(extract_metric result_bytes_sent "$sender_log")"
            receiver_bytes_sent="$(extract_metric result_bytes_sent "$recv_log")"

            if [[ "$run_status" == "0" && "${sender_status:-1}" == "0" && "${receiver_status:-1}" == "0" \
                && -n "$sender_time_ms" && -n "$receiver_time_ms" && -n "$sender_bytes_sent" && -n "$receiver_bytes_sent" ]]; then
                attempt_combined_log="$combined_log"
                break
            fi

            attempt_combined_log="$combined_log"
        done

        if [[ "$run_status" != "0" || "${sender_status:-1}" != "0" || "${receiver_status:-1}" != "0" \
            || -z "$sender_time_ms" || -z "$receiver_time_ms" || -z "$sender_bytes_sent" || -z "$receiver_bytes_sent" ]]; then
            {
                printf "### %s run %s failed\n\n" "$size" "$run_idx"
                printf "| field | value |\n"
                printf "| --- | --- |\n"
                printf "| phase | \`%s\` |\n" "$phase"
                printf "| docker_status | \`%s\` |\n" "$run_status"
                printf "| sender_status | \`%s\` |\n" "${sender_status:-unknown}"
                printf "| receiver_status | \`%s\` |\n\n" "${receiver_status:-unknown}"
                printf '```text\n%s\n```\n\n' "$(cat "$attempt_combined_log")"
            } >> "$raw_log"
            echo "PQ-PSI loopback benchmark failed for size $size run $run_idx" >&2
            cat "$raw_log" >&2
            exit 1
        fi

        round_time_ms="$(python3 -c 'import sys; print(f"{max(float(sys.argv[1]), float(sys.argv[2])):.3f}")' "$sender_time_ms" "$receiver_time_ms")"
        round_comm_bytes="$(python3 -c 'import sys; print(int(sys.argv[1]) + int(sys.argv[2]))' "$sender_bytes_sent" "$receiver_bytes_sent")"

        sender_compute_ms="$(sum_local_compute \
            "$(extract_metric result_prep_ms "$sender_log")" \
            "$(extract_metric result_keygen_ms "$sender_log")" \
            "$(extract_metric result_mask_ms "$sender_log")" \
            "$(extract_metric result_permute_ms "$sender_log")" \
            "$(extract_metric result_perm_inv_ms "$sender_log")" \
            "$(extract_metric result_kem_ms "$sender_log")" \
            "$(extract_metric result_okvs_encode_ms "$sender_log")" \
            "$(extract_metric result_okvs_decode_ms "$sender_log")")"
        receiver_compute_ms="$(sum_local_compute \
            "$(extract_metric result_prep_ms "$recv_log")" \
            "$(extract_metric result_keygen_ms "$recv_log")" \
            "$(extract_metric result_mask_ms "$recv_log")" \
            "$(extract_metric result_permute_ms "$recv_log")" \
            "$(extract_metric result_perm_inv_ms "$recv_log")" \
            "$(extract_metric result_kem_ms "$recv_log")" \
            "$(extract_metric result_okvs_encode_ms "$recv_log")" \
            "$(extract_metric result_okvs_decode_ms "$recv_log")")"
        sender_nonlocal_ms="$(sub_ms "$sender_time_ms" "$sender_compute_ms")"
        receiver_nonlocal_ms="$(sub_ms "$receiver_time_ms" "$receiver_compute_ms")"
        network_lower_ms="$(round_network_lower_bound_ms "$round_comm_bytes" "$sender_bytes_sent" "$receiver_bytes_sent" "$rate_bps")"
        if [[ "$phase" == "measure" ]]; then
            printf '%s\n' "$round_time_ms" >> "$times_file"
            printf '%s\n' "$round_comm_bytes" >> "$comm_file"
            printf '%s\n' "$sender_time_ms" >> "$sender_protocol_file"
            printf '%s\n' "$receiver_time_ms" >> "$receiver_protocol_file"
            printf '%s\n' "$sender_compute_ms" >> "$sender_compute_file"
            printf '%s\n' "$receiver_compute_ms" >> "$receiver_compute_file"
            printf '%s\n' "$sender_nonlocal_ms" >> "$sender_nonlocal_file"
            printf '%s\n' "$receiver_nonlocal_ms" >> "$receiver_nonlocal_file"
            printf '%s\n' "$network_lower_ms" >> "$network_lower_file"
            printf '%s\n' "$sender_compute_ms" >> "$tmp_root/sender-local_compute-${size}.txt"
            printf '%s\n' "$receiver_compute_ms" >> "$tmp_root/receiver-local_compute-${size}.txt"

            printf '%s\n' "$(extract_metric result_prep_ms "$sender_log")" >> "$tmp_root/sender-prep-${size}.txt"
            printf '%s\n' "$(extract_metric result_keygen_ms "$sender_log")" >> "$tmp_root/sender-keygen-${size}.txt"
            printf '%s\n' "$(extract_metric result_mask_ms "$sender_log")" >> "$tmp_root/sender-mask-${size}.txt"
            printf '%s\n' "$(extract_metric result_permute_ms "$sender_log")" >> "$tmp_root/sender-permute-${size}.txt"
            printf '%s\n' "$(extract_metric result_perm_inv_ms "$sender_log")" >> "$tmp_root/sender-perm_inv-${size}.txt"
            printf '%s\n' "$(extract_metric result_kem_ms "$sender_log")" >> "$tmp_root/sender-kem-${size}.txt"
            printf '%s\n' "$(extract_metric result_okvs_encode_ms "$sender_log")" >> "$tmp_root/sender-okvs_encode-${size}.txt"
            printf '%s\n' "$(extract_metric result_okvs_decode_ms "$sender_log")" >> "$tmp_root/sender-okvs_decode-${size}.txt"
            printf '%s\n' "$(extract_metric result_network_send_ms "$sender_log")" >> "$tmp_root/sender-network_send-${size}.txt"
            printf '%s\n' "$(extract_metric result_network_recv_ms "$sender_log")" >> "$tmp_root/sender-network_recv-${size}.txt"

            printf '%s\n' "$(extract_metric result_prep_ms "$recv_log")" >> "$tmp_root/receiver-prep-${size}.txt"
            printf '%s\n' "$(extract_metric result_keygen_ms "$recv_log")" >> "$tmp_root/receiver-keygen-${size}.txt"
            printf '%s\n' "$(extract_metric result_mask_ms "$recv_log")" >> "$tmp_root/receiver-mask-${size}.txt"
            printf '%s\n' "$(extract_metric result_permute_ms "$recv_log")" >> "$tmp_root/receiver-permute-${size}.txt"
            printf '%s\n' "$(extract_metric result_perm_inv_ms "$recv_log")" >> "$tmp_root/receiver-perm_inv-${size}.txt"
            printf '%s\n' "$(extract_metric result_kem_ms "$recv_log")" >> "$tmp_root/receiver-kem-${size}.txt"
            printf '%s\n' "$(extract_metric result_okvs_encode_ms "$recv_log")" >> "$tmp_root/receiver-okvs_encode-${size}.txt"
            printf '%s\n' "$(extract_metric result_okvs_decode_ms "$recv_log")" >> "$tmp_root/receiver-okvs_decode-${size}.txt"
            printf '%s\n' "$(extract_metric result_network_send_ms "$recv_log")" >> "$tmp_root/receiver-network_send-${size}.txt"
            printf '%s\n' "$(extract_metric result_network_recv_ms "$recv_log")" >> "$tmp_root/receiver-network_recv-${size}.txt"
        fi
    done
    if (( warmups_remaining > 0 )); then
        : > "$warmup_done_file"
    fi

    actual_measured_rounds="$(line_count_file "$times_file")"
    mean_ms="$(trim_mean_file "$times_file")"
    comm_kb="$(comm_kb_file "$comm_file")"
    printf "| \`%s\` | \`%s\` | \`%s\` | \`%s\` |\n" "$size" "$mean_ms" "$comm_kb" "$actual_measured_rounds" >> "$OUTPUT_FILE"

    if (( TERM_ONLY == 1 )); then
        printf "\nTwo-process PQ-PSI loopback, size=%s\n" "$size"
        printf "setting: kem=%s pi=%s bob_pi=%s threads=%s channels=%s rate=%s rtt=%s delay=%s rounds=%s warmups=%s\n" \
            "$KEM" "$PI" "$BOB_PI" "$THREADS" "$CHANNELS" "${RATE:-none}" "${RTT:-none}" "${DELAY:-none}" "$actual_measured_rounds" "$WARMUPS"
        printf "summary: wall=%sms comm=%sKB\n" "$mean_ms" "$comm_kb"
        printf "Receiver / party0                      Sender / party1\n"
        printf '%s\n' "---------------------------------------------------------------"
        printf "%-26s %9s    %-24s %9s\n" "receiver_protocol" "$(trim_mean_file "$receiver_protocol_file")" "sender_protocol" "$(trim_mean_file "$sender_protocol_file")"
        printf "%-26s %9s    %-24s %9s\n" "receiver_local_compute" "$(trim_mean_file "$receiver_compute_file")" "sender_local_compute" "$(trim_mean_file "$sender_compute_file")"
        printf "%-26s %9s    %-24s %9s\n" "receiver_nonlocal" "$(trim_mean_file "$receiver_nonlocal_file")" "sender_nonlocal" "$(trim_mean_file "$sender_nonlocal_file")"
        printf "%-26s %9s\n\n" "network_lower_bound" "$(trim_mean_file "$network_lower_file")"
    fi

    {
        printf "\n## Cost Breakdown For Size \`%s\`\n\n" "$size"
        printf "\`local_compute_ms\` excludes blocking network calls. \`nonlocal_ms\` is \`protocol_ms - local_compute_ms\`, so it contains real network latency/bandwidth, TCP/netem overhead, and waiting for the peer to finish computing before it sends.\n\n"
        printf "| item | mean_ms |\n"
        printf "| --- | ---: |\n"
        printf "| receiver_protocol | \`%s\` |\n" "$(mean_file "$receiver_protocol_file")"
        printf "| sender_protocol | \`%s\` |\n" "$(mean_file "$sender_protocol_file")"
        printf "| wall_protocol | \`%s\` |\n" "$(mean_file "$times_file")"
        printf "| receiver_local_compute | \`%s\` |\n" "$(mean_file "$receiver_compute_file")"
        printf "| sender_local_compute | \`%s\` |\n" "$(mean_file "$sender_compute_file")"
        printf "| network_lower_bound | \`%s\` |\n" "$(mean_file "$network_lower_file")"
        printf "| receiver_nonlocal | \`%s\` |\n" "$(mean_file "$receiver_nonlocal_file")"
        printf "| sender_nonlocal | \`%s\` |\n" "$(mean_file "$sender_nonlocal_file")"
        printf "\n### Stage Breakdown For Size \`%s\`\n\n" "$size"
        printf "Trimmed mean drops the lowest and highest 5%% when enough rounds are present; for small smoke runs it equals the raw mean.\n\n"
        printf "| role | stage | mean_ms | trimmed_mean_ms |\n"
        printf "| --- | --- | ---: | ---: |\n"
        for role in receiver sender; do
            for stage in "${stage_names[@]}"; do
                f="$tmp_root/${role}-${stage}-${size}.txt"
                printf "| %s | %s | \`%s\` | \`%s\` |\n" "$role" "$stage" "$(mean_file "$f")" "$(trim_mean_file "$f")"
            done
        done
    } >> "$OUTPUT_FILE"
done

if (( TERM_ONLY == 0 )); then
    printf "\nWrote %s\n" "$OUTPUT_FILE"
fi
