#include <cephyr/cache.hpp>

#include <cephyr/diagnostics.hpp>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <lmdb++.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cephyr {
namespace {

constexpr std::array<char, 8> record_magic = {'C', 'E', 'P', 'H', 'Y', 'R', 'C', '1'};
constexpr std::size_t max_files = 4096;
constexpr std::size_t max_output_bytes = 256ull * 1024 * 1024;
constexpr std::size_t max_path_bytes = 16 * 1024;
constexpr std::size_t max_path_bytes_total = 64ull * 1024 * 1024;
constexpr std::size_t max_record_bytes =
    max_output_bytes + max_path_bytes_total + max_files * 12ull + 1024;
constexpr std::uint64_t map_size_64 = 16ull * 1024 * 1024 * 1024;
constexpr std::size_t map_size_32 = 1024ull * 1024 * 1024;

class InvalidRecord : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string cache_message(std::string_view operation, std::string_view detail) {
    std::string message = "generation cache ";
    message.append(operation);
    message.append(": ");
    message.append(detail);
    return message;
}

bool valid_utf8(std::string_view text) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
    std::size_t at = 0;
    while (at < text.size()) {
        const unsigned char lead = bytes[at++];
        if (lead <= 0x7f)
            continue;

        unsigned trailing = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            trailing = 1;
            value = lead & 0x1f;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            trailing = 2;
            value = lead & 0x0f;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            trailing = 3;
            value = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (trailing > text.size() - at)
            return false;
        for (unsigned index = 0; index < trailing; ++index) {
            const unsigned char next = bytes[at++];
            if ((next & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

bool safe_output_path(std::string_view name) {
    if (name.empty() || name.size() > max_path_bytes || !valid_utf8(name) ||
        name.front() == '/' || name.back() == '/')
        return false;

    std::size_t component_start = 0;
    bool first = true;
    for (std::size_t index = 0; index <= name.size(); ++index) {
        if (index != name.size() && name[index] != '/') {
            const unsigned char character = static_cast<unsigned char>(name[index]);
            if (character == '\\' || character < 0x20 || character == 0x7f)
                return false;
            continue;
        }
        const auto component = name.substr(component_start, index - component_start);
        if (component.empty() || component == "." || component == ".." ||
            (first && component.find(':') != std::string_view::npos))
            return false;
        first = false;
        component_start = index + 1;
    }
    return true;
}

std::uint32_t crc32(std::string_view bytes) {
    std::uint32_t result = 0xffffffffu;
    for (unsigned char byte : bytes) {
        result ^= byte;
        for (unsigned bit = 0; bit != 8; ++bit)
            result = (result >> 1) ^ (0xedb88320u & (0u - (result & 1u)));
    }
    return ~result;
}

void append_u32(std::string &output, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        output.push_back(static_cast<char>((value >> shift) & 0xff));
}

void append_u64(std::string &output, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        output.push_back(static_cast<char>((value >> shift) & 0xff));
}

class Reader {
    std::string_view input_;
    std::size_t offset_ = 0;

public:
    explicit Reader(std::string_view input) : input_(input) {}

    std::size_t remaining() const { return input_.size() - offset_; }

    std::string_view take(std::size_t count) {
        if (count > remaining())
            throw InvalidRecord("truncated record");
        const auto result = input_.substr(offset_, count);
        offset_ += count;
        return result;
    }

    std::uint32_t u32() {
        const auto bytes = take(4);
        std::uint32_t result = 0;
        for (unsigned index = 0; index != 4; ++index)
            result |= std::uint32_t(static_cast<unsigned char>(bytes[index])) << (index * 8);
        return result;
    }

    std::uint64_t u64() {
        const auto bytes = take(8);
        std::uint64_t result = 0;
        for (unsigned index = 0; index != 8; ++index)
            result |= std::uint64_t(static_cast<unsigned char>(bytes[index])) << (index * 8);
        return result;
    }
};

std::string serialize(std::string_view key, const GeneratedFiles &files) {
    if (key.empty())
        throw std::invalid_argument("empty fingerprint");
    if (key.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("fingerprint is too large");
    if (files.size() > max_files)
        throw std::length_error("generated file count exceeds 4096");

    std::size_t content_bytes = 0;
    std::size_t path_bytes = 0;
    for (const auto &[path, contents] : files) {
        if (!safe_output_path(path))
            throw std::invalid_argument("unsafe or invalid generated path");
        if (!valid_utf8(contents))
            throw std::invalid_argument("generated file is not valid UTF-8 text");
        if (contents.size() > max_output_bytes - content_bytes)
            throw std::length_error("generated text exceeds 256 MiB");
        content_bytes += contents.size();
        if (path.size() > max_path_bytes_total - path_bytes)
            throw std::length_error("generated paths exceed cache limit");
        path_bytes += path.size();
    }

    std::string output;
    const std::size_t estimated = record_magic.size() + 4 + 4 + 8 + key.size() +
                                  files.size() * 12 + path_bytes + content_bytes + 4;
    if (estimated > max_record_bytes)
        throw std::length_error("generated cache record exceeds cache limit");
    output.reserve(estimated);
    output.append(record_magic.data(), record_magic.size());
    append_u32(output, static_cast<std::uint32_t>(key.size()));
    append_u32(output, static_cast<std::uint32_t>(files.size()));
    append_u64(output, content_bytes);
    output.append(key);
    for (const auto &[path, contents] : files) {
        append_u32(output, static_cast<std::uint32_t>(path.size()));
        append_u64(output, contents.size());
        output.append(path);
        output.append(contents);
    }
    append_u32(output, crc32(output));
    return output;
}

GeneratedFiles deserialize(std::string_view record, std::string_view expected_key) {
    constexpr std::size_t fixed_size = record_magic.size() + 4 + 4 + 8 + 4;
    if (record.size() < fixed_size || record.size() > max_record_bytes)
        throw InvalidRecord("invalid record size");

    Reader reader(record);
    if (reader.take(record_magic.size()) !=
        std::string_view(record_magic.data(), record_magic.size()))
        throw InvalidRecord("wrong record format version");

    const auto key_size = reader.u32();
    const auto file_count = reader.u32();
    const auto declared_content_bytes = reader.u64();
    if (file_count > max_files || declared_content_bytes > max_output_bytes)
        throw InvalidRecord("record limits exceeded");
    if (reader.take(key_size) != expected_key)
        throw InvalidRecord("record fingerprint mismatch");

    GeneratedFiles files;
    std::size_t content_bytes = 0;
    std::size_t path_bytes = 0;
    for (std::uint32_t index = 0; index != file_count; ++index) {
        const auto path_size = reader.u32();
        const auto content_size = reader.u64();
        if (path_size > max_path_bytes || content_size > max_output_bytes ||
            content_size > std::numeric_limits<std::size_t>::max())
            throw InvalidRecord("file limits exceeded");
        if (path_size > max_path_bytes_total - path_bytes ||
            static_cast<std::size_t>(content_size) > max_output_bytes - content_bytes)
            throw InvalidRecord("aggregate file limits exceeded");

        const auto path_bytes_view = reader.take(path_size);
        const auto contents = reader.take(static_cast<std::size_t>(content_size));
        if (!safe_output_path(path_bytes_view) || !valid_utf8(contents))
            throw InvalidRecord("unsafe path or invalid UTF-8 text");
        path_bytes += path_size;
        content_bytes += static_cast<std::size_t>(content_size);
        if (!files.emplace(std::string(path_bytes_view), std::string(contents)).second)
            throw InvalidRecord("duplicate generated path");
    }
    if (content_bytes != declared_content_bytes || reader.remaining() != 4)
        throw InvalidRecord("record length mismatch");

    const std::uint32_t stored_crc = reader.u32();
    if (stored_crc != crc32(record.substr(0, record.size() - 4)))
        throw InvalidRecord("record checksum mismatch");
    return files;
}

std::size_t environment_map_size() {
    if constexpr (sizeof(std::size_t) >= 8)
        return static_cast<std::size_t>(map_size_64);
    return map_size_32;
}

class DirectoryHandle {
    int fd_;

public:
    explicit DirectoryHandle(int fd) : fd_(fd) {}
    ~DirectoryHandle() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    DirectoryHandle(const DirectoryHandle &) = delete;
    DirectoryHandle &operator=(const DirectoryHandle &) = delete;
    DirectoryHandle(DirectoryHandle &&other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

    int get() const { return fd_; }
};

[[noreturn]] void filesystem_failure(int error, const std::string &operation) {
    throw std::system_error(error, std::generic_category(), operation);
}

void validate_cache_node(const struct stat &status, bool directory,
                         const std::filesystem::path &path) {
    if ((directory && !S_ISDIR(status.st_mode)) ||
        (!directory && (!S_ISREG(status.st_mode) || status.st_nlink != 1)))
        throw std::invalid_argument("cache path has an unsafe file type or link: " +
                                    path.string());
    if (status.st_uid != ::geteuid())
        throw std::invalid_argument("cache path is not owned by the current user: " +
                                    path.string());
    if ((status.st_mode & 0022) != 0)
        throw std::invalid_argument("cache path is group/world-writable: " + path.string());
}

void validate_cache_file(int directory, const char *name, bool may_be_absent) {
    struct stat status {};
    if (::fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
        const int failure = errno;
        if (may_be_absent && failure == ENOENT)
            return;
        filesystem_failure(failure, std::string("inspect cache file ") + name);
    }
    validate_cache_node(status, false, name);
}

DirectoryHandle open_cache_directory(std::filesystem::path path) {
    if (path.native().find('\0') != std::string::npos)
        throw std::invalid_argument("cache directory contains NUL");
    while (!path.has_filename() && path != path.root_path())
        path = path.parent_path();
    const auto name = path.filename();
    if (name.empty() || name == "." || name == "..")
        throw std::invalid_argument("cache directory must name a dedicated directory");

    const auto parent_path = path.has_parent_path() ? path.parent_path() : ".";
    std::filesystem::create_directories(parent_path);
    const DirectoryHandle parent(
        ::open(parent_path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (parent.get() < 0)
        filesystem_failure(errno, "open cache parent directory");

    if (::mkdirat(parent.get(), name.c_str(), 0700) != 0 && errno != EEXIST)
        filesystem_failure(errno, "create private cache directory");
    DirectoryHandle directory(
        ::openat(parent.get(), name.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0)
        filesystem_failure(errno, "open cache directory without following final symlink");

    struct stat status {};
    if (::fstat(directory.get(), &status) != 0)
        filesystem_failure(errno, "inspect cache directory");
    validate_cache_node(status, true, path);
    return directory;
}

// lmdb++'s txn::commit throws before clearing its handle on error. LMDB
// consumes the transaction even if commit fails, so a local guard has to
// release the pointer before propagating the error.
class WriteTransaction : public lmdb::txn {
public:
    explicit WriteTransaction(lmdb::env &environment)
        : lmdb::txn(lmdb::txn::begin(environment)) {}

    void commit() {
        lmdb::txn_commit(std::exchange(_handle, nullptr));
    }
};

} // namespace

struct Cache::Impl {
    Diagnostics &diagnostics;
    std::optional<DirectoryHandle> directory;
    std::unique_ptr<lmdb::env> environment;
    std::optional<lmdb::dbi> database;
    int max_key_size = 0;

    explicit Impl(Diagnostics &value) : diagnostics(value) {}

    void warning(std::string_view operation, std::string_view detail) const {
        diagnostics.warning("cephyr.cache", cache_message(operation, detail), Location{});
    }

    bool valid_key(std::string_view key, std::string_view operation) const {
        if (key.empty()) {
            warning(operation, "empty generation fingerprint; bypassing cache");
            return false;
        }
        if (key.size() > static_cast<std::size_t>(max_key_size)) {
            warning(operation,
                    "generation fingerprint exceeds LMDB's key limit; bypassing cache");
            return false;
        }
        return true;
    }
};

Cache::Cache(const std::filesystem::path &directory, Diagnostics &diagnostics)
    : impl_(std::make_unique<Impl>(diagnostics)) {
    if (directory.empty())
        return;
    try {
        auto root = open_cache_directory(directory);
        validate_cache_file(root.get(), "data.mdb", true);
        validate_cache_file(root.get(), "lock.mdb", true);

        // LMDB accepts a pathname, not a directory fd. Resolve it through
        // the pinned root so renaming a parent cannot redirect its opens.
        const auto native = "/proc/self/fd/" + std::to_string(root.get());
        auto environment = lmdb::env::create();
        environment.set_mapsize(environment_map_size());
        environment.open(native.c_str(), MDB_NOTLS, 0600);
        validate_cache_file(root.get(), "data.mdb", false);
        validate_cache_file(root.get(), "lock.mdb", false);

        WriteTransaction transaction(environment);
        auto database = lmdb::dbi::open(transaction);
        transaction.commit();

        impl_->max_key_size = ::mdb_env_get_maxkeysize(environment);
        if (impl_->max_key_size <= 0)
            throw std::runtime_error("LMDB reported an invalid key limit");
        impl_->database.emplace(std::move(database));
        impl_->environment = std::make_unique<lmdb::env>(std::move(environment));
        impl_->directory.emplace(std::move(root));
    } catch (const std::exception &error) {
        impl_->warning("disabled", error.what());
        impl_->database.reset();
        impl_->environment.reset();
        impl_->directory.reset();
    } catch (...) {
        impl_->warning("disabled", "unknown initialization error");
        impl_->database.reset();
        impl_->environment.reset();
        impl_->directory.reset();
    }
}

Cache::~Cache() = default;

std::optional<GeneratedFiles> Cache::get(std::string_view key) {
    if (!impl_->environment || !impl_->database || !impl_->valid_key(key, "read"))
        return std::nullopt;
    try {
        auto transaction = lmdb::txn::begin(*impl_->environment, nullptr, MDB_RDONLY);
        const lmdb::val cache_key(key.data(), key.size());
        lmdb::val value;
        if (!impl_->database->get(transaction, cache_key, value))
            return std::nullopt; // An ordinary miss is intentionally silent.
        return deserialize(std::string_view(value.data(), value.size()), key);
    } catch (const std::exception &error) {
        impl_->warning("read failed", error.what());
    } catch (...) {
        impl_->warning("read failed", "unknown error");
    }
    return std::nullopt;
}

void Cache::put(std::string_view key, const GeneratedFiles &files) {
    if (!impl_->environment || !impl_->database || !impl_->valid_key(key, "write"))
        return;
    try {
        auto record = serialize(key, files);
        WriteTransaction transaction(*impl_->environment);
        const lmdb::val cache_key(key.data(), key.size());
        lmdb::val value(record.data(), record.size());
        impl_->database->put(transaction, cache_key, value);
        transaction.commit();
    } catch (const std::exception &error) {
        impl_->warning("write failed", error.what());
    } catch (...) {
        impl_->warning("write failed", "unknown error");
    }
}

} // namespace cephyr
