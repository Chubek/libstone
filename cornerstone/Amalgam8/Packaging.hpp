#pragma once

#include "Contracts.hpp"

#include <cstdint>
#include <fstream>

namespace am8::packaging {

struct Snapshot {
    std::string_view path, guard;
    std::size_t size;
    std::uint64_t fingerprint;
    std::string_view include, child;
};

// Reviewed snapshots: byte/shape drift must be inspected before extending the
// allowlist. Fingerprints detect incidental changes; the input tree is trusted.
inline constexpr Snapshot snapshots[] = {
    {"libbignum/BigFloat.hpp", "AMALGAM8_BUNDLED_FIRESTONE_LIBBIGNUM_BIGFLOAT_HPP",
     8795, 0x3239fca328ac02cdULL, "#include \"BigInt.hpp\"\n", "libbignum/BigInt.hpp"},
    {"libbignum/BigInt.hpp", "AMALGAM8_BUNDLED_FIRESTONE_LIBBIGNUM_BIGINT_HPP",
     12931, 0x1ed828fbaa863b92ULL, {}, {}},
    {"libbignum/Complex128.hpp", "AMALGAM8_BUNDLED_FIRESTONE_LIBBIGNUM_COMPLEX128_HPP",
     366, 0x2816b69f4387192aULL, {}, {}},
};
inline constexpr std::string_view mit_notice = R"license(MIT License

Copyright (c) 2026 Chubak Bidpaa <chubakbidpaa@riseup.net>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
)license";

inline const Snapshot* eligible(std::string_view path) {
    for (const auto& snapshot : snapshots)
        if (snapshot.path == path) return &snapshot;
    return nullptr;
}

inline std::uint64_t fingerprint(std::string_view source) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : source) hash = (hash ^ byte) * 1099511628211ULL;
    return hash;
}

inline fs::path source_root(const Syntax& use, const Options& options) {
    std::error_code error;
    auto root = options.include_root;
    if (root.empty()) root = fs::current_path(error);
    if (!error) root = fs::canonical(root, error);
    if (error || !fs::is_directory(root, error) || error)
        fail(use, "cannot locate a Libstone source root for bundling; set the include root");
    return root;
}

inline std::string read_snapshot(const fs::path& root, const fs::path& relative,
                                 std::size_t bytes, const Syntax& use) {
    std::error_code error;
    // Only exact, approved relative paths below the canonical root are permitted.
    // A symlink would change what source tree supplied the licensed bytes.
    const auto path = root / relative;
    if (fs::canonical(path, error) != path || error ||
        !fs::is_regular_file(path, error) || error)
        fail(use, "cannot read approved bundle input '" + relative.generic_string() +
                  "' from the include root");
    std::ifstream stream(path, std::ios::binary);
    std::string source(bytes + 1, '\0');
    stream.read(source.data(), static_cast<std::streamsize>(source.size()));
    if (stream.bad() || stream.gcount() != static_cast<std::streamsize>(bytes))
        fail(use, "bundle input '" + relative.generic_string() +
                  "' does not match the approved snapshot; use it externally instead");
    source.resize(bytes);
    return source;
}

inline void inline_snapshot(std::string& out, const fs::path& root,
                            const Snapshot& snapshot, const Syntax& use) {
    auto file = read_snapshot(root, fs::path("firestone") / snapshot.path,
                              snapshot.size, use);
    std::string_view source = file;
    constexpr std::string_view pragma = "#pragma once\n";
    if (!source.starts_with(pragma) || fingerprint(source) != snapshot.fingerprint)
        fail(use, "Firestone header '" + std::string(snapshot.path) +
                  "' changed since bundling was reviewed; use it externally instead");
    source.remove_prefix(pragma.size());
    out += "\n/* Bundled Firestone header: ";
    out += snapshot.path;
    out += " */\n#ifndef ";
    out += snapshot.guard;
    out += "\n#define ";
    out += snapshot.guard;
    out += '\n';
    if (!snapshot.child.empty()) {
        const auto* child = eligible(snapshot.child);
        if (!child || !source.starts_with(snapshot.include))
            fail(use, "unreviewed Firestone bundle include");
        inline_snapshot(out, root, *child, use);
        source.remove_prefix(snapshot.include.size());
    }
    out += source; // Keep all reviewed bytes except the replaced directives.
    out += "#endif /* ";
    out += snapshot.guard;
    out += " */\n";
}

inline std::string bundle(const Snapshot& snapshot, const Syntax& use, const Options& options) {
    const auto root = source_root(use, options);
    const auto license = read_snapshot(root, "LICENSE", mit_notice.size(), use);
    if (license != mit_notice)
        fail(use, "bundle LICENSE does not match the reviewed Firestone MIT license");
    std::string result = "/*\n";
    result += license;
    result += "*/\n";
    inline_snapshot(result, root, snapshot, use);
    return result;
}

} // namespace am8::packaging
