#include "novostone/PCC/Package.hpp"
#include "novostone/PCC/SMTProof.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <array>
#include <bit>
#include <istream>
#include <limits>
#include <memory>
#include <utility>

namespace novostone::pcc {
namespace {

constexpr std::string_view magic = "NSPCCPKG";
constexpr std::size_t header_bytes = magic.size() + 3 * 4;
constexpr std::size_t signature_bytes = 64;
constexpr std::size_t min_payload_bytes = 9 * 4 + 2 * (16 + 12 + 24);
constexpr std::uint32_t v1_artifact_schema = 1;
static_assert(std::numeric_limits<unsigned char>::digits == 8);
static_assert(artifact_schema_version == v1_artifact_schema);
static_assert(i32_semantics == "novostone.ir.i32.modulo-2^32.v1");

// These tables define wire values independently of enum representation/ordering.
constexpr std::array wire_ops{
    ir::Op::Param, ir::Op::Const, ir::Op::Copy, ir::Op::Add, ir::Op::Sub,
    ir::Op::Mul, ir::Op::Eq, ir::Op::Ult, ir::Op::Select, ir::Op::Load,
    ir::Op::Store, ir::Op::Jump, ir::Op::Branch, ir::Op::Return, ir::Op::Phi};
constexpr std::array wire_transforms{
    TransformKind::ConstantFolding, TransformKind::LocalCSE,
    TransformKind::AlgebraicSimplification, TransformKind::CopyPropagation,
    TransformKind::DeadCodeElimination, TransformKind::BranchFolding,
    TransformKind::PhiSimplification, TransformKind::LoadForwarding,
    TransformKind::ConditionalConstantPropagation};
constexpr std::array work_fields{
    &WorkLimits::max_arity, &WorkLimits::max_blocks,
    &WorkLimits::max_instructions, &WorkLimits::max_references,
    &WorkLimits::max_rewrites, &WorkLimits::max_query_work,
    &WorkLimits::max_query_facts, &WorkLimits::max_analysis_work};

[[noreturn]] void reject(const char* message) { throw PackageError(message); }

void validate_policy(const PackagePolicy& policy) {
  constexpr PackagePolicy ceiling;
  if (policy.max_package_bytes > ceiling.max_package_bytes ||
      policy.max_total_smt_bytes > ceiling.max_total_smt_bytes)
    reject("PCC package policy exceeds v1 byte ceilings");
  for (auto field : work_fields)
    if (policy.max_work.*field > ceiling.max_work.*field)
      reject("PCC package policy exceeds v1 work ceilings");
}

void validate_work(const WorkLimits& work, const PackagePolicy& policy) {
  for (auto field : work_fields) {
    if (work.*field > policy.max_work.*field)
      reject("PCC package work limit exceeds verifier policy");
    if (work.*field == 0 && field != &WorkLimits::max_rewrites &&
        field != &WorkLimits::max_query_work)
      reject("invalid zero PCC package work limit");
  }
}

void put32(std::string& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(std::bit_cast<char>(
        static_cast<unsigned char>((value >> shift) & 0xff)));
}

void put_id(std::string& out, ir::Id id) {
  // Leave room for CCDB's reserve(id), which advances its ID counter.
  if (id <= 0 || id == std::numeric_limits<ir::Id>::max())
    reject("invalid PCC package entity ID");
  const auto value = static_cast<std::uint64_t>(id);
  put32(out, static_cast<std::uint32_t>(value >> 32));
  put32(out, static_cast<std::uint32_t>(value & 0xffffffffu));
}

class Reader {
public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}
  std::string_view take(std::size_t n) {
    if (n > bytes_.size()) reject("truncated PCC package field");
    auto result = bytes_.substr(0, n);
    bytes_.remove_prefix(n);
    return result;
  }
  std::uint32_t u32() {
    std::uint32_t result = 0;
    for (const char octet : take(4))
      result = (result << 8) | std::bit_cast<unsigned char>(octet);
    return result;
  }
  ir::Id id() {
    const auto high = static_cast<std::uint64_t>(u32());
    const auto value = (high << 32) | u32();
    if (value == 0 ||
        value >= static_cast<std::uint64_t>(std::numeric_limits<ir::Id>::max()))
      reject("invalid PCC package entity ID");
    return static_cast<ir::Id>(value);
  }
  std::size_t remaining() const { return bytes_.size(); }

private:
  std::string_view bytes_;
};

std::size_t payload_length(std::string_view header, const PackagePolicy& policy) {
  Reader in(header);
  if (in.take(magic.size()) != magic)
    reject("not a signed PCC wire package");
  if (in.u32() != package_wire_version || in.u32() != v1_artifact_schema)
    reject("unsupported PCC package version");
  const std::size_t count = in.u32();
  constexpr auto overhead = header_bytes + signature_bytes;
  if (count < min_payload_bytes || policy.max_package_bytes < overhead ||
      count > policy.max_package_bytes - overhead)
    reject("PCC package payload length exceeds bounds");
  return count;
}

void encode_constraints(std::string& out, const RewriteConstraints& constraints) {
  std::uint32_t allowed = 0;
  for (std::size_t i = 0; i < wire_transforms.size(); ++i)
    if (constraints.allows(wire_transforms[i])) allowed |= std::uint32_t{1} << i;
  if (static_cast<std::size_t>(std::popcount(allowed)) != constraints.allowed.size())
    reject("unknown PCC package transform");
  put32(out, allowed);
  for (auto field : work_fields)
    put32(out, static_cast<std::uint32_t>(constraints.limits.*field));
}

RewriteConstraints decode_constraints(Reader& in, const PackagePolicy& policy) {
  RewriteConstraints constraints;
  constraints.allowed.clear();
  const auto allowed = in.u32();
  if (allowed >> wire_transforms.size()) reject("unknown PCC package transform");
  for (std::size_t i = 0; i < wire_transforms.size(); ++i)
    if (allowed & (std::uint32_t{1} << i))
      constraints.allow(wire_transforms[i]);
  for (auto field : work_fields) constraints.limits.*field = in.u32();
  validate_work(constraints.limits, policy);
  return constraints;
}

void consume(std::size_t& budget, std::size_t count, const char* error) {
  if (count > budget) reject(error);
  budget -= count;
}

void encode_function(std::string& out, const ir::Function& f, const WorkLimits& work) {
  if (f.arity > work.max_arity || f.blocks.size() > work.max_blocks ||
      f.blocks.empty())
    reject("PCC package function exceeds structural bounds");
  put32(out, f.arity);
  put_id(out, f.entry);
  put32(out, static_cast<std::uint32_t>(f.blocks.size()));
  auto instructions = work.max_instructions;
  auto references = work.max_references;
  for (const auto& [id, block] : f.blocks) {
    if (id != block.id || block.instructions.empty())
      reject("invalid PCC package block");
    consume(instructions, block.instructions.size(), "PCC instruction budget exceeded");
    put_id(out, id);
    put32(out, static_cast<std::uint32_t>(block.instructions.size()));
    for (const auto& instr : block.instructions) {
      put_id(out, instr.id);
      std::size_t op = 0;
      for (; op < wire_ops.size() && wire_ops[op] != instr.op; ++op) {}
      if (op == wire_ops.size()) reject("unknown PCC package opcode");
      put32(out, static_cast<std::uint32_t>(op));
      put32(out, instr.immediate);
      for (const auto* list : {&instr.operands, &instr.targets}) {
        consume(references, list->size(), "PCC reference budget exceeded");
        put32(out, static_cast<std::uint32_t>(list->size()));
        for (const auto ref : *list) put_id(out, ref);
      }
    }
  }
}

ir::Function decode_function(Reader& in, const WorkLimits& work) {
  ir::Function f;
  f.arity = in.u32();
  f.entry = in.id();
  const std::size_t blocks = in.u32();
  if (f.arity > work.max_arity || blocks == 0 || blocks > work.max_blocks ||
      blocks > in.remaining() / 12)
    reject("PCC package function exceeds structural bounds");
  auto instructions = work.max_instructions;
  auto references = work.max_references;
  ir::Id last = 0;
  for (std::size_t b = 0; b < blocks; ++b) {
    ir::Block block{in.id(), {}};
    if (block.id <= last) reject("duplicate or unordered PCC block");
    last = block.id;
    const std::size_t count = in.u32();
    consume(instructions, count, "PCC instruction budget exceeded");
    if (count == 0 || count > in.remaining() / 24)
      reject("invalid PCC instruction count");
    block.instructions.reserve(count);
    for (std::size_t n = 0; n < count; ++n) {
      ir::Instruction instr;
      instr.id = in.id();
      const auto op = in.u32();
      if (op >= wire_ops.size()) reject("unknown PCC package opcode");
      instr.op = wire_ops[op];
      instr.immediate = in.u32();
      for (auto* list : {&instr.operands, &instr.targets}) {
        const std::size_t size = in.u32();
        consume(references, size, "PCC reference budget exceeded");
        if (size > in.remaining() / 8) reject("invalid PCC reference count");
        list->reserve(size);
        for (std::size_t r = 0; r < size; ++r) list->push_back(in.id());
      }
      block.instructions.push_back(std::move(instr));
    }
    f.blocks.emplace(last, std::move(block));
  }
  return f;
}

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

int no_password(char*, int, int, void*) { return 0; }

Key read_key(std::string_view pem, bool private_key) {
  constexpr std::string_view whitespace = " \t\r\n";
  if (pem.empty() || pem.size() > package_max_key_bytes)
    reject("PCC PEM key length exceeds bounds");
  const std::string_view begin = private_key ? "-----BEGIN PRIVATE KEY-----"
                                             : "-----BEGIN PUBLIC KEY-----";
  const std::string_view end = private_key ? "-----END PRIVATE KEY-----"
                                           : "-----END PUBLIC KEY-----";
  const auto last = pem.find_last_not_of(whitespace);
  if (last == std::string_view::npos) reject("expected a PCC Ed25519 PEM key");
  const auto trimmed = pem.substr(0, last + 1);
  if (!trimmed.starts_with(begin) || !trimmed.ends_with(end) ||
      trimmed.size() <= begin.size() + end.size())
    reject("expected a PCC Ed25519 PEM key");
  auto body = trimmed.substr(begin.size(), trimmed.size() - begin.size() - end.size());
  if (!(body.starts_with("\n") || body.starts_with("\r\n")) ||
      !body.ends_with("\n") || body.find('-') != std::string_view::npos)
    reject("expected exactly one PCC Ed25519 PEM key");
  Bio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  if (!bio) reject("could not initialize PCC key reader");
  Key key(private_key ? PEM_read_bio_PrivateKey(bio.get(), nullptr, no_password, nullptr)
                      : PEM_read_bio_PUBKEY(bio.get(), nullptr, no_password, nullptr),
          EVP_PKEY_free);
  if (!key || EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519)
    reject("expected a PCC Ed25519 PEM key");
  char* rest = nullptr;
  const auto length = BIO_get_mem_data(bio.get(), &rest);
  if (length < 0 || (length &&
      std::string_view(rest, static_cast<std::size_t>(length))
          .find_first_not_of(whitespace) != std::string_view::npos))
    reject("unexpected data after PCC PEM key");
  return key;
}

// The proof API replays the accepted prefix using verifier-capped options
// from build(), and independently bounds SCCP proof work and generation.
std::size_t prove(const Artifact& artifact, const PackagePolicy& policy) {
  if (!satie_smtlib2_available()) reject("PCC SMT solver is unavailable");
  auto proof = emit_smt_proof(artifact);
  if (proof.obligations.size() != artifact.transformations().size())
    reject("PCC package proof replay omitted a rewrite");
  std::size_t total_bytes = policy.max_total_smt_bytes;
  for (const auto& obligation : proof.obligations)
    consume(total_bytes, obligation.smtlib2.size(), "PCC SMT byte budget exceeded");
  for (auto& obligation : proof.obligations) {
    const auto result = discharge_smt(obligation, SMTFrontend::SMTLIB2);
    if (result.syntax != SMTSyntaxStatus::Valid ||
        result.status != ObligationStatus::UnsatProved)
      reject("PCC package SMT obligation is not proved");
  }
  return proof.obligations.size();
}

} // namespace

