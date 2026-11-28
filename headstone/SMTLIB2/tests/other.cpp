#include <smtlib2/smtlib2.hpp>
int other_translation_unit() { return smtlib2::Integer("12345678901234567890").to_string() == "12345678901234567890" ? 0 : 1; }
