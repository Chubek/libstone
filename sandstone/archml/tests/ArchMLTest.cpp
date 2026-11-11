#include "Target.hpp"
#include <array>
#include <filesystem>
#include <future>
#include <iostream>
#include <random>

using namespace sandstone::archml;
extern "C" int archml_c_test(const char *path);
namespace {
void check(bool ok, const std::string &message) { if (!ok) throw std::runtime_error(message); }
constexpr std::string_view minimal = "(archml 1 test (target test (bits 64) (endian little)))";
void rejects(std::string text, std::string_view message) {
    try { auto doc = Document::parse(text, "test.aml"); (void)doc; }
    catch (const Error &error) {
        check(std::string_view(error.what()).find(message) != std::string_view::npos,
              "wrong error: " + std::string(error.what()));
        check(error.location.line != 0 && error.location.column != 0, "missing error location");
        return;
    }
    throw std::runtime_error("accepted invalid input: " + text);
}
void fixture(const Document &doc, std::string_view name, std::initializer_list<std::uint64_t> values,
             std::initializer_list<std::uint8_t> expected) {
    const auto bytes = doc.encode(name, std::span(values.begin(), values.size()));
    check(bytes == std::vector<std::uint8_t>(expected), "incorrect fixture " + doc.name() + ":" + std::string(name));
    std::vector<std::uint64_t> decoded;
    check(runtime::decode_one(*doc.instruction(name), bytes, decoded), "fixture decode failed");
    check(decoded == std::vector<std::uint64_t>(values), "fixture decode operands differ");
}
std::uint64_t candidate(const runtime::Operand &op, std::uint64_t raw) {
    raw &= runtime::mask(op.bits);
    if (op.is_signed && op.bits < 64 && (raw & (UINT64_C(1) << (op.bits - 1)))) raw |= ~runtime::mask(op.bits);
    return raw * op.scale;
}
void roundtrip(const Document &doc, const runtime::Instruction &form, std::mt19937_64 &random) {
    if (form.base.empty()) {
        try { runtime::encode(form, {}); } catch (const std::invalid_argument &) { return; }
        throw std::runtime_error("encoded nonbinary operation");
    }
    std::vector<std::uint64_t> values;
    for (const auto &op : form.operands) {
        bool found = false;
        for (std::uint64_t raw = 0; raw < 1024; ++raw) {
            const auto value = candidate(op, raw);
            std::uint64_t normalized;
            if (runtime::normalize(op, value, normalized)) { values.push_back(value); found = true; break; }
        }
        check(found, "no sample operand for " + form.name + ":" + op.name);
    }
    auto test = [&] {
        const auto bytes = runtime::encode(form, values);
        std::vector<std::uint64_t> decoded;
        check(runtime::decode_one(form, bytes, decoded) && decoded == values,
              "roundtrip failed " + doc.name() + ":" + form.name);
        runtime::Decoded any;
        check(doc.decode(bytes, any), "document decode failed");
        check(runtime::encode(doc.instructions()[any.instruction], any.operands) == bytes, "ambiguous decode changed bytes");
        if (bytes.size() > 1) {
            check(!runtime::decode_one(form, std::span(bytes).first(bytes.size() - 1), decoded), "decoded truncated instruction");
        }
    };
    test();
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto saved = values[i];
        const auto &op = form.operands[i];
        const auto sign = UINT64_C(1) << (op.bits - 1);
        const std::array<std::uint64_t, 8> samples{0, 1, runtime::mask(op.bits), sign, sign - 1, random(), random(), random()};
        for (const auto raw : samples) {
            const auto value = candidate(op, raw);
            std::uint64_t normalized;
            if (!runtime::normalize(op, value, normalized)) continue;
            values[i] = value;
            test();
        }
        values[i] = saved;
        for (auto excluded : op.excluded) {
            values[i] = excluded;
            bool rejected = false;
            try { runtime::encode(form, values); } catch (const std::invalid_argument &) { rejected = true; }
            check(rejected, "accepted excluded operand");
        }
        values[i] = saved;
    }
}
}
int main() {
    try {
        rejects("", "syntax");
        rejects("(archml 2 test (target test (bits 64) (endian little)))", "version");
        rejects("(archml 1 test)", "expected");
        rejects(std::string(minimal) + " trailing", "syntax");
        rejects(std::string(minimal) + std::string(1, '\0'), "NUL");
        rejects(std::string(minimal) + std::string(1, '\xff'), "UTF-8");
        rejects(R"((archml 1 test (target test (bits 64) (endian little) (doc "\xff"))))", "UTF-8");
        rejects("(archml 1 test (target test (bits 64) (bits 32) (endian little)))", "duplicate property");
        rejects("(archml 1 test (target test (bits 64) (endian middle)))", "endian");
        rejects("(archml 1 test (target test (bits 64) (endian little) (use (ref feature absent))))", "unresolved");
        rejects("(archml 1 test (target test (bits 64) (endian little)) (feature a (requires (ref feature b))) (feature b (requires (ref feature a))))", "cyclic");
        rejects("(archml 1 test (target test (bits 18446744073709551616) (endian little)))", "integer");
        rejects("(archml 1 test (target test (bits 64) (endian little)) (register r (bits 64) (storage missing) (offset 0)))", "storage");
        rejects("(archml 1 test (target test (bits 64) (endian little)) (pcode-op read (arity 0 0)) (instruction bad (operands) (assembly \"\") (pcode (read)) (encoding (base \"00\"))))", "arguments");
        rejects(std::string(600, '(') + std::string(600, ')'), "nesting");
        rejects("(archml 1 test (target test (bits 64) (endian little) (doc \"bad\\q\")))", "escape");
        const auto escaped = Document::parse(R"((archml 1 test ; comment
          (target test (bits 64) (endian little) (doc "quote\" slash\\ newline\n tab\t zero\x00 literal\\u0001"))))");
        check(json(Document::parse(generate(escaped, "archml")).root()) == json(escaped.root()), "escaped strings did not roundtrip");
        std::vector<std::future<void>> futures;
        for (int i = 0; i < 8; ++i) futures.push_back(std::async(std::launch::async, [] {
            for (int j = 0; j < 20; ++j) check(Document::parse(minimal).name() == "test", "parallel parse failed");
        }));
        for (auto &future : futures) future.get();
        std::mt19937_64 random(0x415243484d4c);
        std::size_t files = 0, forms = 0;
        for (const auto &entry : std::filesystem::directory_iterator(ARCHML_STDSPEC)) {
            if (entry.path().extension() != ".am" && entry.path().extension() != ".aml") continue;
            const auto doc = Document::load(entry.path().string());
            ++files;
            const auto regenerated = generate(doc, "archml");
            check(json(Document::parse(regenerated).root()) == json(doc.root()), "profile metadata lost in roundtrip");
            check(generate(doc, "json") == json(doc.root()) + '\n', "JSON backend mismatch");
            for (const auto &form : doc.instructions()) { ++forms; roundtrip(doc, form, random); }
            if (doc.name() == "riscv64") {
                fixture(doc, "add", {1, 2, 3}, {0xb3, 0x00, 0x31, 0x00});
                fixture(doc, "addi", {1, 0, UINT64_MAX}, {0x93, 0x00, 0xf0, 0xff});
                fixture(doc, "beq", {0, 0, std::uint64_t(-4)}, {0xe3, 0x0e, 0x00, 0xfe});
                auto bad = regenerated;
                auto pos = bad.find("(field rd 7 5 0)"); check(pos != std::string::npos, "missing field");
                bad.replace(pos, 16, "(field rd 32 5 0)"); rejects(bad, "bounds");
                bad = regenerated; pos = bad.find("(field rs1 15 5 0)");
                bad.replace(pos, 18, "(field rs1 7 5 0)"); rejects(bad, "overlapping");
                auto operands = std::array<std::string, 3>{"x0", "x0", "-4"};
                check(runtime::render(*doc.instruction("beq"), operands, {{"target", "loop"}}) == "beq x0, x0, loop", "presentation binding failed");
            } else if (doc.name() == "amd64") {
                fixture(doc, "add64-rr", {0, 1}, {0x48, 0x01, 0xc8});
                fixture(doc, "mov64-ri", {0, UINT64_MAX}, {0x48, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
            } else if (doc.name() == "ppc64") {
                fixture(doc, "add", {3, 4, 5}, {0x7c, 0x64, 0x2a, 0x14});
            } else if (doc.name() == "bpf") {
                fixture(doc, "lddw", {1, UINT64_MAX}, {0x18, 0x01, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255});
            } else if (doc.name() == "dex") {
                fixture(doc, "const-wide", {0, UINT64_C(1) << 63}, {0x18, 0, 0, 0, 0, 0, 0, 0, 0, 0x80});
            } else if (doc.name() == "nvptx") {
                const std::array<std::string, 3> values{"%r0", "%r1", "%r2"};
                check(runtime::render(*doc.instruction("add.s32"), values) == "add.s32 %r0, %r1, %r2;", "PTX template failed");
            }
        }
        check(files == 17 && forms == 657, "unexpected profile/form count: " + std::to_string(files) + "/" + std::to_string(forms));
        check(archml_c_test((std::filesystem::path(ARCHML_STDSPEC) / "riscv64.aml").c_str()) == 0, "C API test failed");
        std::cout << "Validated " << files << " profiles and " << forms << " forms\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
