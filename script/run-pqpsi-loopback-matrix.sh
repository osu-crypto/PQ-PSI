#!/usr/bin/env bash

set -euo pipefail

export LC_ALL=C
export LANG=C
export PATH="/opt/homebrew/bin:/usr/local/bin:/Applications/Docker.app/Contents/Resources/bin:$PATH"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_SCRIPT="$REPO_ROOT/script/benchmark-docker-pqpsi-loopback.sh"
RUNNER_DOCKERFILE="$REPO_ROOT/script/pqpsi-loopback-runner.Dockerfile"
BENCH_DIR="${BENCH_DIR:-$REPO_ROOT/build-docker/benchmarks/rbokvs-pqpsi}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_FILE="${1:-$BENCH_DIR/pqpsi-loopback-matrix-${STAMP}.md}"
LOG_FILE="${LOG_FILE:-$BENCH_DIR/pqpsi-loopback-matrix-${STAMP}.log}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/pqpsi-loopback-matrix.XXXXXX")"
CASE_DIR="${CASE_DIR:-$BENCH_DIR/.matrix-work-${STAMP}}"
RUNNER_IMAGE="${RUNNER_IMAGE:-pqpsi-loopback-runner:local}"
EXISTING_DIR="${EXISTING_DIR:-}"
EXISTING_RESULTS_TSV="${EXISTING_RESULTS_TSV:-}"
ROUND_STEPS="${ROUND_STEPS:-40 60 80 100}"
TRIM_GAP_PCT="${TRIM_GAP_PCT:-2.0}"
BOB_PI_ORDER="${BOB_PI_ORDER:-0}"

SIZES="${SIZES:-128 256 512 1024}"
WARMUPS="${WARMUPS:-3}"
PI_LAMBDA="${PI_LAMBDA:-128}"
RB_LAMBDA="${RB_LAMBDA:-40}"
DOCKER_PLATFORM="${DOCKER_PLATFORM:-linux/amd64}"

mkdir -p "$BENCH_DIR"
mkdir -p "$CASE_DIR"
cleanup_case_dir=1
if [[ -n "${CASE_DIR:-}" && "$CASE_DIR" != "$BENCH_DIR/.matrix-work-${STAMP}" ]]; then
    cleanup_case_dir=0
fi
trap 'rm -rf "$TMP_ROOT"; if (( cleanup_case_dir == 1 )); then rm -rf "$CASE_DIR"; fi' EXIT

log() {
    printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$LOG_FILE"
}

cpu_count() {
    python3 - <<'PY'
import os
print(os.cpu_count() or 8)
PY
}

cpu_range() {
    python3 - "$1" "$2" <<'PY'
import sys
start = int(sys.argv[1])
count = int(sys.argv[2])
if count <= 1:
    print(str(start))
else:
    print(f"{start}-{start + count - 1}")
PY
}

prepare_cpu_sets() {
    local threads="$1"
    local total
    total="$(cpu_count)"
    if (( total >= threads * 2 )); then
        RECV_CPUS="$(cpu_range 0 "$threads")"
        SEND_CPUS="$(cpu_range "$threads" "$threads")"
    else
        local half=$(( total / 2 ))
        if (( half < 1 )); then
            half=1
        fi
        local recv_count="$half"
        local send_count=$(( total - half ))
        if (( send_count < 1 )); then
            send_count=1
        fi
        RECV_CPUS="$(cpu_range 0 "$recv_count")"
        SEND_CPUS="$(cpu_range "$half" "$send_count")"
    fi
    export RECV_CPUS SEND_CPUS
}

ensure_idle() {
    local ids
    ids="$(docker ps -q)"
    if [[ -n "$ids" ]]; then
        log "killing stray containers: $ids"
        docker kill $ids >/dev/null || true
    fi
    for _ in $(seq 1 30); do
        if [[ -z "$(docker ps -q)" ]]; then
            return
        fi
        sleep 1
    done
    log "warning: docker still shows running containers before next case"
}

build_runner_image() {
    log "building runner image $RUNNER_IMAGE"
    docker build --platform "$DOCKER_PLATFORM" -t "$RUNNER_IMAGE" -f "$RUNNER_DOCKERFILE" "$REPO_ROOT" >>"$LOG_FILE" 2>&1
}

