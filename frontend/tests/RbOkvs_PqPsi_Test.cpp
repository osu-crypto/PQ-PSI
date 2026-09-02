#include "pqpsi/pqpsi.h"
#include "pqpsi/model.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace osuCrypto;

namespace
{
    struct NetCfg
    {
        double delayMs = 0.0;
        double bwMBps = 0.0;
    };

    struct Args
    {
        u64 setSize = 256;
        u64 warmups = 1;
        u64 rounds = 5;
        u64 portBase = 43000;
        NetCfg net;
        RbCfg rb{};
        PiCfg pi{};
        KemCfg kem{};
        bool trace = false;
        bool showWarmups = false;
        u64 hits = std::numeric_limits<u64>::max();
        u64 channels = 0;
    };

    struct PartySeries
    {
        std::vector<double> totalMs;
        std::vector<double> setupMs;
        std::vector<double> teardownMs;
        std::vector<double> prepMs;
        std::vector<double> keyMs;
        std::vector<double> maskMs;
        std::vector<double> piMs;
        std::vector<double> okEncMs;
        std::vector<double> okDecMs;
        std::vector<double> sendMs;
        std::vector<double> recvMs;
        std::vector<double> kemMs;
        std::vector<double> invMs;
    };

    struct Series
    {
        std::vector<double> runtimeMs;
        std::vector<double> commKB;
        PartySeries recv;
        PartySeries send;
    };

    struct RunRow
    {
        bool ok = false;
        u64 got = 0;
        u64 want = 0;
        double runtimeMs = 0.0;
        double commKB = 0.0;
        PqPsiRunProfile prof{};
    };

    u64 parseU64(const char* s, u64 fallback)
    {
        if (s == nullptr || *s == '\0')
        {
            return fallback;
        }
        try
        {
            return static_cast<u64>(std::stoull(s));
        }
        catch (...)
        {
            return fallback;
        }
    }

    double parseDouble(const char* s, double fallback)
    {
        if (s == nullptr || *s == '\0')
        {
            return fallback;
        }
        try
        {
            return std::stod(s);
        }
        catch (...)
        {
            return fallback;
        }
    }

    bool isNum(const char* s)
    {
        if (s == nullptr || *s == '\0')
        {
            return false;
        }
        char* end = nullptr;
        std::strtod(s, &end);
        return end != s && end != nullptr && *end == '\0';
    }

    bool argEq(const char* a, const char* b)
    {
        return std::string(a ? a : "") == std::string(b ? b : "");
    }

    void setEnv(const char* key, const std::string& val)
    {
#if defined(_WIN32)
        _putenv_s(key, val.c_str());
#else
        setenv(key, val.c_str(), 1);
#endif
    }

    double meanOf(const std::vector<double>& xs)
    {
        if (xs.empty())
        {
            return 0.0;
        }
        return std::accumulate(xs.begin(), xs.end(), 0.0) / static_cast<double>(xs.size());
    }

    double protocolMean(const PartySeries& p)
    {
        if (p.totalMs.empty())
        {
            return 0.0;
        }

        double sum = 0.0;
        for (size_t i = 0; i < p.totalMs.size(); ++i)
        {
            const double setup = i < p.setupMs.size() ? p.setupMs[i] : 0.0;
            const double teardown = i < p.teardownMs.size() ? p.teardownMs[i] : 0.0;
            const double v = p.totalMs[i] - setup - teardown;
            sum += v > 0.0 ? v : 0.0;
        }
        return sum / static_cast<double>(p.totalMs.size());
    }

    void addParty(PartySeries& out, const PqPsiStageMs& ms)
    {
        out.totalMs.push_back(ms.totalMs);
        out.setupMs.push_back(ms.setupMs);
        out.teardownMs.push_back(ms.teardownMs);
        out.prepMs.push_back(ms.prepMs);
        out.keyMs.push_back(ms.kemKeyGenMs);
        out.maskMs.push_back(ms.maskMs);
        out.piMs.push_back(ms.permuteMs);
        out.okEncMs.push_back(ms.okvsEncodeMs);
        out.okDecMs.push_back(ms.okvsDecodeMs);
        out.sendMs.push_back(ms.networkSendMs);
        out.recvMs.push_back(ms.networkRecvMs);
        out.kemMs.push_back(ms.kemCoreMs);
        out.invMs.push_back(ms.permDecryptMs);
    }

    void addRun(Series& out, const RunRow& row, const NetCfg&)
    {
        out.runtimeMs.push_back(row.runtimeMs);
        out.commKB.push_back(
            (row.prof.party0.networkSendBytes + row.prof.party1.networkSendBytes) / 1024.0);
        addParty(out.recv, row.prof.party0);
        addParty(out.send, row.prof.party1);
    }

