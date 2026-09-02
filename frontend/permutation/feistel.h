#pragma once

#include "api.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

extern "C" int crypto_hash_shake256(unsigned char* h, unsigned long long d, const unsigned char* m, unsigned long long n);

namespace pqperm
{
	class Feistel final : public Perm
	{
	public:
		static constexpr size_t DefaultRounds = 12;

		explicit Feistel(size_t rounds = DefaultRounds, u8 party = 0)
			: rounds_(rounds)
			, party_(static_cast<u8>(party & 1U))
		{
			if (rounds_ == 0)
			{
				throw std::invalid_argument("feistel needs at least one round");
			}
		}

		const char* name() const override
		{
			return "feistel";
		}

		const char* detail() const override
		{
			return "shake256-feistel";
		}

		size_t n() const override
		{
			return KEM_key_size_bit;
		}

		size_t s() const override
		{
			return HalfBytes * 8;
		}

		size_t rounds() const override
		{
			return rounds_;
		}

		void encryptBytes(u8* data, size_t bytes) const override
		{
			check(bytes);
			std::array<u8, HalfBytes> next{};
			u8* prev = data;
			u8* curr = data + HalfBytes;
			for (size_t r = 0; r < rounds_; ++r)
			{
				roundMask(curr, next.data(), r);
				xorInto(prev, next.data());
				std::memcpy(prev, curr, HalfBytes);
				std::memcpy(curr, next.data(), HalfBytes);
			}
		}

		void decryptBytes(u8* data, size_t bytes) const override
		{
			check(bytes);
			std::array<u8, HalfBytes> prev{};
			u8* curr = data;
			u8* next = data + HalfBytes;
			for (size_t r = rounds_; r > 0; --r)
			{
				roundMask(curr, prev.data(), r - 1);
				xorInto(next, prev.data());
				std::memcpy(next, curr, HalfBytes);
				std::memcpy(curr, prev.data(), HalfBytes);
			}
		}

	private:
		static constexpr size_t Bytes = KEM_key_size_bit / 8;
		static constexpr size_t HalfBytes = Bytes / 2;

		size_t rounds_ = DefaultRounds;
		u8 party_ = 0;

		void check(size_t bytes) const
		{
			if (bytes != Bytes)
			{
				throw std::invalid_argument("feistel expects one full PQ row");
			}
		}

		static void xorInto(const u8* in, u8* out)
		{
			for (size_t i = 0; i < HalfBytes; ++i)
			{
				out[i] ^= in[i];
			}
		}

		void roundMask(const u8* curr, u8* out, size_t r) const
		{
			// label party round curr

			std::array<u8, 17 + HalfBytes> input{};
			const char label[8] = { 'P', 'Q', 'F', 'S', 'T', 'L', '1', 0 };
			std::memcpy(input.data(), label, sizeof(label));
			input[8] = party_;
			store64(input.data() + 9, static_cast<u64>(r));
			std::memcpy(input.data() + 17, curr, HalfBytes);
			if (crypto_hash_shake256(out, HalfBytes, input.data(), input.size()) != 0)
			{
				throw std::runtime_error("SHAKE256 failed");
			}
		}

		static void store64(u8* out, u64 x)
		{
			for (size_t i = 0; i < 8; ++i)
			{
				out[i] = static_cast<u8>(x >> (8 * i));
			}
		}
	};
}
