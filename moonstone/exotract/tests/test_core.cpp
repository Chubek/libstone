#include <exotract/Exotract.hpp>
#include <cassert>
#include <iostream>
#include <algorithm>
using namespace moonstone::exotract;
int main() {
    Config config;
    config.name="Fixture";
    config.path=EXOTRACT_FIXTURE;
    config.clang_resource_dir=EXOTRACT_TEST_RESOURCE_DIR;
    config.compiler=EXOTRACT_TEST_COMPILER;
    config.header_only.implementation_defines={"EXOTRACT_IMPL_FIXTURE"};
    config.header_only.declaration_guard="EXOTRACT_FIXTURE_H";
    config.header_only.implementation_guard="EXOTRACT_FIXTURE_IMPLEMENTED";
    Engine engine(config);
    auto r=engine.preprocess();
    assert(r.preprocessed_ok && !r.preprocessed.empty());
    r=engine.parse(std::move(r));
    assert(r.syntax_checked);
    const auto imported=engine.import_cdecl(std::move(r),
        read_file(fs::path(EXOTRACT_FIXTURE).parent_path()/"transport.json"));
    assert(imported.ok());
    assert(imported.symbols.size()==2);
    auto invalid=engine.import_cdecl(imported,"{}");
    assert(!invalid.ok() && !invalid.declarations_complete && invalid.symbols.empty() && invalid.cdecl_json.empty());
    auto again=engine.parse(invalid);
    assert(again.syntax_checked);
    assert(std::none_of(again.diagnostics.begin(),again.diagnostics.end(),[](const Diagnostic &d){return d.code=="EXOTRACT-CDECL-INVALID";}));
    auto manifest=engine.manifest_json(imported);
    assert(manifest.find("et_create")!=std::string::npos);
    assert(manifest.find("\"safe_export\":false")!=std::string::npos);
    assert(manifest.find("\"unknown_effects\":true")!=std::string::npos);
    assert(engine.on1x_manifest_json(imported).find("\"unsafe_by_default\":true")!=std::string::npos);
    auto source=engine.implementation_source();
    assert(source.find("#define EXOTRACT_IMPL_FIXTURE")!=std::string::npos);
    auto store=std::make_shared<Store>();
    auto globals=std::make_shared<GlobalRegistry>(store);
    globals->set("MULIB_KERNEL_DIR","/tmp/a b");
    assert(globals->expand("$GLOBAL{MULIB_KERNEL_DIR}/h.h")=="/tmp/a b/h.h");
    bool caught=false;
    try{(void)globals->expand("$GLOBAL{UNDEFINED}");}catch(const std::exception &){caught=true;}
    assert(caught);
    auto fail=run_command(Command{{"this-executable-does-not-exist-for-exotract-tests"}},config);
    assert(!fail.ok());
    Config broken=config;broken.header_only.implementation_translation_units=2;
    caught=false;try{Engine e(broken);}catch(const std::exception &){caught=true;}
    assert(caught);
    std::cout<<"exotract core tests: passed\n";
}
