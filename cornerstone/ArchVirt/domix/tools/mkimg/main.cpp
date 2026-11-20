#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <stdexcept>

#include <toml++/toml.hpp>

namespace fs = std::filesystem;

namespace {

struct Args {
    fs::path kernel;
    fs::path rootfs;
    fs::path manifest;
    fs::path output;
};

struct FileEntry {
    std::string path;
    std::vector<std::uint8_t> bytes;
    std::uint32_t mode{};
};

template <class T>
void write_pod(std::ostream& out, const T& value) {
    for (unsigned i = 0; i < sizeof(T); ++i) out.put(static_cast<char>(value >> (i * 8)));
}

bool read_all(const fs::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool parse_args(int argc, char** argv, Args& args) {
    bool seen_kernel = false, seen_rootfs = false, seen_manifest = false, seen_output = false;
    for (int i = 1; i < argc; ++i) {
        if (i + 1 >= argc) return false;
        const std::string key = argv[i];
        const fs::path value = argv[++i];
        if (key == "--kernel" && !seen_kernel) { args.kernel = value; seen_kernel = true; }
        else if (key == "--rootfs" && !seen_rootfs) { args.rootfs = value; seen_rootfs = true; }
        else if (key == "--manifest" && !seen_manifest) { args.manifest = value; seen_manifest = true; }
        else if (key == "--output" && !seen_output) { args.output = value; seen_output = true; }
        else return false;
    }
    return !args.kernel.empty() && !args.rootfs.empty() && !args.manifest.empty() && !args.output.empty();
}

bool valid_relative_path(const fs::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& component : path) {
        if (component == "." || component == "..") return false;
    }
    return true;
}

std::vector<FileEntry> collect_rootfs(const fs::path& rootfs) {
    std::vector<FileEntry> entries;
    std::error_code ec;
    if (!fs::is_directory(rootfs, ec)) throw std::runtime_error("rootfs is not a directory");
    for (fs::recursive_directory_iterator it(rootfs, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_symlink(ec)) {
            throw std::runtime_error("Domix image does not support symbolic links: " + it->path().string());
        }
        const bool directory = it->is_directory(ec);
        if (!directory && !it->is_regular_file(ec)) throw std::runtime_error("Unsupported rootfs entry");
        const auto rel = fs::relative(it->path(), rootfs, ec);
        if (ec) throw std::runtime_error(ec.message());
        const auto rel_string = rel.generic_string();
        if (!valid_relative_path(rel)) {
            throw std::runtime_error("Invalid rootfs path");
        }
        std::vector<std::uint8_t> bytes;
        if (!directory && !read_all(it->path(), bytes)) throw std::runtime_error("Failed reading rootfs file");
        const auto perms = it->status(ec).permissions();
        if (ec) throw std::runtime_error(ec.message());
        std::uint32_t mode = 0644u;
        mode = static_cast<std::uint32_t>(perms) & 0777u;
        if (directory) mode |= 0040000;
        if (rel_string.size() > 126) throw std::runtime_error("Domix paths are limited to 126 bytes");
        entries.push_back({rel_string, std::move(bytes), mode});
    }
    if (ec) throw std::runtime_error(ec.message());
    if (entries.size() > 128) throw std::runtime_error("Domix images hold at most 128 entries");
    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.path < b.path;
    });
    return entries;
}

} // namespace