std::string sign_package(const Artifact& artifact, std::string_view private_key_pem,
                         const PackagePolicy& policy) {
  validate_policy(policy);
  validate_work(artifact.constraints().limits, policy);
  if (artifact.smt_proof_text() || artifact.register_allocation_evidence())
    reject("PCC wire v1 does not carry evidence attachments");
  std::string payload;
  encode_constraints(payload, artifact.constraints());
  encode_function(payload, artifact.source(), artifact.constraints().limits);
  encode_function(payload, artifact.transformed(), artifact.constraints().limits);

  std::string out(magic);
  put32(out, package_wire_version);
  put32(out, v1_artifact_schema);
  put32(out, static_cast<std::uint32_t>(payload.size()));
  (void)payload_length(out, policy);
  out += payload;

  const auto key = read_key(private_key_pem, true);
  const Context ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  std::array<unsigned char, signature_bytes> signature{};
  auto length = signature.size();
  if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
      EVP_DigestSign(ctx.get(), signature.data(), &length,
                     reinterpret_cast<const unsigned char*>(out.data()), out.size()) != 1 ||
      length != signature.size())
    reject("could not sign PCC package");
  out.append(reinterpret_cast<const char*>(signature.data()), signature.size());
  return out;
}

std::string read_package(std::istream& input, const PackagePolicy& policy) {
  validate_policy(policy);
  std::string bytes(header_bytes, '\0');
  if (!input.read(bytes.data(), static_cast<std::streamsize>(header_bytes)))
    reject("truncated PCC package header");
  const auto remaining = payload_length(bytes, policy) + signature_bytes;
  bytes.resize(header_bytes + remaining);
  if (!input.read(bytes.data() + header_bytes, static_cast<std::streamsize>(remaining)))
    reject("truncated PCC package body");
  if (input.peek() != std::char_traits<char>::eof())
    reject("trailing data after PCC package");
  if (input.bad() || !input.eof()) reject("could not read PCC package");
  return bytes;
}

