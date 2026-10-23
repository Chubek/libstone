#pragma once
#include <string>
#include <vector>
#include <variant>
#include <functional>
#include <filesystem>
namespace yellowstone::infobank {
struct ISANode { enum Kind {Document,Arch,Profile,Compiler,Legacy,Regclass,Encoding,Op,Register,Field,Value,Call}; Kind kind; std::string name, text; std::vector<ISANode> children; int line=0,column=0; };
using ISAVisitor=std::function<void(const ISANode&)>;
ISANode readISA(const std::filesystem::path&);
void visitISA(const ISANode&, const ISAVisitor&);
class ISAEventStream { ISANode root_; public: explicit ISAEventStream(const std::filesystem::path& p):root_(readISA(p)){}; void each(const ISAVisitor& v) const { visitISA(root_,v); } };
}
