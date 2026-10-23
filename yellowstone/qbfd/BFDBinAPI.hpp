// C++20 ownership and collection helpers over the compiled ArchBFD C ABI.
#pragma once
#include "BFDBinABI.h"

#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace archbfd {
inline std::string_view text(archbfd_string s) {
  return s.size ? std::string_view(s.data, s.size) : std::string_view{};
}
inline std::span<const uint8_t> bytes(archbfd_bytes s) { return {s.data, s.size}; }

class Error : public std::runtime_error {
 public:
  Error(archbfd_status code, const std::string& message)
      : std::runtime_error(message), code_(code) {}
  archbfd_status code() const noexcept { return code_; }
 private:
  archbfd_status code_;
};

namespace detail {
template <class T, class F>
T get(F&& operation) {
  T result{};
  archbfd_error error{};
  auto status = operation(&result, &error);
  if (status != ARCHBFD_OK)
    throw Error(status, error.message[0] ? error.message : archbfd_status_name(status));
  return result;
}
struct ObjectDeleter {
  void operator()(archbfd_object* p) const noexcept { archbfd_close(p); }
};
struct ArchiveDeleter {
  void operator()(archbfd_archive* p) const noexcept { archbfd_archive_close(p); }
};
} // namespace detail

class Archive;
class Binary {
 public:
  Binary() = default;
  Binary(Binary&&) noexcept = default;
  Binary& operator=(Binary&&) noexcept = default;
  Binary(const Binary&) = delete;
  Binary& operator=(const Binary&) = delete;

  static Binary open(const std::filesystem::path& path, const char* target = nullptr) {
    auto name = path.string();
    return Binary(detail::get<archbfd_object*>([&](auto out, auto error) {
      return archbfd_open_file(name.c_str(), target, out, error);
    }));
  }
  static Binary fromMemory(std::span<const uint8_t> data, const char* target = nullptr) {
    return Binary(detail::get<archbfd_object*>([&](auto out, auto error) {
      return archbfd_open_memory(data.data(), data.size(), target, out, error);
    }));
  }
  static Binary machoSlice(std::span<const uint8_t> data, size_t slot) {
    return Binary(detail::get<archbfd_object*>([&](auto out, auto error) {
      return archbfd_open_macho_slice(data.data(), data.size(), slot, out, error);
    }));
  }
  explicit operator bool() const noexcept { return bool(handle_); }
  // Borrowed handle: callers must not close it or transfer its ownership.
  const archbfd_object* nativeHandle() const noexcept { return handle_.get(); }
  archbfd_object_info info() const {
    return detail::get<archbfd_object_info>([&](auto out, auto error) {
      return archbfd_get_object_info(handle_.get(), out, error);
    });
  }
  std::span<const uint8_t> data() const {
    return bytes(detail::get<archbfd_bytes>([&](auto out, auto error) {
      return archbfd_get_bytes(handle_.get(), out, error);
    }));
  }
  archbfd_section section(size_t slot) const {
    return detail::get<archbfd_section>([&](auto out, auto error) {
      return archbfd_get_section(handle_.get(), slot, out, error);
    });
  }
  std::vector<archbfd_section> sections() const {
    std::vector<archbfd_section> result;
    auto count = info().section_count;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) result.push_back(section(i));
    return result;
  }
  std::span<const uint8_t> sectionBytes(size_t slot) const {
    return bytes(detail::get<archbfd_bytes>([&](auto out, auto error) {
      return archbfd_get_section_bytes(handle_.get(), slot, out, error);
    }));
  }
  archbfd_symbol symbol(size_t slot, uint32_t table = ARCHBFD_STATIC_SYMBOLS) const {
    return detail::get<archbfd_symbol>([&](auto out, auto error) {
      return archbfd_get_symbol(handle_.get(), table, slot, out, error);
    });
  }
  std::vector<archbfd_symbol> symbols(uint32_t table = ARCHBFD_STATIC_SYMBOLS) const {
    if (table > ARCHBFD_DYNAMIC_SYMBOLS)
      throw Error(ARCHBFD_INVALID_ARGUMENT, "invalid symbol table selector");
    auto metadata = info();
    auto count = table == ARCHBFD_STATIC_SYMBOLS ? metadata.symbol_count
                                                : metadata.dynamic_symbol_count;
    std::vector<archbfd_symbol> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) result.push_back(symbol(i, table));
    return result;
  }
  std::vector<archbfd_relocation> relocations(size_t sectionSlot) const {
    auto count = detail::get<size_t>([&](auto out, auto error) {
      return archbfd_get_relocation_count(handle_.get(), sectionSlot, out, error);
    });
    std::vector<archbfd_relocation> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i)
      result.push_back(detail::get<archbfd_relocation>([&](auto out, auto error) {
        return archbfd_get_relocation(handle_.get(), sectionSlot, i, out, error);
      }));
    return result;
  }
  std::vector<archbfd_segment> segments() const {
    auto count = info().segment_count;
    std::vector<archbfd_segment> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i)
      result.push_back(detail::get<archbfd_segment>([&](auto out, auto error) {
        return archbfd_get_segment(handle_.get(), i, out, error);
      }));
    return result;
  }
  // Unlike record string views, these strings are owned by the returned vector.
  std::vector<std::string> dependencies() const {
    auto count = info().dependency_count;
    std::vector<std::string> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i)
      result.emplace_back(text(detail::get<archbfd_string>([&](auto out, auto error) {
        return archbfd_get_dependency(handle_.get(), i, out, error);
      })));
    return result;
  }
  std::span<const uint8_t> bytesAtAddress(uint64_t address, uint64_t size) const {
    return bytes(detail::get<archbfd_bytes>([&](auto out, auto error) {
      return archbfd_bytes_at_address(handle_.get(), address, size, out, error);
    }));
  }

 private:
  friend class Archive;
  explicit Binary(archbfd_object* p) : handle_(p) {}
  std::unique_ptr<archbfd_object, detail::ObjectDeleter> handle_;
};

