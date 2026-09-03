<div align="center">

# Post-Quantum Private Set Intersection for Small Sets

Junxin Liu, Mike Rosulek, Ni Trieu

Oregon State University, Arizona State University

[**Overview**](#overview) | [**Building**](#building-the-project) | [**Experiments**](#running-the-experiments) | [**Details**](#implementation-details) | [**Third-Party Code**](#third-party-code) | [**Contact**](#author-contact-information) | [**License**](#license)

</div>

## Overview

This repository contains two benchmark code paths:

- `./` is the PQ-PSI implementation.
- `volepsi/` is our Kyber/VOLE-PSI comparison fork. See `volepsi/README.md` for its build, test, and benchmark commands.

## Building the Project

Baseline:

| Item              | Version / note                                     |
| ----------------- | -------------------------------------------------- |
| OS                | Ubuntu 22.04, or macOS with Docker Desktop         |
| CPU               | x86_64 with AES-NI, PCLMUL, SSE2, SSE4.1           |
| Compiler          | C++14 compiler; tested with GCC on Ubuntu 22.04    |
| CMake             | `>= 3.10`                                          |
| Shell tools       | `bash`, `git`, `make`, `awk`, `python3`            |
| Docker path       | Docker Desktop on macOS, or Docker Engine on Linux |
| Network emulation | Linux `tc`; Docker runs need `--cap-add NET_ADMIN` |

`nasm` is optional. Without it, `cryptoTools` uses its portable SHA1 path.

Ubuntu packages:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git python3 \
  iproute2 util-linux \
  libboost-system-dev libboost-thread-dev \
  libgmp-dev libsodium-dev
```

### Start: macOS + Docker

Use this path on macOS. The built binaries are Linux amd64 binaries, so run
them through the scripts.

```bash
bash script/pqpsi.sh build
bash script/pqpsi.sh test thread 128 127 5 --kem obf-mlkem --pi hctr --threads 4
bash script/pqpsi.sh test process 128 5 --kem obf-mlkem --pi hctr --threads 4
```

`build` writes most output to `miracl-build.log`, `pqpsi-configure.log`, and
`pqpsi-build.log`; the terminal may stay quiet while Docker is compiling.

### Common macOS Docker issue

Use an x86_64 Docker/Colima profile. ARM (`aarch64`) Colima can fail while
building `cryptoTools`.

```bash
colima start x64 --arch x86_64 --cpu 4 --memory 8
docker context use colima-x64
docker info | grep Architecture
```

If Docker reports a Colima socket `EOF`, the active Docker context is pointing
to a profile that is not running. Switch to the running profile, usually
`colima` or `colima-x64`.

CPU pinning is off by default for the Docker benchmark. If you turn it on with
`PIN_CPUS=1`, make sure the selected CPU ranges exist in the Docker VM.

### Start: Linux

Build:

```bash
bash script/build-miracl-linux64.sh thirdparty/linux/miracl/miracl
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPQPSI_BUILD_RBOKVS_BENCH_ONLY=ON
cmake --build build --target pqpsi_party_bench pqpsi_rbokvs_bench rbokvs_g_check rbokvs_pqpsi_test pqpsi_tests -j"$(nproc)"
```

Run tests:

```bash
./bin/pqpsi_tests kemeleon
./bin/pqpsi_tests eckem
./bin/pqpsi_tests rbokvs 128 0.12 64 20
./bin/pqpsi_tests pqpsi-rbokvs 128 1 5 43000 --kem obf-mlkem --pi hctr --threads 4
bash script/pqpsi.sh test process 128 5 --kem obf-mlkem --pi hctr --threads 4
```

On Linux without Docker, `test process` runs the native two-process path.

## Artifact Quickstart

Use these first:

| Goal | Command |
| --- | --- |
| Smoke test | `bash script/pqpsi.sh test process 128 5 --kem obf-mlkem --pi hctr --threads 4` |
| Paper tables | `bash script/pqpsi.sh matrix build-docker/benchmarks/rbokvs-pqpsi/pqpsi-loopback-matrix.md` |
| Full benchmark | `RATE=10gbit THREAD_MODE=multi THREADS=4 bash script/pqpsi.sh bench lan-4thread.md` |

The paper-table matrix runs the no-bob-pi protocol only.

## Running the Experiments

### Test Modes

**Wrapper:**

```bash
bash script/pqpsi.sh test thread 128 127 5 --kem obf-mlkem --pi hctr --threads 4
bash script/pqpsi.sh test process 128 5 --kem obf-mlkem --pi hctr --threads 4
bash script/pqpsi.sh test process 128 5 --kem eckem --pi xoodoo --threads 4
```

**Native Linux direct thread-mode tests:**

```bash
./bin/pqpsi_tests pqpsi-rbokvs 128 1 5 43000 --kem obf-mlkem --pi hctr --threads 4
./bin/pqpsi_tests pqpsi-rbokvs 128 1 5 43000 --kem eckem --pi xoodoo --threads 4
```

| Mode      | Meaning                                           |
| --------- | ------------------------------------------------- |
| `thread`  | one process; Alice and Bob are local threads      |
| `process` | two party processes; this is the mode used for reported benchmark numbers |

By default, process mode uses native processes on Linux and Docker processes on macOS.
Set `PQPSI_PROCESS_BACKEND=docker` or `PQPSI_PROCESS_BACKEND=native` to force a backend.

Useful flags:

| Flag | Default | Notes |
| --- | --- | --- |
| `--kem obf-mlkem\|eckem` | `obf-mlkem` | KEM choice |
| `--pi hctr\|feistel\|keccak1600\|keccak1600-12\|keccak800\|sneik-f512\|xoodoo` | `hctr` | permutation choice |
| `--pi-rounds <n>` | `8` | Feistel round count |
| `--bob-pi` | off | non-optimized protocol; enable Bob's second permutation |
| `--no-bob-pi` | on | optimized protocol; no Bob second permutation |
| `--threads 4` | `4` | worker threads per party |
| `--channels 4` | `threads` | network channels per party |
| `--hits 127` | `n - 1` | intersection size |
| `--single-thread` | off | one worker per party |
| `--rb-eps 0.07` | auto | RB-OKVS expansion |
| `--rb-w 104` | auto | RB-OKVS band width |

### Benchmarks

Benchmarks use two party processes. This matches the setup used for the paper
tables. There are two backends:

- Docker backend: macOS-friendly, uses one Linux Docker container and can apply `tc` network shaping.
- Native backend: Linux-only, no Docker; supports `tc` on `lo` when permitted.

Docker backend smoke test, printed to terminal:

```bash
SIZES=128 ROUNDS=1 WARMUPS=0 RATE=10gbit THREAD_MODE=multi THREADS=4 \
  bash script/pqpsi.sh bench -
```

Docker backend short benchmark reports:

```bash
SIZES=128 ROUNDS=5 WARMUPS=1 RATE=10gbit THREAD_MODE=single THREADS=1 \
  bash script/pqpsi.sh bench lan-single-smoke.md

SIZES=128 ROUNDS=5 WARMUPS=1 RATE=10gbit THREAD_MODE=multi THREADS=4 \
  bash script/pqpsi.sh bench lan-4thread-smoke.md

SIZES=128 ROUNDS=5 WARMUPS=1 RATE=200mbit RTT=80ms THREAD_MODE=multi THREADS=4 \
  bash script/pqpsi.sh bench wan-4thread-smoke.md
```

Docker backend full default benchmark:

```bash
RATE=10gbit THREAD_MODE=multi THREADS=4 bash script/pqpsi.sh bench lan-4thread.md
```

The full default uses `SIZES="128 256 512 1024"`, `ROUNDS=60`, and
`WARMUPS=3`, so it can run for a while with little terminal output.

Native Linux backend, no Docker:

```bash
SIZES=128 ROUNDS=5 WARMUPS=1 THREAD_MODE=multi THREADS=4 \
  bash script/pqpsi.sh bench native native-smoke.md
```

Settings:

| Knob          | Default            | Notes                                        |
| ------------- | ------------------ | -------------------------------------------- |
| `SIZES`       | `128 256 512 1024` | set sizes                                    |
| `WARMUPS`     | `3`                | warmups                                      |
| `ROUNDS`      | `60`               | measured rounds                              |
| `RATE`        | `10gbit`           | `tc netem` rate on `lo`                      |
| `RTT`         | empty              | `RTT=80ms` applies `tc delay 40ms`           |
| `THREAD_MODE` | `multi`            | `single` or `multi`                          |
| `THREADS`     | `4`                | worker threads per party                     |
| `CHANNELS`    | `THREADS`          | network channels                             |
| `KEM`         | `obf-mlkem`        | `obf-mlkem` or `eckem`                       |
| `PI`          | `hctr`             | permutation                                  |
| `BOB_PI`      | `0`                | optimized protocol; set `1` for not optimized |



## Implementation Details

| Path                    | Notes                  |
| ----------------------- | ---------------------- |
| `frontend/pqpsi/`       | protocol code          |
| `frontend/okvs/`        | RB-OKVS                |
| `frontend/kem/`         | KEM choices            |
| `frontend/permutation/` | big permutations       |
| `tests/pqpsi_tests.cpp` | command-line tests     |
| `frontend/benchmarks/`  | two-process benchmarks |
| `script/pqpsi.sh`       | main PQ-PSI wrapper     |
| `volepsi/`              | Kyber/VOLE-PSI fork    |

Use `script/pqpsi.sh` first. It is the main entry point for test and bench.
The loopback matrix script is the other main entry point for paper tables.

| Main entry point | Use |
| --- | --- |
| `script/pqpsi.sh` | test and benchmark wrapper |
| `script/run-pqpsi-loopback-matrix.sh` | paper-table benchmark runner |

| Helper script | Use |
| --- | --- |
| `script/check-linux-pqpsi-deps.sh` | Linux dependency check |
| `script/build-miracl-linux64.sh` | MIRACL build |
| `script/build-docker-pqpsi-bench.sh` | Docker build |
| `script/test-rbokvs-pqpsi.sh` | thread-mode test |
| `script/benchmark-docker-pqpsi-loopback.sh` | two-process benchmark |
| `script/benchmark-docker-pqpsi.sh` | benchmark wrapper |

## KEMs

| Name        | Type         | Row bytes | Notes                                          |
| ----------- | ------------ | --------: | ---------------------------------------------- |
| `obf-mlkem` | post-quantum |       800 | ML-KEM-512 + Kemeleon                          |
| `eckem`     | non-PQ       |        48 | X25519/Elligator2 + 128-bit tag; uses `xoodoo` |

More details: `frontend/kem/README.md`.

## Permutations

`ConsPi` is the new wide-block permutation construction proposed in our paper;
the Keccak and SNEIK entries below are instantiations of it.

| Name            | Notes                             |
| --------------- | --------------------------------- |
| `hctr`          | AES-128-HCTR2                     |
| `feistel`       | SHAKE256 balanced Feistel, configurable rounds |
| `keccak1600`    | ConsPi, Keccak-f[1600], 24 rounds |
| `keccak1600-12` | ConsPi, Keccak-f[1600], 12 rounds |
| `keccak800`     | ConsPi, Keccak-f[800]             |
| `sneik-f512`    | ConsPi, SNEIK-f512                |
| `xoodoo`        | used by `eckem`                   |

## Dataset

Synthetic sets. Default `hits = n - 1`.

- both parties start from one PRNG-generated base set
- party 0 rewrites `n - hits` items
- `--hits` overrides the intersection size

### Third-Party Code

| Component              | Source                                                                     |
| ---------------------- | -------------------------------------------------------------------------- |
| mlkem-native           | https://github.com/pq-code-package/mlkem-native                            |
| Monocypher             | https://github.com/LoupVaillant/Monocypher                                 |
| XKCP / Xoodoo / Keccak | https://github.com/XKCP/XKCP                                               |
| KeccakTools            | https://keccak.team/software.html                                          |
| SNEIK-f512             | https://csrc.nist.gov/Projects/lightweight-cryptography/round-1-candidates |
| MIRACL                 | https://github.com/miracl/MIRACL                                           |
| Boost                  | https://www.boost.org                                                      |
| cryptoTools            | https://github.com/osu-crypto/cryptoTools                                  |

## Author Contact Information

See the paper for author contact details.

## License

See [LICENSE](LICENSE).
