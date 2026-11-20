#include <archon/Archon-IRGen.hpp>

#include <algorithm>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <set>
#include <utility>

namespace irgen = archon::irgen;
using archon::Bytes;
using archon::List;
using archon::Object;
using archon::Value;

static void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class Fn>
static void error(const char* type, const char* message, Fn fn) {
    try {
        fn();
    } catch (const archon::Error& e) {
        check(e.python_type() == type, "wrong Python exception type");
        check(std::string_view(e.what()).find(message) != std::string_view::npos, e.what());
        return;
    }
    throw std::runtime_error("expected archon::Error");
}

static void check_standard_irgen(const std::vector<std::string>& entries) {
    const auto databases = irgen::compile_standard_architectures();
    check(entries.size() == databases.size(), "standard spec compile lost entry points");
    std::set<std::string> targets;
    for (const auto& database : databases) {
        const auto name = database.at("architecture").as_string();
        check(targets.insert(name).second, "duplicate standard spec architecture");
        const Object options{{"target", name}, {"oracle", false}};
        const auto out = irgen::generate_ir("gated", databases, options);
        check(out.count("IR.c") && out.count("Pipeline.c") && out.count("Emitter.c"),
              "missing classic IR pipeline stage for selected stdspec");
        check(out.at("Emitter.c").find("Emitter_target[] = \"" + name + '"') != std::string::npos,
              "classic IRGen selected the wrong stdspec in a mixed database bank");
    }
    const std::set<std::string> bundled{
        "AArch64", "AMD64", "CIL", "JVM", "MIPS32", "PDP11",
        "RISCV32", "RISCV64", "RISCV64IM", "Wasm32",
    };
    check(std::includes(targets.begin(), targets.end(), bundled.begin(), bundled.end()),
          "missing an installed physical or virtual stdspec");
}

