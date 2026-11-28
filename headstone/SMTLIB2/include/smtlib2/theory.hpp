#pragma once
#include "arithmetic.hpp"

namespace smtlib2 {
/// @implstring These flags describe solving, not availability of concrete
/// value primitives. Integer and Rational do not enable arithmetic solving.
/// Future theory solving needs validated model values or conflict explanations,
/// and must preserve unknown for incomplete fragments.
struct TheoryCapabilities {
    bool integers = false, reals = false, bit_vectors = false;
    bool arrays = false, floating_point = false, quantifiers = false;
};
}