parse_case() {
    python3 - "$1" <<'PY'
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
rows = []
for line in text.splitlines():
    m = re.match(r"\|\s*`?(\d+)`?\s*\|\s*`?([0-9.\-]+)`?\s*\|\s*`?([0-9.\-]+)`?\s*\|\s*`?(\d+)`?\s*\|", line)
    if m:
        rows.append((m.group(1), m.group(2), m.group(3)))
for size, wall, comm in rows:
    print(f"{size}\t{wall}\t{comm}")
PY
}

parse_case_stats() {
    python3 - "$1" <<'PY'
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
size = wall = comm = raw_wall = None
for line in text.splitlines():
    m = re.match(r"\|\s*`?(\d+)`?\s*\|\s*`?([0-9.\-]+)`?\s*\|\s*`?([0-9.\-]+)`?\s*\|\s*`?(\d+)`?\s*\|", line)
    if m:
        size, wall, comm = m.group(1), m.group(2), m.group(3)
        break
for line in text.splitlines():
    m = re.match(r"\|\s*wall_protocol\s*\|\s*`?([0-9.\-]+)`?\s*\|", line)
    if m:
        raw_wall = m.group(1)
        break
if size and wall and comm:
    print(f"{size}\t{wall}\t{comm}\t{raw_wall or wall}")
PY
}

case_done_file() {
    local bob="$1"
    local proto="$2"
    local setting="$3"
    local size="$4"
    local name="${bob}-${proto}-${setting}-${size}.md"
    if [[ -n "$EXISTING_DIR" && -f "$EXISTING_DIR/$name" ]]; then
        printf '%s\n' "$EXISTING_DIR/$name"
        return 0
    fi
    return 1
}

existing_result_line() {
    local bob="$1"
    local proto="$2"
    local setting="$3"
    local size="$4"
    awk -F '\t' -v bob="$bob" -v proto="$proto" -v size="$size" -v setting="$setting" '
        $1 == bob && $2 == proto && $3 == size && $4 == setting { print; found = 1; exit }
        END { if (!found) exit 1 }
    ' "$RESULTS_TSV"
}

gap_percent() {
    python3 - "$1" "$2" <<'PY'
import sys
trimmed = float(sys.argv[1])
raw = float(sys.argv[2])
if trimmed == 0:
    print("0")
else:
    print(abs(raw - trimmed) * 100.0 / trimmed)
PY
}

table_label() {
    case "$1" in
        keccak1600) echo "Ours [keccak1600-24]" ;;
        keccak1600-12) echo "Ours [keccak1600-12]" ;;
        sneik-f512) echo "Ours [SNEIK]" ;;
        hctr) echo "Ours [HCTR2]" ;;
        nonpq) echo "Ours [Non-PQ]" ;;
        *) echo "$1" ;;
    esac
}

case_key() {
    local bob="$1"
    local proto="$2"
    local size="$3"
    local setting="$4"
    printf '%s|%s|%s|%s\n' "$bob" "$proto" "$size" "$setting"
}

RESULTS_TSV="$TMP_ROOT/results.tsv"
: > "$RESULTS_TSV"
: > "$LOG_FILE"

log "starting full PQPSI loopback matrix"
log "output file: $OUT_FILE"
log "log file: $LOG_FILE"
log "sizes=$SIZES warmups=$WARMUPS round_steps=$ROUND_STEPS"
if [[ -n "$EXISTING_DIR" ]]; then
    log "reusing existing case files from: $EXISTING_DIR"
fi
if [[ -n "$EXISTING_RESULTS_TSV" ]]; then
    log "reusing existing result rows from: $EXISTING_RESULTS_TSV"
    awk 'NF { print }' "$EXISTING_RESULTS_TSV" >> "$RESULTS_TSV"
fi

build_runner_image
ensure_idle

protocols=(
    "hctr obf-mlkem hctr"
    "feistel obf-mlkem feistel"
    "nonpq eckem xoodoo"
    "sneik-f512 obf-mlkem sneik-f512"
    "keccak1600-12 obf-mlkem keccak1600-12"
    "keccak1600 obf-mlkem keccak1600"
)