VerifiedPackage verify_package(std::string_view bytes, std::string_view public_key_pem,
                               const PackagePolicy& policy) {
  validate_policy(policy);
  const auto payload = payload_length(bytes.substr(0, header_bytes), policy);
  const auto signed_length = header_bytes + payload;
  if (bytes.size() != signed_length + signature_bytes)
    reject("PCC package frame length mismatch");

  const auto key = read_key(public_key_pem, false);
  const Context ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
      EVP_DigestVerify(ctx.get(),
                       reinterpret_cast<const unsigned char*>(bytes.data() + signed_length),
                       signature_bytes, reinterpret_cast<const unsigned char*>(bytes.data()),
                       signed_length) != 1)
    reject("PCC package signature rejected");

  Reader in(bytes.substr(header_bytes, payload));
  const auto constraints = decode_constraints(in, policy);
  const auto source = decode_function(in, constraints.limits);
  const auto candidate_begin = in.remaining();
  (void)decode_function(in, constraints.limits);
  if (in.remaining() != 0) reject("trailing data in PCC package payload");

  try {
    auto artifact = build(source, constraints);
    if (artifact.selection().stop == StopCause::ResourceLimit &&
        artifact.selection().detail != "rewrite limit reached")
      reject("PCC package optimization work budget exhausted");
    std::string expected;
    encode_function(expected, artifact.transformed(), constraints.limits);
    if (expected != bytes.substr(signed_length - candidate_begin, candidate_begin))
      reject("PCC package candidate differs from deterministic replay");
    const auto proved = prove(artifact, policy);
    return {std::move(artifact), proved};
  } catch (const PackageError&) {
    throw;
  } catch (const std::logic_error& error) {
    throw PackageError("PCC package replay rejected: " + std::string(error.what()));
  } catch (const std::runtime_error& error) {
    throw PackageError("PCC package replay failed: " + std::string(error.what()));
  }
}

} // namespace novostone::pcc
