#include "pqpsi/pqpsi.h"
#include "pqpsi/model.h"

#include "Common/Defines.h"
#include "Crypto/PRNG.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace osuCrypto;

namespace
{
	struct Args
	{
		u64 party = 0;
		u64 setSize = 128;
		u64 portBase = 43000;
		u64 hits = std::numeric_limits<u64>::max();
		u64 channels = 0;
		std::string tag = "loopback";
		RbCfg rb{};
		PiCfg pi{};
		KemCfg kem{};
	};

	bool sameBlock(const block& a, const block& b)
	{
		return std::memcmp(&a, &b, sizeof(block)) == 0;
	}

	bool hasBlock(const std::vector<block>& set, const block& x)
	{
		for (const auto& v : set)
		{
			if (sameBlock(v, x))
			{
				return true;
			}
		}
		return false;
	}

	block freshBlock(PRNG& prng, const std::vector<block>& a, const std::vector<block>& b)
	{
		for (;;)
		{
			block x = prng.get<block>();
			if (!hasBlock(a, x) && !hasBlock(b, x))
			{
				return x;
			}
		}
	}

	void makeSets(u64 n, u64 hits, std::vector<block>& party0, std::vector<block>& party1)
	{
		PRNG prng(_mm_set_epi32(4253465, 3434565, 234435, 23987045));
		party0.resize(n);
		party1.resize(n);
		for (u64 i = 0; i < n; ++i)
		{
			party1[i] = prng.get<block>();
			party0[i] = party1[i];
		}

		const u64 misses = (hits > n) ? 0 : (n - hits);
		for (u64 i = 0; i < misses; ++i)
		{
			party0[i] = freshBlock(prng, party0, party1);
		}
	}

	u64 parseU64(const std::string& s, u64 fallback)
	{
		try
		{
			return static_cast<u64>(std::stoull(s));
		}
		catch (...)
		{
			return fallback;
		}
	}

	double parseDouble(const std::string& s, double fallback)
	{
		try
		{
			return std::stod(s);
		}
		catch (...)
		{
			return fallback;
		}
	}

	void setEnv(const char* key, const std::string& val)
	{
#if defined(_WIN32)
		_putenv_s(key, val.c_str());
#else
		setenv(key, val.c_str(), 1);
#endif
	}

	bool argEq(const std::string& a, const char* b)
	{
		return a == std::string(b);
	}

	void usage(const char* prog)
	{
		std::cout
			<< "Usage\n"
			<< "  " << prog << " <party:0|1> <setSize> <portBase> [flags]\n\n"
			<< "Flags\n"
			<< "  --hits <n>          intersection size, default setSize - 1\n"
			<< "  --tag <text>        run tag shared by both party processes\n"
			<< "  --rb-eps <x>        RB-OKVS epsilon\n"
			<< "  --rb-cols <m>       RB-OKVS table columns\n"
			<< "  --rb-w <w>          RB-OKVS band width\n"
			<< "  --rb-lambda <l>     RB-OKVS lambda, default 40\n"
			<< "  --threads <n>       worker threads per party\n"
			<< "  --channels <n>      network channels, default worker threads, capped by workers\n"
			<< "  --single-thread     force one worker thread per party\n"
			<< "  --multi-thread      enable worker threads per party\n"
			<< "  --pi <name>         hctr, feistel, xoodoo, keccak1600, keccak1600-12, sneik-f512, ...\n"
			<< "  --pi-lambda <l>     ConsPi lambda, default 128\n"
			<< "  --pi-rounds <n>     Feistel rounds, default 8\n"
			<< "  --no-bob-pi         skip permutation for Bob/party1 OKVS2\n"
			<< "  --kem <name>        obf-mlkem or eckem\n";
	}

