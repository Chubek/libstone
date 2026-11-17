#include "qmr/Qmr.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace moonstone::qmr;

namespace {
bool throws_qmr(const Engine &engine, std::string_view source,
                const Context &data, std::string_view needle) {
    try {
        (void)engine.render(source, data);
    } catch (const std::exception &e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}
} // namespace

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
    std::filesystem::remove_all(tmp);
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

    // --- Output codecs -------------------------------------------------------
    Options xml_options;
    xml_options.codec = Codec::Xml;
    Context markup{{"x", "a<b'c>&"}};
    assert(Engine(xml_options).render("{{ data.x }}", markup) == "a&lt;b&apos;c&gt;&amp;");
    assert(Engine(xml_options).render("{{{ data.x }}}", markup) == "a<b'c>&");
    Options json_options;
    json_options.codec = Codec::Json;
    Context scalars{{"a", 1}, {"s", std::string("x\"y\\z")}, {"b", true}};
    assert(Engine(json_options).render(
               "[{{ data.a }},{{ data.s }},{{ data.b }},{{ data.missing }}]", scalars) ==
           "[1,\"x\\\"y\\\\z\",true,null]");
    Options plain_options;
    plain_options.codec = Codec::Plain;
    assert(Engine(plain_options).render("{{ data.x }}", markup) == "a<b'c>&");

    // --- Trusted SafeHtml ----------------------------------------------------
    Context trusted{{"html", SafeHtml{"<b>bold</b>"}}, {"plain", std::string("<i>&</i>")}};
    assert(render.render("{{ data.html }}", trusted) == "<b>bold</b>");
    assert(render.render("{{{ data.html }}}", trusted) == "<b>bold</b>");
    // Plain strings stay escaped in HTML context; only explicit trust is raw.
    assert(render.render("{{ data.plain }}", trusted) == "&lt;i&gt;&amp;&lt;/i&gt;");
    // SafeHtml is HTML trust, not XML/JSON trust: still escaped there.
    assert(Engine(xml_options).render("{{ data.html }}", trusted) ==
           "&lt;b&gt;bold&lt;/b&gt;");
    assert(Engine(json_options).render("{{ data.html }}", trusted) == "\"<b>bold</b>\"");
    // Lua-side wrapping marks a value trusted after explicit validation.
    assert(render.render("{{ qmr.safe_html('<i>hi</i>') }}") == "<i>hi</i>");
    failed = false;
    try { (void)render.render("{{ qmr.safe_html({}) }}"); }
    catch (const std::exception &) { failed = true; }
    assert(failed);

    // --- Bounded Lua allocator -------------------------------------------------
    Options tight_memory;
    tight_memory.limits.max_memory_bytes = 256 * 1024;
    assert(throws_qmr(Engine(tight_memory), "{% local t = string.rep('x', 8*1024*1024) %}", {},
                      "memory"));
    Options no_memory;
    no_memory.limits.max_memory_bytes = 0;
    failed = false;
    try { (void)Engine(no_memory).render("x"); }
    catch (const std::exception &e) {
        failed = std::string(e.what()).find("QMR_LUA") != std::string::npos;
    }
    assert(failed);
    // Healthy quota: an ordinary render must not be starved by the allocator.
    Options healthy;
    healthy.limits.max_memory_bytes = 8 * 1024 * 1024;
    assert(Engine(healthy).render("{% local t = {} for i=1,100 do t[i]=i end %}"
                                  "{{ #t }}") == "100");

    // --- Wall-clock deadline ---------------------------------------------------
    Options deadline_options;
    deadline_options.limits.max_duration = std::chrono::milliseconds(20);
    deadline_options.limits.max_instructions = 500000000;
    assert(throws_qmr(Engine(deadline_options), "{% while true do end %}", {}, "QMR_DEADLINE"));
    Options no_deadline;
    no_deadline.limits.max_duration = std::chrono::milliseconds(0);
    assert(Engine(no_deadline).render("{{ 1 + 1 }}") == "2");

    // --- Source-mapped diagnostics ----------------------------------------------
    failed = false;
    try { (void)render.render("line1\nline2\n{{ data.x.y.z }}", ctx); }
    catch (const Error &e) {
        failed = e.code() == "QMR_EVALUATE" && e.file() == "<template>" &&
                 e.line() == 3 && e.column() == 1;
    }
    assert(failed);
    failed = false;
    try { (void)render.render("{% if broken then %}"); }
    catch (const std::exception &e) {
        failed = std::string(e.what()).find("QMR_COMPILE_LUA") != std::string::npos;
    }
    assert(failed);
    // Diagnostics from included templates keep their original file position.
    Options include_error;
    include_error.resolver = [](std::string_view key, std::string_view) -> std::optional<Source> {
        if (key == "broken_part")
            return Source{"broken_part", "ok\n{{ data.missing.field }}"};
        return std::nullopt;
    };
    failed = false;
    try { (void)Engine(include_error).render("head\n{% include 'broken_part' %}"); }
    catch (const Error &e) {
        failed = e.code() == "QMR_EVALUATE" && e.file() == "broken_part" &&
                 e.line() == 2 && e.column() == 1;
    }
    assert(failed);

