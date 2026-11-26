#include <qBFD.hpp>
#include <xbind_codegen.hpp>
#include <xbind_discovery.hpp>

#include <string>

extern "C" int calls(void);

int main(int argc, char** argv) {
  if (argc != 1 || calls()) return 1;
  auto self = qbfd::open(std::filesystem::path(argv[0]));
  if (!self) return 2;
  if (xbind::discovery::available() &&
      xbind::discovery::inventory(argv[0]).find("\"image\":") == std::string::npos)
    return 3;
  xbind::qamrpp::Manifest m;
  m.name = "installed-consumer";
  return xbind::codegen::dump_ir(m).find(m.name) == std::string::npos ? 4 : 0;
}