settings=(
    "lan_single 10gbit none single 1 1"
    "lan_multi 10gbit none multi 4 4"
    "wan_single 200mbit 80ms single 1 1"
    "wan_multi 200mbit 80ms multi 4 4"
)

for bob_pi in $BOB_PI_ORDER; do
    for proto_spec in "${protocols[@]}"; do
        read -r proto_name kem pi <<<"$proto_spec"
        for setting_spec in "${settings[@]}"; do
            read -r setting_name rate rtt thread_mode threads channels <<<"$setting_spec"
            if [[ "$rtt" == "none" ]]; then
                rtt=""
            fi
            for size in $SIZES; do
                if existing_row="$(existing_result_line "$bob_pi" "$proto_name" "$setting_name" "$size")"; then
                    IFS=$'\t' read -r _ _ parsed_size _ wall comm rounds_used <<<"$existing_row"
                    log "skip-existing-result bob_pi=$bob_pi proto=$proto_name setting=$setting_name size=$parsed_size wall_ms=$wall comm_kib=$comm rounds=$rounds_used"
                    continue
                fi
                if existing_file="$(case_done_file "$bob_pi" "$proto_name" "$setting_name" "$size")"; then
                    while IFS=$'\t' read -r parsed_size wall comm raw_wall; do
                        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$bob_pi" "$proto_name" "$parsed_size" "$setting_name" "$wall" "$comm" "existing" >> "$RESULTS_TSV"
                        log "skip-existing bob_pi=$bob_pi proto=$proto_name setting=$setting_name size=$parsed_size wall_ms=$wall comm_kib=$comm source=$(basename "$existing_file")"
                    done < <(parse_case_stats "$existing_file")
                    continue
                fi
                prepare_cpu_sets "$threads"
                ensure_idle
                case_file="$CASE_DIR/${bob_pi}-${proto_name}-${setting_name}-${size}.md"
                sample_dir="$CASE_DIR/${bob_pi}-${proto_name}-${setting_name}-${size}.samples"
                accepted_rounds=""
                accepted_wall=""
                accepted_comm=""
                for rounds in $ROUND_STEPS; do
                    log "running bob_pi=$bob_pi proto=$proto_name kem=$kem pi=$pi setting=$setting_name size=$size rounds=$rounds threads=$threads channels=$channels recv_cpus=$RECV_CPUS send_cpus=$SEND_CPUS"

                    env \
                        DOCKER_IMAGE="$RUNNER_IMAGE" \
                        DOCKER_PLATFORM="$DOCKER_PLATFORM" \
                        SIZES="$size" \
                        WARMUPS="$WARMUPS" \
                        ROUNDS="$rounds" \
                        RATE="$rate" \
                        RTT="$rtt" \
                        THREAD_MODE="$thread_mode" \
                        THREADS="$threads" \
                        CHANNELS="$channels" \
                        PIN_CPUS=1 \
                        RECV_CPUS="$RECV_CPUS" \
                        SEND_CPUS="$SEND_CPUS" \
                        APPEND_ROUNDS=1 \
                        SAMPLE_DIR="$sample_dir" \
                        KEM="$kem" \
                        PI="$pi" \
                        PI_LAMBDA="$PI_LAMBDA" \
                        BOB_PI="$bob_pi" \
                        RB_LAMBDA="$RB_LAMBDA" \
                        bash "$BENCH_SCRIPT" "$case_file" >>"$LOG_FILE" 2>&1

                    while IFS=$'\t' read -r parsed_size wall comm raw_wall; do
                        gap="$(gap_percent "$wall" "$raw_wall")"
                        log "check bob_pi=$bob_pi proto=$proto_name setting=$setting_name size=$parsed_size rounds=$rounds trimmed_ms=$wall raw_ms=$raw_wall gap_pct=$gap"
                        accepted_rounds="$rounds"
                        accepted_wall="$wall"
                        accepted_comm="$comm"
                        if python3 - "$gap" "$TRIM_GAP_PCT" <<'PY'
