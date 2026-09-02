#pragma once

#include "cons.h"
#include "feistel.h"
#include "hctr2.h"
#include "xoodoo.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>

namespace pqperm
{
	enum class Kind
	{
		ConsPi,
		Hctr,
		Feistel,
		Xoodoo,
	};

	struct Cfg
	{
		Kind kind = Kind::ConsPi;
		pi::Kind small = pi::Kind::Keccak1600;
		size_t lambda = 128;
		size_t rounds = Feistel::DefaultRounds;
	};

	inline const char* name(Kind kind)
	{
		switch (kind)
		{
		case Kind::ConsPi:
			return "conspi";
		case Kind::Hctr:
			return "hctr";
		case Kind::Feistel:
			return "feistel";
		case Kind::Xoodoo:
			return "xoodoo";
		default:
			return "unknown";
		}
	}

	inline void set(Cfg& cfg, const std::string& text)
	{
		if (text == "conspi" || text == "cons-pi" || text == "pi")
		{
			cfg.kind = Kind::ConsPi;
			return;
		}
		if (text == "hctr" || text == "hctr2")
		{
			cfg.kind = Kind::Hctr;
			return;
		}
		if (text == "feistel")
		{
			cfg.kind = Kind::Feistel;
			return;
		}
		const std::string prefix = "feistel-";
		if (text.compare(0, prefix.size(), prefix) == 0)
		{
			const auto tail = text.substr(prefix.size());
			if (tail.empty() || !std::all_of(tail.begin(), tail.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
			{
				throw std::invalid_argument("bad feistel round count: " + text);
			}
			cfg.kind = Kind::Feistel;
			cfg.rounds = static_cast<size_t>(std::stoull(tail));
			return;
		}
		if (text == "xoodoo")
		{
			cfg.kind = Kind::Xoodoo;
			return;
		}

		cfg.kind = Kind::ConsPi;
		cfg.small = pi::parseKind(text);
	}

	inline std::unique_ptr<Perm> make(const Cfg& cfg, u8 party = 0)
	{
		switch (cfg.kind)
		{
		case Kind::ConsPi:
			return std::unique_ptr<Perm>(new ConsPi(pi::defaults(cfg.small, KEM_key_size_bit, cfg.lambda), party));
		case Kind::Hctr:
			return std::unique_ptr<Perm>(new Hctr(party));
		case Kind::Feistel:
			return std::unique_ptr<Perm>(new Feistel(cfg.rounds, party));
		case Kind::Xoodoo:
			(void)party;
			return std::unique_ptr<Perm>(new Xoodoo());
		default:
			throw std::invalid_argument("unknown PQ-PSI permutation");
		}
	}
}