int main(int argc, char** argv) try {
    check(argc == 3, "expected package and work directory arguments");
    archon::Interpreter python(argv[1]);
    const std::string package = argv[1], work = argv[2];
    const auto spec = package + "/stdspec/riscv/rv32i.arch";
    const Object compile_options{{"parameters", Object{{"XLEN", 32}}},
                                 {"features", List{"I"}}};
    const auto rv = archon::compile_file(spec, compile_options);
    archon::validate_source(spec, compile_options);
    archon::validate_database(rv);
    const auto db_file = work + "/test.archdb";
    archon::save_database(rv, db_file);
    auto snapshot = archon::inspect_database(archon::load_database(db_file));
    check(snapshot.at("architecture").as_string() == "RISCV32", "wrong target");
    check(snapshot.at("features").at(0).as_string() == "I", "wrong feature");
    check(archon::dump_database(archon::parse_database(archon::dump_database(snapshot))) ==
          archon::dump_database(rv), "database serialization changed");
    check(archon::generate_codec(rv, "demo").find("demo_encode") != std::string::npos, "no codec");
    check(archon::generate_emulator(rv, "demo").find("demo_emu_execute_state") != std::string::npos,
          "no emulator");
    check(archon::generate_emulator_header(rv, "demo").find("demo_emu_state") != std::string::npos,
          "no emulator header");

    auto target = archon::Target(rv);
    const auto add = target.assemble("add x1, x2, x3");
    const auto code = target.encode(add);
    check(code == Bytes{0xb3, 0x00, 0x31, 0x00}, "bad byte conversion");
    check(target.decode(code).operands.at("rs2").as_int64() == 3, "wrong operand");
    check(target.disassemble(code) == "add x1, x2, x3", "bad disassembly");
    auto program = target.assemble_program("start: add x1, x2, x3\naddi x4, x1, @start\n", 64);
    check(target.disassemble_program(program, 64) == "add x1, x2, x3\naddi x4, x1, -4\n",
          "program labels did not resolve");
    List registers(32, 0);
    registers[2] = 5;
    registers[3] = 7;
    auto state = target.new_state({{"registers", registers}});
    check(target.execute(add, registers).at(1).as_uint64() == 12, "wrong register result");
    check(target.execute_program(program, registers, 64).at(4).as_uint64() == 8,
          "wrong register program result");
    check(target.execute_state(add, state).registers().at(1).as_uint64() == 12, "wrong state result");
    check(target.execute_program_state(program, state, 64).registers().at(4).as_uint64() == 8,
          "wrong state program result");
    check(state.registers().at(1).as_uint64() == 0, "execution mutated input");
    error("ArchonError", "test.asm:1:1",
          [&] { target.assemble_program("no such instruction", 0, "test.asm"); });
    check(target.disassemble(code) == "add x1, x2, x3", "Python error leaked to next call");

    auto state_target = archon::Target(archon::compile_file(work + "/state.arch"));
    auto full = state_target.new_state({
        {"scalars", Object{{"PC", 9}}}, {"stacks", Object{{"Data", List{17}}}},
        {"memories", Object{{"RAM", Value::from_bytes(Bytes{0, 11, 0xff, 0})}}},
    });
    auto once = state_target.execute_program_state(state_target.assemble_program("push 42"), full);
    check(once.scalars().at("PC").as_int64() == 42, "bad scalar result");
    check(once.stacks().at("Data").at(1).as_uint64() == 42, "bad stack result");
    check(once.memories().at("RAM").as_bytes() == Bytes{0, 42, 0xff, 0}, "bad memory result");
    check(full.copy().memories().at("RAM").as_bytes() == Bytes{0, 11, 0xff, 0},
          "input state was not retained");

    const auto core_exports = archon::exports();
    check(std::find(core_exports.begin(), core_exports.end(), "parse_source") != core_exports.end(),
          "core export table missing parse_source");
    check(archon::get_export("parse_source").call({"architecture Test {}"}).at("name").as_string()
              == "Test", "generic function call failed");
    error("ArchonError", "unknown Archon API export", [] { archon::get_export("__dict__"); });
    check(Value("\xce\xbb").as_string() == "\xce\xbb", "UTF-8 conversion failed");
    check(Value(std::string("ab\0cd", 5)).as_string().size() == 5, "text was truncated");
    check(Value(-1).as_int64() == -1, "negative signed value changed");
    check(Value(std::numeric_limits<std::uint64_t>::max()).as_uint64() ==
          std::numeric_limits<std::uint64_t>::max(), "large unsigned value changed");
    const auto wide = Value::integer("18446744073709551617");
    check(wide.as_integer_string() == "18446744073709551617", "wide integer changed");
    error("OverflowError", "int", [&] { wide.as_uint64(); });
    check(Value(0.5).as_double() == 0.5, "floating point value changed");
    check(!Value(false).as_bool() && Value().is_none(), "bool/None conversion failed");
    Value copy = snapshot;
    auto moved = std::move(copy);
    check(copy.is_none() && moved.at("architecture").as_string() == "RISCV32", "move lost reference");

    auto worker = std::async(std::launch::async, [target, code] {
        return target.decode(code).name;
    });
    check(worker.get() == "ADD", "main-interpreter worker call failed");

    check(irgen::get_representation("gssa").at("name").as_string() == "gated", "bad IR alias");
    check(irgen::available_representations().size() >= 8, "incomplete IR registry");
    const auto entries = irgen::standard_spec_entry_points();
    check(std::find(entries.begin(), entries.end(), spec) != entries.end(), "missing installed stdspec");
    check_standard_irgen(entries);
    const auto stages = irgen::pipeline_stages();
    check(std::find(stages.begin(), stages.end(), "proof-tool") != stages.end(), "missing proof stage");
    const Object gen_options{{"oracle", false}};
    const auto context = irgen::build_context("gssa", {rv}, gen_options);
    check(context.at("architecture_names").at(0).as_string() == "RISCV32", "bad IRGen context");
    const auto proof = irgen::emit_stage("proof-tool", context);
    const auto out = irgen::generate_ir("gssa", {rv}, gen_options);
    check(out.count("IR.c") && out.count("Translate.py") && out.count("Assembler.sh")
              && out.count("RunSandbox.c") && !out.count("Oracle.c"), "incomplete generated artifacts");
    check(out.at("Proofs.smt") == proof.at("Proofs.smt"), "stage differs from full generation");
    check(out.at("RunSandbox.c") == irgen::emit_stage("sandbox", context).at("RunSandbox.c"),
          "installed sandbox stage differs from full generation");
    const auto output = irgen::write_generated_directory(out, work + "/generated", {spec}, true);
    check(std::filesystem::file_size(output + "/IR.c") == out.at("IR.c").size(), "writer lost data");
    const auto native = archon::compile_file(package + "/stdspec/riscv/rv64im.arch");
    check(irgen::dispatch("resolve_native_target", {native}).at("abi").as_string() == "LP64",
          "native target failed");
    const auto native_artifacts = irgen::emit_stage("backend", irgen::build_context(
        "gated", {native}, {{"profile", "native"}, {"abi", "LP64"}, {"oracle", false}}));
    check(native_artifacts.count("GatedBackend.c") && native_artifacts.count("BackendManifest.json"),
          "installed native backend resources missing");
    const auto macro = irgen::dispatch("macros.emit_empty_artifact", {"mir"});
    const auto stream = irgen::dispatch("macros.parse_stream", {macro});
    check(stream.attribute("artifact").as_string() == "mir", "public macro dispatch failed");
    error("ArchonError", "unknown native assembler flavor", [&] {
        irgen::assemble_object("nonexistent", "nonexistent", "invalid", "input.mir", work + "/a.o");
    });
    error("ArchonError", "entry symbol", [&] {
        irgen::link_executable("nonexistent", {}, "--help", work + "/out.elf");
    });
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
