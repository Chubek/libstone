// Toolchain backends share process/response-file handling, never a shell.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace qld { class ScriptSDK; }
namespace qld::detail {
struct LinkOptions;
struct LinkState;
enum class Backend { Auto, ELF, PE, MachO };
Backend namedBackend(const std::string&);
Backend inputBackend(const std::vector<std::string>& paths);
Backend selectBackend(const LinkOptions&);
// CLI selection examines input magic (including archive members), not suffixes.
Backend invocationBackend(const std::vector<std::string>& arguments);
const char *defaultPeLinker();
const char *defaultMachOLinker();
const char *defaultElfLinker();
bool linkElf(LinkState&, std::vector<uint8_t>&, std::string&, const ScriptSDK&);
bool linkPe(LinkState&, std::vector<uint8_t>&, std::string&);
bool linkMachO(LinkState&, std::vector<uint8_t>&, std::string&);
// Native format selection never implies permission to spawn a toolchain.
bool externalBackendRequested(const LinkOptions&, Backend);
void parseNativeArguments(LinkOptions&, Backend);
bool linkNativeImage(LinkState&, std::vector<uint8_t>&, std::string&);

namespace toolchain {
inline constexpr size_t sourceLimit = 16 * 1024 * 1024;
enum class Quoting { GNU, Windows };
std::string readText(const std::filesystem::path&, size_t limit = sourceLimit);
void writeText(const std::filesystem::path&, const std::string&);
struct Workspace {
  std::filesystem::path path;
  Workspace();
  ~Workspace();
  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;
};
std::vector<std::string> expandArguments(const std::vector<std::string>&,
                                         Quoting = Quoting::GNU);
std::string runProcess(const std::string& executable,
                       const std::vector<std::string>& arguments,
                       const Workspace&, Quoting, const char *format);
// Publish auxiliary files only after image validation. Empty destination means
// in-memory-only: the staged file is discarded with the workspace.
void publish(const std::filesystem::path& staged, const std::string& destination);
void checkDestinations(const std::string& image, const std::vector<std::string>& auxiliary);
void rejectIgnoredOptions(const std::string& diagnostics);
std::string lower(std::string);
// Number of separate values consumed by a native option (for CLI sniffing).
unsigned optionArity(const std::string&);
}
}