    std::string fmt(double v, int prec = 2)
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(prec) << v;
        return os.str();
    }

    void printUsage(const char* prog)
    {
        std::cout
            << "Usage\n"
            << "  " << prog << " <setSize> <warmups> <rounds> <portBase> [delay_ms] [bw_MBps] [rb flags]\n"
            << "\n"
            << "Examples\n"
            << "  " << prog << " 256 1 5 43000\n"
            << "  " << prog << " 512 1 5 43000 --rb-eps 0.10 --rb-w 96\n"
            << "  " << prog << " 1024 1 10 43000 --rb-cols 1096 --rb-w 240 --rb-lambda 40\n"
            << "  " << prog << " 256 1 5 43000 --hits 128\n"
            << "\n"
            << "Flags\n"
            << "  --rb-lambda <v>\n"
            << "  --rb-eps <v>\n"
            << "  --rb-cols <v>\n"
            << "  --rb-w <v>\n"
            << "  --kem <obf-mlkem|eckem>\n"
            << "  --pi <conspi|hctr|feistel|xoodoo|keccak800|keccak1600|keccak1600-12|sneik-f512>\n"
            << "  --pi-rounds <v>\n"
            << "  --no-bob-pi\n"
            << "  --pi-lambda <v>\n"
            << "  --threads <v>\n"
            << "  --channels <v>\n"
            << "  --hits <v>\n"
            << "  --misses <v>\n"
            << "  --single-thread\n"
            << "  --multi-thread\n"
            << "  --trace\n"
            << "  --show-warmups\n";
    }

    Args parseArgs(int argc, char** argv)
    {
        Args args;
        int i = 1;

        if (argc <= 1 || argEq(argv[1], "-h") || argEq(argv[1], "--help") || argEq(argv[1], "help"))
        {
            printUsage(argv[0]);
            std::exit(0);
        }

        if (argc > i && isNum(argv[i])) args.setSize = parseU64(argv[i++], args.setSize);
        if (argc > i && isNum(argv[i])) args.warmups = parseU64(argv[i++], args.warmups);
        if (argc > i && isNum(argv[i])) args.rounds = parseU64(argv[i++], args.rounds);
        if (argc > i && isNum(argv[i])) args.portBase = parseU64(argv[i++], args.portBase);
        if (argc > i && isNum(argv[i])) args.net.delayMs = parseDouble(argv[i++], args.net.delayMs);
        if (argc > i && isNum(argv[i])) args.net.bwMBps = parseDouble(argv[i++], args.net.bwMBps);

        while (i < argc)
        {
            if (argEq(argv[i], "--rb-lambda") && i + 1 < argc)
            {
                args.rb.lambda = parseU64(argv[++i], args.rb.lambda);
            }
            else if (argEq(argv[i], "--rb-eps") && i + 1 < argc)
            {
                args.rb.eps = parseDouble(argv[++i], args.rb.eps);
            }
            else if (argEq(argv[i], "--rb-cols") && i + 1 < argc)
            {
                args.rb.columns = parseU64(argv[++i], args.rb.columns);
            }
            else if (argEq(argv[i], "--rb-w") && i + 1 < argc)
            {
                args.rb.bandWidth = parseU64(argv[++i], args.rb.bandWidth);
            }
            else if (argEq(argv[i], "--pi") && i + 1 < argc)
            {
                setPi(args.pi, argv[++i]);
            }
            else if (argEq(argv[i], "--pi-lambda") && i + 1 < argc)
            {
                args.pi.lambda = parseU64(argv[++i], args.pi.lambda);
            }
            else if (argEq(argv[i], "--pi-rounds") && i + 1 < argc)
            {
                args.pi.rounds = parseU64(argv[++i], args.pi.rounds);
            }
            else if (argEq(argv[i], "--no-bob-pi") || argEq(argv[i], "--bob-no-pi"))
            {
                args.pi.bobPi = false;
            }
            else if (argEq(argv[i], "--bob-pi"))
            {
                args.pi.bobPi = true;
            }
            else if (argEq(argv[i], "--kem") && i + 1 < argc)
            {
                setKem(args.kem, argv[++i]);
            }
            else if ((argEq(argv[i], "--threads") || argEq(argv[i], "--worker-threads")) && i + 1 < argc)
            {
                args.rb.workerThreads = parseU64(argv[++i], args.rb.workerThreads);
                args.rb.multiThread = args.rb.workerThreads != 1;
            }
            else if ((argEq(argv[i], "--channels") || argEq(argv[i], "--net-channels")) && i + 1 < argc)
            {
                args.channels = parseU64(argv[++i], args.channels);
            }
            else if (argEq(argv[i], "--hits") && i + 1 < argc)
            {
                args.hits = parseU64(argv[++i], args.hits);
            }
            else if (argEq(argv[i], "--misses") && i + 1 < argc)
            {
                const u64 misses = parseU64(argv[++i], 0);
                args.hits = (misses > args.setSize) ? 0 : (args.setSize - misses);
            }
            else if (argEq(argv[i], "--single-thread"))
            {
                args.rb.multiThread = false;
                args.rb.workerThreads = 1;
            }
            else if (argEq(argv[i], "--multi-thread"))
            {
                args.rb.multiThread = true;
            }
            else if (argEq(argv[i], "--trace"))
            {
                args.trace = true;
            }
            else if (argEq(argv[i], "--show-warmups"))
            {
                args.showWarmups = true;
            }
            else
            {
                throw std::invalid_argument(std::string("unknown arg: ") + argv[i]);
            }
            ++i;
        }

        return args;
    }

    RunRow runOne(const Args& args, u64 portBase)
    {
        setEnv("PQPSI_PORT_BASE", std::to_string(portBase));
        setEnv("PQPSI_TRACE", args.trace ? "1" : "0");
        setEnv("PQPSI_SIM_NET_DELAY_MS", fmt(args.net.delayMs, 3));
        setEnv("PQPSI_SIM_NET_BW_MBPS", fmt(args.net.bwMBps, 3));
        setEnv("PQPSI_NET_CHANNELS", std::to_string(args.channels));

        RunRow row;
        const auto t0 = std::chrono::steady_clock::now();
        row.ok = rbRun(args.setSize, row.got, row.want, &args.rb, &row.prof, args.hits, &args.pi, &args.kem);
        const auto t1 = std::chrono::steady_clock::now();
        row.runtimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        row.commKB =
            (row.prof.party0.networkSendBytes + row.prof.party1.networkSendBytes) / 1024.0;
        return row;
    }

    void printHeader(const Args& args, const RbInfo& rb)
    {
        auto perm = makePi(args.kem.kind == PsiKemKind::EcKem ? PiCfg{ pqperm::Kind::Xoodoo } : args.pi);
        const u64 channels = args.rb.multiThread
            ? std::max<u64>(1, std::min<u64>(args.channels == 0 ? rb.workerThreads : args.channels, rb.workerThreads))
            : 1;
        std::cout
            << "RB-OKVS PQPSI Test\n"
            << "setSize=" << args.setSize
            << " warmups=" << args.warmups
            << " rounds=" << args.rounds
            << " portBase=" << args.portBase
            << " delayMs=" << fmt(args.net.delayMs)
            << " bwMBps=" << fmt(args.net.bwMBps)
            << " hits=" << ((args.hits == std::numeric_limits<u64>::max()) ? std::string("default") : std::to_string(args.hits))
            << "\n";
        std::cout << "kem=" << name(args.kem.kind) << "\n";
        std::cout
            << "rb: eps=" << fmt(rb.eps, 3)
            << " w=" << rb.bandWidth
            << " m=" << rb.columns
            << " lambda=" << rb.lambda
            << " lambda_real=" << fmt(rb.lambdaReal)
            << " thread_mode=" << (args.rb.multiThread ? "multi" : "single")
            << " party_threads=2"
            << " worker_threads=" << rb.workerThreads
            << " net_channels=" << channels
            << "\n"
            << "pi: " << perm->name()
            << " detail=" << perm->detail()
            << " n=" << perm->n()
            << " s=" << perm->s()
            << " lambda=" << args.pi.lambda
            << " rounds=" << perm->rounds()
            << " bob_pi=" << (args.pi.bobPi ? "on" : "off")
            << "\n\n";
    }

    void printRunRow(size_t idx, const RunRow& row, bool warmup)
    {
        const double p0Protocol = std::max(
            0.0,
            pqpsiProtocolMs(row.prof.party0));
        const double p1Protocol = std::max(
            0.0,
            pqpsiProtocolMs(row.prof.party1));
        const double protocol = std::max(p0Protocol, p1Protocol);

        std::cout
            << (warmup ? "[warmup] " : "[round ] ")
            << std::setw(2) << (idx + 1)
            << "  status=" << (row.ok ? "ok" : "fail")
            << "  hits=" << row.got << "/" << row.want
            << "  protocol=" << fmt(protocol)
            << " ms"
            << "  p0=" << fmt(p0Protocol)
            << " ms"
            << "  p1=" << fmt(p1Protocol)
            << " ms"
            << "  comm=" << fmt(row.commKB)
            << " KB"
            << "\n";
    }

    void printSideBySide(const Series& ser)
    {
        struct RowDef
        {
            const char* name;
            const std::vector<double>* a;
            const std::vector<double>* b;
        };

        const std::vector<double> recvProtocol{protocolMean(ser.recv)};
        const std::vector<double> sendProtocol{protocolMean(ser.send)};
        const std::vector<RowDef> rows = {
            {"protocol", &recvProtocol, &sendProtocol},
            {"prep_alloc", &ser.recv.prepMs, &ser.send.prepMs},
            {"keygen", &ser.recv.keyMs, &ser.send.keyMs},
            {"mask_pre", &ser.recv.maskMs, &ser.send.maskMs},
            {"permute", &ser.recv.piMs, &ser.send.piMs},
            {"permute_inv", &ser.recv.invMs, &ser.send.invMs},
            {"kem_ops", &ser.recv.kemMs, &ser.send.kemMs},
            {"okvs_enc", &ser.recv.okEncMs, &ser.send.okEncMs},
            {"okvs_dec", &ser.recv.okDecMs, &ser.send.okDecMs},
            {"net_send", &ser.recv.sendMs, &ser.send.sendMs},
            {"net_recv", &ser.recv.recvMs, &ser.send.recvMs},
        };

        auto knownMean = [&](const PartySeries& p)
        {
            return meanOf(p.prepMs)
                + meanOf(p.keyMs)
                + meanOf(p.maskMs)
                + meanOf(p.piMs)
                + meanOf(p.invMs)
                + meanOf(p.kemMs)
                + meanOf(p.okEncMs)
                + meanOf(p.okDecMs)
                + meanOf(p.sendMs)
                + meanOf(p.recvMs);
        };

        const double recvOtherRaw = protocolMean(ser.recv) - knownMean(ser.recv);
        const double sendOtherRaw = protocolMean(ser.send) - knownMean(ser.send);
        const double recvOther = recvOtherRaw > 0.0 ? recvOtherRaw : 0.0;
        const double sendOther = sendOtherRaw > 0.0 ? sendOtherRaw : 0.0;

        std::cout
            << "Receiver                              Sender\n"
            << "stage         recv_ms        stage         send_ms\n"
            << "--------------------------------------------------\n";
        for (const auto& row : rows)
        {
            std::cout
                << std::left << std::setw(12) << row.name
                << std::right << std::setw(9) << fmt(meanOf(*row.a))
                << "    "
                << std::left << std::setw(12) << row.name
                << std::right << std::setw(9) << fmt(meanOf(*row.b))
                << "\n";
        }
        std::cout
            << std::left << std::setw(12) << "other"
            << std::right << std::setw(9) << fmt(recvOther)
            << "    "
            << std::left << std::setw(12) << "other"
            << std::right << std::setw(9) << fmt(sendOther)
            << "\n";
        std::cout << "\n";
    }

}

