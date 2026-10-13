#include "../xbind.h"

#include <QaMRpp.hpp>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using ::qamrpp::Context;
using ::qamrpp::NativeFn;
using ::qamrpp::Value;
using ::qamrpp::ValuePtr;

ValuePtr nil_value() { return std::make_shared<Value>(); }
ValuePtr string_value(const std::string &value) { return std::make_shared<Value>(value); }
ValuePtr int_value(int64_t value) { return std::make_shared<Value>(value); }
ValuePtr table_value() { return Value::make_table(); }

ValuePtr borrow(qamrpp_value *value) {
  if (!value) {
    return nil_value();
  }
  return ValuePtr(reinterpret_cast<Value *>(value), [](Value *) {});
}

qamrpp_value *finish(Context &ctx, const ValuePtr &value) {
  ctx.last_call_returns.clear();
  return reinterpret_cast<qamrpp_value *>(new Value(*(value ? value : nil_value())));
}

qamrpp_value *fail(qamrpp_context *ctx, qamrpp_error_code code, const char *message) {
  qamrpp_set_error(ctx, code, message);
  return finish(*reinterpret_cast<Context *>(ctx), nil_value());
}

void raw_set(Context &ctx, const ValuePtr &table, const std::string &key, const ValuePtr &value) {
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

void array_set(Context &ctx, const ValuePtr &table, int64_t index, const ValuePtr &value) {
  ctx.table_raw_set(table, int_value(index), value ? value : nil_value());
}

std::string string_arg(qamrpp_value **argv, size_t argc, size_t index, const char *field) {
  ValuePtr value = index < argc ? borrow(argv[index]) : nil_value();
  if (!value || value->type != Value::STRING || value->string_value.empty()) {
    throw std::runtime_error(std::string(field) + " must be a non-empty string");
  }
  return value->string_value;
}

ValuePtr optional_table(qamrpp_value **argv, size_t argc, size_t index) {
  if (index >= argc || !argv[index]) {
    return nil_value();
  }
  ValuePtr table = borrow(argv[index]);
  return table && table->type == Value::TABLE ? table : nil_value();
}

std::string string_field(Context &ctx, const ValuePtr &table, const std::string &key) {
  ValuePtr value = raw_get(ctx, table, key);
  return value && value->type == Value::STRING ? value->string_value : std::string();
}

ValuePtr copy_string_array(Context &ctx, const ValuePtr &source) {
  ValuePtr array = table_value();
  if (!source || source->type != Value::TABLE) {
    return array;
  }
  for (int64_t index = 1;; ++index) {
    ValuePtr item = array_get(ctx, source, index);
    if (!item || item->type == Value::NIL) {
      break;
    }
    if (item->type == Value::STRING) {
      array_set(ctx, array, index, string_value(item->string_value));
    }
  }
  return array;
}

void copy_string_field(Context &ctx,
                       const ValuePtr &dst,
                       const ValuePtr &src,
                       const std::string &key) {
  std::string value = string_field(ctx, src, key);
  if (!value.empty()) {
    raw_set(ctx, dst, key, string_value(value));
  }
}

ValuePtr make_target(Context &ctx, const std::string &name, const ValuePtr &options) {
  ValuePtr table = table_value();
  raw_set(ctx, table, "kind", string_value("target"));
  raw_set(ctx, table, "name", string_value(name));
  copy_string_field(ctx, table, options, "module");
  copy_string_field(ctx, table, options, "crate");
  raw_set(ctx, table, "features", copy_string_array(ctx, raw_get(ctx, options, "features")));
  raw_set(ctx, table, "typedefs", copy_string_array(ctx, raw_get(ctx, options, "typedefs")));
  return table;
}

ValuePtr make_param(Context &ctx, const std::string &name, const std::string &c_type) {
  ValuePtr table = table_value();
  raw_set(ctx, table, "kind", string_value("param"));
  raw_set(ctx, table, "name", string_value(name));
  raw_set(ctx, table, "c", string_value(c_type));
  return table;
}

ValuePtr copy_param_array(Context &ctx, const ValuePtr &source) {
  ValuePtr array = table_value();
  if (!source || source->type != Value::TABLE) {
    return array;
  }
  for (int64_t index = 1;; ++index) {
    ValuePtr item = array_get(ctx, source, index);
    if (!item || item->type == Value::NIL) {
      break;
    }
    if (item->type == Value::TABLE) {
      const std::string name = string_field(ctx, item, "name");
      const std::string c_type = string_field(ctx, item, "c");
      if (!name.empty() && !c_type.empty()) {
        array_set(ctx, array, index, make_param(ctx, name, c_type));
      }
    }
  }
  return array;
}

ValuePtr make_function(Context &ctx, const std::string &name, const ValuePtr &options) {
  ValuePtr table = table_value();
  std::string returns = string_field(ctx, options, "returns");
  if (returns.empty()) {
    returns = "void";
  }
  raw_set(ctx, table, "kind", string_value("function"));
  raw_set(ctx, table, "name", string_value(name));
  raw_set(ctx, table, "returns", string_value(returns));
  raw_set(ctx, table, "params", copy_param_array(ctx, raw_get(ctx, options, "params")));
  raw_set(ctx, table, "features", copy_string_array(ctx, raw_get(ctx, options, "features")));
  return table;
}

ValuePtr copy_target_array(Context &ctx, const ValuePtr &source) {
  ValuePtr array = table_value();
  if (!source || source->type != Value::TABLE) {
    return array;
  }
  for (int64_t index = 1;; ++index) {
    ValuePtr item = array_get(ctx, source, index);
    if (!item || item->type == Value::NIL) {
      break;
    }
    if (item->type == Value::TABLE) {
      const std::string name = string_field(ctx, item, "name");
      if (!name.empty()) {
        array_set(ctx, array, index, make_target(ctx, name, item));
      }
    }
  }
  return array;
}

ValuePtr copy_function_array(Context &ctx, const ValuePtr &source) {
  ValuePtr array = table_value();
  if (!source || source->type != Value::TABLE) {
    return array;
  }
  for (int64_t index = 1;; ++index) {
    ValuePtr item = array_get(ctx, source, index);
    if (!item || item->type == Value::NIL) {
      break;
    }
    if (item->type == Value::TABLE) {
      const std::string name = string_field(ctx, item, "name");
      if (!name.empty()) {
        array_set(ctx, array, index, make_function(ctx, name, item));
      }
    }
  }
  return array;
}

ValuePtr make_library(Context &ctx, const ValuePtr &options) {
  if (!options || options->type != Value::TABLE) {
    throw std::runtime_error("library expects an options table");
  }
  const std::string name = string_field(ctx, options, "name");
  if (name.empty()) {
    throw std::runtime_error("library name is required");
  }
  ValuePtr table = table_value();
  raw_set(ctx, table, "kind", string_value("library"));
  raw_set(ctx, table, "name", string_value(name));
  raw_set(ctx, table, "version", string_value(XBIND_VERSION));
  raw_set(ctx, table, "headers", copy_string_array(ctx, raw_get(ctx, options, "headers")));
  raw_set(ctx, table, "stdlib", copy_string_array(ctx, raw_get(ctx, options, "stdlib")));
  raw_set(ctx, table, "targets", copy_target_array(ctx, raw_get(ctx, options, "targets")));
  raw_set(ctx, table, "functions", copy_function_array(ctx, raw_get(ctx, options, "functions")));
  return table;
}

template <typename Fn>
qamrpp_value *guard(qamrpp_context *ctx, Fn &&fn) {
  try {
    return finish(*reinterpret_cast<Context *>(ctx), fn(*reinterpret_cast<Context *>(ctx)));
  } catch (const std::exception &error) {
    return fail(ctx, QAMRPP_ERR_BAD_ARGUMENT, error.what());
  }
}

qamrpp_value *lua_version(qamrpp_context *ctx, qamrpp_value **, size_t) {
  return guard(ctx, [](Context &) { return string_value(xbind_version()); });
}

qamrpp_value *lua_target_count(qamrpp_context *ctx, qamrpp_value **, size_t) {
  return guard(ctx, [](Context &) { return int_value(static_cast<int64_t>(xbind_target_count())); });
}

qamrpp_value *lua_find_target(qamrpp_context *ctx, qamrpp_value **argv, size_t argc) {
  return guard(ctx, [&](Context &context) {
    const std::string name = string_arg(argv, argc, 0, "target name");
    const xbind_target_info *target = xbind_find_target(name.c_str());
    return target ? make_target(context, target->name, nil_value()) : nil_value();
  });
}

qamrpp_value *lua_target(qamrpp_context *ctx, qamrpp_value **argv, size_t argc) {
  return guard(ctx, [&](Context &context) {
    return make_target(context, string_arg(argv, argc, 0, "target name"), optional_table(argv, argc, 1));
  });
}

qamrpp_value *lua_typedef(qamrpp_context *ctx, qamrpp_value **argv, size_t argc) {
  return guard(ctx, [&](Context &context) {
    ValuePtr table = table_value();
    raw_set(context, table, "kind", string_value("typedef"));
    raw_set(context, table, "c", string_value(string_arg(argv, argc, 0, "typedef C type")));
    raw_set(context, table, "native", string_value(string_arg(argv, argc, 1, "typedef native type")));
    return table;
  });
}

qamrpp_value *lua_param(qamrpp_context *ctx, qamrpp_value **argv, size_t argc) {
  return guard(ctx, [&](Context &context) {
    return make_param(context, string_arg(argv, argc, 0, "param name"),
                      string_arg(argv, argc, 1, "param C type"));
  });
}

qamrpp_value *lua_func(qamrpp_context *ctx, qamrpp_value **argv, size_t argc) {
  return guard(ctx, [&](Context &context) {
    return make_function(context, string_arg(argv, argc, 0, "function name"), optional_table(argv, argc, 1));
  });
}

qamrpp_value *lua_module(qamrpp_context *ctx, qamrpp_value **argv, size_t argc) {
  return guard(ctx, [&](Context &context) {
    ValuePtr table = table_value();
    raw_set(context, table, "kind", string_value("module"));
    raw_set(context, table, "name", string_value(string_arg(argv, argc, 0, "module name")));
    return table;
  });
}

qamrpp_value *lua_library(qamrpp_context *ctx, qamrpp_value **argv, size_t argc) {
  return guard(ctx, [&](Context &context) {
    return make_library(context, argc > 0 ? borrow(argv[0]) : nil_value());
  });
}

qamrpp_value *lua_validate_library(qamrpp_context *ctx, qamrpp_value **argv, size_t argc) {
  return guard(ctx, [&](Context &context) {
    ValuePtr table = argc > 0 ? borrow(argv[0]) : nil_value();
    if (string_field(context, table, "kind") != "library" ||
        string_field(context, table, "name").empty()) {
      throw std::runtime_error("manifest must be produced by xbind.library");
    }
    return make_library(context, table);
  });
}

qamrpp_value *lua_require(qamrpp_context *ctx, qamrpp_value **argv, size_t argc);

const qamrpp_native_binding kBindings[] = {
    {"require", lua_require},
    {"xbind_version", lua_version},
    {"xbind_target_count", lua_target_count},
    {"xbind_find_target", lua_find_target},
    {"xbind_library", lua_library},
    {"xbind_target", lua_target},
    {"xbind_typedef", lua_typedef},
    {"xbind_param", lua_param},
    {"xbind_func", lua_func},
    {"xbind_function", lua_func},
    {"xbind_module", lua_module},
    {"xbind_validate_library", lua_validate_library},
};

ValuePtr module_table(Context &ctx) {
  ValuePtr table = table_value();
  raw_set(ctx, table, "VERSION", string_value(XBIND_VERSION));
  for (const qamrpp_native_binding &binding : kBindings) {
    if (std::strcmp(binding.name, "require") == 0) {
      continue;
    }
    const char *export_name = binding.name;
    if (std::strncmp(export_name, "xbind_", 6) == 0) {
      export_name += 6;
    }
    ValuePtr fn = ctx.lookup_name(binding.name);
    if (fn) {
      raw_set(ctx, table, export_name, fn);
    }
  }
  return table;
}

qamrpp_value *lua_require(qamrpp_context *ctx, qamrpp_value **argv, size_t argc) {
  return guard(ctx, [&](Context &context) {
    const std::string name = string_arg(argv, argc, 0, "module name");
    if (name == "lxbind" || name == "xbind") {
      return module_table(context);
    }
    throw std::runtime_error("unknown XBind Lua module: " + name);
  });
}

int on_load(qamrpp_context *ctx, const qamrpp_host_api *) {
  Context &context = *reinterpret_cast<Context *>(ctx);
  context.assign_name("xbind", module_table(context));
  context.assign_name("lxbind", module_table(context));
  return 0;
}

const qamrpp_library_descriptor kDescriptor = {
    QAMRPP_LIBRARY_API_VERSION,
    "xbind",
    kBindings,
    QAMRPP_ARRAY_COUNT(kBindings),
    on_load,
    nullptr,
};

} // namespace

extern "C" QAMRPP_EXPORT const qamrpp_library_descriptor *qamrpp_get_library_descriptor(void) {
  return &kDescriptor;
}
