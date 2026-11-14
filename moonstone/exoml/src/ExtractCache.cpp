#include "ExtractCache.hpp"
#include "ExtractorBuild.hpp"
#include <cerrno>
#include <filesystem>
#include <openssl/evp.h>
#include <system_error>
#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace moonstone::exoml::cdecl::detail {
std::string sha256(std::string_view bytes) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), result, &length, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 failed");
    std::string out;
    for (unsigned i = 0; i < length; ++i) {
        out += "0123456789abcdef"[result[i] >> 4];
        out += "0123456789abcdef"[result[i] & 15];
    }
    return out;
}
std::string normalization_cache_key(const TranslationUnit &evidence, const ExtractLimits &l,
                                    std::string_view ast_hash, std::string_view build_identity) {
    return sha256(std::string(build_identity) + ':' + std::string(extractor_version) + ':' +
                  std::string(ast_hash) + ':' + encode_cache_unit(evidence) + ':' +
                  std::to_string(l.declarations) + ':' + std::to_string(l.types) + ':' +
                  std::to_string(l.depth) + ':' + std::to_string(l.input_bytes) + ':' +
                  std::to_string(l.entries) + ':' + std::to_string(l.cache_bytes));
}
namespace {
#ifdef __linux__
constexpr std::string_view magic = "EXOML-CACHE-2\n";
[[noreturn]] void failure(const std::string &operation) {
    auto error = std::error_code(errno, std::generic_category());
    throw std::runtime_error("discovery cache: " + operation + ": " + error.message());
}
struct File {
    int fd = -1;
    explicit File(int descriptor = -1) : fd(descriptor) {}
    ~File() {
        if (fd >= 0)
            ::close(fd);
    }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
};
struct Temporary {
    std::string path;
    File file;
    explicit Temporary(const std::string &directory)
        : path(directory + "/.pending-XXXXXX"), file(::mkstemp(path.data())) {
        if (file.fd < 0)
            failure("create private temporary");
        if (::fcntl(file.fd, F_SETFD, FD_CLOEXEC) < 0) {
            auto error = errno;
            ::unlink(path.c_str());
            errno = error;
            failure("mark temporary close-on-exec");
        }
    }
    ~Temporary() {
        if (!path.empty())
            ::unlink(path.c_str());
    }
};
std::string read_file(int fd, std::size_t limit) {
    struct stat info{};
    if (::fstat(fd, &info) < 0)
        failure("stat artifact");
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) ||
        info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > limit)
        throw std::invalid_argument("discovery cache: nonprivate/nonregular/oversized artifact");
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto n = ::pread(fd, bytes.data() + offset, bytes.size() - offset, off_t(offset));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            failure("read artifact");
        if (n == 0)
            throw std::invalid_argument("discovery cache: truncated artifact");
        offset += std::size_t(n);
    }
    return bytes;
}
void write_file(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        auto n = ::write(fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            failure("write artifact");
        bytes.remove_prefix(std::size_t(n));
    }
    if (::fsync(fd) < 0)
        failure("sync artifact");
}
#endif
} // namespace
struct NormalizationCache::Impl {
    const ExtractOptions &options;
    CacheReport &report;
    TranslationUnit evidence;
    std::string directory;
#ifdef __linux__
    File root;
#endif
    Impl(const ExtractOptions &o, CacheReport &r, const TranslationUnit &u)
        : options(o), report(r), evidence(u) {}
};

