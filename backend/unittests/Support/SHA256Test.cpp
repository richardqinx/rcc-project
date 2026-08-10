// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Support/SHA256.h"

#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

int fail(const char *Message) {
  std::cerr << Message << '\n';
  return 1;
}

} // namespace

int main() {
  if (rcc::sha256Hex("") != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
    return fail("SHA-256 must match the empty-input standard vector");
  }
  if (rcc::sha256Hex("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
    return fail("SHA-256 must match the short standard vector");
  }
  constexpr std::string_view MultiBlock =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  if (rcc::sha256Hex(MultiBlock) !=
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") {
    return fail("SHA-256 must match the multi-block standard vector");
  }

  const std::string MillionA(1'000'000, 'a');
  if (rcc::sha256Hex(MillionA) !=
      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") {
    return fail("SHA-256 must match the million-byte standard vector");
  }

  const std::string Binary("a\0b", 3);
  const auto *Bytes = reinterpret_cast<const std::byte *>(Binary.data());
  if (rcc::sha256Hex(std::span<const std::byte>(Bytes, Binary.size())) !=
      "59b271ae1bbcb1d31d41929817f4b16fb439eb4f31520b5ad1d5ce98920a7138") {
    return fail("SHA-256 must hash embedded null bytes without truncation");
  }
  return 0;
}
