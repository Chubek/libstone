#pragma once

#include "novostone/PCC/PCC.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>

namespace novostone::pcc {

inline constexpr std::uint32_t package_wire_version = 1;
inline constexpr std::size_t package_max_key_bytes = 16 * 1024;

// These defaults are hard ceilings for v1. Local policy may tighten but not
// raise them. The work values bound each optimizer run; proof verification
// performs at most one additional bounded prefix replay.
struct PackagePolicy {
  std::size_t max_package_bytes = 64 * 1024;
  WorkLimits max_work{64, 32, 256, 1024, 16, 1'000'000, 50'000, 100'000};
  std::size_t max_total_smt_bytes = 512 * 1024;
};

class PackageError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct VerifiedPackage {
  Artifact artifact; // regenerated locally, never reconstructed from claims
  std::size_t proved_obligations;
};

// Produces deterministic Ed25519-signed bytes. This authenticates an artifact
// but does not assert that it is proved. PEM must be unencrypted PKCS#8;
// evidence attachments are not representable by this source-level format.
[[nodiscard]] std::string sign_package(const Artifact& artifact,
                                       std::string_view private_key_pem,
                                       const PackagePolicy& policy = {});

// Reads one framed package and requires EOF, including on an SSH stream.
// Checks length before allocating the body. Does NOT authenticate the bytes.
[[nodiscard]] std::string read_package(std::istream& input,
                                       const PackagePolicy& policy = {});

// Throws on any failure. The public key is an independently configured
// Ed25519 SubjectPublicKeyInfo PEM, not a key supplied in the package.
// No package script, executable or purported result is accepted.
[[nodiscard]] VerifiedPackage verify_package(
    std::string_view package, std::string_view trusted_public_key_pem,
    const PackagePolicy& policy = {});

} // namespace novostone::pcc
