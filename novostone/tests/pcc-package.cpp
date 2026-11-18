#include "novostone/PCC/Package.hpp"
#include "novostone/PCC/SMTProof.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <array>
#include <bit>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>

using namespace novostone;

namespace {

void require(bool condition, std::string_view reason) {
  if (!condition) throw std::runtime_error(std::string(reason));
}

template<class Function>
void rejected(Function f, std::string_view reason) {
  try {
    f();
  } catch (const pcc::PackageError& error) {
    if (std::string_view(error.what()).find(reason) != std::string_view::npos)
      return;
    throw std::runtime_error("wrong package rejection: " + std::string(error.what()));
  }
  throw std::runtime_error("unexpected package acceptance; expected " + std::string(reason));
}

struct Keys {
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
  std::string private_pem, public_pem;
};

Keys make_keys(int type = EVP_PKEY_ED25519) {
  auto ctx = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
      EVP_PKEY_CTX_new_id(type, nullptr), EVP_PKEY_CTX_free);
  require(ctx && EVP_PKEY_keygen_init(ctx.get()) == 1, "cannot initialize test keygen");
  if (type == EVP_PKEY_RSA)
    require(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) == 1, "cannot set RSA bits");
  EVP_PKEY* raw = nullptr;
  require(EVP_PKEY_keygen(ctx.get(), &raw) == 1, "cannot generate test key");
  Keys keys;
  keys.key.reset(raw);
  const auto pem = [&](bool private_key) {
    auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), BIO_free);
    require(static_cast<bool>(bio), "cannot allocate test BIO");
    require((private_key ? PEM_write_bio_PrivateKey(bio.get(), raw, nullptr, nullptr, 0,
                                                  nullptr, nullptr)
                         : PEM_write_bio_PUBKEY(bio.get(), raw)) == 1,
            "cannot write test key");
    char* bytes = nullptr;
    const auto length = BIO_get_mem_data(bio.get(), &bytes);
    require(length > 0, "empty test key");
    return std::string(bytes, static_cast<std::size_t>(length));
  };
  keys.private_pem = pem(true);
  keys.public_pem = pem(false);
  return keys;
}

void set32(std::string& bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i)
    bytes.at(offset + i) = std::bit_cast<char>(
        static_cast<unsigned char>((value >> (24 - i * 8)) & 0xff));
}

void resign(std::string& bytes, const Keys& keys) {
  auto ctx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
      EVP_MD_CTX_new(), EVP_MD_CTX_free);
  std::array<unsigned char, 64> signature{};
  auto length = signature.size();
  const auto message = bytes.size() - length;
  require(ctx && EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, keys.key.get()) == 1 &&
              EVP_DigestSign(ctx.get(), signature.data(), &length,
                             reinterpret_cast<const unsigned char*>(bytes.data()), message) == 1 &&
              length == signature.size(),
          "cannot re-sign test package");
  bytes.replace(message, length, reinterpret_cast<const char*>(signature.data()), length);
}

void tests() {
  constexpr std::string_view source_text = R"(novostone 1 2 1
block 1
instr 2 param 0 0 0
instr 3 param 1 0 0
instr 4 add 0 2 2 3 0
instr 5 add 0 2 3 2 0
instr 6 return 0 1 5 0
)";
  constexpr std::string_view multiple_text = R"(novostone 1 2 1
block 1
instr 2 param 0 0 0
instr 3 param 1 0 0
instr 4 add 0 2 2 3 0
instr 5 add 0 2 3 2 0
instr 6 sub 0 2 5 2 0
instr 7 sub 0 2 4 2 0
instr 8 add 0 2 7 6 0
instr 9 return 0 1 8 0
)";
  constexpr std::string_view phi_text = R"(novostone 2 3 1
