#include "xbind_qamrpp.hpp"

#include "xbind.h"

#include <QaMRpp.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace xbind::qamrpp {
namespace {

using ::qamrpp::Context;
using ::qamrpp::NativeFn;
using ::qamrpp::Value;
using ::qamrpp::ValuePtr;

ValuePtr nil_value() { return std::make_shared<Value>(); }

ValuePtr string_value(const std::string &value) {
  return std::make_shared<Value>(value);
}

ValuePtr int_value(int64_t value) { return std::make_shared<Value>(value); }

ValuePtr single_return(Context &ctx, const ValuePtr &value) {
  ctx.last_call_returns.assign(1, value ? value : nil_value());
  return value ? value : nil_value();
}

std::string read_text_file(const std::string &path) {
  std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
  if (!input.good()) {
    throw std::runtime_error("could not read XBind spec: " + path);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string require_string_arg(const std::vector<ValuePtr> &args,
                               size_t index,
                               const char *name) {
  if (index >= args.size() || !args[index] || args[index]->type != Value::STRING ||
      args[index]->string_value.empty()) {
    throw std::runtime_error(std::string(name) + " must be a non-empty string");
  }
  return args[index]->string_value;
}

ValuePtr optional_table_arg(const std::vector<ValuePtr> &args, size_t index) {
  if (index >= args.size() || !args[index] || args[index]->type == Value::NIL) {
    return Value::make_table();
  }
  if (args[index]->type != Value::TABLE) {
    throw std::runtime_error("options must be a table");
  }
  return args[index];
}

ValuePtr required_table_arg(const std::vector<ValuePtr> &args,
                            size_t index,
                            const char *name) {
  if (index >= args.size() || !args[index] || args[index]->type != Value::TABLE) {
    throw std::runtime_error(std::string(name) + " must be a table");
  }
  return args[index];
}

void raw_set(Context &ctx,
             const ValuePtr &table,
             const std::string &key,
             const ValuePtr &value) {
  ctx.table_raw_set(table, string_value(key), value ? value : nil_value());
}

ValuePtr raw_get(Context &ctx, const ValuePtr &table, const std::string &key) {
  if (!table || table->type != Value::TABLE) {
    return nil_value();
  }
  return ctx.table_raw_get(table, string_value(key));
}

ValuePtr array_get(Context &ctx, const ValuePtr &table, int64_t index) {
  if (!table || table->type != Value::TABLE) {
    return nil_value();
  }
  return ctx.table_raw_get(table, int_value(index));
}

std::string string_field(Context &ctx,
                         const ValuePtr &table,
                         const std::string &key,
                         bool required = false) {
  ValuePtr value = raw_get(ctx, table, key);
  if (!value || value->type == Value::NIL) {
    if (required) {
      throw std::runtime_error("manifest field '" + key + "' is required");
    }
    return std::string();
  }
  if (value->type != Value::STRING) {
    throw std::runtime_error("manifest field '" + key + "' must be a string");
  }
  return value->string_value;
}

void set_default_array(Context &ctx, const ValuePtr &table, const std::string &key) {
  ValuePtr value = raw_get(ctx, table, key);
  if (!value || value->type == Value::NIL) {
    raw_set(ctx, table, key, Value::make_table());
  } else if (value->type != Value::TABLE) {
    throw std::runtime_error(key + " must be an array table");
  }
}

void set_default_string(Context &ctx,
                        const ValuePtr &table,
                        const std::string &key,
                        const std::string &value) {
  ValuePtr current = raw_get(ctx, table, key);
  if (!current || current->type == Value::NIL) {
    raw_set(ctx, table, key, string_value(value));
  } else if (current->type != Value::STRING) {
    throw std::runtime_error(key + " must be a string");
  }
}

ValuePtr make_lxbind_module(Context &ctx);

ValuePtr native_target(Context &ctx, std::vector<ValuePtr> &args) {
  const std::string name = require_string_arg(args, 0, "target name");
  ValuePtr table = optional_table_arg(args, 1);
  raw_set(ctx, table, "kind", string_value("target"));
  raw_set(ctx, table, "name", string_value(name));
  set_default_array(ctx, table, "features");
  set_default_array(ctx, table, "typedefs");
  return single_return(ctx, table);
}

ValuePtr native_typedef(Context &ctx, std::vector<ValuePtr> &args) {
  const std::string c_type = require_string_arg(args, 0, "typedef c type");
  const std::string native_type = require_string_arg(args, 1, "typedef native type");
  ValuePtr table = optional_table_arg(args, 2);
  raw_set(ctx, table, "kind", string_value("typedef"));
  raw_set(ctx, table, "c", string_value(c_type));
  raw_set(ctx, table, "native", string_value(native_type));
  return single_return(ctx, table);
}

ValuePtr native_param(Context &ctx, std::vector<ValuePtr> &args) {
  const std::string name = require_string_arg(args, 0, "param name");
  const std::string c_type = require_string_arg(args, 1, "param c type");
  ValuePtr table = optional_table_arg(args, 2);
  raw_set(ctx, table, "kind", string_value("param"));
  raw_set(ctx, table, "name", string_value(name));
  raw_set(ctx, table, "c", string_value(c_type));
  return single_return(ctx, table);
}

ValuePtr native_func(Context &ctx, std::vector<ValuePtr> &args) {
  const std::string name = require_string_arg(args, 0, "function name");
  ValuePtr table = optional_table_arg(args, 1);
  raw_set(ctx, table, "kind", string_value("function"));
  raw_set(ctx, table, "name", string_value(name));
  set_default_string(ctx, table, "returns", "void");
  set_default_array(ctx, table, "params");
  set_default_array(ctx, table, "features");
  return single_return(ctx, table);
}

ValuePtr native_module(Context &ctx, std::vector<ValuePtr> &args) {
  const std::string name = require_string_arg(args, 0, "module name");
  ValuePtr table = optional_table_arg(args, 1);
  raw_set(ctx, table, "kind", string_value("module"));
  raw_set(ctx, table, "name", string_value(name));
  set_default_array(ctx, table, "functions");
  set_default_array(ctx, table, "typedefs");
  return single_return(ctx, table);
}

ValuePtr native_library(Context &ctx, std::vector<ValuePtr> &args) {
  ValuePtr table = required_table_arg(args, 0, "library options");
  raw_set(ctx, table, "kind", string_value("library"));
  (void)string_field(ctx, table, "name", true);
  set_default_string(ctx, table, "version", XBIND_VERSION);
  set_default_array(ctx, table, "headers");
  set_default_array(ctx, table, "targets");
  set_default_array(ctx, table, "typedefs");
  set_default_array(ctx, table, "functions");
  set_default_array(ctx, table, "modules");
  set_default_array(ctx, table, "stdlib");
  return single_return(ctx, table);
}

ValuePtr native_validate_library(Context &ctx, std::vector<ValuePtr> &args) {
  ValuePtr table = required_table_arg(args, 0, "manifest");
  const std::string kind = string_field(ctx, table, "kind", true);
  if (kind != "library") {
    throw std::runtime_error("manifest must be produced by xbind.library");
  }
  (void)string_field(ctx, table, "name", true);
  return single_return(ctx, table);
}

void table_set_native(Context &ctx,
                      const ValuePtr &table,
                      const std::string &name,
                      NativeFn function) {
  raw_set(ctx, table, name, std::make_shared<Value>(std::move(function)));
}

ValuePtr make_lxbind_module(Context &ctx) {
  ValuePtr module = Value::make_table();
  raw_set(ctx, module, "VERSION", string_value(XBIND_VERSION));
  table_set_native(ctx, module, "library", native_library);
  table_set_native(ctx, module, "target", native_target);
  table_set_native(ctx, module, "typedef", native_typedef);
  table_set_native(ctx, module, "param", native_param);
  table_set_native(ctx, module, "func", native_func);
  table_set_native(ctx, module, "function", native_func);
  table_set_native(ctx, module, "module", native_module);
  table_set_native(ctx, module, "validate_library", native_validate_library);
  return module;
}

void install_lxbind(Context &ctx) {
  ValuePtr module = make_lxbind_module(ctx);
  ctx.assign_name("xbind", module);
  ctx.assign_name("lxbind", module);
  ctx.register_native("require", [module](Context &call_ctx, std::vector<ValuePtr> &args) -> ValuePtr {
    const std::string name = require_string_arg(args, 0, "module name");
    if (name == "lxbind" || name == "xbind") {
      return single_return(call_ctx, module);
    }
    throw std::runtime_error("unknown XBind Lua module: " + name);
  });
}

std::vector<std::string> string_array(Context &ctx,
                                      const ValuePtr &table,
                                      const std::string &field) {
  std::vector<std::string> result;
  ValuePtr array = raw_get(ctx, table, field);
  if (!array || array->type == Value::NIL) {
    return result;
  }
  if (array->type != Value::TABLE) {
    throw std::runtime_error("manifest field '" + field + "' must be an array");
  }
  for (int64_t index = 1;; ++index) {
    ValuePtr value = array_get(ctx, array, index);
    if (!value || value->type == Value::NIL) {
      break;
    }
    if (value->type != Value::STRING) {
      throw std::runtime_error("manifest field '" + field + "' contains a non-string value");
    }
    result.push_back(value->string_value);
  }
  return result;
}

std::vector<std::string> target_array(Context &ctx, const ValuePtr &table) {
  std::vector<std::string> result;
  ValuePtr array = raw_get(ctx, table, "targets");
  if (!array || array->type == Value::NIL) {
    return result;
  }
  if (array->type != Value::TABLE) {
    throw std::runtime_error("manifest field 'targets' must be an array");
  }
  for (int64_t index = 1;; ++index) {
    ValuePtr value = array_get(ctx, array, index);
    if (!value || value->type == Value::NIL) {
      break;
    }
    if (value->type != Value::TABLE) {
      throw std::runtime_error("manifest target entries must be tables");
    }
    result.push_back(string_field(ctx, value, "name", true));
  }
  return result;
}

std::vector<Param> param_array(Context &ctx, const ValuePtr &function_table) {
  std::vector<Param> result;
  ValuePtr array = raw_get(ctx, function_table, "params");
  if (!array || array->type == Value::NIL) {
    return result;
  }
  if (array->type != Value::TABLE) {
    throw std::runtime_error("function params must be an array");
  }
  for (int64_t index = 1;; ++index) {
    ValuePtr value = array_get(ctx, array, index);
    if (!value || value->type == Value::NIL) {
      break;
    }
    if (value->type != Value::TABLE) {
      throw std::runtime_error("function params entries must be tables");
    }
    Param param;
    param.name = string_field(ctx, value, "name", true);
    param.c_type = string_field(ctx, value, "c", true);
    result.push_back(std::move(param));
  }
  return result;
}

std::vector<Function> function_array(Context &ctx, const ValuePtr &table) {
  std::vector<Function> result;
  ValuePtr array = raw_get(ctx, table, "functions");
  if (!array || array->type == Value::NIL) {
    return result;
  }
  if (array->type != Value::TABLE) {
    throw std::runtime_error("manifest field 'functions' must be an array");
  }
  for (int64_t index = 1;; ++index) {
    ValuePtr value = array_get(ctx, array, index);
    if (!value || value->type == Value::NIL) {
      break;
    }
    if (value->type != Value::TABLE) {
      throw std::runtime_error("manifest function entries must be tables");
    }
    Function function;
    function.name = string_field(ctx, value, "name", true);
    function.returns = string_field(ctx, value, "returns", true);
    function.params = param_array(ctx, value);
    result.push_back(std::move(function));
  }
  return result;
}

Manifest manifest_from_value(Context &ctx, const ValuePtr &value, std::string path) {
  if (!value || value->type != Value::TABLE) {
    throw std::runtime_error("XBind spec did not return a library table");
  }
  const std::string kind = string_field(ctx, value, "kind", true);
  if (kind != "library") {
    throw std::runtime_error("XBind spec returned kind='" + kind + "', expected 'library'");
  }
  Manifest manifest;
  manifest.path = std::move(path);
  manifest.name = string_field(ctx, value, "name", true);
  manifest.version = string_field(ctx, value, "version", true);
  manifest.headers = string_array(ctx, value, "headers");
  manifest.stdlib_modules = string_array(ctx, value, "stdlib");
  manifest.targets = target_array(ctx, value);
  manifest.functions = function_array(ctx, value);
  return manifest;
}

void dump_list(std::ostringstream &out,
               const char *name,
               const std::vector<std::string> &values) {
  out << name << ":";
  if (values.empty()) {
    out << "\n";
    return;
  }
  for (const std::string &value : values) {
    out << " " << value;
  }
  out << "\n";
}

} // namespace

Manifest load_spec_file(const std::string &path) {
  Context ctx;
  install_lxbind(ctx);
  try {
    ValuePtr result = ctx.run(read_text_file(path));
    return manifest_from_value(ctx, result, path);
  } catch (const std::exception &error) {
    throw std::runtime_error(path + ": " + error.what());
  }
}

std::string dump_manifest(const Manifest &manifest) {
  std::ostringstream out;
  out << "spec: " << manifest.path << "\n";
  out << "library: " << manifest.name << "\n";
  out << "version: " << manifest.version << "\n";
  dump_list(out, "headers", manifest.headers);
  dump_list(out, "stdlib", manifest.stdlib_modules);
  dump_list(out, "targets", manifest.targets);
  out << "functions:\n";
  for (const Function &function : manifest.functions) {
    out << "  " << function.name << " -> " << function.returns << "(";
    for (size_t index = 0; index < function.params.size(); ++index) {
      if (index != 0) {
        out << ", ";
      }
      out << function.params[index].name << ": " << function.params[index].c_type;
    }
    out << ")\n";
  }
  return out.str();
}

} // namespace xbind::qamrpp
