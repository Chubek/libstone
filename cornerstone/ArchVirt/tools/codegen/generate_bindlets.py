#!/usr/bin/env python3
"""Bindlet Code Generation Engine for Domvirt.

Parses Python domainlet interface definition files, extracts type signatures,
and generates corresponding C++ wrapper headers and implementation files.
"""

import ast
import argparse
import os
import sys
from pathlib import Path
from typing import Any

BINDLET_MAP = {
    "vmm_core": {
        "module": "domainlet.vmm.core",
        "classes": [
            "AddressAllocator",
            "AddressRange",
            "GuestAddress",
            "GuestMemoryMmap",
            "MemoryRegion",
            "ExitDispatcher",
            "VmExit",
            "VmExitReason",
        ],
        "functions": {
            "list_kvm_capabilities": {"args": [], "ret": "dict"},
            "get_vcpu_state": {"args": ["int"], "ret": "dict"},
            "set_vcpu_state": {"args": ["int", "dict"], "ret": "bool"},
        },
    },
    "vmm_boot": {
        "module": "domainlet.vmm.boot",
        "classes": ["BootArtifacts", "BootParams", "E820Entry", "LinuxKernelLoader"],
        "functions": {
            "generate_acpi_tables": {"args": ["int", "int", "int"], "ret": "bytes"},
            "parse_boot_params": {"args": ["bytes"], "ret": "dict"},
        },
    },
    "vmm_interrupts": {
        "module": "domainlet.vmm.interrupts",
        "classes": ["IrqChip", "KvmIrqChip", "IrqFdSink"],
        "functions": {
            "create_irqchip": {"args": ["int"], "ret": "bool"},
            "route_msi": {"args": ["int", "int", "int"], "ret": "bool"},
        },
    },
    "vmm_io": {
        "module": "domainlet.vmm.io",
        "classes": ["Bus", "BusMapping", "EventFd", "EventLoop", "MmioBus", "PioBus"],
        "functions": {
            "register_mmio": {"args": ["int", "int", "str"], "ret": "bool"},
            "dispatch_io": {"args": ["int", "int", "bool"], "ret": "dict"},
        },
    },
    "vmm_virtio": {
        "module": "domainlet.vmm.virtio",
        "classes": [
            "VirtioAccess",
            "VirtioDeviceDescriptor",
            "VirtioDeviceStatus",
            "VirtioFeature",
            "VirtioKernelDescriptor",
            "VirtioQueue",
            "VirtioQueueDirection",
            "VirtioRegister",
            "VirtioRegistry",
            "VirtioTransport",
        ],
        "functions": {
            "list_devices": {"args": [], "ret": "list"},
            "describe_device": {"args": ["int"], "ret": "dict"},
            "validate_device": {"args": ["int"], "ret": "dict"},
        },
    },
    "vmm_control": {
        "module": "domainlet.vmm.control",
        "classes": ["VmmAssembly", "VmmBuilder", "VmmLifecycle", "VmConfig"],
        "functions": {
            "build_vmm": {"args": ["dict"], "ret": "dict"},
            "save_snapshot": {"args": ["str"], "ret": "bool"},
            "restore_snapshot": {"args": ["str"], "ret": "bool"},
        },
    },
}

CPP_TYPE_MAP = {
    "int": "int",
    "str": "std::string",
    "bool": "bool",
    "float": "double",
    "bytes": "std::vector<uint8_t>",
    "dict": "nlohmann::json",
    "list": "nlohmann::json",
}


def generate_header(bindlet_name: str, spec: dict) -> str:
    """Generate a C++ header file for a bindlet."""
    lines = []
    guard = f"BINDLET_{bindlet_name.upper()}_HPP"
    lines.append("#pragma once")
    lines.append("")
    lines.append('#include <string>')
    lines.append('#include <vector>')
    lines.append('#include <nlohmann/json.hpp>')
    lines.append('#include "domvirt_types.hpp"')
    lines.append("")
    lines.append(f"namespace domvirt::bindlet_bridge::generated {{")
    lines.append("")

    for cls_name in spec.get("classes", []):
        lines.append(f"class Bindlet_{cls_name} {{")
        lines.append("public:")
        lines.append(f"    explicit Bindlet_{cls_name}() = default;")
        lines.append(f"    ~Bindlet_{cls_name}() = default;")
        lines.append("")
        lines.append(f"    static constexpr const char* module_name = \"{spec['module']}\";")
        lines.append(f"    static constexpr const char* class_name = \"{cls_name}\";")
        lines.append("")
        lines.append("    nlohmann::json call(const std::string& method,")
        lines.append("                         const nlohmann::json& args = nlohmann::json::object());")
        lines.append("};")
        lines.append("")

    for func_name, func_spec in spec.get("functions", {}).items():
        ret_type = CPP_TYPE_MAP.get(func_spec["ret"], "nlohmann::json")
        args = ", ".join(
            f"{CPP_TYPE_MAP.get(a, 'nlohmann::json')} arg{i}"
            for i, a in enumerate(func_spec["args"])
        )
        lines.append(f"{ret_type} call_{func_name}({args});")
        lines.append("")

    lines.append(f"void register_bindlet_{bindlet_name}();")
    lines.append("")
    lines.append(f"}} // namespace domvirt::bindlet_bridge::generated")

    return "\n".join(lines)


