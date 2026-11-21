#ifndef CEPHYR_DRIVER_IO_HPP
#define CEPHYR_DRIVER_IO_HPP

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <string_view>
#include <system_error>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace cephyr::detail {

class MemoryFile {
public:
    explicit MemoryFile(const char *label)
    {
#ifdef SYS_memfd_create
        fd_ = static_cast<int>(syscall(SYS_memfd_create, label, MFD_CLOEXEC));
        if (fd_ < 0 && errno != ENOSYS && errno != EPERM && errno != EACCES && errno != EINVAL)
            throw std::system_error(errno, std::generic_category(), "memfd_create");
        if (fd_ >= 0 && fd_ <= STDERR_FILENO) {
            const int next = fcntl(fd_, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            const int error = errno;
            close(fd_);
            fd_ = next;
            if (next < 0)
                throw std::system_error(error, std::generic_category(), "duplicate memfd");
        }
        if (fd_ >= 0 && access("/proc/self/fd", X_OK) != 0) {
            close(fd_);
            fd_ = -1;
        }
#endif
    }
    ~MemoryFile() { if (fd_ >= 0) close(fd_); }
    MemoryFile(const MemoryFile &) = delete;
    MemoryFile &operator=(const MemoryFile &) = delete;
    MemoryFile(MemoryFile &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    int fd() const { return fd_; }
    std::filesystem::path path() const { return "/proc/self/fd/" + std::to_string(fd_); }

    void write(std::string_view bytes) const
    {
        if (ftruncate(fd_, static_cast<off_t>(bytes.size())) < 0)
            throw std::filesystem::filesystem_error("cannot size memory file", path(),
                                                    {errno, std::generic_category()});
        if (!bytes.empty()) {
            // Reserve backing pages first so exhaustion is reported as an
            // I/O failure, rather than SIGBUS during the mapped copy.
            const int error = posix_fallocate(fd_, 0, static_cast<off_t>(bytes.size()));
            if (error)
                throw std::filesystem::filesystem_error("cannot allocate memory file", path(),
                                                        {error, std::generic_category()});
            Mapping map(fd_, bytes.size(), PROT_READ | PROT_WRITE);
            std::memcpy(map.data, bytes.data(), bytes.size());
        }
    }
    std::string read(std::size_t size) const
    {
        Mapping map(fd_, size, PROT_READ);
        return std::string(static_cast<const char *>(map.data), size);
    }

private:
    struct Mapping {
        void *data;
        std::size_t size;
        Mapping(int fd, std::size_t size, int protection)
            : data(mmap(nullptr, size, protection, MAP_SHARED, fd, 0)), size(size)
        {
            if (data == MAP_FAILED)
                throw std::filesystem::filesystem_error("cannot map memory file",
                                                        {errno, std::generic_category()});
        }
        ~Mapping() { munmap(data, size); }
        Mapping(const Mapping &) = delete;
        Mapping &operator=(const Mapping &) = delete;
    };
    int fd_ = -1;
};

class TempDirectory {
public:
    TempDirectory(const std::filesystem::path &parent, const char *pattern)
    {
        auto name = (parent / pattern).string();
        if (!mkdtemp(name.data()))
            throw std::filesystem::filesystem_error("cannot create temporary directory", parent,
                std::error_code(errno, std::generic_category()));
        path = std::move(name);
    }
    ~TempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored); // only the private directory created by mkdtemp
    }
    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;
    std::filesystem::path path;
};

inline void save(const std::filesystem::path &path, std::string_view bytes)
{
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream)
        throw std::filesystem::filesystem_error("cannot write output", path,
            std::error_code(errno ? errno : EIO, std::generic_category()));
}

} // namespace cephyr::detail

#endif
