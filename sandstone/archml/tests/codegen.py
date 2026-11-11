"""Compile and execute generated headers without linking the ArchML library."""
import json
from pathlib import Path
import subprocess
import sys

cli, compiler, specs, build = sys.argv[1:]
build = Path(build)
build.mkdir(parents=True, exist_ok=True)
profiles = sorted(p for p in Path(specs).iterdir() if p.suffix in (".am", ".aml"))
includes, checks = [], []
# Punctuation in profile names and embedded NUL in strings must survive codegen.
edge = b'''(archml 1 -test (target -test (bits 8) (endian little))
  (pcode-op nop (arity 0 0))
  (instruction nop (operands) (encoding (base "00"))
    (pcode (nop)) (assembly "a\\x00b")))'''
(build / "edge.hpp").write_bytes(subprocess.check_output([cli, "-"], input=edge))
includes.append('#include "edge.hpp"')
checks.append('require(archml_x_2d74657374::find("nop")->assembly == std::string("a\\0b", 3));')
for profile in profiles:
    args = [cli, "--target", "cpp", str(profile)]
    text = subprocess.check_output(args)
    assert text == subprocess.check_output(args), "nondeterministic generation"
    header = build / (profile.stem + ".hpp")
    header.write_bytes(text)
    includes.append(f'#include "{header.name}"')
    metadata = json.loads(subprocess.check_output([cli, "--target", "json", str(profile)]))
    assert metadata[0] == {"symbol": "archml"} and metadata[2] == {"symbol": profile.stem}
    checks.append(f"exercise(archml_{profile.stem}::instructions);")

source = build / "main.cpp"
source.write_text("\n".join(includes) + r'''
#include <array>
#include <iostream>
void require(bool ok) { if (!ok) throw std::runtime_error("generated-code check failed"); }
void exercise(const std::vector<sandstone::archml::runtime::Instruction> &forms) {
    namespace rt = sandstone::archml::runtime;
    for (const auto &form : forms) {
        if (form.base.empty()) {
            bool rejected = false;
            try { rt::encode(form, {}); } catch (const std::invalid_argument &) { rejected = true; }
            require(rejected);
            continue;
        }
        std::vector<std::uint64_t> values;
        for (const auto &op : form.operands) {
            std::uint64_t value = 0, normalized;
            while (!rt::normalize(op, value, normalized) && value < 1024) ++value;
            require(value < 1024);
            values.push_back(value);
        }
        auto bytes = rt::encode(form, values);
        std::vector<std::uint64_t> decoded;
        require(rt::decode_one(form, bytes, decoded) && decoded == values);
    }
}
int main() {
''' + "\n".join(checks) + r'''
    const std::array<std::uint64_t, 3> values{1, 2, 3};
    require(archml_riscv64::encode("add", values) == std::vector<std::uint8_t>{0xb3, 0, 0x31, 0});
    const std::array<std::string, 3> names{"%r0", "%r1", "%r2"};
    require(archml_nvptx::render("add.s32", names) == "add.s32 %r0, %r1, %r2;");
    require(archml_riscv64::metadata_json.find("selection") != std::string_view::npos);
}
''')
executable = build / "generated-test"
subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic",
                str(source), "-o", str(executable)], check=True)
subprocess.run([str(executable)], check=True)

# Errors must not truncate an existing output. stdin and explicit namespaces work.
output = build / "preserved.hpp"
output.write_text("preserve me")
invalid = subprocess.run([cli, "--namespace", "class", "-o", str(output), str(profiles[0])], capture_output=True)
assert invalid.returncode and output.read_text() == "preserve me"
invalid = subprocess.run([cli, "--target", "unknown", str(profiles[0])], capture_output=True)
assert invalid.returncode
invalid = subprocess.run([cli, "--check", "--target", "json", str(profiles[0])], capture_output=True)
assert invalid.returncode
result = subprocess.check_output([cli, "--namespace", "custom_profile", "-"], input=profiles[0].read_bytes())
assert b"namespace custom_profile {" in result
print(f"Compiled and exercised {len(profiles)} standalone generated profiles")