import sys
gap = float(sys.argv[1])
thr = float(sys.argv[2])
sys.exit(0 if gap <= thr else 1)
PY
                        then
                            break 2
                        fi
                    done < <(parse_case_stats "$case_file")
                done

                printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$bob_pi" "$proto_name" "$size" "$setting_name" "$accepted_wall" "$accepted_comm" "$accepted_rounds" >> "$RESULTS_TSV"
                log "done bob_pi=$bob_pi proto=$proto_name setting=$setting_name size=$size wall_ms=$accepted_wall comm_kib=$accepted_comm rounds_used=$accepted_rounds"
            done
        done
    done
done

python3 - "$RESULTS_TSV" "$OUT_FILE" "$WARMUPS" "$ROUND_STEPS" "$RUNNER_IMAGE" <<'PY'
import sys
from pathlib import Path

rows = [line.rstrip("\n").split("\t") for line in Path(sys.argv[1]).read_text().splitlines() if line.strip()]
out = Path(sys.argv[2])
warmups = sys.argv[3]
round_steps = sys.argv[4]
runner = sys.argv[5]

sizes = ["128", "256", "512", "1024"]
size_labels = {
    "128": "`2^7 = 128`",
    "256": "`2^8 = 256`",
    "512": "`2^9 = 512`",
    "1024": "`2^10 = 1024`",
}
protocol_order = [
    ("keccak1600", "Ours [keccak1600-24]"),
    ("keccak1600-12", "Ours [keccak1600-12]"),
    ("sneik-f512", "Ours [SNEIK]"),
    ("hctr", "Ours [HCTR2]"),
    ("nonpq", "Ours [Non-PQ]"),
]
setting_order = [
    ("lan_single", "LAN Single Mean (ms)"),
    ("lan_multi", "LAN 4-thread Mean (ms)"),
    ("wan_single", "200 Mbps Single Mean (ms)"),
    ("wan_multi", "200 Mbps 4-thread Mean (ms)"),
]

data = {}
round_map = {}
for bob, proto, size, setting, wall, comm, rounds_used in rows:
    data[(bob, proto, size, setting)] = (wall, comm)
    round_map[(bob, proto, size, setting)] = rounds_used

lines = []
lines.append("# PQ-PSI Loopback Matrix Summary")
lines.append("")
lines.append(f"- Adaptive round steps: `{round_steps}`")
lines.append(f"- Warmups per case: `{warmups}`")
lines.append("- Main wall-clock values use the benchmark script's trimmed mean, which drops the lowest and highest 5% of measured rounds.")
lines.append("- All runs are two-process loopback runs inside one Docker container with `tc netem` on `lo`.")
lines.append("- CPU pinning is enabled so receiver and sender use disjoint core sets when enough CPUs are available.")
lines.append(f"- Docker runner image: `{runner}`")
lines.append("")

for bob in ("1", "0"):
    bob_label = "Bob PI On" if bob == "1" else "Bob PI Off"
    lines.append(f"## {bob_label}")
    lines.append("")
    lines.append("| n | Protocol | LAN Single Mean (ms) | LAN 4-thread Mean (ms) | 200 Mbps Single Mean (ms) | 200 Mbps 4-thread Mean (ms) | Communication (KiB) |")
    lines.append("| --- | --- | ---: | ---: | ---: | ---: | ---: |")
    for size in sizes:
        first = True
        for proto, label in protocol_order:
            vals = []
            comm = "-"
            for setting, _ in setting_order:
                wall, c = data.get((bob, proto, size, setting), ("-", "-"))
                vals.append(wall)
                if comm == "-" and c != "-":
                    comm = c
            n_cell = size_labels[size] if first else ""
            first = False
            lines.append(f"| {n_cell} | {label} | `{vals[0]}` | `{vals[1]}` | `{vals[2]}` | `{vals[3]}` | `{comm}` |")
        lines.append("")

    lines.append("### Rounds Used")
    lines.append("")
    lines.append("| Size | Protocol | Setting | Rounds |")
    lines.append("| --- | --- | --- | ---: |")
    for size in sizes:
        for proto, label in protocol_order:
            for setting, _ in setting_order:
                rounds_used = round_map.get((bob, proto, size, setting))
                if rounds_used is None:
                    continue
                lines.append(f"| {size_labels[size]} | {label} | `{setting}` | `{rounds_used}` |")
    lines.append("")

out.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY

log "done: $OUT_FILE"
printf '%s\n' "$OUT_FILE"
