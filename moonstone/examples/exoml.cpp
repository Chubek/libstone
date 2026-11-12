#include <exoml/ExoML.hpp>
#include <iostream>
int main(){moonstone::exoml::Interpreter interp;interp.register_service("Text:Echo",[](const auto& args)->moonstone::exoml::Value{return args.at(0);});auto mod=moonstone::exoml::Parser("Answer ~setconst! Text:Echo(\"hello\");").parse();auto errors=interp.execute(mod);for(auto& e:errors)std::cerr<<moonstone::format(e)<<'\n';if(errors.empty())std::cout<<moonstone::exoml::stringify(interp.symbols().at("Answer"))<<'\n';return errors.empty()?0:1;}