int main(int argc, char** argv) try {
    Args args;
    if (!parse_args(argc, argv, args)) {
        std::cerr << "usage: domix_mkimg --kernel ... --rootfs ... --manifest ... --output ...\n";
        return 1;
    }
    std::error_code fs_ec;
    if (!fs::is_regular_file(args.kernel, fs_ec) || !fs::is_regular_file(args.manifest, fs_ec) ||
        !fs::is_directory(args.rootfs, fs_ec)) {
        std::cerr << "kernel and manifest must be files; rootfs must be a directory\n";
        return 1;
    }

    toml::table manifest;
    try {
        manifest = toml::parse_file(args.manifest.string());
    } catch (const toml::parse_error& err) {
        std::cerr << err.description() << '\n';
        return 1;
    }
    const auto format = manifest["image_format_version"].value<std::int64_t>();
    const auto shell = manifest["default_shell"].value<std::string>();
    const auto arch = manifest["architecture"].value<std::string>();
    if (!format || *format != 1 || !shell || shell->empty() ||
        !arch || (*arch != "x86_64" && *arch != "aarch64" && *arch != "riscv64")) {
        std::cerr << "manifest must declare image_format_version=1, default_shell, and a supported architecture\n";
        return 1;
    }

    std::vector<std::uint8_t> kernel;
    if (!read_all(args.kernel, kernel)) {
        std::cerr << "failed to read kernel\n";
        return 1;
    }
    if (kernel.size() < 64 || kernel[0] != 0x7f || kernel[1] != 'E' ||
        kernel[2] != 'L' || kernel[3] != 'F' || kernel[4] != 2 ||
        kernel[5] != 1) {
        std::cerr << "kernel must be a little-endian 64-bit ELF image\n";
        return 1;
    }
    const std::uint32_t machine = *arch == "riscv64" ? 243u : *arch == "aarch64" ? 183u : 62u;
    if ((kernel[18] | (static_cast<unsigned>(kernel[19]) << 8)) != machine) {
        std::cerr << "kernel architecture does not match manifest\n";
        return 1;
    }

    std::ifstream manifest_file(args.manifest, std::ios::binary);
    const std::vector<std::uint8_t> manifest_bytes{
        std::istreambuf_iterator<char>(manifest_file), std::istreambuf_iterator<char>()
    };
    if (!manifest_file) {
        std::cerr << "failed to read manifest\n";
        return 1;
    }

    const auto rootfs_entries = collect_rootfs(args.rootfs);
    if (rootfs_entries.empty()) {
        std::cerr << "rootfs contains no regular files\n";
        return 1;
    }
    if (std::none_of(rootfs_entries.begin(), rootfs_entries.end(), [](const auto& entry) {
            return entry.path == "init" && !(entry.mode & 0040000);
        })) throw std::runtime_error("Domix image requires an /init script");
    std::uint64_t total = 40 + kernel.size() + manifest_bytes.size();
    for (const auto& entry : rootfs_entries) total += 16 + entry.path.size() + entry.bytes.size();
    if (total > 24 * 1024 * 1024) throw std::runtime_error("Domix image exceeds the 24 MiB boot region");
    if (const auto* required = manifest["required_tools"].as_array()) {
        for (const auto& node : *required) {
            const auto tool = node.value<std::string>();
            if (tool && (!valid_relative_path(*tool) ||
                         !fs::is_regular_file(args.rootfs / "bin" / *tool, fs_ec))) {
                std::cerr << "rootfs is missing required tool: " << *tool << '\n';
                return 1;
            }
        }
    }

    if (!args.output.parent_path().empty()) {
        fs::create_directories(args.output.parent_path());
    }
    const fs::path temp_output = args.output.string() + ".tmp";
    std::ofstream out(temp_output, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "failed to open output\n";
        return 1;
    }

    out.write("DOMIXIMG", 8);
    write_pod(out, std::uint32_t{1}); write_pod(out, machine);
    write_pod(out, static_cast<std::uint64_t>(kernel.size()));
    write_pod(out, static_cast<std::uint64_t>(manifest_bytes.size()));
    write_pod(out, static_cast<std::uint64_t>(rootfs_entries.size()));
    out.write(reinterpret_cast<const char*>(kernel.data()), static_cast<std::streamsize>(kernel.size()));
    out.write(reinterpret_cast<const char*>(manifest_bytes.data()), static_cast<std::streamsize>(manifest_bytes.size()));

    for (const auto& entry : rootfs_entries) {
        const std::uint32_t path_size = static_cast<std::uint32_t>(entry.path.size());
        const std::uint64_t data_size = static_cast<std::uint64_t>(entry.bytes.size());
        write_pod(out, path_size);
        out.write(entry.path.data(), static_cast<std::streamsize>(entry.path.size()));
        write_pod(out, entry.mode);
        write_pod(out, data_size);
        out.write(reinterpret_cast<const char*>(entry.bytes.data()), static_cast<std::streamsize>(entry.bytes.size()));
    }

    if (!out) {
        std::cerr << "failed to write output\n";
        out.close();
        fs::remove(temp_output, fs_ec);
        return 1;
    }
    out.close();
    fs::rename(temp_output, args.output, fs_ec);
    if (fs_ec) {
        std::cerr << "failed to publish output: " << fs_ec.message() << '\n';
        fs::remove(temp_output, fs_ec);
        return 1;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "domix_mkimg: " << error.what() << '\n';
    return 1;
}