	Args parse(int argc, char** argv)
	{
		if (argc < 4)
		{
			throw std::runtime_error("missing required args");
		}

		Args args;
		args.party = parseU64(argv[1], 0);
		args.setSize = parseU64(argv[2], 128);
		args.portBase = parseU64(argv[3], 43000);
		args.hits = args.setSize == 0 ? 0 : args.setSize - 1;

		for (int i = 4; i < argc; ++i)
		{
			const std::string a(argv[i]);
			auto need = [&](const char* name) -> std::string
			{
				if (i + 1 >= argc)
				{
					throw std::runtime_error(std::string("missing value for ") + name);
				}
				return std::string(argv[++i]);
			};

			if (argEq(a, "--hits"))
			{
				args.hits = parseU64(need("--hits"), args.hits);
			}
			else if (argEq(a, "--tag"))
			{
				args.tag = need("--tag");
			}
			else if (argEq(a, "--rb-lambda"))
			{
				args.rb.lambda = parseU64(need("--rb-lambda"), args.rb.lambda);
			}
			else if (argEq(a, "--rb-eps"))
			{
				args.rb.eps = parseDouble(need("--rb-eps"), args.rb.eps);
			}
			else if (argEq(a, "--rb-cols"))
			{
				args.rb.columns = parseU64(need("--rb-cols"), args.rb.columns);
			}
			else if (argEq(a, "--rb-w"))
			{
				args.rb.bandWidth = parseU64(need("--rb-w"), args.rb.bandWidth);
			}
			else if (argEq(a, "--threads") || argEq(a, "--worker-threads"))
			{
				args.rb.workerThreads = parseU64(need("--threads"), args.rb.workerThreads);
				args.rb.multiThread = args.rb.workerThreads != 1;
			}
			else if (argEq(a, "--channels") || argEq(a, "--net-channels"))
			{
				args.channels = parseU64(need("--channels"), args.channels);
			}
			else if (argEq(a, "--single-thread"))
			{
				args.rb.multiThread = false;
				args.rb.workerThreads = 1;
			}
			else if (argEq(a, "--multi-thread"))
			{
				args.rb.multiThread = true;
			}
			else if (argEq(a, "--pi"))
			{
				setPi(args.pi, need("--pi"));
			}
			else if (argEq(a, "--pi-lambda"))
			{
				args.pi.lambda = parseU64(need("--pi-lambda"), args.pi.lambda);
			}
			else if (argEq(a, "--pi-rounds"))
			{
				args.pi.rounds = parseU64(need("--pi-rounds"), args.pi.rounds);
			}
			else if (argEq(a, "--no-bob-pi") || argEq(a, "--bob-no-pi"))
			{
				args.pi.bobPi = false;
			}
			else if (argEq(a, "--bob-pi"))
			{
				args.pi.bobPi = true;
			}
			else if (argEq(a, "--kem"))
			{
				setKem(args.kem, need("--kem"));
			}
			else
			{
				throw std::runtime_error("unknown arg: " + a);
			}
		}

		if (args.party > 1)
		{
			throw std::runtime_error("party must be 0 or 1");
		}
		if (args.hits > args.setSize)
		{
			args.hits = args.setSize;
		}
		(void)RbOkvsResolve(args.setSize, args.rb);
		return args;
	}

	u64 asU64(double v)
	{
		return static_cast<u64>(std::llround(v));
	}