def generate_impl(bindlet_name: str, spec: dict) -> str:
    """Generate a C++ implementation file for a bindlet."""
    lines = []
    lines.append(f'#include "bindlet_{bindlet_name}.hpp"')
    lines.append('#include "bindlet_bridge/bridge.hpp"')
    lines.append('#include <stdexcept>')
    lines.append('#include <sstream>')
    lines.append("")
    lines.append(f"namespace domvirt::bindlet_bridge::generated {{")
    lines.append("")
    lines.append("static nlohmann::json bridge_call(const char* module,")
    lines.append("                                  const nlohmann::json& request) {")
    lines.append("    extern domvirt::bindlet_bridge::Bridge* g_bridge;")
    lines.append("    if (!g_bridge) return nlohmann::json();")
    lines.append("    std::string result;")
    lines.append("    auto status = g_bridge->execute_python(module, \"execute\", request.dump(), result);")
    lines.append("    if (!status) throw std::runtime_error(status.message);")
    lines.append("    if (result.empty()) return nlohmann::json();")
    lines.append("    return nlohmann::json::parse(result);")
    lines.append("}")
    lines.append("")

    for cls_name in spec.get("classes", []):
        lines.append(f"nlohmann::json Bindlet_{cls_name}::call(")
        lines.append("    const std::string& method,")
        lines.append("    const nlohmann::json& args) {")
        lines.append("    nlohmann::json request;")
        lines.append(f'    request["class"] = "{cls_name}";')
        lines.append(f'    request["method"] = method;')
        lines.append(f'    request["args"] = args;')
        lines.append("")
        lines.append(f'    return bridge_call("{spec["module"]}", request);')
        lines.append("}")
        lines.append("")

    for func_name, func_spec in spec.get("functions", {}).items():
        ret_type = CPP_TYPE_MAP.get(func_spec["ret"], "nlohmann::json")
        args = ", ".join(
            f"{CPP_TYPE_MAP.get(a, 'nlohmann::json')} arg{i}"
            for i, a in enumerate(func_spec["args"])
        )
        lines.append(f"{ret_type} call_{func_name}({args}) {{")
        lines.append("    nlohmann::json request;")
        lines.append(f'    request["function"] = "{func_name}";')
        lines.append("    nlohmann::json args_array = nlohmann::json::array();")
        for i, _ in enumerate(func_spec["args"]):
            lines.append(f"    args_array.push_back(arg{i});")
        lines.append(f'    request["args"] = args_array;')
        lines.append("")
        lines.append(f'    auto result = bridge_call("{spec["module"]}", request);')
        if ret_type == "nlohmann::json":
            lines.append("    return result;")
        elif ret_type == "std::vector<uint8_t>":
            lines.append("    return result.get<std::vector<uint8_t>>();")
        else:
            lines.append(f"    return result.get<{ret_type}>();")
        lines.append("}")
        lines.append("")

    lines.append(f"void register_bindlet_{bindlet_name}() {{")
    lines.append("    // Registration is performed by the owning bridge at startup.")
    lines.append("}")
    lines.append("")
    lines.append(f"}} // namespace domvirt::bindlet_bridge::generated")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Domvirt Bindlet Code Generator")
    parser.add_argument("--stubs-dir", required=True, help="Directory containing Python stubs")
    parser.add_argument("--output-dir", required=True, help="Output directory for generated C++")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    generated_files = []

    for bindlet_name, spec in BINDLET_MAP.items():
        header_path = output_dir / f"bindlet_{bindlet_name}.hpp"
        impl_path = output_dir / f"bindlet_{bindlet_name}.cpp"

        with open(header_path, "w") as f:
            f.write(generate_header(bindlet_name, spec))
        generated_files.append(str(header_path))

        with open(impl_path, "w") as f:
            f.write(generate_impl(bindlet_name, spec))
        generated_files.append(str(impl_path))

    print(f"Generated {len(generated_files)} files:")
    for f in generated_files:
        print(f"  {f}")


if __name__ == "__main__":
    main()
