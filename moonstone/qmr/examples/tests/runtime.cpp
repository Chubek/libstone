#include "qmr/Qmr.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

using namespace moonstone::qmr;
int main() {
    Engine render;
    Context ctx{{"name", "<Ada & Bob>"}, {"nums", Value::Array{1, 2, 3}},
                {"user", Value::Object{{"name", "Grace"}, {"admin", true}}}};
    assert(render.render("Hello {{ data.name }}!", ctx) == "Hello &lt;Ada &amp; Bob&gt;!");
    assert(render.render("{{{ data.name }}}", ctx) == "<Ada & Bob>");
    assert(render.render("{% for _,n in ipairs(data.nums) do %}{{n}}{% end %}", ctx) == "123");
    assert(render.render("{% if data.user.admin then %}{{ data.user.name }}{% else %}No{% end %}", ctx) == "Grace");
    assert(render.render("{{ qmr.default(nil, 'fallback') }}") == "fallback");
    assert(render.render("{{ qmr.escape_url('a b') }}") == "a%20b");
    bool failed = false;
    try { (void)render.render("{{ require('os') }}"); }
    catch (const std::exception &) { failed = true; }
    assert(failed);
    failed = false;
    try { (void)render.render("{{ data.nums }}", ctx); }
    catch (const std::exception &) { failed = true; }
    assert(failed);
    failed = false;
    try { (void)render.render("{% while true do end %}"); }
    catch (const std::exception &e) {
        failed = std::string(e.what()).find("QMR_INSTRUCTION_LIMIT") != std::string::npos;
    }
    assert(failed);
    Options small;
    small.limits.max_output_bytes = 2;
    failed = false;
    try { (void)Engine(small).render("abcdef"); }
    catch (const std::exception &e) {
        failed = std::string(e.what()).find("QMR_OUTPUT_LIMIT") != std::string::npos;
    }
    assert(failed);
    Options incl;
    incl.resolver = [](std::string_view key, std::string_view) -> std::optional<Source> {
        if (key == "part") return Source{"part", "Hello {{ data.user.name }}"};
        if (key == "cycle") return Source{"cycle", "{% include 'cycle' %}"};
        return std::nullopt;
    };
    Engine with_includes(incl);
    assert(with_includes.render("{% include 'part' %}!", ctx) == "Hello Grace!");
    incl.resolver = [](std::string_view key, std::string_view) -> std::optional<Source> {
        if (key == "item") return Source{"item", "{{ vars.name }}"};
        return std::nullopt;
    };
    assert(Engine(incl).render("{% for _,x in ipairs(data.nums) do %}"
                              "{% include 'item', {name=x} %}{% end %}", ctx) == "123");
    failed = false;
    try { (void)with_includes.render("{% include 'cycle' %}"); }
    catch (const std::exception &e) {
        failed = std::string(e.what()).find("QMR_INCLUDE_CYCLE") != std::string::npos;
    }
    assert(failed);

    auto tmp = std::filesystem::temp_directory_path() / "moonstone-qmr-tests";
    std::filesystem::create_directories(tmp);
    auto root = tmp / "root";
    std::filesystem::create_directories(root);
    { std::ofstream f(root / "main.qmr"); f << "{% include 'part.qmr' %}"; }
    { std::ofstream f(root / "part.qmr"); f << "{{ data.user.name }}"; }
    assert(render.render_file(root / "main.qmr", ctx, root) == "Grace");
    failed = false;
    { std::ofstream f(root / "bad.qmr"); f << "{% include '../outside.qmr' %}"; }
    try { (void)render.render_file(root / "bad.qmr", ctx, root); }
    catch (const std::exception &) { failed = true; }
    assert(failed);
    std::filesystem::remove_all(tmp);
}
