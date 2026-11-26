// dinvoke.cpp - CLI over dynload (open) + dyninvoke (call) + qBFD (inspect).
#include "dyninvoke.h"
#include "dynload.h"

#include <options.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using qobj::OptionSpec;

namespace {
constexpr OptionSpec kHelp{'h', "help", false, "", "show this help"};
constexpr OptionSpec kAbi{0, "abi", true, "name", "call ABI (auto, sysv_amd64, win64, aapcs64, darwin_arm64, riscv64, riscv32)"};
constexpr OptionSpec kRet{0, "ret", true, "type", "return type (default i32)"};
constexpr OptionSpec kArg{0, "arg", true, "type:value", "argument (repeatable)"};
constexpr OptionSpec kLayout{0, "layout", false, "", "dump register/stack layout and exit"};
constexpr OptionSpec kInspect{'i', "inspect", false, "", "inspect ABI and symbol without loading"};
constexpr OptionSpec kIsaDir{0, "isa-dir", true, "dir", "sandstone ISA directory"};
constexpr OptionSpec kLibPath{'L', "library-path", true, "dir", "dynload search path"};

bool splitArg(const std::string& s, std::string& type, std::string& value) {
  auto p = s.find(':');
  if (p == std::string::npos) return false;
  type = s.substr(0, p);
  value = s.substr(p + 1);
  return !type.empty();
}

bool parseInt(const std::string& s, int64_t& out) {
  try {
    size_t u = 0;
    long long v = std::stoll(s, &u, 0);
    if (u != s.size()) return false;
    out = v;
    return true;
  } catch (...) {
    return false;
  }
}
bool parseUint(const std::string& s, uint64_t& out) {
  auto first = s.find_first_not_of(" \t\n\r\f\v");
  if (first == std::string::npos || s[first] == '-') return false;
  try {
    size_t u = 0;
    unsigned long long v = std::stoull(s, &u, 0);
    if (u != s.size()) return false;
    out = v;
    return true;
  } catch (...) {
    return false;
  }
}

using Value = std::array<unsigned char, 8>;

template<class T> bool store(Value& dest, T value) {
  static_assert(sizeof(T) <= sizeof(Value));
  memcpy(dest.data(), &value, sizeof value);
  return true;
}

template<class T> bool storeSigned(Value& b, const std::string& s) {
  int64_t value = 0;
  return parseInt(s, value) && std::in_range<T>(value) && store(b, static_cast<T>(value));
}

template<class T> bool storeUnsigned(Value& b, const std::string& s) {
  uint64_t value = 0;
  return parseUint(s, value) && std::in_range<T>(value) && store(b, static_cast<T>(value));
}

bool parseValue(dinv_kind_t kind, const std::string& s, Value& value) {
  switch (kind) {
    case DINV_BOOL:
      return (s == "true" || s == "1" || s == "false" || s == "0") &&
             store(value, s == "true" || s == "1");
    case DINV_I8: return storeSigned<int8_t>(value, s);
    case DINV_U8: return storeUnsigned<uint8_t>(value, s);
    case DINV_I16: return storeSigned<int16_t>(value, s);
    case DINV_U16: return storeUnsigned<uint16_t>(value, s);
    case DINV_I32: return storeSigned<int32_t>(value, s);
    case DINV_U32: return storeUnsigned<uint32_t>(value, s);
    case DINV_I64: return storeSigned<int64_t>(value, s);
    case DINV_U64: return storeUnsigned<uint64_t>(value, s);
    case DINV_PTR: {
      if (s.starts_with("str:")) return store(value, s.c_str() + 4);
      uint64_t address = 0;
      return parseUint(s, address) && std::in_range<uintptr_t>(address) &&
             store(value, reinterpret_cast<void*>(static_cast<uintptr_t>(address)));
    }
    case DINV_F32:
    case DINV_F64: {
      char* end = nullptr;
      errno = 0;
      if (kind == DINV_F32) store(value, strtof(s.c_str(), &end));
      else store(value, strtod(s.c_str(), &end));
      return errno != ERANGE && end != s.c_str() && *end == '\0';
    }
    default: return false;
  }
}

template<class T> T load(const Value& b) {
  T value;
  memcpy(&value, b.data(), sizeof value);
  return value;
}

void printValue(dinv_kind_t kind, const Value& b) {
  switch (kind) {
    case DINV_BOOL: std::cout << (load<unsigned char>(b) != 0); break;
    case DINV_I8: std::cout << +load<int8_t>(b); break;
    case DINV_U8: std::cout << +load<uint8_t>(b); break;
    case DINV_I16: std::cout << load<int16_t>(b); break;
    case DINV_U16: std::cout << load<uint16_t>(b); break;
    case DINV_I32: std::cout << load<int32_t>(b); break;
    case DINV_U32: std::cout << load<uint32_t>(b); break;
    case DINV_I64: std::cout << load<int64_t>(b); break;
    case DINV_U64: std::cout << load<uint64_t>(b); break;
    case DINV_F32: std::cout << load<float>(b); break;
    case DINV_F64: std::cout << load<double>(b); break;
    case DINV_PTR: std::cout << load<void*>(b); break;
    default: return;
  }
  std::cout << '\n';
}

const char* locName(dinv_loc_kind_t w) {
  switch (w) {
    case DINV_LOC_GPR: return "gpr";
    case DINV_LOC_FPR: return "fpr";
    case DINV_LOC_STACK: return "stack";
    default: return "void";
  }
}
}  // namespace

