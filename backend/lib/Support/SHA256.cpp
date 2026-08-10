// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Support/SHA256.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {
namespace {

constexpr std::array<std::uint32_t, 64> RoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

constexpr std::array<std::uint32_t, 8> InitialState{
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
};

[[nodiscard]] std::uint32_t loadBigEndian(const std::uint8_t *Input) noexcept {
  return (static_cast<std::uint32_t>(Input[0]) << 24U) |
         (static_cast<std::uint32_t>(Input[1]) << 16U) |
         (static_cast<std::uint32_t>(Input[2]) << 8U) | static_cast<std::uint32_t>(Input[3]);
}

void storeBigEndian(std::uint32_t Value, std::uint8_t *Output) noexcept {
  Output[0] = static_cast<std::uint8_t>(Value >> 24U);
  Output[1] = static_cast<std::uint8_t>(Value >> 16U);
  Output[2] = static_cast<std::uint8_t>(Value >> 8U);
  Output[3] = static_cast<std::uint8_t>(Value);
}

void compress(std::array<std::uint32_t, 8> &State, const std::uint8_t *Block) noexcept {
  std::array<std::uint32_t, 64> Words{};
  for (std::size_t Index = 0; Index < 16; ++Index)
    Words[Index] = loadBigEndian(Block + Index * 4U);
  for (std::size_t Index = 16; Index < Words.size(); ++Index) {
    const std::uint32_t S0 = std::rotr(Words[Index - 15], 7) ^ std::rotr(Words[Index - 15], 18) ^
                             (Words[Index - 15] >> 3U);
    const std::uint32_t S1 = std::rotr(Words[Index - 2], 17) ^ std::rotr(Words[Index - 2], 19) ^
                             (Words[Index - 2] >> 10U);
    Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
  }

  std::uint32_t A = State[0];
  std::uint32_t B = State[1];
  std::uint32_t C = State[2];
  std::uint32_t D = State[3];
  std::uint32_t E = State[4];
  std::uint32_t F = State[5];
  std::uint32_t G = State[6];
  std::uint32_t H = State[7];
  for (std::size_t Index = 0; Index < Words.size(); ++Index) {
    const std::uint32_t Sum1 = std::rotr(E, 6) ^ std::rotr(E, 11) ^ std::rotr(E, 25);
    const std::uint32_t Choice = (E & F) ^ (~E & G);
    const std::uint32_t Temporary1 = H + Sum1 + Choice + RoundConstants[Index] + Words[Index];
    const std::uint32_t Sum0 = std::rotr(A, 2) ^ std::rotr(A, 13) ^ std::rotr(A, 22);
    const std::uint32_t Majority = (A & B) ^ (A & C) ^ (B & C);
    const std::uint32_t Temporary2 = Sum0 + Majority;
    H = G;
    G = F;
    F = E;
    E = D + Temporary1;
    D = C;
    C = B;
    B = A;
    A = Temporary1 + Temporary2;
  }
  State[0] += A;
  State[1] += B;
  State[2] += C;
  State[3] += D;
  State[4] += E;
  State[5] += F;
  State[6] += G;
  State[7] += H;
}

[[nodiscard]] std::string toHex(const SHA256Digest &Digest) {
  constexpr std::string_view Digits = "0123456789abcdef";
  std::string Result;
  Result.resize(Digest.size() * 2U);
  for (std::size_t Index = 0; Index < Digest.size(); ++Index) {
    Result[Index * 2U] = Digits[Digest[Index] >> 4U];
    Result[Index * 2U + 1U] = Digits[Digest[Index] & 0x0fU];
  }
  return Result;
}

} // namespace

SHA256Digest sha256(std::span<const std::byte> Input) {
  const auto *Bytes = reinterpret_cast<const std::uint8_t *>(Input.data());
  std::array<std::uint32_t, 8> State = InitialState;
  std::size_t Offset = 0;
  while (Input.size() - Offset >= 64U) {
    compress(State, Bytes + Offset);
    Offset += 64U;
  }

  std::vector<std::uint8_t> Tail;
  Tail.reserve(128);
  Tail.insert(Tail.end(), Bytes + Offset, Bytes + Input.size());
  Tail.push_back(0x80U);
  while ((Tail.size() + 8U) % 64U != 0)
    Tail.push_back(0);
  const std::uint64_t BitLength = static_cast<std::uint64_t>(Input.size()) * 8U;
  for (unsigned Byte = 0; Byte < 8; ++Byte) {
    const unsigned Shift = 56U - Byte * 8U;
    Tail.push_back(static_cast<std::uint8_t>(BitLength >> Shift));
  }
  for (std::size_t Index = 0; Index < Tail.size(); Index += 64U)
    compress(State, Tail.data() + Index);

  SHA256Digest Result{};
  for (std::size_t Index = 0; Index < State.size(); ++Index)
    storeBigEndian(State[Index], Result.data() + Index * 4U);
  return Result;
}

SHA256Digest sha256(std::string_view Input) {
  const auto *Bytes = reinterpret_cast<const std::byte *>(Input.data());
  return sha256(std::span<const std::byte>(Bytes, Input.size()));
}

std::string sha256Hex(std::span<const std::byte> Input) {
  return toHex(sha256(Input));
}

std::string sha256Hex(std::string_view Input) {
  return toHex(sha256(Input));
}

} // namespace rcc
