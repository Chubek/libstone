#include "RewriteBanks.hpp"
#include <json-c/json.h>
#include <yaml.h>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace novostone::action {
namespace {
struct YamlParser {
  yaml_parser_t parser{};
  YamlParser() {
    if (!yaml_parser_initialize(&parser)) throw std::bad_alloc();
  }
  ~YamlParser() { yaml_parser_delete(&parser); }
};
struct YamlDocument {
  yaml_document_t document{};
  bool loaded = false;
  ~YamlDocument() { if (loaded) yaml_document_delete(&document); }
};
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
BankValue scalar(const yaml_node_t& node) {
  std::string value(reinterpret_cast<const char*>(node.data.scalar.value),
                    node.data.scalar.length);
  const std::string tag(reinterpret_cast<const char*>(node.tag));
  if (tag != YAML_STR_TAG && tag != YAML_NULL_TAG && tag != YAML_BOOL_TAG &&
      tag != YAML_INT_TAG && tag != YAML_FLOAT_TAG)
    throw std::invalid_argument("unsupported YAML scalar tag " + tag);
  if (node.data.scalar.style != YAML_PLAIN_SCALAR_STYLE && tag == YAML_STR_TAG)
    return BankValue(value);
  if (value == "null" || value == "~" || value.empty()) return {};
  if (value == "true") return BankValue(true);
  if (value == "false") return BankValue(false);
  std::int64_t integer = 0;
  auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), integer);
  if (end == value.data() + value.size()) {
    if (error == std::errc{}) return BankValue(integer);
    if (error == std::errc::result_out_of_range)
      throw std::invalid_argument("attachment integer exceeds signed 64-bit range");
  }
  if (value.find_first_of(".eE") != std::string::npos) {
    double number = 0;
    auto [last, status] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (last == value.data() + value.size()) {
      if (status != std::errc{} || !std::isfinite(number))
        throw std::invalid_argument("non-finite or out-of-range attachment number");
      return BankValue(number);
    }
  }
  if (tag != YAML_STR_TAG)
    throw std::invalid_argument("invalid explicitly typed YAML scalar");
  return BankValue(value);
}
BankValue yamlValue(yaml_document_t& document, yaml_node_t* node,
                    std::size_t depth, std::size_t& count,
                    std::set<const yaml_node_t*>& active) {
  if (!node || depth == 0 || ++count > 65536)
    throw std::invalid_argument("attachment depth/node limit exceeded");
  if (!active.insert(node).second)
    throw std::invalid_argument("cyclic YAML alias");
  BankValue result;
  switch (node->type) {
  case YAML_SCALAR_NODE: result = scalar(*node); break;
  case YAML_SEQUENCE_NODE: {
    if (std::string(reinterpret_cast<const char*>(node->tag)) != YAML_SEQ_TAG)
      throw std::invalid_argument("unsupported YAML sequence tag");
    BankValue::Array array;
    for (auto it = node->data.sequence.items.start; it != node->data.sequence.items.top; ++it)
      array.push_back(yamlValue(document, yaml_document_get_node(&document, *it),
                                depth - 1, count, active));
    result = BankValue(std::move(array));
    break;
  }
  case YAML_MAPPING_NODE: {
    if (std::string(reinterpret_cast<const char*>(node->tag)) != YAML_MAP_TAG)
      throw std::invalid_argument("unsupported YAML mapping tag");
    BankValue::Object object;
    for (auto it = node->data.mapping.pairs.start; it != node->data.mapping.pairs.top; ++it) {
      auto key = yaml_document_get_node(&document, it->key);
      if (!key || key->type != YAML_SCALAR_NODE ||
          std::string(reinterpret_cast<const char*>(key->tag)) != YAML_STR_TAG)
        throw std::invalid_argument("attachment object keys must be strings");
      std::string name(reinterpret_cast<const char*>(key->data.scalar.value),
                       key->data.scalar.length);
      if (name == "<<") throw std::invalid_argument("YAML merge keys are unsupported");
      if (object.contains(name)) throw std::invalid_argument("duplicate attachment key '" + name + "'");
      object.emplace(name, yamlValue(document, yaml_document_get_node(&document, it->value),
                                     depth - 1, count, active));
    }
    result = BankValue(std::move(object));
    break;
  }
  default: throw std::invalid_argument("invalid YAML node");
  }
  active.erase(node);
  return result;
}
BankValue fromJson(json_object* object, std::size_t depth) {
  if (!depth) throw std::invalid_argument("attachment depth limit exceeded");
  switch (json_object_get_type(object)) {
  case json_type_null: return {};
  case json_type_boolean: return BankValue(json_object_get_boolean(object) != 0);
  case json_type_int: return BankValue(static_cast<std::int64_t>(json_object_get_int64(object)));
  case json_type_double: {
    auto value = json_object_get_double(object);
    if (!std::isfinite(value)) throw std::invalid_argument("non-finite JSON number");
    return BankValue(value);
  }
  case json_type_string:
    return BankValue(std::string(json_object_get_string(object),
                                static_cast<std::size_t>(json_object_get_string_len(object))));
  case json_type_array: {
    BankValue::Array result;
    for (std::size_t i = 0; i < json_object_array_length(object); ++i)
      result.push_back(fromJson(json_object_array_get_idx(object, i), depth - 1));
    return BankValue(std::move(result));
  }
  case json_type_object: {
    BankValue::Object result;
    json_object_object_foreach(object, key, value) {
      result.emplace(key, fromJson(value, depth - 1));
    }
    return BankValue(std::move(result));
  }
  }
  throw std::invalid_argument("unsupported JSON value");
}
json_object* toJson(const BankValue& value, std::size_t depth) {
  if (!depth) throw std::invalid_argument("JSON output depth limit exceeded");
  return std::visit([&](const auto& item) -> json_object* {
    using T = std::decay_t<decltype(item)>;
    if constexpr (std::is_same_v<T, std::nullptr_t>) return nullptr;
    else if constexpr (std::is_same_v<T, bool>) return json_object_new_boolean(item);
    else if constexpr (std::is_same_v<T, std::int64_t>) return json_object_new_int64(item);
    else if constexpr (std::is_same_v<T, double>) {
      if (!std::isfinite(item)) throw std::invalid_argument("non-finite JSON output");
      return json_object_new_double(item);
    } else if constexpr (std::is_same_v<T, std::string>) {
      if (item.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("JSON string too large");
      return json_object_new_string_len(item.data(), static_cast<int>(item.size()));
    } else {
      Json object(std::is_same_v<T, BankValue::Array> ? json_object_new_array() :
                                                       json_object_new_object(), json_object_put);
      if (!object) throw std::bad_alloc();
      if constexpr (std::is_same_v<T, BankValue::Array>) {
        for (const auto& child : item) {
          Json converted(toJson(child, depth - 1), json_object_put);
          if (json_object_array_add(object.get(), converted.get()) != 0) throw std::bad_alloc();
          converted.release();
        }
      } else {
        for (const auto& [key, child] : item) {
          if (key.find('\0') != std::string::npos)
            throw std::invalid_argument("NUL byte in JSON object key");
          Json converted(toJson(child, depth - 1), json_object_put);
          if (json_object_object_add(object.get(), key.c_str(), converted.get()) != 0) throw std::bad_alloc();
          converted.release();
        }
      }
      return object.release();
    }
  }, value.value);
}
}
BankValue parseBankAttachment(std::string_view text, std::string_view format,
                              std::string source, std::size_t max_depth) {
  try {
    if (format != "json" && format != "yaml")
      throw std::invalid_argument("attachment format must be json or yaml");
    if (!max_depth || max_depth > 256 || text.size() > 1048576)
      throw std::invalid_argument("invalid attachment size/depth limit");
    YamlParser parser;
    yaml_parser_set_input_string(&parser.parser,
      reinterpret_cast<const unsigned char*>(text.data()), text.size());
    YamlDocument document;
    if (!yaml_parser_load(&parser.parser, &document.document))
      throw std::invalid_argument("YAML parse error at line " +
        std::to_string(parser.parser.problem_mark.line + 1) + ": " +
        (parser.parser.problem ? parser.parser.problem : "invalid document"));
    document.loaded = true;
    std::size_t count = 0;
    std::set<const yaml_node_t*> active;
    auto root = yaml_document_get_root_node(&document.document);
    if (!root) throw std::invalid_argument("empty attachment");
    auto value = yamlValue(document.document, root, max_depth, count, active);
    YamlDocument trailing;
    if (!yaml_parser_load(&parser.parser, &trailing.document))
      throw std::invalid_argument("invalid trailing YAML document");
    trailing.loaded = true;
    if (yaml_document_get_root_node(&trailing.document))
      throw std::invalid_argument("multiple YAML documents are unsupported");
    if (format == "yaml") return value;
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)>
      tokener(json_tokener_new_ex(static_cast<int>(max_depth)), json_tokener_free);
    if (!tokener) throw std::bad_alloc();
    json_tokener_set_flags(tokener.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    std::string input(text); input += '\n';
    Json json(json_tokener_parse_ex(tokener.get(), input.data(), static_cast<int>(input.size())), json_object_put);
    if (json_tokener_get_error(tokener.get()) != json_tokener_success ||
        json_tokener_get_parse_end(tokener.get()) != input.size())
      throw std::invalid_argument("invalid JSON attachment");
    return fromJson(json.get(), max_depth);
  } catch (const std::invalid_argument& error) {
    throw std::invalid_argument(source + ": " + error.what());
  }
}
std::string bankValueJson(const BankValue& value) {
  Json object(toJson(value, 256), json_object_put);
  return json_object_to_json_string_ext(object.get(), JSON_C_TO_STRING_PLAIN);
}
} // namespace novostone::action
