// ld.c - C API implementation for the qobjld linker.
//
// Compiled as C++ (see ld/CMakeLists.txt LANGUAGE property) so it can use
// the header-only readers in ../fd/ through the internal driver. The public
// contract is the C header ld.h; the C++ wrapper ld.hpp calls these
// functions.
#include "ld.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "internal.hpp"
#include "parser.hpp"
#include "ScriptSDK.hpp"

extern const char* qld_detail_version();

namespace {

void setErr(char** errmsg, const std::string& msg) {
  if (!errmsg) return;
  *errmsg = nullptr;
  if (msg.empty()) return;
  char* p = (char*)std::malloc(msg.size() + 1);
  if (!p) return;
  std::memcpy(p, msg.c_str(), msg.size() + 1);
  *errmsg = p;
}

qld::detail::LinkOptions toInternal(const qld_options_t* o) {
  qld::detail::LinkOptions in;
  if (!o) return in;
  for (size_t i = 0; i < o->n_inputs; ++i)
    if (o->inputs[i]) in.inputs.emplace_back(o->inputs[i]);
  for (size_t i = 0; i < o->n_libpaths; ++i)
    if (o->libpaths[i]) in.libPaths.emplace_back(o->libpaths[i]);
  for (size_t i = 0; i < o->n_libs; ++i)
    if (o->libs[i]) in.libs.emplace_back(o->libs[i]);
  if (o->output) in.output = o->output;
  if (o->entry) { in.entry = o->entry; in.hasEntry = true; }
  if (o->script) in.script = o->script;
  if (o->use_script_text && o->script_text) {
    in.scriptText = o->script_text;
    in.scriptIsText = true;
  }
  if (o->has_base) {
    in.base = o->base;
    in.hasBase = true;
  }
  switch (o->mode) {
    case QLD_MODE_SHARED: in.mode = qld::detail::Mode::Shared; break;
    case QLD_MODE_RELOCATABLE:
      in.mode = qld::detail::Mode::Relocatable;
      break;
    default: in.mode = qld::detail::Mode::Exec; break;
  }
  in.pie = o->pie != 0;
  if (in.pie && in.mode == qld::detail::Mode::Exec) {
    // PIE executables link as shared-type images with an entry point.
    in.mode = qld::detail::Mode::Shared;
  }
  in.gcSections = o->gc_sections != 0;
  in.stripDebug = o->strip_debug != 0;
  in.allowUndefined = o->allow_undefined != 0;
  in.verbose = o->verbose != 0;
  if (o->map_file) in.mapFile = o->map_file;
  if (o->emulator) in.emulator = o->emulator;
  return in;
}

bool validOptions(const qld_options_t* o, std::string& error) {
  if (!o) { error = "null options"; return false; }
  if (o->n_inputs && !o->inputs) { error = "inputs is null"; return false; }
  if (o->n_libpaths && !o->libpaths) { error = "libpaths is null"; return false; }
  if (o->n_libs && !o->libs) { error = "libs is null"; return false; }
  if (o->use_script_text && !o->script_text) {
    error = "script_text is null while use_script_text is set";
    return false;
  }
  if (o->mode < QLD_MODE_EXEC || o->mode > QLD_MODE_RELOCATABLE) {
    error = "invalid link mode";
    return false;
  }
  return true;
}

bool writeOutput(const std::string& path, const std::vector<uint8_t>& bytes,
                 std::string& error) {
  if (path.empty()) return true;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot write " + path;
    return false;
  }
  out.write(reinterpret_cast<const char*>(bytes.data()),
            std::streamsize(bytes.size()));
  out.close();
  if (!out) {
    error = "write failed: " + path;
    return false;
  }
#ifndef _WIN32
  // Linked executables should run: mirror the input mode with +x.
  ::chmod(path.c_str(), 0755);
#endif
  return true;
}

