#pragma once

#include "sandstone.hpp"
#include "spec_value.hpp"

namespace domsymas::detail {
using json::Value;

struct RecordResult {
    bool ok = false;
    std::string error;
};

RecordResult build_spec_records(const Value &doc, Builder &builder);

// Expose internal metadata through the usual C lookup views. Unlike opening
// serialized SymAS, this does not apply the legacy file format's limits.
DomSymAs_Bank *make_metadata_bank(const Builder &builder);
}
