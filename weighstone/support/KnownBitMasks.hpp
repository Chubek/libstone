#pragma once

namespace weighstone::support {

// Shared by arbitrary-precision and machine-word clients. The caller supplies
// nonoverlapping masks confined to a common bit width.
template<class Mask>
struct KnownBitMasks {
    Mask zero{};
    Mask one{};

    bool all_known(const Mask& width_mask) const { return (zero | one) == width_mask; }
    bool masked_by(const KnownBitMasks& rhs, const Mask& width_mask) const {
        return (zero | rhs.one) == width_mask;
    }
    bool disjoint(const KnownBitMasks& rhs, const Mask& width_mask) const {
        return (zero | rhs.zero) == width_mask;
    }
    KnownBitMasks join(const KnownBitMasks& rhs) const {
        return {zero & rhs.zero, one & rhs.one};
    }
    KnownBitMasks bit_and(const KnownBitMasks& rhs) const {
        return {zero | rhs.zero, one & rhs.one};
    }
    KnownBitMasks bit_or(const KnownBitMasks& rhs) const {
        return {zero & rhs.zero, one | rhs.one};
    }
    KnownBitMasks bit_xor(const KnownBitMasks& rhs) const {
        return {(zero & rhs.zero) | (one & rhs.one),
                (zero & rhs.one) | (one & rhs.zero)};
    }
};

}