class Archive {
 public:
  Archive() = default;
  Archive(Archive&&) noexcept = default;
  Archive& operator=(Archive&&) noexcept = default;
  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;

  static Archive open(const std::filesystem::path& path) {
    auto name = path.string();
    return Archive(detail::get<archbfd_archive*>([&](auto out, auto error) {
      return archbfd_archive_open_file(name.c_str(), out, error);
    }));
  }
  static Archive fromMemory(std::span<const uint8_t> data, const char* label = nullptr) {
    return Archive(detail::get<archbfd_archive*>([&](auto out, auto error) {
      return archbfd_archive_open_memory(data.data(), data.size(), label, out, error);
    }));
  }
  explicit operator bool() const noexcept { return bool(handle_); }
  size_t size() const {
    return detail::get<size_t>([&](auto out, auto error) {
      return archbfd_archive_member_count(handle_.get(), out, error);
    });
  }
  archbfd_member member(size_t slot) const {
    return detail::get<archbfd_member>([&](auto out, auto error) {
      return archbfd_archive_get_member(handle_.get(), slot, out, error);
    });
  }
  Binary openMember(size_t slot) {
    return Binary(detail::get<archbfd_object*>([&](auto out, auto error) {
      return archbfd_archive_open_member(handle_.get(), slot, out, error);
    }));
  }
 private:
  explicit Archive(archbfd_archive* p) : handle_(p) {}
  std::unique_ptr<archbfd_archive, detail::ArchiveDeleter> handle_;
};

inline std::vector<archbfd_macho_slice> machoSlices(std::span<const uint8_t> data) {
  auto count = detail::get<size_t>([&](auto out, auto error) {
    return archbfd_macho_slice_count(data.data(), data.size(), out, error);
  });
  std::vector<archbfd_macho_slice> result;
  result.reserve(count);
  for (size_t i = 0; i < count; ++i)
    result.push_back(detail::get<archbfd_macho_slice>([&](auto out, auto error) {
      return archbfd_get_macho_slice(data.data(), data.size(), i, out, error);
    }));
  return result;
}
} // namespace archbfd