int main(int argc, char** argv) {
  qobj::Options opts("dinvoke", "[options] <library> <symbol>",
                     {kHelp, kAbi, kRet, kArg, kLayout, kInspect, kIsaDir, kLibPath});
  if (!opts.parse(argc, argv)) {
    std::cerr << opts.error() << '\n' << opts.usage();
    return 2;
  }
  if (opts.has(kHelp)) {
    std::cout << opts.usage();
    return 0;
  }
  auto pos = opts.positional();
  if (pos.size() != 2) {
    std::cerr << opts.usage();
    return 2;
  }
  std::string libPath = pos[0], symbol = pos[1];

  dinv_abi_t abi = DINV_ABI_AUTO;
  if (opts.has(kAbi)) {
    if (dinv_abi_for_name(opts.value(kAbi).c_str(), &abi) != DINV_OK) {
      std::cerr << "unknown ABI: " << opts.value(kAbi) << '\n';
      return 2;
    }
  }
  const dinv_type_t* retType = &dinv_type_i32;
  if (opts.has(kRet)) {
    if (dinv_type_for_name(opts.value(kRet).c_str(), &retType) != DINV_OK) {
      std::cerr << "unknown return type: " << opts.value(kRet) << '\n';
      return 2;
    }
  }
  struct ParsedArg {
    const dinv_type_t* type = nullptr;
    std::string value;
  };
  std::vector<ParsedArg> pargs;
  for (auto& a : opts.valueList(kArg)) {
    std::string t, v;
    if (!splitArg(a, t, v)) {
      std::cerr << "bad --arg (want type:value): " << a << '\n';
      return 2;
    }
    const dinv_type_t* ty = nullptr;
    if (dinv_type_for_name(t.c_str(), &ty) != DINV_OK) {
      std::cerr << "unknown arg type: " << t << '\n';
      return 2;
    }
    pargs.push_back({ty, v});
  }

  const char* isaDir = opts.has(kIsaDir) ? opts.value(kIsaDir).c_str() : nullptr;

  // Resolve once through dynload's search paths, without running constructors.
  tdyn_env_t* rawEnv = nullptr;
  if (tdyn_env_create(&rawEnv) != TDYN_OK) {
    std::cerr << "dynload env: " << tdyn_last_error() << '\n';
    return 1;
  }
  std::unique_ptr<tdyn_env_t, decltype(&tdyn_env_destroy)> env(rawEnv, tdyn_env_destroy);
  for (auto& dir : opts.valueList(kLibPath)) {
    if (tdyn_env_add_search_path(env.get(), dir.c_str()) != TDYN_OK) {
      std::cerr << "search path: " << tdyn_last_error() << '\n';
      return 1;
    }
  }
  tdyn_library_t* offline = nullptr;
  if (tdyn_env_open(env.get(), libPath.c_str(), TDYN_OPEN_INSPECT_ONLY, &offline) != TDYN_OK) {
    std::cerr << "inspect: " << tdyn_last_error() << '\n';
    return 1;
  }
  libPath = tdyn_library_path(offline);

  // qBFD inspect via dyninvoke (format/arch -> ABI + symbol presence).
  dinv_abi_t fileAbi = DINV_ABI_AUTO;
  int found = 0;
  dinv_status_t irc =
      dinv_inspect_object(libPath.c_str(), symbol.c_str(), &fileAbi, &found);
  if (opts.has(kInspect)) {
    std::cout << "file: " << libPath << '\n';
    if (irc == DINV_OK) {
      std::cout << "qbfd-abi: " << dinv_abi_name(fileAbi) << '\n';
      std::cout << "isa: " << dinv_abi_isa_arch(fileAbi) << " ("
                << dinv_abi_isa_file(fileAbi) << ")\n";
      std::cout << "symbol " << symbol << ": " << (found ? "found" : "missing")
                << '\n';
    } else {
      std::cout << "inspect: " << dinv_last_error() << '\n';
    }
    size_t ng = 0, nf = 0;
    dinv_abi_t checkAbi = (abi == DINV_ABI_AUTO && irc == DINV_OK) ? fileAbi : abi;
    if (checkAbi != DINV_ABI_AUTO &&
        dinv_isa_regcounts(checkAbi, isaDir, &ng, &nf) == DINV_OK)
      std::cout << "isa-regs: gpr=" << ng << " fpr=" << nf << '\n';
    else if (checkAbi != DINV_ABI_AUTO)
      std::cout << "isa-regs: unavailable (" << dinv_last_error() << ")\n";
  }
  if (irc != DINV_OK) {
    if (!opts.has(kInspect)) std::cerr << "inspect: " << dinv_last_error() << '\n';
    return 1;
  }
  if (abi != DINV_ABI_AUTO && abi != fileAbi && !opts.has(kLayout)) {
    std::cerr << "ABI mismatch: file uses " << dinv_abi_name(fileAbi) << '\n';
    return 1;
  }
  if (opts.has(kInspect) && !opts.has(kLayout)) return found ? 0 : 1;
  if (abi == DINV_ABI_AUTO) abi = fileAbi;
  if (!opts.has(kLayout) && !dinv_abi_can_execute(abi)) {
    std::cerr << "native execution not available for " << dinv_abi_name(abi) << '\n';
    return 1;
  }
  if (!opts.has(kLayout)) {
    tdyn_symbol_info_t info{};
    if (tdyn_library_find_symbol_info(offline, symbol.c_str(), TDYN_SYMBOL_TABLE_DYNAMIC,
                                      &info) != TDYN_OK ||
        info.undefined || info.hidden ||
        (info.kind != TDYN_SYMBOL_FUNCTION && info.kind != TDYN_SYMBOL_INDIRECT)) {
      std::cerr << "not a callable dynamic symbol in this image: " << symbol << '\n';
      return 1;
    }
  }

  // Build cif.
  std::vector<const dinv_type_t*> argTypes;
  for (auto& p : pargs) argTypes.push_back(p.type);
  dinv_cif_t* rawCif = nullptr;
  if (dinv_cif_create(abi, retType, argTypes.size(), argTypes.data(), &rawCif) != DINV_OK) {
    std::cerr << "cif: " << dinv_last_error() << '\n';
    return 1;
  }
  std::unique_ptr<dinv_cif_t, decltype(&dinv_cif_destroy)> cifOwner(rawCif, dinv_cif_destroy);
  const auto* cif = cifOwner.get();

  if (opts.has(kLayout)) {
    std::cout << "abi: " << dinv_abi_name(dinv_cif_abi(cif)) << '\n';
    std::cout << "gpr-used: " << dinv_cif_gpr_used(cif)
              << " fpr-used: " << dinv_cif_fpr_used(cif)
              << " stack-bytes: " << dinv_cif_stack_bytes(cif)
              << " hidden-ret: " << (dinv_cif_uses_hidden_ret(cif) ? "yes" : "no")
              << '\n';
    auto printSlots = [](const std::vector<dinv_slot_t>& slots) {
      std::cout << ":";
      for (auto& s : slots) {
        std::cout << " " << locName(s.where);
        if (s.where == DINV_LOC_GPR || s.where == DINV_LOC_FPR)
          std::cout << s.reg;
        else
          std::cout << "+" << s.stack_offset;
        std::cout << ":" << s.size << "@" << s.field_offset;
      }
      std::cout << '\n';
    };
    for (size_t i = 0; i < argTypes.size(); ++i) {
      size_t n = 0;
      dinv_cif_arg_slots(cif, i, nullptr, &n);
      std::vector<dinv_slot_t> slots(n);
      dinv_cif_arg_slots(cif, i, slots.data(), &n);
      std::cout << "arg" << i;
      printSlots(slots);
    }
    size_t n = 0;
    dinv_cif_return_slots(cif, nullptr, &n);
    std::vector<dinv_slot_t> slots(n);
    dinv_cif_return_slots(cif, slots.data(), &n);
    std::cout << "return";
    printSlots(slots);
    return 0;
  }

  std::vector<Value> values(pargs.size());
  std::vector<void*> callArgs(pargs.size());
  for (size_t i = 0; i < pargs.size(); ++i) {
    if (!parseValue(pargs[i].type->kind, pargs[i].value, values[i])) {
      std::cerr << "invalid " << dinv_kind_name(pargs[i].type->kind) << " value: "
                << pargs[i].value << '\n';
      return 2;
    }
    callArgs[i] = values[i].data();
  }

  // The environment owns the native module until after the foreign call.
  tdyn_library_t* lib = nullptr;
  uint32_t flags = TDYN_OPEN_NOW | TDYN_OPEN_LOCAL | TDYN_OPEN_REQUIRE_INSPECT;
  if (tdyn_env_open(env.get(), libPath.c_str(), flags, &lib) != TDYN_OK) {
    std::cerr << "open: " << tdyn_last_error() << '\n';
    return 1;
  }
  void* fn = nullptr;
  if (tdyn_library_symbol(lib, symbol.c_str(), &fn) != TDYN_OK) {
    std::cerr << "resolve: " << tdyn_last_error() << '\n';
    return 1;
  }

  alignas(8) Value result{};
  void* retPtr = retType->kind == DINV_VOID ? nullptr : result.data();
  dinv_status_t rc = dinv_call(cifOwner.get(), fn, retPtr, callArgs.data());
  if (rc != DINV_OK) {
    std::cerr << "call: " << dinv_last_error() << '\n';
    return 1;
  }
  printValue(retType->kind, result);
  return 0;
}
