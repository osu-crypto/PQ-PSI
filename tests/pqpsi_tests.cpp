#include "frontend/pqpsi/pqpsi.h"
#include "frontend/kem/eckem/eckem.h"
#include "frontend/kem/obf-mlkem/backend/MlKem.h"
#include "frontend/kem/obf-mlkem/codec/Kemeleon.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace osuCrypto;

namespace
{
    bool argEq(const char* a, const char* b)
    {
        return std::string(a ? a : "") == std::string(b ? b : "");
    }

    u64 parseU64(const char* s, u64 fallback)
    {
        if (s == nullptr || *s == '\0')
            return fallback;
        return static_cast<u64>(std::stoull(s));
    }

    double parseDouble(const char* s, double fallback)
    {
        if (s == nullptr || *s == '\0')
            return fallback;
        return std::stod(s);
    }

    void setEnv(const char* key, const std::string& val)
    {
#if defined(_WIN32)
        _putenv_s(key, val.c_str());
#else
        setenv(key, val.c_str(), 1);
#endif
    }

    std::string fmt(double v, int prec = 2)
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(prec) << v;
        return os.str();
    }

    u8 hexNibble(char c)
    {
        if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<u8>(10 + c - 'a');
        if (c >= 'A' && c <= 'F') return static_cast<u8>(10 + c - 'A');
        throw std::invalid_argument("bad hex digit");
    }

    std::vector<u8> hexBytes(const char* hex)
    {
        const std::string s(hex ? hex : "");
        if ((s.size() % 2) != 0)
            throw std::invalid_argument("odd hex length");

        std::vector<u8> out(s.size() / 2);
        for (size_t i = 0; i < out.size(); ++i)
        {
            out[i] = static_cast<u8>((hexNibble(s[i * 2]) << 4) | hexNibble(s[i * 2 + 1]));
        }
        return out;
    }

    double meanOf(const std::vector<double>& xs)
    {
        if (xs.empty())
            return 0.0;
        return std::accumulate(xs.begin(), xs.end(), 0.0) / static_cast<double>(xs.size());
    }

    struct NetCfg
    {
        double delayMs = 0.0;
        double bwMBps = 0.0;
    };

    struct PqArgs
    {
        u64 setSize = 256;
        u64 warmups = 1;
        u64 rounds = 5;
        u64 portBase = 43000;
        u64 hits = std::numeric_limits<u64>::max();
        NetCfg net;
        RbCfg rb{};
        PiCfg pi{};
        KemCfg kem{};
        u64 channels = 0;
    };

    struct PqRow
    {
        bool ok = false;
        u64 got = 0;
        u64 want = 0;
        double runtimeMs = 0.0;
        double commKB = 0.0;
        PqPsiRunProfile prof{};
    };

    PqArgs parsePqArgs(int argc, char** argv, int start)
    {
        PqArgs args;
        int i = start;
        if (argc > i) args.setSize = parseU64(argv[i++], args.setSize);
        if (argc > i) args.warmups = parseU64(argv[i++], args.warmups);
        if (argc > i) args.rounds = parseU64(argv[i++], args.rounds);
        if (argc > i) args.portBase = parseU64(argv[i++], args.portBase);

        while (i < argc)
        {
            if (argEq(argv[i], "--rb-lambda") && i + 1 < argc)
                args.rb.lambda = parseU64(argv[++i], args.rb.lambda);
            else if (argEq(argv[i], "--rb-eps") && i + 1 < argc)
                args.rb.eps = parseDouble(argv[++i], args.rb.eps);
            else if (argEq(argv[i], "--rb-cols") && i + 1 < argc)
                args.rb.columns = parseU64(argv[++i], args.rb.columns);
            else if (argEq(argv[i], "--rb-w") && i + 1 < argc)
                args.rb.bandWidth = parseU64(argv[++i], args.rb.bandWidth);
            else if (argEq(argv[i], "--pi") && i + 1 < argc)
                setPi(args.pi, argv[++i]);
            else if (argEq(argv[i], "--pi-rounds") && i + 1 < argc)
                args.pi.rounds = parseU64(argv[++i], args.pi.rounds);
            else if (argEq(argv[i], "--pi-lambda") && i + 1 < argc)
                args.pi.lambda = parseU64(argv[++i], args.pi.lambda);
            else if (argEq(argv[i], "--no-bob-pi") || argEq(argv[i], "--bob-no-pi"))
                args.pi.bobPi = false;
            else if (argEq(argv[i], "--bob-pi"))
                args.pi.bobPi = true;
            else if (argEq(argv[i], "--kem") && i + 1 < argc)
                setKem(args.kem, argv[++i]);
            else if ((argEq(argv[i], "--threads") || argEq(argv[i], "--worker-threads")) && i + 1 < argc)
            {
                args.rb.workerThreads = parseU64(argv[++i], args.rb.workerThreads);
                args.rb.multiThread = args.rb.workerThreads != 1;
            }
            else if ((argEq(argv[i], "--channels") || argEq(argv[i], "--net-channels")) && i + 1 < argc)
                args.channels = parseU64(argv[++i], args.channels);
            else if (argEq(argv[i], "--hits") && i + 1 < argc)
                args.hits = parseU64(argv[++i], args.hits);
            else if (argEq(argv[i], "--misses") && i + 1 < argc)
            {
                const u64 miss = parseU64(argv[++i], 0);
                args.hits = miss > args.setSize ? 0 : args.setSize - miss;
            }
            else if (argEq(argv[i], "--delay-ms") && i + 1 < argc)
                args.net.delayMs = parseDouble(argv[++i], args.net.delayMs);
            else if (argEq(argv[i], "--bw-mbps") && i + 1 < argc)
                args.net.bwMBps = parseDouble(argv[++i], args.net.bwMBps);
            else if (argEq(argv[i], "--single-thread"))
            {
                args.rb.multiThread = false;
                args.rb.workerThreads = 1;
            }
            else if (argEq(argv[i], "--multi-thread"))
                args.rb.multiThread = true;
            else
                throw std::invalid_argument(std::string("unknown arg: ") + argv[i]);
            ++i;
        }
        return args;
    }

    PqRow runPqOne(const PqArgs& args, u64 portBase)
    {
        setEnv("PQPSI_PORT_BASE", std::to_string(portBase));
        setEnv("PQPSI_TRACE", "0");
        setEnv("PQPSI_SIM_NET_DELAY_MS", fmt(args.net.delayMs, 3));
        setEnv("PQPSI_SIM_NET_BW_MBPS", fmt(args.net.bwMBps, 3));
        setEnv("PQPSI_NET_CHANNELS", std::to_string(args.channels));

        PqRow row;
        const auto t0 = std::chrono::steady_clock::now();
        row.ok = rbRun(args.setSize, row.got, row.want, &args.rb, &row.prof, args.hits, &args.pi, &args.kem);
        const auto t1 = std::chrono::steady_clock::now();
        row.runtimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        row.commKB =
            (row.prof.party0.networkSendBytes + row.prof.party1.networkSendBytes) / 1024.0;
        return row;
    }

    int runPqPsi(int argc, char** argv, int start)
    {
        const auto args = parsePqArgs(argc, argv, start);
        std::vector<PqRow> rows;
        rows.reserve(args.rounds);

        for (u64 i = 0; i < args.warmups; ++i)
        {
            (void)runPqOne(args, args.portBase + i * 100);
        }
        for (u64 i = 0; i < args.rounds; ++i)
        {
            rows.push_back(runPqOne(args, args.portBase + (args.warmups + i) * 100));
        }

        bool ok = true;
        std::vector<double> runMs, commKB, recvMs, sendMs;
        for (const auto& row : rows)
        {
            ok = ok && row.ok;
            runMs.push_back(row.runtimeMs);
            commKB.push_back(row.commKB);
            recvMs.push_back(row.prof.party0.totalMs);
            sendMs.push_back(row.prof.party1.totalMs);
        }

        const auto rb = RbOkvsResolve(args.setSize, args.rb);
        const u64 channels = args.rb.multiThread
            ? std::max<u64>(1, std::min<u64>(args.channels == 0 ? rb.workerThreads : args.channels, rb.workerThreads))
            : 1;
        std::cout
            << "pqpsi-rbokvs\n"
            << "set=" << args.setSize
            << " rounds=" << args.rounds
            << " hits=" << (args.hits == std::numeric_limits<u64>::max() ? args.setSize - 1 : args.hits)
            << " thread_mode=" << (args.rb.multiThread ? "multi" : "single")
            << " party_threads=2"
            << " worker_threads=" << rb.workerThreads
            << " net_channels=" << channels
            << " kem=" << name(args.kem.kind)
            << " pi=" << makePi(args.kem.kind == PsiKemKind::EcKem ? PiCfg{ pqperm::Kind::Xoodoo } : args.pi)->name()
            << " bob_pi=" << (args.pi.bobPi ? "on" : "off")
            << " status=" << (ok ? "ok" : "fail") << "\n"
            << "runtime_ms=" << fmt(meanOf(runMs))
            << " comm_kb=" << fmt(meanOf(commKB))
            << " recv_ms=" << fmt(meanOf(recvMs))
            << " send_ms=" << fmt(meanOf(sendMs))
            << "\n";
        return ok ? 0 : 1;
    }

    struct RbArgs
    {
        u64 n = 256;
        double eps = 0.10;
        u64 w = 64;
        u64 rounds = 20;
        bool multiThread = true;
        size_t workerThreads = 4;
    };

    RbArgs parseRbArgs(int argc, char** argv, int start)
    {
        RbArgs args;
        int i = start;
        if (argc > i) args.n = parseU64(argv[i++], args.n);
        if (argc > i) args.eps = parseDouble(argv[i++], args.eps);
        if (argc > i) args.w = parseU64(argv[i++], args.w);
        if (argc > i && argv[i][0] != '-') args.rounds = parseU64(argv[i++], args.rounds);
        while (i < argc)
        {
            if (argEq(argv[i], "--single-thread"))
            {
                args.multiThread = false;
                args.workerThreads = 1;
            }
            else if (argEq(argv[i], "--multi-thread"))
                args.multiThread = true;
            else if ((argEq(argv[i], "--threads") || argEq(argv[i], "--worker-threads")) && i + 1 < argc)
            {
                args.workerThreads = parseU64(argv[++i], args.workerThreads);
                args.multiThread = args.workerThreads != 1;
            }
            else
                throw std::invalid_argument(std::string("unknown arg: ") + argv[i]);
            ++i;
        }
        return args;
    }

    bool runRbOne(const RbArgs& args, u64 seed, double& runMs)
    {
        PRNG prng(toBlock(0x72626f6b7673ULL, seed));
        std::vector<block> keys(args.n);
        std::vector<block> vals(args.n);
        for (size_t i = 0; i < args.n; ++i)
        {
            keys[i] = prng.get<block>() ^ toBlock(seed, static_cast<u64>(i + 1));
            vals[i] = prng.get<block>() ^ toBlock(0xfeed0000ULL + seed, static_cast<u64>(i));
        }

        RBParams rb;
        rb.lambda = 1;
        rb.eps = args.eps;
        rb.bandWidth = args.w;
        rb.multiThread = args.multiThread;
        rb.workerThreads = args.workerThreads;
        rb.columns = static_cast<size_t>(std::ceil((1.0 + args.eps) * static_cast<double>(std::max<u64>(args.n, 1))));

        std::vector<block> table;
        const auto t0 = std::chrono::steady_clock::now();
        const bool encOk = RBEncode(keys, vals, table, rb);
        std::vector<block> got;
        if (encOk)
        {
            RBDecode(table, keys, got, rb);
        }
        const auto t1 = std::chrono::steady_clock::now();
        runMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (!encOk || got.size() != vals.size())
            return false;
        for (size_t i = 0; i < vals.size(); ++i)
        {
            if (std::memcmp(&got[i], &vals[i], sizeof(block)) != 0)
                return false;
        }
        return true;
    }

    int runRbOkvs(int argc, char** argv, int start)
    {
        const auto args = parseRbArgs(argc, argv, start);
        u64 pass = 0;
        std::vector<double> runMs;
        runMs.reserve(args.rounds);

        for (u64 i = 0; i < args.rounds; ++i)
        {
            double ms = 0.0;
            const bool ok = runRbOne(args, i + 1, ms);
            runMs.push_back(ms);
            pass += ok ? 1 : 0;
        }

        const double g40 = 40.0 - 2.751 * args.eps * static_cast<double>(args.w);
        std::cout
            << "rbokvs\n"
            << "n=" << args.n
            << " eps=" << fmt(args.eps, 2)
            << " w=" << args.w
            << " rounds=" << args.rounds
            << " thread_mode=" << (args.multiThread ? "multi" : "single")
            << " worker_threads=" << RBWorkerThreads(args.multiThread, args.workerThreads)
            << " pass=" << pass << "/" << args.rounds
            << " g_40=" << fmt(g40)
            << " runtime_ms=" << fmt(meanOf(runMs))
            << "\n";
        return pass == args.rounds ? 0 : 1;
    }

    void fillSeed(span<u8> seed, u8 base)
    {
        for (u64 i = 0; i < seed.size(); ++i)
        {
            seed[i] = static_cast<u8>(base + i);
        }
    }

    void kemeleonCheckMode(MlKem::Mode mode)
    {
        MlKem kem(mode);
        Kemeleon codec(mode);

        std::array<u8, MlKem::KeyGenSeedSize> keySeed;
        std::array<u8, MlKem::EncapSeedSize> encSeed;
        fillSeed(keySeed, 0x20);
        fillSeed(encSeed, 0x80);

        MlKem::KeyPair pair;
        std::vector<u8> keyData;
        bool keyOk = false;
        for (u64 i = 0; i < 256 && !keyOk; ++i)
        {
            keySeed[0] = static_cast<u8>(0x20 + i);
            pair = kem.keyGen(keySeed);
            keyOk = codec.encodeKey(pair.publicKey, keyData);
        }
        if (!keyOk)
            throw std::runtime_error("kemeleon key encode failed");

        MlKem::EncapResult enc;
        std::vector<u8> cipherData;
        bool cipherOk = false;
        for (u64 i = 0; i < 256 && !cipherOk; ++i)
        {
            encSeed[0] = static_cast<u8>(0x80 + i);
            enc = kem.encaps(pair.publicKey, encSeed);
            for (u64 j = 0; j < 4096 && !cipherOk; ++j)
            {
                cipherOk = codec.encodeCipher(enc.cipherText, cipherData);
            }
        }
        if (!cipherOk)
            throw std::runtime_error("kemeleon cipher encode failed");

        std::vector<u8> pkOut;
        std::vector<u8> ctOut;
        if (!codec.decodeKey(keyData, pkOut) || pkOut != pair.publicKey)
            throw std::runtime_error("kemeleon decodeKey failed");
        if (!codec.decodeCipher(cipherData, ctOut) || ctOut != enc.cipherText)
            throw std::runtime_error("kemeleon decodeCipher failed");
    }

    int runKemeleon()
    {
        kemeleonCheckMode(MlKem::Mode::MlKem512);
        kemeleonCheckMode(MlKem::Mode::MlKem768);
        kemeleonCheckMode(MlKem::Mode::MlKem1024);
        std::cout << "kemeleon\nstatus=ok\n";
        return 0;
    }

    int runEcKem()
    {
        EcKem kem;
        const auto spec = EcKem::spec();
        if (spec.pkBytes != 32 || spec.skBytes != 32 || spec.ctBytes != 48 || spec.ssBytes != 16)
        {
            throw std::runtime_error("eckem sizes changed unexpectedly");
        }

        std::array<u8, EcKem::KeySeedBytes> keySeed{};
        std::array<u8, EcKem::KeySeedBytes> otherSeed{};
        std::array<u8, EcKem::EncSeedBytes> encSeed{};
        fillSeed(keySeed, 0x11);
        fillSeed(otherSeed, 0x52);
        fillSeed(encSeed, 0x93);

        const auto pair = kem.keyGen(keySeed);
        const auto other = kem.keyGen(otherSeed);
        const auto enc = kem.encap(pair.pk, encSeed);

        std::array<u8, EcKem::TagBytes> tag{};
        if (!kem.decap(pair.sk, enc.ct, tag) || tag != enc.tag)
        {
            throw std::runtime_error("eckem decap failed");
        }

        if (kem.decap(other.sk, enc.ct, tag))
        {
            throw std::runtime_error("eckem accepted ciphertext under the wrong key");
        }

        auto bad = enc.ct;
        bad[0] ^= 1;
        if (kem.decap(pair.sk, bad, tag))
        {
            throw std::runtime_error("eckem accepted a corrupted ciphertext");
        }

        u64 bot = 0;
        for (u64 t = 0; t < 256; ++t)
        {
            for (size_t i = 0; i < bad.size(); ++i)
            {
                bad[i] = static_cast<u8>(0xA7 + 31 * t + 17 * i);
            }
            bot += kem.decap(pair.sk, bad, tag) ? 0 : 1;
        }
        if (bot != 256)
        {
            throw std::runtime_error("eckem random ciphertext sparsity check failed");
        }

        std::cout
            << "eckem\n"
            << "status=ok\n"
            << "pk_bytes=" << spec.pkBytes << "\n"
            << "sk_bytes=" << spec.skBytes << "\n"
            << "ct_bytes=" << spec.ctBytes << "\n"
            << "ss_bytes=" << spec.ssBytes << "\n"
            << "bot_checks=" << bot << "/256\n";
        return 0;
    }

    int runEcKemBench(int argc, char** argv, int start)
    {
        const u64 n = argc > start ? parseU64(argv[start], 128) : 128;
        const u64 rounds = argc > start + 1 ? parseU64(argv[start + 1], 20) : 20;

        EcKem kem;
        std::array<u8, EcKem::TagBytes> tag{};
        volatile u64 checksum = 0;

        auto makeKeySeed = [](u64 row, u64 round)
        {
            std::array<u8, EcKem::KeySeedBytes> seed{};
            for (size_t i = 0; i < seed.size(); ++i)
            {
                seed[i] = static_cast<u8>(0x31 + row * 17 + round * 29 + i * 7);
            }
            return seed;
        };

        auto makeEncSeed = [](u64 row, u64 round)
        {
            std::array<u8, EcKem::EncSeedBytes> seed{};
            for (size_t i = 0; i < seed.size(); ++i)
            {
                seed[i] = static_cast<u8>(0x83 + row * 19 + round * 23 + i * 11);
            }
            return seed;
        };

        auto ms = [](auto begin, auto end)
        {
            return std::chrono::duration<double, std::milli>(end - begin).count();
        };

        std::vector<EcKem::KeyPair> pairs(n);
        std::vector<EcKem::Encap> encs(n);
        std::vector<double> seededKeyMs;
        std::vector<double> seededEncMs;
        std::vector<double> decMs;
        std::vector<double> randomKeyMs;
        std::vector<double> randomEncMs;
        seededKeyMs.reserve(rounds);
        seededEncMs.reserve(rounds);
        decMs.reserve(rounds);
        randomKeyMs.reserve(rounds);
        randomEncMs.reserve(rounds);

        for (u64 r = 0; r < rounds; ++r)
        {
            auto t0 = std::chrono::steady_clock::now();
            for (u64 i = 0; i < n; ++i)
            {
                pairs[i] = kem.keyGen(makeKeySeed(i, r));
                checksum += pairs[i].pk[0];
            }
            auto t1 = std::chrono::steady_clock::now();
            seededKeyMs.push_back(ms(t0, t1));

            for (u64 i = 0; i < n; ++i)
            {
                encs[i] = kem.encap(pairs[i].pk, makeEncSeed(i, r));
                checksum += encs[i].ct[0];
            }
            auto t2 = std::chrono::steady_clock::now();
            seededEncMs.push_back(ms(t1, t2));

            for (u64 i = 0; i < n; ++i)
            {
                if (!kem.decap(pairs[i].sk, encs[i].ct, tag) || tag != encs[i].tag)
                {
                    throw std::runtime_error("eckem-bench decap failed");
                }
                checksum += tag[0];
            }
            auto t3 = std::chrono::steady_clock::now();
            decMs.push_back(ms(t2, t3));

            for (u64 i = 0; i < n; ++i)
            {
                pairs[i] = kem.keyGen();
                checksum += pairs[i].pk[0];
            }
            auto t4 = std::chrono::steady_clock::now();
            randomKeyMs.push_back(ms(t3, t4));

            for (u64 i = 0; i < n; ++i)
            {
                encs[i] = kem.encap(pairs[i].pk);
                checksum += encs[i].ct[0];
            }
            auto t5 = std::chrono::steady_clock::now();
            randomEncMs.push_back(ms(t4, t5));
        }

        std::cout
            << "eckem-bench\n"
            << "status=ok\n"
            << "n=" << n << "\n"
            << "rounds=" << rounds << "\n"
            << "seeded_keygen_ms=" << fmt(meanOf(seededKeyMs)) << "\n"
            << "seeded_encap_ms=" << fmt(meanOf(seededEncMs)) << "\n"
            << "decap_ms=" << fmt(meanOf(decMs)) << "\n"
            << "random_keygen_ms=" << fmt(meanOf(randomKeyMs)) << "\n"
            << "random_encap_ms=" << fmt(meanOf(randomEncMs)) << "\n"
            << "checksum=" << checksum << "\n";
        return 0;
    }

    int runHctr2()
    {
        struct Vec
        {
            const char* key;
            const char* tweak;
            const char* plain;
            const char* cipher;
        };

        // Google HCTR2 AES-128 vectors from github.com/google/hctr2.
        const std::array<Vec, 6> vecs{{
            {
                "74f98f60786abfa85b0bbba059e0f91e",
                "",
                "6b26837bdc1c583dc142c6ab7b3f43b0",
                "dd05a8ae51f1e8212fd6c33b9467036d",
            },
            {
                "8d36a6f8fe0ad10effbdf374ea1a5919",
                "",
                "99d5cbe30b34037f655f60fad017667f7a331e941c3ae9eebae09458e6a136",
                "99b50bd965c89e8976d3ce7215c795daff048c8fe6050b2b44a290fa2bf3fe",
            },
            {
                "03e1412f973bef6d4a9fa09f3f0601d2",
                "",
                "bc3aa5c7b70bc65fe48f35df8e4a5b8cfd90fccef69315aaa92e51eaf830a1e63fba4fbae6d9ddb34e469d895c8a886a788bd625f92e75c5f1472232bb948e549e63091901265260360434a0a98b06bc7bfc4c655af93fa4c8f9b4e68538925506b82597c9c59f9f966d2be1d7c691bba8290595bedb339609d687f83381ea7d",
                "b6e1673512469b8218f7f54b065a72f7fe172d6edcfe80b2b745a916c1c13fe2eadc838b74a29a532690462eaff2fdf52533be50f63aaa47058ddd28761752bcc7cee101625b8d3b6aef0b4792348c444089a79ea34cae06c74d4ffad6d239ae5b7e6fddd3dc97e2d6af8f5ffce146ba09f0ad1ba5dc40d5914a39557662fb55",
            },
            {
                "e893a5428fa81bb4c2462d5a8ec3318b",
                "86",
                "2367096be680e11f57dc68641471b95c",
                "d4a95bdf8de60134ea75fa14f388b835",
            },
            {
                "8171c4d67e21d6a250235be986da8e3f",
                "10",
                "5477797774f15a78b691d443b14be3560b88f5b95977549508e85ebd7f31bb",
                "0a999af4e64b89473ae7ce0c1e06a03275e74f5640af8f15af291e04a097cd",
            },
            {
                "ce6239eb619bbf54fa1afcee49da9774",
                "",
                "650bf6c1baaf38493b0d6e52168da76c8efb28d5f9e26cec3b4f1a7d016e10be10e280aa36663eb301ebfc640b5de9788544e54708919c057b48ec76c0397f857f090bfde70d842ff0a322cbdf1d68af0d233762f8d103a8fda0d86cba40a5d2c1292bb2b39e6202b5d7b5c9bdf83a2479b6b21b9eb3950aa58eae3c80abea8a642af68f46b5e920c4f8d705f01f0c008a7fd931022bb1cedbee230a571e93f9d058a4e8fc586f55dc2c49711ad9f9ac024d80588017ce846011949e5839dd783e123193ecc944a252e9c0b7a868b9df1ec5095f84114ac06e2c4fa2b36b49ee964e186ed1f718c7f872ff1fa3f218aa948a388e477a3c2a274506edd883bb",
                "1ef34e0b44d700f0b82c6889c80efb0ec6f021f69d0a936fe4c455ea3e6f08f6778878b4409c48d64cc4733a6674278fd59a5dbb621a894dcfa7ecebf3d580a78fe99c4c382a8bae77ac28ada91c2a495d16b5a5b15b4d571afe8b7f10b60ebadb9219b83d95142753667ed3526c0adcc4495b0450792acb54cfedee5a004d0032c16a258fe567628c2ce757d3dc3ef9a6e158df9a43fa72c641d853c23bfa6dacfedc56381780e2814ecbe0f915bccae0bd1710f9ecd898acf43bc96ed14c40ab3cad34e2210aa104934dd796e58a28ad7473d07ea1d942c8db937065889095166749ad3dc5d06b9c6ac04b89086df66085c535d31b6de6ce65c7efebfa0c",
            },
        }};

        for (const auto& v : vecs)
        {
            auto key = hexBytes(v.key);
            auto tweak = hexBytes(v.tweak);
            auto plain = hexBytes(v.plain);
            auto cipher = hexBytes(v.cipher);
            if (key.size() != 16 || plain.size() != cipher.size() || plain.size() < 16)
                throw std::runtime_error("bad HCTR2 test vector");

            pqperm::hctr2::Key hkey(key.data(), tweak.data(), tweak.size());
            auto got = plain;
            pqperm::hctr2::crypt(got.data(), got.size(), hkey, true);
            if (got != cipher)
                throw std::runtime_error("HCTR2 encrypt vector failed");
            pqperm::hctr2::crypt(got.data(), got.size(), hkey, false);
            if (got != plain)
                throw std::runtime_error("HCTR2 decrypt vector failed");
        }

        std::cout << "hctr2\nstatus=ok\nvectors=" << vecs.size() << "\n";
        return 0;
    }

    int runSneik()
    {
        auto small = pi::makePerm(pi::Kind::SneikF512);
        for (u64 t = 0; t < 1024; ++t)
        {
            pi::Perm::Buf state(small->bytes());
            for (size_t i = 0; i < state.size(); ++i)
            {
                state[i] = static_cast<u8>(0xA5 + 13 * i + t);
            }

            const auto want = state;
            small->apply(state);
            small->invert(state);
            if (state != want)
            {
                throw std::runtime_error("SNEIK-f512 inverse roundtrip failed");
            }
        }

        PiCfg cfg;
        setPi(cfg, "sneik-f512");
        auto cons = makePi(cfg, 0);
        std::vector<u8> key(KEM_key_size_bit / 8);
        for (size_t i = 0; i < key.size(); ++i)
        {
            key[i] = static_cast<u8>(0x3C + 7 * i);
        }

        const auto want = key;
        cons->encryptBytes(key.data(), key.size());
        cons->decryptBytes(key.data(), key.size());
        if (key != want)
        {
            throw std::runtime_error("ConsPi/SNEIK-f512 roundtrip failed");
        }

        std::cout
            << "sneik-f512\n"
            << "status=ok\n"
            << "roundtrip=1024\n"
            << "conspi_rounds=" << cons->rounds() << "\n"
            << "conspi_s=" << cons->s() << "\n";
        return 0;
    }

    int runXoodoo()
    {
        pqperm::Xoodoo pi;
        for (u64 t = 0; t < 1024; ++t)
        {
            std::array<u8, pqperm::Xoodoo::Bytes> row{};
            for (size_t i = 0; i < row.size(); ++i)
            {
                row[i] = static_cast<u8>(0x41 + 19 * i + t);
            }

            const auto want = row;
            pi.encryptBytes(row.data(), row.size());
            pi.decryptBytes(row.data(), row.size());
            if (row != want)
            {
                throw std::runtime_error("Xoodoo inverse roundtrip failed");
            }
        }

        std::cout
            << "xoodoo\n"
            << "status=ok\n"
            << "roundtrip=1024\n"
            << "bytes=" << pqperm::Xoodoo::Bytes << "\n"
            << "rounds=" << pqperm::Xoodoo::Rounds << "\n";
        return 0;
    }

    void usage(const char* prog)
    {
        std::cout
            << "Usage\n"
            << "  " << prog << " pqpsi-rbokvs [setSize] [warmups] [rounds] [portBase] [--hits v] [--rb-eps v] [--rb-w v] [--rb-cols v] [--rb-lambda v] [--kem obf-mlkem|eckem] [--pi conspi|hctr|feistel|xoodoo|keccak800|keccak1600|keccak1600-12|sneik-f512] [--pi-rounds v] [--no-bob-pi] [--threads v] [--channels v] [--single-thread|--multi-thread]\n"
            << "  " << prog << " rbokvs [setSize] [eps] [w] [rounds] [--threads v] [--single-thread|--multi-thread]\n"
            << "  " << prog << " hctr2\n"
            << "  " << prog << " sneik\n"
            << "  " << prog << " xoodoo\n"
            << "  " << prog << " eckem\n"
            << "  " << prog << " eckem-bench [n] [rounds]\n"
            << "  " << prog << " kemeleon\n"
            << "\n"
            << "Notes\n"
            << "  pqpsi-rbokvs is the in-process/thread protocol test.\n"
            << "  For the two-process loopback test, use: bash script/pqpsi.sh test process ...\n";
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc <= 1 || argEq(argv[1], "-h") || argEq(argv[1], "--help") || argEq(argv[1], "help"))
        {
            usage(argv[0]);
            return 0;
        }

        if (argEq(argv[1], "pqpsi-rbokvs"))
            return runPqPsi(argc, argv, 2);
        if (argEq(argv[1], "rbokvs"))
            return runRbOkvs(argc, argv, 2);
        if (argEq(argv[1], "hctr2"))
            return runHctr2();
        if (argEq(argv[1], "sneik") || argEq(argv[1], "sneik-f512"))
            return runSneik();
        if (argEq(argv[1], "xoodoo"))
            return runXoodoo();
        if (argEq(argv[1], "eckem"))
            return runEcKem();
        if (argEq(argv[1], "eckem-bench"))
            return runEcKemBench(argc, argv, 2);
        if (argEq(argv[1], "kemeleon"))
            return runKemeleon();

        throw std::invalid_argument(std::string("unknown test: ") + argv[1]);
    }
    catch (const std::exception& e)
    {
        std::cerr << "pqpsi_tests failed: " << e.what() << "\n";
        return 1;
    }
}