int main(int argc, char** argv)
{
    try
    {
        const Args args = parseArgs(argc, argv);
        const RbInfo rb = RbOkvsResolve(args.setSize, args.rb);
        printHeader(args, rb);

        for (u64 i = 0; i < args.warmups; ++i)
        {
            const RunRow row = runOne(args, args.portBase + i * 1000);
            if (args.showWarmups)
            {
                printRunRow(static_cast<size_t>(i), row, true);
            }
        }

        Series ser;
        bool allOk = true;
        u64 base = args.portBase + args.warmups * 1000;
        for (u64 i = 0; i < args.rounds; ++i)
        {
            const RunRow row = runOne(args, base + i * 1000);
            printRunRow(static_cast<size_t>(i), row, false);
            addRun(ser, row, args.net);
            allOk = allOk && row.ok;
        }

        std::cout << "\n";
        printSideBySide(ser);
        std::cout
            << "avg_protocol: party0=" << fmt(protocolMean(ser.recv))
            << " ms  party1=" << fmt(protocolMean(ser.send))
            << " ms  comm=" << fmt(meanOf(ser.commKB))
            << " KB\n";
        std::cout << "overall: " << (allOk ? "ok" : "needs_check") << "\n";
        return allOk ? 0 : 2;
    }
    catch (const std::exception& e)
    {
        std::cerr << "rbokvs_pqpsi_test failed: " << e.what() << "\n";
        return 1;
    }
}
