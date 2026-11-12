#include <exoml/ExoML.hpp>
#include <cassert>
using namespace moonstone::exoml;
int main(){
 Interpreter vm;vm.register_service("Math:Add",[](const std::vector<Value>& a)->Value{return std::get<std::int64_t>(a.at(0))+std::get<std::int64_t>(a.at(1));});
 auto mod=Parser("X ~setconst! Math:Add(2,3); Y ~get! Math:Add($X, 4);").parse();assert(vm.execute(mod).empty());assert(std::get<std::int64_t>(vm.symbols().at("Y"))==9);
 assert(!vm.execute(Parser("X ~setvar! Math:Add(1,2);").parse()).empty());
 vm.register_service("Native:Call",[](const std::vector<Value>&)->Value{return std::int64_t(7);},true);
 assert(!vm.execute(Parser("Z ~hatch! Native:Call();").parse()).empty());vm.enable_hatch(true);vm.allow_service("Native:Call");assert(vm.execute(Parser("Z ~hatch! Native:Call();").parse()).empty());
}
