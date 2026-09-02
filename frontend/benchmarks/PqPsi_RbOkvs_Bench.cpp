#include "pqpsi/pqpsi.h"
#include "pqpsi/model.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/utsname.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <direct.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

using namespace osuCrypto;

namespace
{
	struct Stats
	{
		double mean = 0.0;
		double median = 0.0;
		double p95 = 0.0;
		double p99 = 0.0;
		double min = 0.0;
		double max = 0.0;
	};

	struct NetMeta
	{
		double delayMs = 0.0;
		double bwMBps = 0.0;
		std::string type = "loopback";
		std::string topology = "same-host";
		std::string link = "N/A";
		std::string note = "no WAN emulation";
		std::string emu = "app";
		std::string file;
	};

	struct Args
	{
		std::string out;
		u64 setSize = 64;
		u64 warmups = 1;
		u64 rounds = 5;
		u64 portSeed = 23000;
		NetMeta net;
		RbCfg rb{};
		PiCfg pi{};
		KemCfg kem{};
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
		std::vector<double> txBytes;
		std::vector<double> rxBytes;

		void reserve(size_t n)
		{
			totalMs.reserve(n);
			setupMs.reserve(n);
			teardownMs.reserve(n);
			prepMs.reserve(n);
			keyMs.reserve(n);
			maskMs.reserve(n);
			piMs.reserve(n);
			okEncMs.reserve(n);
			okDecMs.reserve(n);
			sendMs.reserve(n);
			recvMs.reserve(n);
			kemMs.reserve(n);
			invMs.reserve(n);
			txBytes.reserve(n);
			rxBytes.reserve(n);
		}

		void add(const PqPsiStageMs& ms)
		{
			totalMs.push_back(ms.totalMs);
			setupMs.push_back(ms.setupMs);
			teardownMs.push_back(ms.teardownMs);
			prepMs.push_back(ms.prepMs);
			keyMs.push_back(ms.kemKeyGenMs);
			maskMs.push_back(ms.maskMs);
			piMs.push_back(ms.permuteMs);
			okEncMs.push_back(ms.okvsEncodeMs);
			okDecMs.push_back(ms.okvsDecodeMs);
			sendMs.push_back(ms.networkSendMs);
			recvMs.push_back(ms.networkRecvMs);
			kemMs.push_back(ms.kemCoreMs);
			invMs.push_back(ms.permDecryptMs);
			txBytes.push_back(ms.networkSendBytes);
			rxBytes.push_back(ms.networkRecvBytes);
		}
	};

	struct Series
	{
		std::vector<double> runUs;
		PartySeries p0;
		PartySeries p1;

		void reserve(size_t n)
		{
			runUs.reserve(n);
			p0.reserve(n);
			p1.reserve(n);
		}

		void add(double runUsValue, const PqPsiRunProfile& prof)
		{
			runUs.push_back(runUsValue);
			p0.add(prof.party0);
			p1.add(prof.party1);
		}
	};

	struct Counts
	{
		u64 pass = 0;
		u64 fail = 0;
		u64 mismatch = 0;
		u64 portRetry = 0;
	};

	struct RunRes
	{
		bool ok = false;
		u64 got = 0;
		u64 want = 0;
		double runUs = 0.0;
		u64 portRetry = 0;
		PqPsiRunProfile prof{};
	};

	std::string nowIsoLike()
	{
		const auto now = std::chrono::system_clock::now();
		const auto tt = std::chrono::system_clock::to_time_t(now);
		std::tm tm{};
#if defined(_WIN32)
		localtime_s(&tm, &tt);
#else
		localtime_r(&tt, &tm);
#endif
		char buf[64];
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
		return std::string(buf);
	}

	std::string unameLine()
	{
		struct utsname u {};
		if (uname(&u) != 0)
		{
			return "unknown";
		}
		return std::string(u.sysname) + " " + u.release + " " + u.version + " " + u.machine;
	}