int runInternal(const qld_options_t* opts, std::vector<uint8_t>& outBytes,
                char** errmsg, const qld::ScriptSDK *sdk = nullptr) try {
  std::string validation;
  if (!validOptions(opts, validation)) {
    setErr(errmsg, validation);
    return 1;
  }
  qld::detail::LinkState state;
  state.options = toInternal(opts);
  if (sdk && (state.options.scriptIsText || !state.options.script.empty())) {
    std::string source = state.options.scriptText;
    if (!state.options.scriptIsText) {
      std::ifstream in(state.options.script, std::ios::binary);
      if (!in) throw std::runtime_error("cannot open script " + state.options.script);
      source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    state.options.scriptText = sdk->preprocess(source, state.options.scriptIsText ? "<script>" : state.options.script);
    state.options.scriptIsText = true;
  }
  std::string error;
  std::vector<uint8_t> bytes;
  if (!qld::detail::linkState(state, bytes, error)) {
    setErr(errmsg, error.empty() ? "link failed" : error);
    return 1;
  }
  // Write -o and -Map side effects here so both C and C++ paths share them.
  if (!state.options.output.empty()) {
    if (!writeOutput(state.options.output, bytes, error)) {
      setErr(errmsg, error);
      return 1;
    }
  }
  if (state.options.verbose && !state.warnings.empty()) {
    std::string w;
    for (const auto& s : state.warnings) w += "qobjld: warning: " + s + "\n";
    // Warnings do not fail the link; surface through errmsg as info? Keep
    // stdout behaviour to the CLI; the API stays quiet.
    (void)w;
  }
  outBytes = std::move(bytes);
  if (errmsg) *errmsg = nullptr;
  return 0;
} catch (const std::exception &e) {
  setErr(errmsg, e.what()); return QLD_ERROR;
} catch (...) {
  setErr(errmsg, "linker extension threw an exception"); return QLD_ERROR;
}

}  // namespace

const char* qld_version(void) { return "1.0"; }

void qld_options_init(qld_options_t* o) {
  if (!o) return;
  std::memset(o, 0, sizeof(*o));
  o->mode = QLD_MODE_EXEC;
}

const char* qld_status_string(int status) {
  switch (status) {
    case QLD_OK: return "ok";
    case QLD_INVALID_ARGUMENT: return "invalid argument";
    case QLD_OUT_OF_MEMORY: return "out of memory";
    case QLD_IO_ERROR: return "I/O error";
    case QLD_PARSE_ERROR: return "parse error";
    case QLD_UNDEFINED_SYMBOL: return "undefined symbol";
    case QLD_UNSUPPORTED: return "unsupported";
    default: return "link error";
  }
}

void qld_free_string(char* s) { std::free(s); }

void qld_free_bytes(uint8_t* p, size_t n) {
  (void)n;
  std::free(p);
}

int qld_link(const qld_options_t* opts, char** errmsg) {
  std::vector<uint8_t> bytes;
  return runInternal(opts, bytes, errmsg);
}

static int linkBytes(const qld_options_t* opts, uint8_t** out_bytes,
                   size_t* out_size, char** errmsg, const qld::ScriptSDK *sdk = nullptr) {
  if (out_bytes) *out_bytes = nullptr;
  if (out_size) *out_size = 0;
  if (!out_bytes || !out_size) { setErr(errmsg, "output pointers must not be null"); return QLD_INVALID_ARGUMENT; }
  std::vector<uint8_t> bytes;
  int rc = runInternal(opts, bytes, errmsg, sdk);
  if (rc != 0) {
    if (out_bytes) *out_bytes = nullptr;
    if (out_size) *out_size = 0;
    return rc;
  }
  uint8_t* p = nullptr;
  if (!bytes.empty()) {
    p = (uint8_t*)std::malloc(bytes.size());
    if (!p) {
      setErr(errmsg, "out of memory");
      if (out_size) *out_size = 0;
      if (out_bytes) *out_bytes = nullptr;
      return 1;
    }
    std::memcpy(p, bytes.data(), bytes.size());
  }
  if (out_bytes) *out_bytes = p;
  if (out_size) *out_size = bytes.size();
  return 0;
}

int qld_link_bytes(const qld_options_t* opts, uint8_t** out_bytes,
                   size_t* out_size, char** errmsg) {
  return linkBytes(opts, out_bytes, out_size, errmsg);
}

int qld_check_script(const char* text, char** errmsg) {
  if (!text) {
    setErr(errmsg, "null script");
    return 1;
  }
  qld::script::ScriptFile sf;
  std::string error;
  std::string expanded;
  try { expanded = qld::ScriptSDK{}.preprocess(text); }
  catch (const std::exception &e) { setErr(errmsg, e.what()); return 1; }
  if (!qld::script::parseText(expanded, sf, error)) {
    setErr(errmsg, error.empty() ? "script parse error" : error);
    return 1;
  }
  if (errmsg) *errmsg = nullptr;
  return 0;
}

