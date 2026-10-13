#include <QaMRpp.hpp>

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

std::string read_file(const char *path) {
  std::ifstream input(path, std::ios::in | std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

qamrpp::ValuePtr string_key(const char *key) {
  return std::make_shared<qamrpp::Value>(std::string(key));
}

std::string field(qamrpp::Context &ctx, const qamrpp::ValuePtr &table, const char *key) {
  qamrpp::ValuePtr value = ctx.table_raw_get(table, string_key(key));
  return value && value->type == qamrpp::Value::STRING ? value->string_value : std::string();
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: qamrpp_module <libqamrpp_xbind> <spec>\n";
    return 2;
  }

  qamrpp::Context ctx;
  if (!ctx.load_library(argv[1])) {
    std::cerr << "failed to load QaMRpp XBind module: " << argv[1] << "\n";
    return 1;
  }

  qamrpp::ValuePtr manifest = ctx.run(read_file(argv[2]));
  if (!manifest || manifest->type != qamrpp::Value::TABLE) {
    std::cerr << "spec did not return a table\n";
    return 1;
  }
  if (field(ctx, manifest, "kind") != "library" || field(ctx, manifest, "name") != "hello") {
    std::cerr << "unexpected manifest from QaMRpp module\n";
    return 1;
  }
  return 0;
}