block 1
instr 2 param 0 0 0
instr 3 param 1 0 0
instr 4 param 2 0 0
instr 5 branch 0 1 2 2 10 20
block 10
instr 11 jump 0 0 1 30
block 20
instr 21 jump 0 0 1 30
block 30
instr 31 phi 0 2 3 4 2 10 20
instr 32 return 0 1 31 0
)";
  const auto keys = make_keys();
  const auto wrong = make_keys();
  const auto rsa = make_keys(EVP_PKEY_RSA);
  const auto source = ir::parse(std::string(source_text));
  const pcc::PackagePolicy policy;
  pcc::RewriteConstraints constraints;
  constraints.limits = policy.max_work;
  const auto artifact = pcc::build(source, constraints);
  require(artifact.transformations().size() == 1, "test did not get one CSE");
  const auto bytes = pcc::sign_package(artifact, keys.private_pem);
  require(bytes == pcc::sign_package(pcc::build(source, constraints), keys.private_pem),
          "signed package is nondeterministic");
  std::istringstream stream(bytes);
  require(pcc::read_package(stream) == bytes, "frame roundtrip changed bytes");
  const auto verify = [&](std::string_view candidate) {
    return pcc::verify_package(candidate, keys.public_pem);
  };
  const auto phi = pcc::build(ir::parse(std::string(phi_text)), constraints);
  const auto phi_bytes = pcc::sign_package(phi, keys.private_pem);
  auto unordered_blocks = phi_bytes;
  set32(unordered_blocks, 76, 20);
  resign(unordered_blocks, keys);
  rejected([&] { (void)verify(unordered_blocks); }, "duplicate or unordered PCC block");
  if (pcc::satie_smtlib2_available()) {
    const auto verified = verify(bytes);
    require(verified.proved_obligations == 1, "roundtrip did not prove obligation");
    require(verified.artifact.canonical_core() == artifact.canonical_core(),
            "roundtrip changed artifact");
    require(pcc::sign_package(verified.artifact, keys.private_pem) == bytes,
            "roundtrip changed wire bytes");
    require(verify(phi_bytes).artifact.canonical_core() == phi.canonical_core(),
            "multi-block/phi wire roundtrip changed artifact");
    auto tiny = policy;
    tiny.max_total_smt_bytes = 1;
    rejected([&] { (void)pcc::verify_package(bytes, keys.public_pem, tiny); },
             "SMT byte budget");
    auto multi_constraints = constraints;
    multi_constraints.allow(pcc::TransformKind::CopyPropagation);
    const auto multiple = pcc::build(ir::parse(std::string(multiple_text)), multi_constraints);
    require(multiple.transformations().size() > 1, "test did not get multiple rewrites");
    require(verify(pcc::sign_package(multiple, keys.private_pem)).proved_obligations ==
                multiple.transformations().size(),
            "did not prove every step");
    auto identity = constraints;
    identity.allowed.clear();
    const auto baseline = pcc::build(source, identity);
    require(verify(pcc::sign_package(baseline, keys.private_pem)).proved_obligations == 0,
            "identity artifact has a proof obligation");
    identity = constraints;
    identity.limits.max_rewrites = 0;
    const auto limited = pcc::build(source, identity);
    require(verify(pcc::sign_package(limited, keys.private_pem)).proved_obligations == 0,
            "explicit zero-rewrite prefix was not verified");
  } else {
    rejected([&] { (void)verify(bytes); }, "solver is unavailable");
    rejected([&] { (void)verify(phi_bytes); }, "solver is unavailable");
  }

  rejected([&] { (void)pcc::verify_package(bytes, wrong.public_pem); }, "signature rejected");
  rejected([&] { (void)pcc::verify_package(bytes, keys.private_pem); }, "expected a PCC Ed25519");
  rejected([&] { (void)pcc::verify_package(bytes, rsa.public_pem); }, "expected a PCC Ed25519");
  rejected([&] { (void)pcc::verify_package(bytes, ""); }, "key length");
  rejected([&] { (void)pcc::verify_package(bytes, keys.public_pem + wrong.public_pem); },
           "exactly one PCC Ed25519");
  rejected([&] { (void)pcc::sign_package(artifact, rsa.private_pem); }, "expected a PCC Ed25519");
  rejected([&] { (void)pcc::sign_package(artifact, keys.public_pem); }, "expected a PCC Ed25519");

  auto unauthenticated = bytes;
  unauthenticated.at(20) ^= 1;
  rejected([&] { (void)verify(unauthenticated); }, "signature rejected");
  unauthenticated = bytes;
  unauthenticated.back() ^= 1;
  rejected([&] { (void)verify(unauthenticated); }, "signature rejected");
  for (std::size_t n = 0; n < bytes.size(); ++n) {
    const std::string_view truncated(bytes.data(), n);
    rejected([&] { (void)verify(truncated); },
             n < 20 ? "truncated" : "frame length");
    std::istringstream frame(std::string(truncated), std::ios::binary);
    rejected([&] { (void)pcc::read_package(frame); }, "truncated");
  }
  rejected([&] { (void)verify(bytes + "\n"); }, "frame length");
  rejected([&] { (void)verify("pcc-package 1\nartifact-core 5\nfake!"); }, "wire package");
  std::istringstream extra(bytes + "\n");
  rejected([&] { (void)pcc::read_package(extra); }, "trailing data");
  auto smaller = policy;
  smaller.max_package_bytes = bytes.size() - 1;
  rejected([&] { (void)pcc::verify_package(bytes, keys.public_pem, smaller); }, "length");
  smaller = policy;
  --smaller.max_work.max_rewrites;
  rejected([&] { (void)pcc::verify_package(bytes, keys.public_pem, smaller); }, "work limit");
  auto larger = policy;
  ++larger.max_work.max_blocks;
  rejected([&] { (void)pcc::verify_package(bytes, keys.public_pem, larger); }, "ceiling");

  const auto malformed = [&](std::size_t offset, std::uint32_t value, std::string_view reason) {
    auto bad = bytes;
    set32(bad, offset, value);
    resign(bad, keys);
    rejected([&] { (void)verify(bad); }, reason);
  };
  malformed(8, 2, "unsupported PCC package version");
  malformed(12, 2, "unsupported PCC package version");
  malformed(16, 0xffffffffu, "payload length");
  std::istringstream too_large(bytes.substr(0, 16) + std::string(4, '\xff'));
  rejected([&] { (void)pcc::read_package(too_large); }, "payload length");
  malformed(20, 0x80000000u, "unknown PCC package transform");
  malformed(24, 0, "zero PCC package work limit");
  malformed(28, 33, "work limit");
  malformed(44, 0, "optimization work budget exhausted");
  malformed(56, 65, "structural bounds");
  malformed(60, 0xffffffffu, "invalid PCC package entity ID");
  malformed(68, 0xffffffffu, "structural bounds");
  malformed(80, 0xffffffffu, "instruction budget");
  malformed(92, 0xffffffffu, "unknown PCC package opcode");
  malformed(96, 42, "replay rejected"); // Invalid param index inside a signed source.
  malformed(100, 0xffffffffu, "reference budget");
  auto trailing_payload = bytes;
  trailing_payload.insert(trailing_payload.size() - 64, "fake-smt", 8);
  set32(trailing_payload, 16, static_cast<std::uint32_t>(trailing_payload.size() - 84));
  resign(trailing_payload, keys);
  rejected([&] { (void)verify(trailing_payload); }, "trailing data in PCC package payload");

  // A correctly signed, structurally valid but incorrect candidate cannot be
  // accepted just because the signer or encrypted transport is trusted.
  auto changed_candidate = bytes;
  set32(changed_candidate, changed_candidate.size() - 64 - 8, 3);
  resign(changed_candidate, keys);
  rejected([&] { (void)verify(changed_candidate); }, "candidate differs");
}

} // namespace

int main() {
  try {
    tests();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pcc-package: " << error.what() << '\n';
    return 1;
  }
}
