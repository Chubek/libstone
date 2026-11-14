#include <exotract/Exotract.hpp>
#include <cassert>
#if defined(TEST_EXTRACT)
#include <exoml/Extract.hpp>
#endif

int main() {
    namespace et = moonstone::exotract;
    et::Config config;
    config.name = "Installed";
    config.path = TEST_HEADER;
    config.clang_resource_dir = TEST_RESOURCES;
    config.compiler = TEST_COMPILER;
    config.preprocessor.main = "compiler";
    et::Engine engine(config);
    const auto result = engine.inspect();
    assert(result.syntax_checked);
#if defined(TEST_EXTRACT)
    namespace cdecl = moonstone::exoml::cdecl;
    cdecl::ExtractOptions options;
    options.header = TEST_HEADER;
    options.resource_directory = TEST_RESOURCES;
    const auto unit = cdecl::extract_c11(options);
    assert(unit.complete && result.ok());
    assert(cdecl::describe_translation_unit(unit) == result.cdecl_json);
    assert(engine.on1x_manifest_json(result).find("installed_api") != std::string::npos);
#else
    assert(!result.declarations_complete);
#endif
}