NormalizationCache::NormalizationCache(const ExtractOptions &o, CacheReport &r,
                                       const TranslationUnit &evidence, CXTranslationUnit tu)
    : impl_(std::make_unique<Impl>(o, r, evidence)) {
#ifdef __linux__
    if (o.cache_directory.find('\0') != std::string::npos)
        throw std::invalid_argument("NUL in cache directory");
    auto &p = *impl_;
    p.directory = std::filesystem::absolute(o.cache_directory).lexically_normal().string();
    if (::mkdir(p.directory.c_str(), 0700) < 0 && errno != EEXIST)
        failure("create directory (parent must exist)");
    p.root.fd = ::open(p.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (p.root.fd < 0)
        failure("open directory without following symlinks");
    struct stat info{};
    if (::fstat(p.root.fd, &info) < 0)
        failure("stat directory");
    if (info.st_uid != ::geteuid() || (info.st_mode & 0077))
        throw std::invalid_argument(
            "discovery cache directory must be user-owned and private (0700)");
    // Resolve through the opened directory descriptor even if an ancestor is renamed.
    p.directory = "/proc/self/fd/" + std::to_string(p.root.fd);
    Temporary native(p.directory);
    auto saved = clang_saveTranslationUnit(tu, native.path.c_str(), clang_defaultSaveOptions(tu));
    if (saved != CXSaveError_None)
        throw std::runtime_error("cannot serialize fresh compiler AST for cache key: " +
                                 std::to_string(saved));
    File ast(::open(native.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (ast.fd < 0)
        failure("open fresh AST");
    // Clang may replace the exclusive temporary with a default-mode file. It
    // is still inside our private directory; narrow its mode before reading.
    if (::fchmod(ast.fd, 0600) < 0)
        failure("make fresh AST private");
    auto ast_hash = sha256(read_file(ast.fd, o.limits.cache_bytes));
    r.key = normalization_cache_key(evidence, o.limits, ast_hash, EXOML_EXTRACT_BUILD_ID);
    r.status = CacheStatus::Miss;
#else
    (void)tu;
    throw std::runtime_error("discovery normalization caching is supported on Linux only");
#endif
}
NormalizationCache::~NormalizationCache() = default;

std::optional<TranslationUnit> NormalizationCache::load() {
#ifdef __linux__
    auto &p = *impl_;
    auto filename = p.report.key + ".cdecl-cache";
    File file(
        ::openat(p.root.fd, filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.fd < 0 && errno == ENOENT)
        return std::nullopt;
    try {
        if (file.fd < 0)
            failure("open artifact without following symlinks");
        auto bytes = read_file(file.fd, p.options.limits.cache_bytes);
        std::string prefix = std::string(magic) + p.report.key + '\n';
        if (!bytes.starts_with(prefix) || bytes.size() < prefix.size() + 65)
            throw std::invalid_argument(
                "discovery cache: wrong artifact version/key or truncated envelope");
        auto hash = std::string_view(bytes).substr(prefix.size(), 64);
        auto payload = std::string_view(bytes).substr(prefix.size() + 65);
        if (bytes[prefix.size() + 64] != '\n' || sha256(payload) != hash)
            throw std::invalid_argument("discovery cache: artifact checksum mismatch");
        auto unit = decode_cache_unit(payload, p.options.limits);
        auto expected = unit;
        expected.types.clear();
        expected.declarations.clear();
        expected.fingerprint.clear();
        if (encode_cache_unit(expected) != encode_cache_unit(p.evidence))
            throw std::invalid_argument("discovery cache: artifact provenance drift");
        p.report.status = CacheStatus::Hit;
        return unit;
    } catch (const std::exception &e) {
        p.report.status = CacheStatus::Rebuilt;
        p.report.reason = e.what();
        return std::nullopt;
    }
#else
    throw std::runtime_error("discovery normalization caching is supported on Linux only");
#endif
}
void NormalizationCache::store(const TranslationUnit &unit) {
#ifdef __linux__
    auto &p = *impl_;
    auto payload = encode_cache_unit(unit);
    auto bytes = std::string(magic) + p.report.key + '\n' + sha256(payload) + '\n' + payload;
    if (bytes.size() > p.options.limits.cache_bytes)
        throw std::length_error("discovery cache artifact byte budget exceeded");
    Temporary temporary(p.directory);
    write_file(temporary.file.fd, bytes);
    auto filename = p.report.key + ".cdecl-cache";
    if (::renameat(p.root.fd, std::filesystem::path(temporary.path).filename().c_str(), p.root.fd,
                   filename.c_str()) < 0)
        failure("atomically publish artifact");
    temporary.path.clear();
    p.report.stored = true;
#else
    (void)unit;
    throw std::runtime_error("discovery normalization caching is supported on Linux only");
#endif
}
} // namespace moonstone::exoml::cdecl::detail