    // --- Compiled-template cache -------------------------------------------------
    auto cache = std::make_shared<ProgramCache>(64 * 1024 * 1024);
    Options cached_options;
    cached_options.cache = cache;
    std::atomic<int> version{1};
    cached_options.resolver = [&](std::string_view key,
                                  std::string_view) -> std::optional<Source> {
        if (key == "part") return Source{"part", "v" + std::to_string(version.load())};
        return std::nullopt;
    };
    Engine cached(cached_options);
    assert(cached.render("A{% include 'part' %}B") == "Av1B");
    assert(cached.render("A{% include 'part' %}B") == "Av1B");
    assert(cache->entries() == 2);
    version = 2; // changed source text = new identity, old entry invalidated by use
    assert(cached.render("A{% include 'part' %}B") == "Av2B");
    assert(cache->entries() == 3);
    cache->invalidate(Source{"part", "v2"});
    assert(cache->entries() == 2);
    assert(cached.render("A{% include 'part' %}B") == "Av2B");
    cache->clear();
    assert(cache->entries() == 0 && cache->bytes() == 0);
    // Byte budget forces eviction but rendering keeps working.
    auto small_cache = std::make_shared<ProgramCache>(512);
    Options eviction_options;
    eviction_options.cache = small_cache;
    eviction_options.resolver = [&](std::string_view key,
                                   std::string_view) -> std::optional<Source> {
        if (key == "part") return Source{"part", std::string(512, 'x')};
        return std::nullopt;
    };
    Engine evicting(eviction_options);
    assert(evicting.render("{% include 'part' %}") == std::string(512, 'x'));
    assert(small_cache->entries() <= 2);
    assert(small_cache->bytes() <= 512);

    // --- Symlink escapes and root violations ------------------------------------
    {
        auto outside = tmp / "outside.qmr";
        { std::ofstream f(outside); f << "escaped"; }
        std::filesystem::create_directories(root / "subdir");
        std::error_code ec;
        std::filesystem::create_directory_symlink(tmp, root / "subdir" / "link", ec);
        bool link_rejected = false;
        if (!ec) {
            try {
                (void)render.render_file(root / "main.qmr", ctx, root);
            } catch (const std::exception &) { }
            // The escape attempt is an *include* from inside the root:
            { std::ofstream f(root / "via_link.qmr"); f << "{% include 'subdir/link/outside.qmr' %}"; }
            try { (void)render.render_file(root / "via_link.qmr", ctx, root); }
            catch (const std::exception &) { link_rejected = true; }
            assert(link_rejected);
        }
        // A main template outside the root is rejected outright.
        bool root_rejected = false;
        try { (void)render.render_file(outside, ctx, root); }
        catch (const std::exception &e) {
            root_rejected = std::string(e.what()).find("QMR_ROOT") != std::string::npos;
        }
        assert(root_rejected);
    }

    // --- Concurrent rendering with independent VMs -------------------------------
    {
        const std::string expected = render.render(
            "{% for _,n in ipairs(data.nums) do %}{{ data.name }}{{ n }}{% end %}", ctx);
        auto shared_cache = std::make_shared<ProgramCache>();
        Options thread_options;
        thread_options.cache = shared_cache;
        thread_options.resolver = [](std::string_view key,
                                    std::string_view) -> std::optional<Source> {
            if (key == "item") return Source{"item", "[{{ vars.n }}]"};
            return std::nullopt;
        };
        const Engine shared(thread_options);
        std::atomic<int> mismatches{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&]() {
                for (int i = 0; i < 25; ++i) {
                    if (shared.render("{% for _,n in ipairs(data.nums) do %}"
                                      "{% include 'item', {n=n} %}{% end %}", ctx) !=
                        "[1][2][3]")
                        ++mismatches;
                    if (shared.render("{% for _,n in ipairs(data.nums) do %}"
                                      "{{ data.name }}{{ n }}{% end %}", ctx) != expected)
                        ++mismatches;
                }
            });
        }
        for (auto &thread : threads) thread.join();
        assert(mismatches == 0);
        assert(shared_cache->entries() >= 1);
    }

    std::filesystem::remove_all(tmp);
    return 0;
}
