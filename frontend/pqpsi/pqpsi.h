#pragma once

#include "Crypto/PRNG.h"
#include "Network/Channel.h"
#include "Network/BtEndpoint.h"
#include "okvs/rbokvs.h"
#include "permutation/permutation.h"
#include <limits>
#include <Common/ByteStream.h>
#include <fstream>
#include <util.h>

using namespace osuCrypto;

struct PqPsiStageMs
{
	double totalMs = 0.0;
	double setupMs = 0.0;
	double teardownMs = 0.0;
	double prepMs = 0.0;
	double kemKeyGenMs = 0.0;
	double maskMs = 0.0;
	double permuteMs = 0.0;
	double okvsEncodeMs = 0.0;
	double okvsDecodeMs = 0.0;
	double networkSendMs = 0.0;
	double networkRecvMs = 0.0;
	double networkSendBytes = 0.0;
	double networkRecvBytes = 0.0;
	double kemCoreMs = 0.0; // encaps or decaps
	double permDecryptMs = 0.0;
};

struct PqPsiRunProfile
{
	PqPsiStageMs party0;
	PqPsiStageMs party1;
};

using RbCfg = osuCrypto::RBParams;
using RbInfo = osuCrypto::RBResolved;

struct PiCfg
{
	pqperm::Kind kind = pqperm::Kind::ConsPi;
	pi::Kind small = pi::Kind::Keccak1600;
	size_t lambda = 128;
	size_t rounds = pqperm::Feistel::DefaultRounds;
	bool bobPi = false;
};

enum class PsiKemKind : u8
{
	ObfMlKem,
	EcKem
};

struct KemCfg
{
	PsiKemKind kind = PsiKemKind::ObfMlKem;
};

inline const char* name(PsiKemKind kind)
{
	switch (kind)
	{
	case PsiKemKind::ObfMlKem:
		return "obf-mlkem";
	case PsiKemKind::EcKem:
		return "eckem";
	default:
		return "unknown";
	}
}

inline void setKem(KemCfg& kem, const std::string& text)
{
	if (text == "obf-mlkem" || text == "mlkem" || text == "kemeleon")
	{
		kem.kind = PsiKemKind::ObfMlKem;
		return;
	}
	if (text == "eckem" || text == "ec-kem")
	{
		kem.kind = PsiKemKind::EcKem;
		return;
	}
	throw std::invalid_argument("unknown PQ-PSI KEM: " + text);
}

inline size_t kemRowBytes(const KemCfg& kem)
{
	switch (kem.kind)
	{
	case PsiKemKind::ObfMlKem:
		return KEM_key_size_bit / 8;
	case PsiKemKind::EcKem:
		return 48;
	default:
		throw std::invalid_argument("unknown PQ-PSI KEM row size");
	}
}

inline size_t kemRowBlocks(const KemCfg& kem)
{
	const size_t bytes = kemRowBytes(kem);
	if ((bytes % sizeof(block)) != 0)
	{
		throw std::invalid_argument("PQ-PSI KEM row must be block aligned");
	}
	return bytes / sizeof(block);
}

inline pqperm::Cfg toPermCfg(const PiCfg& pi)
{
	pqperm::Cfg cfg;
	cfg.kind = pi.kind;
	cfg.small = pi.small;
	cfg.lambda = pi.lambda;
	cfg.rounds = pi.rounds;
	return cfg;
}

inline void setPi(PiCfg& pi, const std::string& text)
{
	auto cfg = toPermCfg(pi);
	pqperm::set(cfg, text);
	pi.kind = cfg.kind;
	pi.small = cfg.small;
	pi.lambda = cfg.lambda;
	pi.rounds = cfg.rounds;
}

inline std::unique_ptr<pqperm::Perm> makePi(const PiCfg& pi, u8 party = 0)
{
	return pqperm::make(toPermCfg(pi), party);
}

void pqpsi(
	u64 me,
	u64 setSize,
	std::vector<block> set,
	const RbCfg* rb = nullptr,
	u64* hitOut = nullptr,
	PqPsiStageMs* msOut = nullptr,
	const PiCfg* pi = nullptr,
	const KemCfg* kem = nullptr);

void psiMain();
void rbMain();
bool rbCheck(u64& got, u64& want, const RbCfg* rb = nullptr);
bool rbRun(
	u64 setSize,
	u64& got,
	u64& want,
	const RbCfg* rb = nullptr,
	PqPsiRunProfile* out = nullptr,
	u64 hitTarget = std::numeric_limits<u64>::max(),
	const PiCfg* pi = nullptr,
	const KemCfg* kem = nullptr);