// --- Incremental builder -----------------------------------------------

struct qld_linker {
  qld::detail::LinkOptions opts;
  qld::ScriptSDK sdk;
};

int qld_register_directive(qld_linker_t *self, const char *name,
    qld_directive_handler callback, void *user, char **errmsg) {
  try {
    if (!self || !name || !callback) throw std::runtime_error("invalid directive registration");
    self->sdk.registerDirective(name, [callback, user](const qld::ScriptSDK::Arguments &args) {
      std::vector<const char *> pointers;
      for (const auto &a : args) pointers.push_back(a.c_str());
      struct Output { std::string text; bool failed = false; } output;
      char error[1024] = {};
      auto sink = [](void *opaque, const char *text, size_t size) -> int {
        auto &out = *static_cast<Output *>(opaque);
        if (out.failed || (!text && size) || size > 16 * 1024 * 1024 - out.text.size() || (size && std::memchr(text, 0, size))) {
          out.failed = true; return 1;
        }
        try { if (size) out.text.append(text, size); return 0; }
        catch (...) { out.failed = true; return 1; }
      };
      int status = callback(user, pointers.data(), pointers.size(), sink, &output, error, sizeof error);
      error[sizeof error - 1] = 0;
      if (output.failed) throw std::runtime_error("invalid or oversized generated script");
      if (status)
        throw std::runtime_error(error[0] ? error : "script directive failed");
      return output.text;
    });
    if (errmsg) *errmsg = nullptr;
    return 0;
  } catch (const std::exception &e) { setErr(errmsg, e.what()); return QLD_ERROR; }
}
int qld_define_macro(qld_linker_t *self, const char *name,
    const char *const *parameters, size_t count, const char *body, char **errmsg) {
  try {
    if (!self || !name || !body || (count && !parameters)) throw std::runtime_error("invalid macro definition");
    std::vector<std::string> params;
    for (size_t i = 0; i < count; ++i) {
      if (!parameters[i]) throw std::runtime_error("null macro parameter");
      params.emplace_back(parameters[i]);
    }
    self->sdk.defineMacro(name, std::move(params), body);
    if (errmsg) *errmsg = nullptr;
    return 0;
  } catch (const std::exception &e) { setErr(errmsg, e.what()); return QLD_ERROR; }
}
int qld_expand_script(qld_linker_t *self, const char *source, char **text, char **errmsg) {
  if (text) *text = nullptr;
  try {
    if (!self || !source || !text) throw std::runtime_error("invalid expansion arguments");
    auto out = self->sdk.preprocess(source);
    *text = static_cast<char *>(std::malloc(out.size() + 1));
    if (!*text) throw std::bad_alloc();
    std::memcpy(*text, out.c_str(), out.size() + 1);
    if (errmsg) *errmsg = nullptr;
    return 0;
  } catch (const std::exception &e) { setErr(errmsg, e.what()); return QLD_ERROR; }
  catch (...) { setErr(errmsg, "script directive threw an exception"); return QLD_ERROR; }
}

qld_linker_t* qld_create(void) { return new (std::nothrow) qld_linker(); }

void qld_destroy(qld_linker_t* self) { delete self; }

int qld_add_input(qld_linker_t* self, const char* path) {
  if (!self || !path) return 1;
  self->opts.inputs.emplace_back(path);
  return 0;
}

int qld_add_libpath(qld_linker_t* self, const char* path) {
  if (!self || !path) return 1;
  self->opts.libPaths.emplace_back(path);
  return 0;
}

int qld_add_library(qld_linker_t* self, const char* name) {
  if (!self || !name) return 1;
  self->opts.libs.emplace_back(name);
  return 0;
}

void qld_set_output(qld_linker_t* self, const char* path) {
  if (!self) return;
  self->opts.output = path ? path : "";
}

void qld_set_entry(qld_linker_t* self, const char* symbol) {
  if (!self) return;
  self->opts.entry = symbol ? symbol : "_start";
  self->opts.hasEntry = symbol != nullptr;
}

void qld_set_script_path(qld_linker_t* self, const char* path) {
  if (!self) return;
  self->opts.script = path ? path : "";
  self->opts.scriptIsText = false;
}

