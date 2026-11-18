#include "c99.hpp"
#include <array>
#include <fstream>
#include <iostream>

namespace {
void help(std::ostream& out) {
  out<<"usage: traceml-cli [--emit-c] [--module NAME] [-o OUTPUT] [FILE|-]\n"
     <<"Emit a C99 translation unit to link with libtraceml.\n";
}
std::string read(std::istream& stream) {
  constexpr size_t limit=1024*1024;
  std::array<char,4096> buffer;
  std::string text;
  while(stream.read(buffer.data(),buffer.size())||stream.gcount()) {
    auto count=static_cast<size_t>(stream.gcount());
    if(count>limit-text.size())throw std::runtime_error("TraceML input exceeds 1 MiB");
    text.append(buffer.data(),count);
  }
  if(stream.bad()||!stream.eof())throw std::runtime_error("cannot read input");
  return text;
}
}

int main(int argc,char** argv) {
  try {
    std::string input,output,module="main";
    bool positional=false;
    for(int i=1;i<argc;++i) {
      std::string_view arg=argv[i];
      if(!positional&&(arg=="--help"||arg=="-h")){help(std::cout);return 0;}
      if(!positional&&arg=="--"){positional=true;continue;}
      if(!positional&&arg=="--emit-c")continue;
      if(!positional&&(arg=="--module"||arg=="-o"||arg=="--output")) {
        if(i+1==argc||!argv[i+1][0])throw std::runtime_error("missing argument for "+std::string(arg));
        (arg=="--module"?module:output)=argv[++i];
      }else if(!positional&&arg!="-"&&arg.starts_with('-'))throw std::runtime_error("unknown option: "+std::string(arg));
      else {
        if(!input.empty()||arg.empty())throw std::runtime_error("expected at most one input file");
        input=arg;
      }
    }
    std::string text;
    if(input.empty()||input=="-")text=read(std::cin);
    else {
      std::ifstream stream(input,std::ios::binary);
      if(!stream)throw std::runtime_error("cannot open "+input);
      text=read(stream);
    }
    auto program=limestone::traceml::compile(text);
    if(!program)throw std::runtime_error(program.error().message);
    auto c=limestone::traceml::emit_c99(program.value(),module);
    if(!c)throw std::runtime_error(c.error().message);
    if(output.empty()||output=="-") {
      if(!(std::cout<<c.value()<<std::flush))throw std::runtime_error("cannot write stdout");
    }else {
      std::ofstream file(output,std::ios::binary);
      file<<c.value();
      file.close();
      if(!file)throw std::runtime_error("cannot write "+output);
    }
    return 0;
  }catch(const std::exception& error) {
    std::cerr<<"traceml: "<<error.what()<<'\n';
    return 1;
  }
}
