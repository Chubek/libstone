#pragma once
#if __has_include("../../../core/Core.hpp")
#include "../../../core/Core.hpp"
#else
#include <moonstone/core/Core.hpp>
#endif
#if __has_include("../../../core/SafetyEffects.hpp")
#include "../../../core/SafetyEffects.hpp"
#else
#include <moonstone/core/SafetyEffects.hpp>
#endif
#include "CDecl.hpp"
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace moonstone::exoml::contract {
inline constexpr unsigned contract_schema_version = 2;

enum class OwnershipKind {
    Scalar,
    BorrowedImmutable,
    BorrowedMutable,
    OwnedUnique,
    Shared,
    Consumed,
    OutParameter,
    InOutParameter,
    OpaqueUnsafe,
    RetainedCallback,
    Transferred
};
enum class LifetimeOrigin {
    Parameter,
    Receiver,
    ReturnedOwner,
    GlobalStatic,
    CallerBuffer,
    CallbackRegistration,
    ExplicitRegion
};
enum class InvalidationKind {
    OwnerDrop,
    OwnerMove,
    Mutation,
    Reallocation,
    RegistrationDestruction,
    NextCall,
    ExplicitOperation,
    ConcurrencyDependent
};
enum class ReturnKind { Value, Borrow, Owned, Nullable, TaggedError };
enum class ErrorKind {
    None,
    Sentinel,
    NullOnFailure,
    BooleanReturn,
    IntCode,
    Errno,
    OutError,
    CustomClassifier
};
enum class TransferBranch { Success, Failure, Unconditional, Conditional };
enum class ContractState { Discovered, Annotated, Validated, Reviewed, UsageChecked };
enum class EffectKind {
    Read,
    Write,
    Allocate,
    Free,
    Escape,
    IO,
    Thread,
    Suspend,
    ForeignCall
};
enum class ReviewStatus { Unreviewed, AutoInferred, Reviewed, Verified };
enum class ThreadSafety { SingleThread, ThreadSafe, ThreadAffine, SignalSafe };
enum class CallbackMode { Synchronous, Retained };

struct LifetimeSpec {
    LifetimeOrigin origin = LifetimeOrigin::Parameter;
    std::uint32_t parameter_index = 0;
    std::string region_name;
};
struct InvalidationRule {
    InvalidationKind kind = InvalidationKind::OwnerDrop;
    std::string description;
};
struct ReturnContract {
    ReturnKind kind = ReturnKind::Value;
    bool nullable = false;
    std::optional<LifetimeSpec> lifetime;
    std::string destroy_with;
    std::string allocation_source;
    std::string deallocator_family;
    bool nullable_means_success = false;
    std::string alias_of;
};
struct ParameterContract {
    std::uint32_t index = 0;
    std::string name;
    bool nullable = false;
    OwnershipKind ownership = OwnershipKind::Scalar;
    std::optional<LifetimeSpec> lifetime;
    std::optional<std::uint32_t> length_parameter;
    std::optional<std::uint32_t> count_parameter;
    std::string encoding;
    std::optional<std::uint32_t> alignment;
    std::optional<TransferBranch> transfer_branch;
    std::string transfer_predicate;
};
struct ErrorContract {
    ErrorKind kind = ErrorKind::None;
    std::string sentinel_value;
    bool out_valid_on_failure = false;
    bool cleanup_required_on_failure = false;
    std::string error_parameter;
};
struct ConditionalTransfer {
    TransferBranch branch = TransferBranch::Unconditional;
    std::string from_owner;
    std::string to_owner;
    std::string predicate;
};
struct DestructorContract {
    std::string function_name;
    std::optional<std::uint32_t> context_parameter;
    std::string allocator_family;
    bool idempotent = false;
    bool null_safe = false;
    bool thread_affine = false;
    bool may_fail = false;
};
struct CallbackContract {
    std::string signature;
    CallbackMode mode = CallbackMode::Synchronous;
    std::optional<std::uint32_t> user_data_parameter;
    std::optional<LifetimeSpec> lifetime;
    std::string unregister_function;
    bool reentrant = false;
    bool may_throw = false;
};
struct ConcurrencyContract {
    ThreadSafety thread_safety = ThreadSafety::SingleThread;
    bool registration_thread_affine = false;
    bool signal_safe = false;
    std::string callback_origin;
    std::string blocking_behavior;
    bool suspension_safe = false;
};
struct EffectContract {
    std::set<EffectKind> effects;
    bool unknown = false;
};
struct ContractProvenance {
    std::string header;
    std::string revision;
    SourceSpan source_location;
    std::string author;
    ReviewStatus review_status = ReviewStatus::Unreviewed;
    std::string evidence_method;
    std::string toolchain_fingerprint;
};

struct FunctionContract {
    std::string c_name;
    ReturnContract return_contract;
    std::vector<ParameterContract> parameters;
    ErrorContract error_contract;
    EffectContract effects;
    ConcurrencyContract concurrency;
    std::vector<CallbackContract> callbacks;
    std::vector<DestructorContract> destructors;
    std::vector<ConditionalTransfer> transfers;
    std::vector<InvalidationRule> invalidations;
    ContractProvenance provenance;
};

struct ContractBundle {
    unsigned schema_version = contract_schema_version;
    std::string library;
    std::vector<std::string> headers;
    std::vector<FunctionContract> functions;
    ContractProvenance bundle_provenance;
    ContractState state = ContractState::Discovered;
};

// Parse a *.exocontract.json sidecar. Throws std::invalid_argument on malformed
// JSON or structural violations (unknown keys, wrong types).
ContractBundle parse_contract_sidecar(std::string_view json);

// Deterministic JSON serialization. Round-trips through parse_contract_sidecar
// without semantic loss. Key order is canonical (alphabetical within objects).
std::string serialize_contract(const ContractBundle &bundle);

// Validate a contract bundle against a CDecl translation unit.
// Returns diagnostics for every Gate B violation; an empty result means valid.
Diagnostics validate_contract(const ContractBundle &bundle,
                              const cdecl::TranslationUnit &unit);

// Project a FunctionContract into the legacy OwnershipContract for compatibility
// with existing BindingModule / code generation paths. Lossy: rich contract
// information not representable in the coarse model is dropped.
struct OwnershipProjection {
    OwnershipContract ownership;
};
OwnershipProjection project_to_ownership(const FunctionContract &contract,
                                         std::size_t parameter_count);

// Check whether a bundle's state permits safe export. Requires every function
// to have been reviewed or verified.
bool is_safe_for_export(const ContractBundle &bundle);
} // namespace moonstone::exoml::contract
