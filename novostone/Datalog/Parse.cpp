#include "novostone/Datalog/Datalog.hpp"
#include "Syntax.h"

#include <algorithm>
#include <charconv>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>

namespace novostone::datalog {
namespace {
std::pair<std::size_t, std::size_t> location(std::string_view text, std::size_t offset) {
  std::size_t line = 1, column = 1;
  for (std::size_t i = 0; i < std::min(offset, text.size()); ++i) {
    if (text[i] == '\n') { ++line; column = 1; } else ++column;
  }
  return {line, column};
}
std::string diagnostic(const std::string& source, std::string_view text, std::size_t offset, const std::string& message) {
  auto [line, column] = location(text, offset);
  return source + ':' + std::to_string(line) + ':' + std::to_string(column) + ": " + message;
}
std::string spelling(novo_dl_string_t value) { return {value.data, value.length}; }
int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
std::string unquote(novo_dl_string_t value) {
  auto s = spelling(value);
  if (s.empty() || s.front() != '"') return s;
  std::string result;
  for (std::size_t i = 1; i + 1 < s.size(); ++i) {
    auto c = s[i];
    if (c != '\\') { result += c; continue; }
    switch (s[++i]) {
      case '\\': result += '\\'; break;
      case '"': result += '"'; break;
      case 'n': result += '\n'; break;
      case 'r': result += '\r'; break;
      case 't': result += '\t'; break;
      case '0': result += '\0'; break;
      case 'x':
        if (i + 2 >= s.size() - 1 || hex(s[i + 1]) < 0 || hex(s[i + 2]) < 0)
          throw std::invalid_argument("expected two hex digits after \\x");
        result += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])); i += 2; break;
      default: throw std::invalid_argument("unknown string escape");
    }
  }
  return result;
}
std::string quote(const std::string& text) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
    else if (c < 32 || c >= 127) { result += "\\x"; result += digits[c >> 4]; result += digits[c & 15]; }
    else result += static_cast<char>(c);
  }
  return result + '"';
}
}
ParseError::ParseError(std::string source_name, std::string_view text, std::size_t at, const std::string& message)
    : std::invalid_argument(diagnostic(source_name, text, at, message)), source(std::move(source_name)),
      offset(std::min(at, text.size())), line(location(text, at).first), column(location(text, at).second) {}

