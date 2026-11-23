#include <cephyr/cache.hpp>
#include <cephyr/diagnostics.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory() {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                   ("cephyr-cache-test-" + std::to_string(seed) + "-" +
                    std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path, error))
                return;
        }
        throw std::runtime_error("cannot create temporary cache test directory");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

std::string diagnostics_text(const cephyr::Diagnostics &diagnostics) {
    std::ostringstream output;
    diagnostics.render(output);
    return output.str();
}

void corrupt_record(const std::filesystem::path &directory, std::string_view key) {
    // Locate the record by its stored key, independently of the filename layout.
    for (const auto &file : std::filesystem::directory_iterator(directory)) {
        std::fstream io(file.path(), std::ios::binary | std::ios::in | std::ios::out);
        io.seekg(24);
        std::string found(key.size(), '\0');
        if (!io.read(found.data(), found.size()) || found != key) continue;
        io.seekg(-1, std::ios::end);
        char byte;
        io.get(byte);
        io.seekp(-1, std::ios::end);
        io.put(byte ^ 1);
        assert(io.good());
        return;
    }
    throw std::runtime_error("missing record to corrupt");
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    const auto directory = temporary.path / "cache";
    constexpr std::string_view first_key =
        "archon-native:"
        "1a7a7d15b877c39d71b642d7f92f2c7356c2fdf2bc73973169cd8d35df0e3508";
    constexpr std::string_view second_key =
        "archon-native:"
        "426b659115a2cf817efe68f4dd0e31a5b30bbfc5c3682222fb0dff2e54be4afe";
    const cephyr::GeneratedFiles files{
        {"IR.c", "int generated(void) { return 1; }\n"},
        {"include/IR.h", "#pragma once\nint generated(void);\n"},
    };

    // An absent embedding cache path is an intentional silent disable.
    {
        cephyr::Diagnostics diagnostics;
        cephyr::Cache disabled({}, diagnostics);
        disabled.put(first_key, files);
        assert(!disabled.get(first_key));
        assert(diagnostics_text(diagnostics).empty());
    }

    {
        cephyr::Diagnostics diagnostics;
        cephyr::Cache cache(directory, diagnostics);
        assert(!cache.get(first_key));
        assert(diagnostics_text(diagnostics).empty()); // ordinary miss

        cache.put(first_key, files);
        const auto hit = cache.get(first_key);
        assert(hit && *hit == files);

        // A changed complete generation fingerprint invalidates the entry.
        assert(!cache.get(second_key));
        assert(diagnostics_text(diagnostics).empty());

        const cephyr::GeneratedFiles changed{{"IR.c", "changed\n"}};
        cache.put(second_key, changed);
        // Invalid replacements must not discard the previous complete tree.
        cache.put(second_key, {{"../bad", "ignored"}});
        assert(cache.get(first_key) == files);
        assert(cache.get(second_key) == changed);
    }

#ifdef __linux__
    constexpr auto others = std::filesystem::perms::group_all |
                            std::filesystem::perms::others_all;
    assert((std::filesystem::status(directory).permissions() & others) == std::filesystem::perms::none);
    for (const auto &file : std::filesystem::directory_iterator(directory))
        assert((file.status().permissions() & others) == std::filesystem::perms::none);

    const auto expect_disabled = [&](const std::filesystem::path &path, std::string_view reason) {
        cephyr::Diagnostics diagnostics;
        cephyr::Cache cache(path, diagnostics);
        cache.put(first_key, files);
        assert(!cache.get(first_key));
        assert(diagnostics_text(diagnostics).find(reason) != std::string::npos);
    };
    const auto cache_alias = temporary.path / "cache-alias";
    std::filesystem::create_directory_symlink(directory, cache_alias);
    expect_disabled(cache_alias / "", "final symlink");

    const auto unsafe = temporary.path / "unsafe";
    std::filesystem::create_directory(unsafe);
    std::filesystem::permissions(unsafe, std::filesystem::perms::group_write,
                                 std::filesystem::perm_options::add);
    expect_disabled(unsafe, "group/world-writable");
    assert(std::filesystem::is_empty(unsafe));
    std::filesystem::permissions(unsafe, std::filesystem::perms::group_write,
                                 std::filesystem::perm_options::remove);

    const auto unrelated = temporary.path / "unrelated";
    std::ofstream(unrelated) << "not cache data";
    // Use the actual destination of first_key for the substitution tests.
    { cephyr::Diagnostics d; cephyr::Cache cache(unsafe, d); cache.put(first_key, files); }
    const auto record = std::filesystem::directory_iterator(unsafe)->path();
    std::filesystem::remove(record);
    std::filesystem::create_symlink(unrelated, record);
    expect_disabled(unsafe, "unsafe file type or link");
    std::filesystem::remove(record);
    std::filesystem::create_hard_link(unrelated, record);
    expect_disabled(unsafe, "unsafe file type or link");
    assert(std::filesystem::file_size(unrelated) == std::string_view("not cache data").size());
#endif

    // Invalid file names are never persisted or returned to a publisher.
    {
        cephyr::Diagnostics diagnostics;
        cephyr::Cache cache(directory, diagnostics);
        assert(cache.get(first_key) == files); // persisted across environment reopen
        cache.put("unsafe-path", {{"../escape", "not published\n"}});
        assert(!cache.get("unsafe-path"));
        assert(diagnostics_text(diagnostics).find("cephyr.cache") != std::string::npos);
    }

    // A damaged value is a warned miss, not generated output or a fatal error.
    corrupt_record(directory, first_key);
    {
        cephyr::Diagnostics diagnostics;
        cephyr::Cache cache(directory, diagnostics);
        assert(!cache.get(first_key));
        const auto warning = diagnostics_text(diagnostics);
        assert(warning.find("cephyr.cache") != std::string::npos);
        assert(warning.find("read failed") != std::string::npos);
        assert(warning.find("checksum mismatch") != std::string::npos);
    }

    // Keys are never truncated to fit and therefore cannot alias.
    {
        cephyr::Diagnostics diagnostics;
        cephyr::Cache cache(directory, diagnostics);
        std::string binary(511, '/');
        binary[0] = '\0';
        cache.put(binary, files);
        assert(cache.get(binary) == files);
        binary.back() = '.';
        assert(!cache.get(binary));
        const std::string oversized_key(4096, 'x');
        cache.put(oversized_key, files);
        assert(!cache.get(oversized_key));
        assert(diagnostics_text(diagnostics).find("key limit") != std::string::npos);
    }

#if defined(__unix__) || defined(__APPLE__)
    // Atomic replacement permits independent writers without exposing a
    // partially written generation.
    constexpr std::string_view process_keys[] = {"process-0", "process-1"};
    pid_t children[2]{};
    for (unsigned index = 0; index != 2; ++index) {
        children[index] = ::fork();
        assert(children[index] >= 0);
        if (children[index] == 0) {
            int result = 1;
            {
                cephyr::Diagnostics diagnostics;
                cephyr::Cache cache(directory, diagnostics);
                const cephyr::GeneratedFiles output{
                    {"process.txt", std::to_string(index) + "\n"}};
                const cephyr::GeneratedFiles payload{{"a", std::string(8192, char('a' + index))},
                                                    {"b", std::string(8192, char('a' + index))}};
                for (unsigned round = 0; round < 20; ++round) {
                    cache.put("contended", payload);
                    auto snapshot = cache.get("contended");
                    if (!snapshot || snapshot->at("a") != snapshot->at("b")) ::_exit(2);
                }
                cache.put(process_keys[index], output);
                const auto hit = cache.get(process_keys[index]);
                result = hit && *hit == output ? 0 : 1;
            }
            ::_exit(result);
        }
    }
    for (pid_t child : children) {
        int status = 0;
        assert(::waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    {
        cephyr::Diagnostics diagnostics;
        cephyr::Cache cache(directory, diagnostics);
        for (unsigned index = 0; index != 2; ++index) {
            const auto hit = cache.get(process_keys[index]);
            assert(hit && hit->at("process.txt") == std::to_string(index) + "\n");
        }
        assert(diagnostics_text(diagnostics).empty());
    }
#endif
}
