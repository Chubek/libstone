#ifndef XBIND_QAMRPP_HPP
#define XBIND_QAMRPP_HPP

#include <string>
#include <vector>

namespace xbind::qamrpp {

struct Param {
  std::string name;
  std::string c_type;
};

struct Function {
  std::string name;
  std::string returns;
  std::vector<Param> params;
};

struct Manifest {
  std::string path;
  std::string name;
  std::string version;
  std::vector<std::string> headers;
  std::vector<std::string> stdlib_modules;
  std::vector<std::string> targets;
  std::vector<Function> functions;
};

Manifest load_spec_file(const std::string &path);
std::string dump_manifest(const Manifest &manifest);

} // namespace xbind::qamrpp

#endif /* XBIND_QAMRPP_HPP */
