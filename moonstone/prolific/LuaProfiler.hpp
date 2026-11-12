#pragma once
#include "Prolific.hpp"

namespace moonstone::prolific {

struct ScriptLimits {
    std::size_t instructions = 1000000, findings = 1000, text = 4096;
    std::size_t source_bytes = 1024 * 1024;
};

// Replay a Recorder/Cache snapshot in a fresh Lua VM with native
// Kaguya-backed lprofcc/lprofvm. Do not supply fabricated snapshots.
// Blocking on the caller; independent calls own independent VMs. Errors return
// analysis_status="failed" and no partial findings. This is NOT a sandbox:
// use only trusted scripts or isolate the entire profiler in another process.
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
Report analyze(Snapshot snapshot, std::string_view script, ScriptLimits limits = {});

} // namespace moonstone::prolific
