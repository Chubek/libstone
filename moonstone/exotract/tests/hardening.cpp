#include <exotract/Exotract.hpp>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <unistd.h>
using namespace moonstone::exotract;

template<class F> void rejects(F fn) {
    bool rejected=false;
    try { fn(); } catch(const std::exception &) { rejected=true; }
    assert(rejected);
}
int main(int argc,char **argv) {
    if(argc>1) {
        const std::string mode=argv[1];
        if(mode=="echo") { std::cout<<argv[2];return 0; }
        if(mode=="stdin") { return std::cin.get()==EOF?0:1; }
        if(mode=="noise") { for(int i=0;i<10000;++i){std::cout<<std::string(256,'o');std::cerr<<std::string(256,'e');}return 0; }
        if(mode=="sleep") { std::this_thread::sleep_for(std::chrono::seconds(5));return 0; }
        if(mode=="descendant") {
            if(fork()==0) { std::this_thread::sleep_for(std::chrono::milliseconds(600));write_file_atomic(argv[2],"escaped");_exit(0); }
            return 0;
        }
        if(mode=="store") { Store s(argv[2]);s.put(argv[3],argv[4]);assert(s.get(argv[3])==argv[4]);return 0; }
        if(mode=="read") { Store s(argv[2]);auto v=s.get(argv[3]);if(!v)return 2;std::cout<<*v;return 0; }
        if(mode=="duplicate") { Store s(argv[2]);rejects([&]{Store duplicate(argv[2]);});return 0; }
        if(mode=="write") { write_file_atomic(argv[2],argv[3]);return 0; }
        return 4;
    }
    Config c;c.name="Fixture";c.path=EXOTRACT_FIXTURE;
    auto temporary=fs::temp_directory_path()/("exotract-hardening-"+std::to_string(getpid()));
    fs::create_directories(temporary);
    const auto exe=fs::absolute(argv[0]).string();
    auto call=[&](std::vector<std::string> args){args.insert(args.begin(),exe);return run_command(Command{args},c);};
    assert(call({"echo","' $(touch should-not-exist) ; &"}).out=="' $(touch should-not-exist) ; &");
    assert(call({"stdin"}).ok());
    c.max_output_bytes=1024;
    auto noisy=call({"noise"});assert(noisy.overflow && !noisy.ok() && noisy.out.size()+noisy.err.size()<=1024);
    c.max_output_bytes=1024*1024;c.command_timeout_ms=100;
    assert(call({"sleep"}).timed_out);
    auto marker=temporary/"descendant";
    assert(call({"descendant",marker.string()}).timed_out);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    assert(!fs::exists(marker));
    rejects([&]{run_command(Command{{exe}},c,{{"","x"}});});
    c.command_timeout_ms=15000;
    auto store=std::make_shared<Store>();auto globals=std::make_shared<GlobalRegistry>(store);
    globals->set("BAD","MULIB_IMPL_MICROJS");
    auto cfg=c;cfg.defines={"$GLOBAL{BAD}"};rejects([&]{Engine e(cfg,store,globals);});
    cfg=c;cfg.header_only.implementation_defines={"X","X"};rejects([&]{Engine e(cfg);});
    cfg=c;cfg.undefines={"BAD\n#define INJECT"};rejects([&]{Engine e(cfg);});
    cfg=c;cfg.compiler_arguments={"-fplugin=anything"};rejects([&]{Engine e(cfg);});
    cfg=c;cfg.path.clear();rejects([&]{Engine e(cfg);});
    const auto output=temporary/"output";
    std::vector<std::thread> writers;
    for(int i=0;i<12;++i)writers.emplace_back([&,i]{write_file_atomic(output,std::string(100000,char('a'+i)));});
    for(auto &t:writers)t.join();
    auto bytes=read_file(output);assert(bytes.size()==100000 && bytes==std::string(100000,bytes[0]));
    rejects([&]{read_file(output,32);});
    for(const auto &entry:fs::directory_iterator(temporary))assert(entry.path().filename().string().find(".tmp-")==std::string::npos);
    fs::remove_all(temporary);
    std::cout<<"process, configuration and atomic publication checks passed\n";
}