Module parse(std::string_view text, std::string source, ParseLimits limits) {
  if (text.size() > limits.bytes) throw ParseError(source, text, 0, "Datalog source byte limit exceeded");
  auto nul = text.find('\0');
  if (nul != std::string_view::npos) throw ParseError(source, text, nul, "embedded NUL; use a string escape");
  std::string input(text);
  std::unique_ptr<novo_dl_syntax, decltype(&novo_dl_syntax_free)> syntax(nullptr, novo_dl_syntax_free);
  {
    static std::mutex parser_mutex;
    std::lock_guard lock(parser_mutex);
    std::size_t offset = 0;
    syntax.reset(novo_dl_parse_syntax(input.c_str(), &offset));
    if (!syntax) throw ParseError(source, text, offset, "invalid Datalog syntax or parser allocation failure");
  }
  auto statements = novo_dl_syntax_document(syntax.get())->statements;
  if (statements.length > limits.statements)
    throw ParseError(source, text, 0, "Datalog statement limit exceeded");
  Module result;
  std::size_t offset = 0;
  try {
    // Declarations can follow uses. Elaboration never infers a type or EDB/IDB
    // role from facts or rules, and publication happens only after validation.
    for (std::size_t i = 0; i < statements.length; ++i) {
      const auto& s = *statements.items[i]; offset = s.offset;
      if (s.kind != novo_dl_Decl_kind) continue;
      const auto& declaration = s.v.Decl;
      if (declaration.columns.length > 256) throw std::invalid_argument("relation exceeds 256 columns");
      std::vector<Type> columns;
      std::set<std::string> names;
      for (std::size_t j = 0; j < declaration.columns.length; ++j) {
        const auto& column = *declaration.columns.items[j];
        auto name = unquote(column.name), type = spelling(column.type);
        if (!name.empty() && !names.insert(name).second) throw std::invalid_argument("duplicate column name: " + name);
        if (type == "integer") columns.push_back(Type::Integer);
        else if (type == "symbol") columns.push_back(Type::Symbol);
        else throw std::invalid_argument("unknown column type: " + type);
      }
      result.program.declare(unquote(declaration.name), std::move(columns), declaration.derived);
    }
    auto atom = [&](novo_dl_atom_t a) {
      offset = a->offset;
      Atom value{unquote(a->name), {}, a->negated};
      if (a->terms.length > 256) throw std::invalid_argument("atom exceeds 256 terms");
      for (std::size_t j = 0; j < a->terms.length; ++j) {
        const auto& t = *a->terms.items[j]; offset = t.offset;
        switch (t.kind) {
          case novo_dl_Variable_kind: value.terms.push_back(Term::var(unquote(t.v.Variable.spelling))); break;
          case novo_dl_Symbol_kind: value.terms.push_back(Term::constant(unquote(t.v.Symbol.spelling))); break;
          case novo_dl_Wildcard_kind: value.terms.push_back(Term::any()); break;
          case novo_dl_Integer_kind: {
            auto s = spelling(t.v.Integer.spelling); std::int64_t integer;
            auto parsed = std::from_chars(s.data(), s.data() + s.size(), integer);
            if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size())
              throw std::invalid_argument("integer outside signed 64-bit range");
            value.terms.push_back(Term::constant(integer)); break;
          }
        }
      }
      return value;
    };
    for (std::size_t i = 0; i < statements.length; ++i) {
      const auto& s = *statements.items[i];
      if (s.kind == novo_dl_Fact_kind) {
        auto a = atom(s.v.Fact.value); offset = s.offset;
        Tuple row;
        for (const auto& t : a.terms) {
          if (t.kind != Term::Kind::Constant) throw std::invalid_argument("facts require constant terms");
          row.push_back(t.value);
        }
        result.program.validate_fact(a.relation, row);
        result.facts[a.relation].insert(std::move(row));
      } else if (s.kind == novo_dl_Rule_kind) {
        Rule rule{atom(s.v.Rule.head), {}};
        if (s.v.Rule.body.length > 256) throw std::invalid_argument("rule exceeds 256 body atoms");
        for (std::size_t j = 0; j < s.v.Rule.body.length; ++j) rule.body.push_back(atom(s.v.Rule.body.items[j]));
        offset = s.offset;
        result.program.validate_rule(rule);
        result.program.add(std::move(rule));
      }
    }
    offset = 0; // Stratification is a whole-program property.
    result.program.validate();
  } catch (const std::invalid_argument& error) { throw ParseError(source, text, offset, error.what()); }
  return result;
}

std::string dump(const Module& module) {
  module.program.validate();
  std::ostringstream out; out.imbue(std::locale::classic());
  for (const auto& [name, schema] : module.program.schemas()) {
    out << ".decl " << quote(name) << '(';
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
      if (i) out << ", ";
      out << (schema.columns[i] == Type::Integer ? "integer" : "symbol");
    }
    out << ')' << (schema.derived ? " derived" : "") << ".\n";
  }
  auto value = [&](const Value& v) {
    if (auto integer = std::get_if<std::int64_t>(&v)) out << *integer;
    else out << quote(std::get<std::string>(v));
  };
  auto atom = [&](const Atom& a) {
    if (a.negated) out << '!';
    out << quote(a.relation) << '(';
    for (std::size_t i = 0; i < a.terms.size(); ++i) {
      if (i) out << ", ";
      const auto& t = a.terms[i];
      if (t.kind == Term::Kind::Wildcard) out << '_';
      else if (t.kind == Term::Kind::Variable) out << '?' << quote(std::get<std::string>(t.value));
      else value(t.value);
    }
    out << ')';
  };
  for (const auto& rule : module.program.rules()) {
    atom(rule.head); out << " :- ";
    for (std::size_t i = 0; i < rule.body.size(); ++i) { if (i) out << ", "; atom(rule.body[i]); }
    out << ".\n";
  }
  for (const auto& [name, rows] : module.facts) {
    if (!module.program.schemas().count(name) || module.program.schemas().at(name).derived)
      throw std::invalid_argument("facts require a declared base relation: " + name);
    for (const auto& row : rows) {
      module.program.validate_fact(name, row);
      out << quote(name) << '(';
      for (std::size_t i = 0; i < row.size(); ++i) { if (i) out << ", "; value(row[i]); }
      out << ").\n";
    }
  }
  return out.str();
}
} // namespace novostone::datalog