	void printResult(const Args& args, const PqPsiStageMs& ms, u64 got)
	{
		const auto rb = RbOkvsResolve(args.setSize, args.rb);
		auto perm = makePi(args.kem.kind == PsiKemKind::EcKem ? PiCfg{ pqperm::Kind::Xoodoo } : args.pi);
		const u64 channels = args.rb.multiThread
			? std::max<u64>(1, std::min<u64>(args.channels == 0 ? rb.workerThreads : args.channels, rb.workerThreads))
			: 1;
		const bool match = args.party == 0 ? (got == args.hits) : true;

		std::cout << std::fixed;
		std::cout << "result_status " << (match ? "ok" : "mismatch") << "\n";
		std::cout << "result_party " << args.party << "\n";
		std::cout << "result_role " << (args.party == 0 ? "receiver" : "sender") << "\n";
		std::cout << "result_time_ms " << pqpsiProtocolMs(ms) << "\n";
		std::cout << "result_total_ms " << ms.totalMs << "\n";
		std::cout << "result_setup_ms " << ms.setupMs << "\n";
		std::cout << "result_teardown_ms " << ms.teardownMs << "\n";
		std::cout << "result_prep_ms " << ms.prepMs << "\n";
		std::cout << "result_keygen_ms " << ms.kemKeyGenMs << "\n";
		std::cout << "result_mask_ms " << ms.maskMs << "\n";
		std::cout << "result_permute_ms " << ms.permuteMs << "\n";
		std::cout << "result_perm_inv_ms " << ms.permDecryptMs << "\n";
		std::cout << "result_kem_ms " << ms.kemCoreMs << "\n";
		std::cout << "result_okvs_encode_ms " << ms.okvsEncodeMs << "\n";
		std::cout << "result_okvs_decode_ms " << ms.okvsDecodeMs << "\n";
		std::cout << "result_network_send_ms " << ms.networkSendMs << "\n";
		std::cout << "result_network_recv_ms " << ms.networkRecvMs << "\n";
		std::cout << "result_phase1_alice_ms " << (args.party == 0 ? pqpsiAlice1Ms(ms) : 0.0) << "\n";
		std::cout << "result_phase2_bob_ms " << (args.party == 1 ? pqpsiBob2Ms(ms) : 0.0) << "\n";
		std::cout << "result_phase3_alice_ms " << (args.party == 0 ? pqpsiAlice3Ms(ms) : 0.0) << "\n";
		std::cout << "result_local_compute_ms " << pqpsiLocalMs(ms) << "\n";
		std::cout << "result_bytes_sent " << asU64(ms.networkSendBytes) << "\n";
		std::cout << "result_bytes_recv " << asU64(ms.networkRecvBytes) << "\n";
		std::cout << "result_hits " << got << "\n";
		std::cout << "result_expected_hits " << args.hits << "\n";
		std::cout << "result_set_size " << args.setSize << "\n";
		std::cout << "result_rb_m " << rb.columns << "\n";
		std::cout << "result_rb_w " << rb.bandWidth << "\n";
		std::cout << "result_rb_eps " << rb.eps << "\n";
		std::cout << "result_rb_lambda " << rb.lambda << "\n";
		std::cout << "result_rb_lambda_real " << rb.lambdaReal << "\n";
		std::cout << "result_worker_threads " << rb.workerThreads << "\n";
		std::cout << "result_net_channels " << channels << "\n";
		std::cout << "result_thread_mode " << (args.rb.multiThread ? "multi" : "single") << "\n";
		std::cout << "result_kem " << name(args.kem.kind) << "\n";
		std::cout << "result_kem_row_bytes " << kemRowBytes(args.kem) << "\n";
		std::cout << "result_pi " << perm->name() << "\n";
		std::cout << "result_pi_detail " << perm->detail() << "\n";
		std::cout << "result_pi_n " << perm->n() << "\n";
		std::cout << "result_pi_s " << perm->s() << "\n";
		std::cout << "result_pi_rounds " << perm->rounds() << "\n";
		std::cout << "result_bob_pi " << (args.pi.bobPi ? "on" : "off") << "\n";
	}
}

int main(int argc, char** argv)
{
	if (argc > 1)
	{
		const std::string a(argv[1]);
		if (a == "-h" || a == "--help" || a == "help")
		{
			usage(argv[0]);
			return 0;
		}
	}

	try
	{
		const Args args = parse(argc, argv);
		std::vector<block> set0;
		std::vector<block> set1;
		makeSets(args.setSize, args.hits, set0, set1);

		setEnv("PQPSI_PORT_BASE", std::to_string(args.portBase));
		setEnv("PQPSI_RUN_TAG", args.tag);
		setEnv("PQPSI_NET_CHANNELS", std::to_string(args.channels));
		setEnv("PQPSI_TRACE", "0");

		u64 got = 0;
		PqPsiStageMs ms{};
		if (args.party == 0)
		{
			pqpsi(0, args.setSize, set0, &args.rb, &got, &ms, &args.pi, &args.kem);
		}
		else
		{
			pqpsi(1, args.setSize, set1, &args.rb, nullptr, &ms, &args.pi, &args.kem);
		}

		printResult(args, ms, got);
		return (args.party == 0 && got != args.hits) ? 2 : 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "result_status error\n";
		std::cerr << "result_error " << e.what() << "\n";
		return 1;
	}
}