void qld_set_script_text(qld_linker_t* self, const char* text) {
  if (!self) return;
  self->opts.scriptText = text ? text : "";
  self->opts.scriptIsText = true;
}

void qld_set_base(qld_linker_t* self, uint64_t base) {
  if (!self) return;
  self->opts.base = base;
  self->opts.hasBase = true;
}

void qld_set_mode(qld_linker_t* self, qld_mode_t mode) {
  if (!self) return;
  switch (mode) {
    case QLD_MODE_SHARED:
      self->opts.mode = qld::detail::Mode::Shared;
      break;
    case QLD_MODE_RELOCATABLE:
      self->opts.mode = qld::detail::Mode::Relocatable;
      break;
    default: self->opts.mode = qld::detail::Mode::Exec; break;
  }
}

void qld_set_flag(qld_linker_t* self, const char* flag, int value) {
  if (!self || !flag) return;
  std::string f = flag;
  bool v = value != 0;
  if (f == "pie") {
    self->opts.pie = v;
    if (v) self->opts.mode = qld::detail::Mode::Shared;
  } else if (f == "gc-sections" || f == "gc_sections") {
    self->opts.gcSections = v;
  } else if (f == "strip-debug" || f == "strip_debug") {
    self->opts.stripDebug = v;
  } else if (f == "allow-undefined" || f == "allow_undefined") {
    self->opts.allowUndefined = v;
  } else if (f == "verbose") {
    self->opts.verbose = v;
  }
}

static qld_options_t toC(const qld_linker* self, std::vector<const char*>& a,
                         std::vector<const char*>& b,
                         std::vector<const char*>& c) {
  qld_options_t o{};
  for (const auto& s : self->opts.inputs) a.push_back(s.c_str());
  for (const auto& s : self->opts.libPaths) b.push_back(s.c_str());
  for (const auto& s : self->opts.libs) c.push_back(s.c_str());
  o.inputs = a.empty() ? nullptr : a.data();
  o.n_inputs = a.size();
  o.libpaths = b.empty() ? nullptr : b.data();
  o.n_libpaths = b.size();
  o.libs = c.empty() ? nullptr : c.data();
  o.n_libs = c.size();
  o.output = self->opts.output.empty() ? nullptr : self->opts.output.c_str();
  o.entry = self->opts.hasEntry ? self->opts.entry.c_str() : nullptr;
  o.script = self->opts.script.empty() ? nullptr : self->opts.script.c_str();
  o.script_text =
      self->opts.scriptIsText ? self->opts.scriptText.c_str() : nullptr;
  o.use_script_text = self->opts.scriptIsText ? 1 : 0;
  o.base = self->opts.base;
  o.has_base = self->opts.hasBase ? 1 : 0;
  o.mode = self->opts.mode == qld::detail::Mode::Shared
               ? QLD_MODE_SHARED
               : self->opts.mode == qld::detail::Mode::Relocatable
                     ? QLD_MODE_RELOCATABLE
                     : QLD_MODE_EXEC;
  o.pie = self->opts.pie ? 1 : 0;
  o.gc_sections = self->opts.gcSections ? 1 : 0;
  o.strip_debug = self->opts.stripDebug ? 1 : 0;
  o.allow_undefined = self->opts.allowUndefined ? 1 : 0;
  o.verbose = self->opts.verbose ? 1 : 0;
  o.map_file =
      self->opts.mapFile.empty() ? nullptr : self->opts.mapFile.c_str();
  return o;
}

int qld_run(qld_linker_t* self, char** errmsg) {
  if (!self) {
    setErr(errmsg, "null linker");
    return 1;
  }
  std::vector<const char*> a, b, c;
  qld_options_t o = toC(self, a, b, c);
  std::vector<uint8_t> bytes;
  return runInternal(&o, bytes, errmsg, &self->sdk);
}

int qld_run_bytes(qld_linker_t* self, uint8_t** out_bytes, size_t* out_size,
                  char** errmsg) {
  if (!self) {
    setErr(errmsg, "null linker");
    return 1;
  }
  std::vector<const char*> a, b, c;
  qld_options_t o = toC(self, a, b, c);
  return linkBytes(&o, out_bytes, out_size, errmsg, &self->sdk);
}
