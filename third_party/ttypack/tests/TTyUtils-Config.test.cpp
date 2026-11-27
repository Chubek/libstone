#include <TTyUtils/TTyUtils-Config.hpp>
#include <cassert>
#include <limits>
#include <unistd.h>

int main() {
    namespace config = ttyutils::config;
    char directory[] = "/tmp/ttyutils-config-test.XXXXXX";
    assert(::mkdtemp(directory));
    const auto base = std::filesystem::path(directory);
    const auto system = base / "system";
    const auto user = base / "user";
    const auto project = base / "project";
    std::filesystem::create_directory(system);
    std::filesystem::create_directory(user);
    std::filesystem::create_directory(project);
    assert(::setenv("XDG_CONFIG_DIRS", system.c_str(), 1) == 0);
    assert(::setenv("XDG_CONFIG_HOME", user.c_str(), 1) == 0);
    std::ofstream(system / "sample.toml") << "[editor]\n tabstop=8\n zoom = 1.25\n";
    std::ofstream(user / "sample.toml") << "[editor]\ntabstop = 2\nname = \"test # value\\n\"\n";
    std::ofstream(project / "sample.toml") << "[editor]\n tabstop = 4 # override\nbroken = nope\n";
    auto store = config::Store::load("sample", project);
    assert(store.get<int>("editor.tabstop") == 4);
    assert(store.get<double>("editor.zoom") == 1.25);
    assert(store.get<std::string>("editor.name") == "test # value\n");
    assert(store.diagnostics().size() == 1 && store.diagnostics().front().line == 3);
    assert(store.get<int>("editor.broken", 13) == 13);
    store.set("wide", std::numeric_limits<long long>::max());
    assert(store.get<int>("wide", 9) == 9);
    const config::Schema schema{{"editor.tabstop"}, {{"editor.tabstop", config::Schema::Type::Integer}}};
    assert(schema.valid(store.values()));

    int updates = 0;
    config::watch(store, [&] { ++updates; });
    assert(updates == 0);
    std::filesystem::remove(project / "sample.toml");
    assert(store.reload() && updates == 1);
    assert(store.get<int>("editor.tabstop") == 2);
    assert(!store.reload() && updates == 1);
    std::filesystem::remove_all(base);
}