	std::string cpuModel()
	{
#if defined(__APPLE__)
		char buf[256];
		size_t len = sizeof(buf);
		if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0)
		{
			return std::string(buf);
		}
		return "unknown";
#else
		return "unknown";
#endif
	}

	Stats summarize(std::vector<double> v)
	{
		Stats s;
		if (v.empty())
		{
			return s;
		}

		std::sort(v.begin(), v.end());
		const size_t n = v.size();
		const auto idx = [&](double q)
		{
			size_t i = static_cast<size_t>(q * static_cast<double>(n - 1));
			if (i >= n)
			{
				i = n - 1;
			}
			return i;
		};

		s.mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(n);
		s.median = v[idx(0.50)];
		s.p95 = v[idx(0.95)];
		s.p99 = v[idx(0.99)];
		s.min = v.front();
		s.max = v.back();
		return s;
	}

	std::vector<double> protocolTimes(const PartySeries& p)
	{
		std::vector<double> out;
		out.reserve(p.totalMs.size());
		for (size_t i = 0; i < p.totalMs.size(); ++i)
		{
			const double setup = i < p.setupMs.size() ? p.setupMs[i] : 0.0;
			const double teardown = i < p.teardownMs.size() ? p.teardownMs[i] : 0.0;
			const double v = p.totalMs[i] - setup - teardown;
			out.push_back(v > 0.0 ? v : 0.0);
		}
		return out;
	}

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

	std::string getenvOr(const char* key, const char* fallback)
	{
		const char* v = std::getenv(key);
		if (v == nullptr || *v == '\0')
		{
			return std::string(fallback);
		}
		return std::string(v);
	}

	std::string trimCopy(const std::string& s)
	{
		size_t b = 0;
		while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
		{
			++b;
		}
		size_t e = s.size();
		while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
		{
			--e;
		}
		return s.substr(b, e - b);
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

	void loadNetFile(NetMeta& net)
	{
		if (net.file.empty())
		{
			return;
		}

		std::ifstream in(net.file);
		if (!in)
		{
			throw std::runtime_error("failed to open network settings file: " + net.file);
		}

		std::string line;
		while (std::getline(in, line))
		{
			line = trimCopy(line);
			if (line.empty() || line[0] == '#')
			{
				continue;
			}

			const auto pos = line.find('=');
			if (pos == std::string::npos)
			{
				continue;
			}

			const std::string key = trimCopy(line.substr(0, pos));
			const std::string val = trimCopy(line.substr(pos + 1));

			if (key == "sim_delay_ms")
			{
				net.delayMs = parseDouble(val.c_str(), net.delayMs);
			}
			else if (key == "sim_bw_MBps")
			{
				net.bwMBps = parseDouble(val.c_str(), net.bwMBps);
			}
			else if (key == "network_type")
			{
				net.type = val;
			}
			else if (key == "network_topology")
			{
				net.topology = val;
			}
			else if (key == "link_speed")
			{
				net.link = val;
			}
			else if (key == "network_note")
			{
				net.note = val;
			}
		}
	}

	bool canBindLoopbackPort(u32 port)
	{
#if defined(_WIN32)
		(void)port;
		return true;
#else
		const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
		{
			return false;
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(static_cast<uint16_t>(port));
		const int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
		::close(fd);
		return rc == 0;
#endif
	}

	double pct(double part, double whole)
	{
		if (whole <= 0.0)
		{
			return 0.0;
		}
		return (part * 100.0) / whole;
	}

	std::string safeNum(double v)
	{
		std::ostringstream os;
		os << std::fixed << std::setprecision(2) << v;
		std::string s = os.str();
		while (!s.empty() && s.back() == '0')
		{
			s.pop_back();
		}
		if (!s.empty() && s.back() == '.')
		{
			s.pop_back();
		}
		for (char& ch : s)
		{
			if (ch == '.')
			{
				ch = 'p';
			}
		}
		return s.empty() ? "0" : s;
	}

	std::string defaultOut(const Args& args)
	{
		const auto rb = RbOkvsResolve(args.setSize, args.rb);
		std::ostringstream name;
		name
			<< "build/benchmarks/rbokvs-pqpsi/"
			<< "set-" << args.setSize
			<< "_warm-" << args.warmups
			<< "_rounds-" << args.rounds
			<< "_delay-" << safeNum(args.net.delayMs)
			<< "_bw-" << safeNum(args.net.bwMBps)
			<< "_m-" << rb.columns
			<< "_w-" << rb.bandWidth
			<< "_eps-" << safeNum(rb.eps)
			<< "_lam-" << rb.lambda
			<< "_thr-" << (args.rb.multiThread ? "multi" : "single")
			<< "_workers-" << rb.workerThreads
			<< ".md";
		return name.str();
	}

	std::string parentDir(const std::string& path)
	{
		const size_t pos = path.find_last_of("/\\");
		if (pos == std::string::npos)
		{
			return std::string();
		}
		return path.substr(0, pos);
	}

	void mkOne(const std::string& path)
	{
		if (path.empty())
		{
			return;
		}
#if defined(_WIN32)
		_mkdir(path.c_str());
#else
		::mkdir(path.c_str(), 0755);
#endif
	}

	void ensureDir(const std::string& path)
	{
		if (path.empty())
		{
			return;
		}

		std::string cur;
		for (size_t i = 0; i < path.size(); ++i)
		{
			const char ch = path[i];
			cur.push_back(ch);
			if (ch == '/' || ch == '\\')
			{
				if (cur.size() == 1)
				{
					continue;
				}
				mkOne(cur);
			}
		}
		mkOne(cur);
	}

	void printUsage(const char* prog)
	{
		std::cout
			<< "Usage\n"
			<< "  " << prog << " [out.md] <setSize> <warmups> <rounds> <portBase> [delay_ms] [bw_MBps] [net_file] [rb flags]\n"
			<< "\n"
			<< "Single size only\n"
			<< "  This binary runs one set size and writes one markdown report\n"
			<< "\n"
			<< "Sweep many sizes\n"
			<< "  Use script/pqpsi.sh bench for 2^7 to 2^10 summary output\n"
			<< "\n"
			<< "Examples\n"
			<< "  " << prog << " 512 1 5 43000\n"
			<< "  " << prog << " my-run.md 512 1 5 43000 2.0 10.0 frontend/benchmarks/network_settings.example.conf\n"
			<< "  " << prog << " 512 1 5 43000 --rb-eps 0.25\n"
			<< "  " << prog << " 512 1 5 43000 --rb-cols 640 --rb-w 64 --rb-lambda 40\n"
			<< "  " << prog << " 512 1 5 43000 --pi keccak800\n"
			<< "  " << prog << " 512 1 5 43000 --pi sneik-f512\n"
			<< "  " << prog << " 512 1 5 43000 --pi hctr\n"
			<< "  " << prog << " 512 1 5 43000 --pi feistel --pi-rounds 8\n"
			<< "  " << prog << " 512 1 5 43000 --no-bob-pi\n"
			<< "  " << prog << " 512 1 5 43000 --kem eckem\n"
			<< "  " << prog << " 512 1 5 43000 --threads 4\n"
			<< "  " << prog << " 512 1 5 43000 --single-thread\n";
	}

	Args parseArgs(int argc, char** argv)
	{
		Args args;
		int i = 1;
		args.net.delayMs = parseDouble(std::getenv("PQPSI_SIM_NET_DELAY_MS"), 0.0);
		args.net.bwMBps = parseDouble(std::getenv("PQPSI_SIM_NET_BW_MBPS"), 0.0);
		args.net.type = getenvOr("PQPSI_BENCH_NET_TYPE", "loopback");
		args.net.topology = getenvOr("PQPSI_BENCH_TOPOLOGY", "same-host");
		args.net.link = getenvOr("PQPSI_BENCH_LINK", "N/A");
		args.net.note = getenvOr("PQPSI_BENCH_NET_NOTE", "no WAN emulation");
		args.net.emu = getenvOr("PQPSI_NET_EMU", "app");
		args.rb.lambda = parseU64(std::getenv("PQPSI_RB_LAMBDA"), args.rb.lambda);
		args.rb.eps = parseDouble(std::getenv("PQPSI_RB_EPS"), args.rb.eps);
		args.rb.columns = parseU64(std::getenv("PQPSI_RB_COLS"), args.rb.columns);
		args.rb.bandWidth = parseU64(std::getenv("PQPSI_RB_W"), args.rb.bandWidth);
		args.rb.workerThreads = parseU64(std::getenv("PQPSI_THREADS"), args.rb.workerThreads);
		if (const char* piEnv = std::getenv("PQPSI_PI"))
		{
			setPi(args.pi, piEnv);
		}
		if (const char* bobPiEnv = std::getenv("PQPSI_BOB_PI"))
		{
			const std::string v(bobPiEnv);
			args.pi.bobPi = !(v == "0" || v == "false" || v == "off" || v == "no");
		}
		if (const char* kemEnv = std::getenv("PQPSI_KEM"))
		{
			setKem(args.kem, kemEnv);
		}
		args.pi.lambda = parseU64(std::getenv("PQPSI_PI_LAMBDA"), args.pi.lambda);

		if (argc > i && !isNum(argv[i]))
		{
			args.out = argv[i++];
		}
		if (argc > i)
		{
			args.setSize = parseU64(argv[i++], args.setSize);
		}
		if (argc > i)
		{
			args.warmups = parseU64(argv[i++], args.warmups);
		}
		if (argc > i)
		{
			args.rounds = parseU64(argv[i++], args.rounds);
		}
		if (argc > i)
		{
			args.portSeed = parseU64(argv[i++], args.portSeed);
		}

		double cliDelay = args.net.delayMs;
		double cliBw = args.net.bwMBps;
		bool hasCliDelay = false;
		bool hasCliBw = false;
		if (argc > i && isNum(argv[i]))
		{
			cliDelay = parseDouble(argv[i++], cliDelay);
			hasCliDelay = true;
		}
		if (argc > i && isNum(argv[i]))
		{
			cliBw = parseDouble(argv[i++], cliBw);
			hasCliBw = true;
		}
		if (argc > i && argv[i][0] != '-')
		{
			args.net.file = argv[i++];
		}

		loadNetFile(args.net);
		if (hasCliDelay)
		{
			args.net.delayMs = cliDelay;
		}
		if (hasCliBw)
		{
			args.net.bwMBps = cliBw;
		}

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
			else if (argEq(argv[i], "--single-thread"))
			{
				args.rb.multiThread = false;
				args.rb.workerThreads = 1;
			}
			else if (argEq(argv[i], "--multi-thread"))
			{
				args.rb.multiThread = true;
			}
			else
			{
				throw std::runtime_error(std::string("unknown arg: ") + argv[i]);
			}
			++i;
		}

		(void)RbOkvsResolve(args.setSize, args.rb);

		if (args.out.empty())
		{
			args.out = defaultOut(args);
		}
		else if (args.out.find('/') == std::string::npos && args.out.find('\\') == std::string::npos)
		{
			args.out = "build/benchmarks/rbokvs-pqpsi/" + args.out;
		}

		return args;
	}

	RunRes runOne(const Args& args, u64 benchIdx)
	{
		const u64 maxRetry = 32;
		const u64 minPort = 20000;
		const u64 maxPort = 65000;
		const u64 portSpan = maxPort - minPort;
		const u64 portStride = 64;
		const u64 retryStride = 1;
		RunRes res{};
		const u64 seed = (args.portSeed >= minPort && args.portSeed < maxPort)
			? args.portSeed
			: minPort + (args.portSeed % portSpan);

		for (u64 tryIdx = 0; tryIdx <= maxRetry; ++tryIdx)
		{
			const u64 portBase = minPort + ((seed - minPort + benchIdx * portStride + tryIdx * retryStride) % portSpan);
			const u64 endpointPort = portBase + 1;
			if (!canBindLoopbackPort(static_cast<u32>(endpointPort)))
			{
				++res.portRetry;
				continue;
			}

			setEnv("PQPSI_PORT_BASE", std::to_string(portBase));
			setEnv("PQPSI_RUN_TAG", std::to_string(benchIdx) + "_" + std::to_string(tryIdx) + "_" + std::to_string(portBase));
			setEnv("PQPSI_TRACE", "0");
			setEnv("PQPSI_SIM_NET_DELAY_MS", std::to_string(args.net.delayMs));
			setEnv("PQPSI_SIM_NET_BW_MBPS", std::to_string(args.net.bwMBps));

			try
			{
				const auto t0 = std::chrono::steady_clock::now();
				res.ok = rbRun(args.setSize, res.got, res.want, &args.rb, &res.prof, std::numeric_limits<u64>::max(), &args.pi, &args.kem);
				const auto t1 = std::chrono::steady_clock::now();
				res.runUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
				return res;
			}
			catch (const std::runtime_error& e)
			{
				const std::string msg = e.what();
				if (msg.find("Address already in use") != std::string::npos ||
					msg.find("BtEndpoint connect timeout") != std::string::npos)
				{
					++res.portRetry;
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					continue;
				}
				throw;
			}
		}

		throw std::runtime_error("pqpsi_rbokvs_bench ports busy after retries");
	}

	void writeParty(
		std::ofstream& out,
		const char* name,
		const Stats& protocol,
		const Stats& prep,
		const Stats& key,
		const Stats& mask,
		const Stats& pi,
		const Stats& okEnc,
		const Stats& okDec,
		const Stats& send,
		const Stats& recv,
		const Stats& kem,
		const Stats& inv)
	{
		const double known = prep.mean
			+ key.mean
			+ mask.mean
			+ pi.mean
			+ inv.mean
			+ kem.mean
			+ okEnc.mean
			+ okDec.mean
			+ send.mean
			+ recv.mean;
		const double other = protocol.mean > known ? protocol.mean - known : 0.0;

		out << "### " << name << " (mean protocol " << protocol.mean << " ms)\n";
		out << "| stage | mean_ms | pct_protocol |\n";
		out << "|---|---:|---:|\n";
		out << "| prep_alloc | " << prep.mean << " | " << pct(prep.mean, protocol.mean) << "% |\n";
		out << "| keygen | " << key.mean << " | " << pct(key.mean, protocol.mean) << "% |\n";
		out << "| mask_precompute | " << mask.mean << " | " << pct(mask.mean, protocol.mean) << "% |\n";
		out << "| permute | " << pi.mean << " | " << pct(pi.mean, protocol.mean) << "% |\n";
		out << "| permute_inverse | " << inv.mean << " | " << pct(inv.mean, protocol.mean) << "% |\n";
		out << "| kem_ops | " << kem.mean << " | " << pct(kem.mean, protocol.mean) << "% |\n";
		out << "| okvs_encode | " << okEnc.mean << " | " << pct(okEnc.mean, protocol.mean) << "% |\n";
		out << "| okvs_decode | " << okDec.mean << " | " << pct(okDec.mean, protocol.mean) << "% |\n";
		out << "| network_send | " << send.mean << " | " << pct(send.mean, protocol.mean) << "% |\n";
		out << "| network_recv | " << recv.mean << " | " << pct(recv.mean, protocol.mean) << "% |\n";
		out << "| other | " << other << " | " << pct(other, protocol.mean) << "% |\n";
		out << "\n";
	}

	void writeReport(const Args& args, const Series& series, const Counts& cnt)
	{
		ensureDir(parentDir(args.out));

		std::ofstream out(args.out, std::ios::out | std::ios::trunc);
		if (!out)
		{
			throw std::runtime_error("failed to open output file: " + args.out);
		}

		const auto p0ProtocolTimes = protocolTimes(series.p0);
		const auto p1ProtocolTimes = protocolTimes(series.p1);
		std::vector<double> protocolRoundMs;
		protocolRoundMs.reserve(series.runUs.size());
		for (size_t i = 0; i < series.runUs.size(); ++i)
		{
			protocolRoundMs.push_back(std::max(p0ProtocolTimes[i], p1ProtocolTimes[i]));
		}

		const auto protocol = summarize(protocolRoundMs);
		const auto p0Protocol = summarize(p0ProtocolTimes);
		const auto p0Prep = summarize(series.p0.prepMs);
		const auto p0Key = summarize(series.p0.keyMs);
		const auto p0Mask = summarize(series.p0.maskMs);
		const auto p0Pi = summarize(series.p0.piMs);
		const auto p0OkEnc = summarize(series.p0.okEncMs);
		const auto p0OkDec = summarize(series.p0.okDecMs);
		const auto p0Send = summarize(series.p0.sendMs);
		const auto p0Recv = summarize(series.p0.recvMs);
		const auto p0Kem = summarize(series.p0.kemMs);
		const auto p0Inv = summarize(series.p0.invMs);
		const auto p0Tx = summarize(series.p0.txBytes);
		const auto p0Rx = summarize(series.p0.rxBytes);
		const auto p1Protocol = summarize(p1ProtocolTimes);
		const auto p1Prep = summarize(series.p1.prepMs);
		const auto p1Key = summarize(series.p1.keyMs);
		const auto p1Mask = summarize(series.p1.maskMs);
		const auto p1Pi = summarize(series.p1.piMs);
		const auto p1OkEnc = summarize(series.p1.okEncMs);
		const auto p1OkDec = summarize(series.p1.okDecMs);
		const auto p1Send = summarize(series.p1.sendMs);
		const auto p1Recv = summarize(series.p1.recvMs);
		const auto p1Kem = summarize(series.p1.kemMs);
		const auto p1Inv = summarize(series.p1.invMs);
		const auto p1Tx = summarize(series.p1.txBytes);
		const auto p1Rx = summarize(series.p1.rxBytes);
		const double totalCommBytes = p0Tx.mean + p1Tx.mean;
		std::vector<double> phase1AliceMs;
		std::vector<double> phase2BobMs;
		std::vector<double> phase3AliceMs;
		std::vector<double> netDAMs;
		std::vector<double> netDBMs;
		std::vector<double> modelMs;
		std::vector<double> modelGapMs;
		phase1AliceMs.reserve(series.runUs.size());
		phase2BobMs.reserve(series.runUs.size());
		phase3AliceMs.reserve(series.runUs.size());
		netDAMs.reserve(series.runUs.size());
		netDBMs.reserve(series.runUs.size());
		modelMs.reserve(series.runUs.size());
		modelGapMs.reserve(series.runUs.size());
		for (size_t i = 0; i < series.runUs.size(); ++i)
		{
			const double a1 = series.p0.prepMs[i]
				+ series.p0.keyMs[i]
				+ series.p0.maskMs[i]
				+ series.p0.piMs[i]
				+ series.p0.okEncMs[i];
			const double b2 = series.p1.prepMs[i]
				+ series.p1.maskMs[i]
				+ series.p1.okDecMs[i]
				+ series.p1.invMs[i]
				+ series.p1.kemMs[i]
				+ series.p1.piMs[i]
				+ series.p1.okEncMs[i];
			const double a3 = series.p0.okDecMs[i]
				+ series.p0.invMs[i]
				+ series.p0.kemMs[i];
			const double da = pqpsiMsgMs(series.p0.txBytes[i], args.net.delayMs, args.net.bwMBps);
			const double db = pqpsiMsgMs(series.p1.txBytes[i], args.net.delayMs, args.net.bwMBps);
			const double model = a1 + da + b2 + db + a3;
			const double wall = std::max(p0ProtocolTimes[i], p1ProtocolTimes[i]);
			phase1AliceMs.push_back(a1);
			phase2BobMs.push_back(b2);
			phase3AliceMs.push_back(a3);
			netDAMs.push_back(da);
			netDBMs.push_back(db);
			modelMs.push_back(model);
			modelGapMs.push_back(wall - model);
		}
		const auto phase1Alice = summarize(phase1AliceMs);
		const auto phase2Bob = summarize(phase2BobMs);
		const auto phase3Alice = summarize(phase3AliceMs);
		const auto netDA = summarize(netDAMs);
		const auto netDB = summarize(netDBMs);
		const auto model = summarize(modelMs);
		const auto modelGap = summarize(modelGapMs);
		const bool ok = (cnt.fail == 0 && cnt.mismatch == 0);
		const auto rb = RbOkvsResolve(args.setSize, args.rb);
		auto perm = makePi(args.kem.kind == PsiKemKind::EcKem ? PiCfg{ pqperm::Kind::Xoodoo } : args.pi);

		out << "# PQPSI RB-OKVS Benchmark\n\n";
		out << std::fixed << std::setprecision(2);

		out << "## Summary\n";
		out << "| item | value |\n";
		out << "|---|---:|\n";
		out << "| set_size_per_party | " << args.setSize << " |\n";
		out << "| measured_rounds | " << args.rounds << " |\n";
		out << "| rb_lambda | " << rb.lambda << " |\n";
		out << "| rb_lambda_real | " << rb.lambdaReal << " |\n";
		out << "| thread_mode | " << (args.rb.multiThread ? "multi" : "single") << " |\n";
		out << "| party_threads | 2 |\n";
		out << "| worker_threads | " << rb.workerThreads << " |\n";
		out << "| worker_threads_requested | " << (args.rb.workerThreads == 0 ? "auto" : std::to_string(args.rb.workerThreads)) << " |\n";
		out << "| rb_eps | " << rb.eps << " |\n";
		out << "| rb_m | " << rb.columns << " |\n";
		out << "| rb_w | " << rb.bandWidth << " |\n";
		out << "| kem | " << name(args.kem.kind) << " |\n";
		out << "| kem_row_bytes | " << kemRowBytes(args.kem) << " |\n";
		out << "| pi | " << perm->name() << " |\n";
		out << "| pi_detail | " << perm->detail() << " |\n";
		out << "| pi_n | " << perm->n() << " |\n";
		out << "| pi_s | " << perm->s() << " |\n";
		out << "| pi_lambda | " << args.pi.lambda << " |\n";
		out << "| pi_rounds | " << perm->rounds() << " |\n";
		out << "| bob_pi | " << (args.pi.bobPi ? "on" : "off") << " |\n";
		out << "| sim_net_delay_ms | " << args.net.delayMs << " |\n";
		out << "| sim_net_bw_MBps | " << args.net.bwMBps << " |\n";
		out << "| network_emulator | " << args.net.emu << " |\n";
		out << "| protocol_runtime_mean_ms | " << protocol.mean << " |\n";
		out << "| protocol_model_mean_ms | " << model.mean << " |\n";
		out << "| wall_minus_model_mean_ms | " << modelGap.mean << " |\n";
		out << "| protocol_runtime_median_ms | " << protocol.median << " |\n";
		out << "| protocol_runtime_p95_ms | " << protocol.p95 << " |\n";
		out << "| protocol_runtime_p99_ms | " << protocol.p99 << " |\n";
		out << "| protocol_runtime_min_ms | " << protocol.min << " |\n";
		out << "| protocol_runtime_max_ms | " << protocol.max << " |\n";
		out << "| party0_protocol_mean_ms | " << p0Protocol.mean << " |\n";
		out << "| party1_protocol_mean_ms | " << p1Protocol.mean << " |\n";
		out << "| total_communication_kb_mean | " << (totalCommBytes / 1024.0) << " |\n";
		out << "| overall_status | " << (ok ? "ok" : "needs_check") << " |\n";
		out << "\n";

		out << "## Communication\n";
		out << "| party | tx_kb_mean | rx_kb_mean | tx+rx_kb_mean |\n";
		out << "|---|---:|---:|---:|\n";
		out << "| party0 | " << (p0Tx.mean / 1024.0) << " | " << (p0Rx.mean / 1024.0) << " | " << ((p0Tx.mean + p0Rx.mean) / 1024.0) << " |\n";
		out << "| party1 | " << (p1Tx.mean / 1024.0) << " | " << (p1Rx.mean / 1024.0) << " | " << ((p1Tx.mean + p1Rx.mean) / 1024.0) << " |\n";
		out << "| total_over_wire | " << (totalCommBytes / 1024.0) << " | - | " << (totalCommBytes / 1024.0) << " |\n";
		out << "\n";

		out << "## Protocol Model\n";
		out << "This model follows the critical path: Alice phase 1, network transfer of `D_A`, Bob phase 2, network transfer of `D_B`, Alice phase 3. It intentionally excludes setup/teardown and does not add both parties' local clocks together.\n\n";
		out << "| step | mean_ms |\n";
		out << "|---|---:|\n";
		out << "| phase1_alice_ms | " << phase1Alice.mean << " |\n";
		out << "| net_DA_ms | " << netDA.mean << " |\n";
		out << "| phase2_bob_ms | " << phase2Bob.mean << " |\n";
		out << "| net_DB_ms | " << netDB.mean << " |\n";
		out << "| phase3_alice_ms | " << phase3Alice.mean << " |\n";
		out << "| protocol_model_ms | " << model.mean << " |\n";
		out << "| wall_protocol_ms | " << protocol.mean << " |\n";
		out << "| wall_minus_model_ms | " << modelGap.mean << " |\n";
		out << "\n";

		out << "```text\n";
		out << "Alice / party0                         Bob / party1\n";
		out << "phase1: keygen+mask+pi+enc  " << phase1Alice.mean << "\n";
		out << "send D_A net                 " << netDA.mean << "  -> recv D_A\n";
		out << "                                      phase2: dec+kem+pi+enc  " << phase2Bob.mean << "\n";
		out << "recv D_B                         <- send D_B net              " << netDB.mean << "\n";
		out << "phase3: dec+inv+decap       " << phase3Alice.mean << "\n";
		out << "model_total                 " << model.mean << "\n";
		out << "wall_protocol               " << protocol.mean << "\n";
		out << "wall_minus_model            " << modelGap.mean << "\n";
		out << "```\n\n";

		out << "## Party Results\n";
		writeParty(out, "party0", p0Protocol, p0Prep, p0Key, p0Mask, p0Pi, p0OkEnc, p0OkDec, p0Send, p0Recv, p0Kem, p0Inv);
		writeParty(out, "party1", p1Protocol, p1Prep, p1Key, p1Mask, p1Pi, p1OkEnc, p1OkDec, p1Send, p1Recv, p1Kem, p1Inv);

		out << "## Protocol Distribution\n";
		out << "| metric | ms |\n";
		out << "|---|---:|\n";
		out << "| protocol_runtime_mean_ms | " << protocol.mean << " |\n";
		out << "| protocol_runtime_median_ms | " << protocol.median << " |\n";
		out << "| protocol_runtime_p95_ms | " << protocol.p95 << " |\n";
		out << "| protocol_runtime_p99_ms | " << protocol.p99 << " |\n";
		out << "| protocol_runtime_min_ms | " << protocol.min << " |\n";
		out << "| protocol_runtime_max_ms | " << protocol.max << " |\n";
		out << "\n";

		out << "## Per-Round Results\n";
		out << "| round | protocol_runtime_ms | protocol_model_ms | wall_minus_model_ms | party0_protocol_ms | party1_protocol_ms | total_communication_kb |\n";
		out << "|---:|---:|---:|---:|---:|---:|---:|\n";
		for (size_t i = 0; i < series.runUs.size(); ++i)
		{
			const double p0Prot = p0ProtocolTimes[i];
			const double p1Prot = p1ProtocolTimes[i];
			out << "| " << (i + 1)
				<< " | " << std::max(p0Prot, p1Prot)
				<< " | " << modelMs[i]
				<< " | " << modelGapMs[i]
				<< " | " << p0Prot
				<< " | " << p1Prot
				<< " | " << ((series.p0.txBytes[i] + series.p1.txBytes[i]) / 1024.0)
				<< " |\n";
		}
		out << "\n";

		out << "## Correctness\n";
		out << "| metric | value |\n";
		out << "|---|---:|\n";
		out << "| correctness_pass_rounds | " << cnt.pass << " |\n";
		out << "| correctness_fail_rounds | " << cnt.fail << " |\n";
		out << "| intersection_mismatch_rounds | " << cnt.mismatch << " |\n";
		out << "| overall_status | " << (ok ? "ok" : "needs_check") << " |\n";
		out << "\n";

		out << "## Details\n";
		out << "| item | value |\n";
		out << "|---|---|\n";
		out << "| timestamp | " << nowIsoLike() << " |\n";
		out << "| host_uname | " << unameLine() << " |\n";
		out << "| cpu_model | " << cpuModel() << " |\n";
		out << "| hw_threads | " << std::thread::hardware_concurrency() << " |\n";
		out << "| compiler | " << __VERSION__ << " |\n";
#if defined(NDEBUG)
		out << "| build_mode | release |\n";
#else
		out << "| build_mode | debug |\n";
#endif
		out << "| parties | 2 |\n";
		out << "| kem_mode | " << name(args.kem.kind) << " |\n";
		out << "| okvs_type | RB |\n";
		out << "| protocol_variant | pqpsi default |\n";
		out << "| rb_lambda | " << rb.lambda << " |\n";
		out << "| rb_lambda_real | " << rb.lambdaReal << " |\n";
		out << "| thread_mode | " << (args.rb.multiThread ? "multi" : "single") << " |\n";
		out << "| party_threads | 2 |\n";
		out << "| worker_threads | " << rb.workerThreads << " |\n";
		out << "| worker_threads_requested | " << (args.rb.workerThreads == 0 ? "auto" : std::to_string(args.rb.workerThreads)) << " |\n";
		out << "| rb_eps | " << rb.eps << " |\n";
		out << "| rb_columns | " << rb.columns << " |\n";
		out << "| rb_band_width | " << rb.bandWidth << " |\n";
		out << "| rb_rate | " << (static_cast<double>(args.setSize) / static_cast<double>(rb.columns)) << " |\n";
		out << "| rb_explicit_cols | " << (args.rb.columns ? "yes" : "no") << " |\n";
		out << "| rb_explicit_w | " << (args.rb.bandWidth ? "yes" : "no") << " |\n";
		out << "| transport | BtEndpoint over local loopback |\n";
		out << "| host | localhost |\n";
		out << "| network_type | " << args.net.type << " |\n";
		out << "| network_topology | " << args.net.topology << " |\n";
		out << "| link_speed | " << args.net.link << " |\n";
		out << "| network_note | " << args.net.note << " |\n";
		out << "| network_emulator | " << args.net.emu << " |\n";
		out << "| network_settings_file | " << (args.net.file.empty() ? "none" : args.net.file) << " |\n";
		out << "| port_base_seed | " << args.portSeed << " |\n";
		out << "| per_round_port_step | 64 |\n";
		out << "| port_retry_step | 101 |\n";
		out << "| port_retry_total | " << cnt.portRetry << " |\n";
		out << "| warmup_rounds | " << args.warmups << " |\n";
		out << "| measured_rounds | " << args.rounds << " |\n";
		out << "| timing_unit_primary | ms |\n";
		out << "| trace_output | disabled |\n";
	}
}

int main(int argc, char** argv)
{
	if (argc > 1)
	{
		const std::string arg1 = argv[1];
		if (arg1 == "-h" || arg1 == "--help" || arg1 == "help")
		{
			printUsage(argv[0]);
			return 0;
		}
	}

	try
	{
		const Args args = parseArgs(argc, argv);
		Series series;
		series.reserve(args.rounds);
		Counts cnt;

		for (u64 i = 0; i < args.warmups + args.rounds; ++i)
		{
			RunRes res = runOne(args, i);
			cnt.portRetry += res.portRetry;

			if (i < args.warmups)
			{
				continue;
			}

			series.add(res.runUs, res.prof);
			if (res.ok)
			{
				++cnt.pass;
			}
			else
			{
				++cnt.fail;
			}
			if (res.got != res.want)
			{
				++cnt.mismatch;
			}
		}

		writeReport(args, series, cnt);
		std::cout << "wrote benchmark report: " << args.out << "\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << "\n";
		return 1;
	}
}
