#include <syren/Syren.hpp>
#include <syren/RuntimeDiag.hpp>
#include <cassert>
#include <sstream>
using namespace moonstone;
int main(){syren::Catalog c;c.add(syren::parse_rule(R"((error (id "M1") (name "moved") (message "bad {value}") (pattern (use ?value)) (repairs (repair "clone"))))"));auto ds=c.diagnose(sexpr::Parser("(use x)").one(),{"test.ex",2,3,2,5});assert(ds.size()==1&&ds[0].message=="bad x");std::ostringstream s;syren::StderrSink sink(s);sink.emit(ds[0]);assert(s.str().find("M1")!=std::string::npos);auto sarif=syren::to_sarif(ds);assert(sarif.find("2.1.0")!=std::string::npos);syren::RuntimeRecorder r(1);r.record({"ffi","broken"});r.record({"gc","invalid"});assert(r.snapshot().size()==1&&r.snapshot()[0].category=="gc");}
