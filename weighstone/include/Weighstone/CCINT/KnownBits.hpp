#pragma once
#include "BitVector.hpp"
#include "../../../support/KnownBitMasks.hpp"
#include <stdexcept>
namespace weighstone::ccint {
class KnownBits {
    unsigned width_;
    support::KnownBitMasks<BigInt> bits_;
    void check(const KnownBits& b) const { if(width_!=b.width_) throw std::invalid_argument("mismatched widths"); }
public:
    KnownBits(unsigned width, BigInt known_zero=0, BigInt known_one=0)
        : width_(width) {
        // Materialize the value: a Boost expression would reference a temporary.
        const BigInt mask=BitVector(width,0).mask();
        bits_={known_zero&mask, known_one&mask};
        if ((bits_.zero&bits_.one)!=0) throw std::invalid_argument("conflicting known bits");
    }
    static KnownBits constant(const BitVector& v) { return {v.width(), v.mask()^v.unsigned_value(), v.unsigned_value()}; }
    unsigned width() const noexcept { return width_; }
    const BigInt& known_zero() const noexcept { return bits_.zero; }
    const BigInt& known_one() const noexcept { return bits_.one; }
    bool is_constant() const { return bits_.all_known(power_of_two(width_)-1); }
    BitVector constant_value() const { if(!is_constant()) throw std::logic_error("not constant"); return {width_,bits_.one}; }
    KnownBits join(const KnownBits& b) const {
        check(b);
        const auto bits=bits_.join(b.bits_);
        return {width_,bits.zero,bits.one};
    }
    KnownBits bit_and(const KnownBits& b) const {
        check(b);
        const auto bits=bits_.bit_and(b.bits_);
        return {width_,bits.zero,bits.one};
    }
    KnownBits bit_or(const KnownBits& b) const {
        check(b);
        const auto bits=bits_.bit_or(b.bits_);
        return {width_,bits.zero,bits.one};
    }
    KnownBits bit_xor(const KnownBits& b) const {
        check(b);
        const auto bits=bits_.bit_xor(b.bits_);
        return {width_,bits.zero,bits.one};
    }
};
}
