#!/usr/bin/env bash

set -euo pipefail

export PATH="/opt/homebrew/bin:/usr/local/bin:/Applications/Docker.app/Contents/Resources/bin:$PATH"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

usage() {
    cat <<'EOF'
Usage
  bash script/pqpsi.sh build
  bash script/pqpsi.sh test thread [setSize] [hits] [rounds] [-- extra flags]
  bash script/pqpsi.sh test process [setSize] [rounds] [-- extra flags]
  bash script/pqpsi.sh bench [out.md]
  bash script/pqpsi.sh bench native [out.md]
  bash script/pqpsi.sh matrix [out.md]

Common examples
  bash script/pqpsi.sh build
  bash script/pqpsi.sh test thread 128 127 5 --kem obf-mlkem --pi hctr --threads 4
  bash script/pqpsi.sh test process 128 5 --kem eckem --pi xoodoo --threads 4
  RATE=200mbit RTT=80ms THREAD_MODE=multi THREADS=4 bash script/pqpsi.sh bench wan.md

Modes
  test thread   one process, receiver/sender are two local threads.
  test process  two party processes. Native on Linux, Docker on macOS by default.
  bench         two-process benchmark that writes a markdown report.
  bench native  native Linux two-process benchmark, no Docker or tc.
  matrix        full loopback matrix helper used for paper-style summary tables.

Defaults
  KEM=obf-mlkem
  PI=hctr
  BOB_PI=0
  THREAD_MODE=multi
  THREADS=4
  CHANNELS=THREADS
  RATE=10gbit for process/bench unless overridden
  RTT= empty; use RTT=80ms for the 200 Mbps WAN setting
EOF
}

apply_common_flags() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --)
                shift
                ;;
            --kem)
                export KEM="$2"
                shift 2
                ;;
            --pi)
                export PI="$2"
                shift 2
                ;;
            --pi-lambda)
                export PI_LAMBDA="$2"
                shift 2
                ;;
            --pi-rounds)
                export PI_ROUNDS="$2"
                shift 2
                ;;
            --bob-pi)
                export BOB_PI=1
                shift
                ;;
            --no-bob-pi|--bob-no-pi)
                export BOB_PI=0
                shift
                ;;
            --threads|--worker-threads)
                export THREADS="$2"
                export THREAD_MODE=multi
                shift 2
                ;;
            --channels|--net-channels)
                export CHANNELS="$2"
                shift 2
                ;;
            --single-thread)
                export THREAD_MODE=single
                export THREADS=1
                export CHANNELS=1
                shift
                ;;
            --multi-thread)
                export THREAD_MODE=multi
                shift
                ;;
            --rb-lambda)
                export RB_LAMBDA="$2"
                shift 2
                ;;
            --rb-eps)
                export RB_EPS="$2"
                shift 2
                ;;
            --rb-w)
                export RB_W="$2"
                shift 2
                ;;
            --rb-cols)
                export RB_COLS="$2"
                shift 2
                ;;
            --hits)
                export HITS="$2"
                shift 2
                ;;
            --misses)
                echo "--misses is only supported in thread mode right now; use --hits for process mode." >&2
                exit 1
                ;;
            --rate)
                export RATE="$2"
                shift 2
                ;;
            --rtt)
                export RTT="$2"
                shift 2
                ;;
            --delay)
                export DELAY="$2"
                shift 2
                ;;
            *)
                echo "Unsupported process-mode flag: $1" >&2
                echo "Use env vars for benchmark-only knobs, or update script/pqpsi.sh if this should be common." >&2
                exit 1
                ;;
        esac
    done
}

if [[ $# -eq 0 || "${1:-}" == "-h" || "${1:-}" == "--help" || "${1:-}" == "help" ]]; then
    usage
    exit 0
fi

cmd="$1"
shift

case "$cmd" in
    build)
        exec bash script/build-docker-pqpsi-bench.sh "$@"
        ;;

    test)
        mode="${1:-}"
        if [[ -z "$mode" || "$mode" == "-h" || "$mode" == "--help" ]]; then
            usage
            exit 0
        fi
        shift

        case "$mode" in
            thread|threads)
                set_size="${1:-${SET_SIZE:-128}}"
                hits="${2:-${HITS:-$(( set_size - 1 ))}}"
                rounds="${3:-${ROUNDS:-5}}"
                shift_count=$#
                if (( shift_count > 3 )); then
                    shift_count=3
                fi
                shift "$shift_count"
                if [[ "${1:-}" == "--" ]]; then
                    shift
                fi
                exec bash script/test-rbokvs-pqpsi.sh "$set_size" "$hits" "${WARMUPS:-1}" "$rounds" "${PORT_BASE:-43000}" "$@"
                ;;

            process|processes)
                set_size="${1:-${SET_SIZE:-128}}"
                rounds="${2:-${ROUNDS:-5}}"
                shift_count=$#
                if (( shift_count > 2 )); then
                    shift_count=2
                fi
                shift "$shift_count"
                if [[ "${1:-}" == "--" ]]; then
                    shift
                fi
                export SIZES="$set_size"
                export ROUNDS="$rounds"
                export WARMUPS="${WARMUPS:-1}"
                export RATE="${RATE:-10gbit}"
                export THREAD_MODE="${THREAD_MODE:-multi}"
                export THREADS="${THREADS:-4}"
                apply_common_flags "$@"
                backend="${PQPSI_PROCESS_BACKEND:-auto}"
                if [[ "$backend" == "native" ]]; then
                    exec bash script/benchmark-native-pqpsi.sh -
                fi
                if [[ "$backend" == "docker" ]]; then
                    exec bash script/benchmark-docker-pqpsi-loopback.sh -
                fi
                if [[ "$backend" == "auto" && "$(uname -s)" == "Linux" ]]; then
                    exec bash script/benchmark-native-pqpsi.sh -
                fi
                exec bash script/benchmark-docker-pqpsi-loopback.sh -
                ;;

            *)
                echo "Unknown test mode: $mode" >&2
                usage >&2
                exit 1
                ;;
        esac
        ;;

    bench|benchmark)
        if [[ "${1:-}" == "native" ]]; then
            shift
            exec bash script/benchmark-native-pqpsi.sh "$@"
        fi
        if [[ "${PQPSI_BENCH_BACKEND:-docker}" == "native" ]]; then
            exec bash script/benchmark-native-pqpsi.sh "$@"
        fi
        exec bash script/benchmark-docker-pqpsi.sh "$@"
        ;;

    matrix)
        if [[ ! -f script/run-pqpsi-loopback-matrix.sh ]]; then
            echo "Missing script/run-pqpsi-loopback-matrix.sh" >&2
            exit 1
        fi
        exec bash script/run-pqpsi-loopback-matrix.sh "$@"
        ;;

    *)
        echo "Unknown command: $cmd" >&2
        usage >&2
        exit 1
        ;;
esac
