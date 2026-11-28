#include "mahak/Cli.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "mahak/Adapter.hpp"
#include "mahak/Engine.hpp"
#include "mahak/Manifest.hpp"
#include "mahak/Measurement.hpp"
#include "mahak/Report.hpp"
#include "mahak/Storage.hpp"
#include "mahak/Version.hpp"

namespace moonstone::mahak {
inline namespace v1 {
namespace {

namespace fs = std::filesystem;

struct Options {
    std::string verb;
    std::vector<std::string> arguments;
    std::string profile;
    std::string store;
    std::string format = "terminal";
    std::string out;
    std::string session;
    std::string work_root;
    std::string metric = "wall";
    std::string seed;
    bool keep_work = false;
    bool fail_on_regression = false;
    bool help = false;
    bool version = false;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::set<std::string> benchmarks;
    std::set<std::string> targets;
    std::set<std::string> groups;
    std::set<std::string> tags;
};

void print_usage(std::ostream &out) {
    out << "mahak " << mahak_version_string << " - Moonstone benchmarking suite\n\n";
    out << "usage: mahak <verb> [options] [arguments]\n\n";
    out << "verbs:\n";
    out << "  validate <manifest>      parse and validate a manifest (exit 2 if invalid)\n";
    out << "  plan <manifest>          expand a manifest into benchmark cases\n";
    out << "  run <manifest>           execute a manifest and commit a session\n";
    out << "  compare <base> <cand>    compare two committed sessions\n";
    out << "  report --session <id>    render a committed session\n";
    out << "  history [--metric NAME]  list committed sessions\n";
    out << "  recommend                recommendations are not enabled in this build\n";
    out << "  doctor                    probe the host, compiler and perf availability\n\n";
    out << "options:\n";
    out << "  --profile NAME           store sessions under the named profile\n";
    out << "  --store DIR              override the history store directory\n";
    out << "  --format FMT             terminal, md, html, csv, json\n";
    out << "  --out FILE               write rendered output to FILE\n";
    out << "  --session ID             select the session for report\n";
    out << "  --metric NAME            metric for history (default wall)\n";
    out << "  --work-root DIR          directory for case work directories\n";
    out << "  --keep-work              keep case work directories\n";
    out << "  --benchmark ID           filter benchmarks (repeatable, comma separated)\n";
    out << "  --target ID              filter targets (repeatable, comma separated)\n";
    out << "  --group NAME             filter benchmark groups\n";
    out << "  --tag NAME               filter benchmark tags (any match)\n";
    out << "  --param NAME=VALUE       pin a matrix parameter\n";
    out << "  --seed N                 override the planning seed\n";
    out << "  --fail-on-regression     exit 4 when compare confirms a regression\n";
    out << "  --help, --version\n\n";
    out << "exit codes: 0 success, 1 internal, 2 invalid configuration, 3 execution failure,\n";
    out << "            4 confirmed regression, 5 missing item\n";
}

void split_into(std::string_view text, std::set<std::string> &target) {
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t comma = text.find(',', begin);
        const std::size_t stop = comma == std::string_view::npos ? text.size() : comma;
        const std::string entry(text.substr(begin, stop - begin));
        if (!entry.empty())
            target.insert(entry);
        if (comma == std::string_view::npos)
            break;
        begin = comma + 1;
    }
}

std::string data_root() {
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0')
        return std::string(xdg) + "/mahak";
    if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0')
        return std::string(home) + "/.local/share/mahak";
    return "/tmp/mahak";
}

std::string store_root(const Options &options) {
    if (!options.store.empty())
        return options.store;
    const std::string root = data_root();
    if (!options.profile.empty())
        return root + "/profiles/" + options.profile;
    return root + "/default";
}

int fail_message(const std::string &message) {
    std::cerr << message << "\n";
    return 2;
}

int fail(ErrorCategory category, const std::string &message) {
    std::cerr << "mahak: error: " << message << "\n";
    switch (category) {
    case ErrorCategory::InvalidInput:
        return 2;
    case ErrorCategory::NotFound:
        return 5;
    case ErrorCategory::Unsupported:
        return 5;
    case ErrorCategory::BudgetExhausted:
    case ErrorCategory::Internal:
        return 1;
    }
    return 1;
}

int emit(const std::string &text, const Options &options) {
    if (options.out.empty()) {
        std::cout << text;
        if (!text.empty() && text.back() != '\n')
            std::cout << '\n';
        return 0;
    }
    std::ofstream stream(options.out, std::ios::binary);
    if (!stream) {
        std::cerr << "mahak: error: cannot write '" << options.out << "'\n";
        return 1;
    }
    stream << text;
    return 0;
}

std::string render_session(const SessionRecord &session, const Options &options) {
    if (options.format == "md" || options.format == "markdown")
        return render_markdown(session);
    if (options.format == "html")
        return render_html(session);
    if (options.format == "csv")
        return render_csv(session);
    if (options.format == "json") {
        auto text = encode_session_text(session, true);
        return text ? text.value() : std::string();
    }
    return render_terminal(session);
}

Result<SessionRecord> load_session(const Options &options, std::string_view id) {
    auto store = HistoryStore::open(store_root(options));
    if (!store)
        return store.error();
    return store.value().load(id);
}

int verb_validate(const Options &options) {
    if (options.arguments.empty())
        return fail(ErrorCategory::InvalidInput, "validate requires a manifest path");
    auto manifest = load_manifest(options.arguments.front());
    if (!manifest)
        return fail(manifest.error().category, manifest.error().message);
    const Diagnostics diagnostics = validate_manifest(manifest.value());
    bool invalid = false;
    for (const auto &diagnostic : diagnostics) {
        std::cerr << format_diagnostics({diagnostic}) << "\n";
        invalid = invalid || diagnostic.severity == Severity::Error;
    }
    if (invalid)
        return 2;
    std::cout << "ok: " << options.arguments.front() << " (" << manifest.value().targets.size()
              << " targets, " << manifest.value().benchmarks.size() << " benchmarks)\n";
    return 0;
}

int verb_plan(const Options &options) {
    if (options.arguments.empty())
        return fail(ErrorCategory::InvalidInput, "plan requires a manifest path");
    auto manifest = load_manifest(options.arguments.front());
    if (!manifest)
        return fail(manifest.error().category, manifest.error().message);
    const Diagnostics diagnostics = validate_manifest(manifest.value());
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.severity == Severity::Error)
            return fail_message(format_diagnostics({diagnostic}));
    }
    PlanOptions plan_options;
    plan_options.benchmarks = options.benchmarks;
    plan_options.targets = options.targets;
    plan_options.groups = options.groups;
    plan_options.tags = options.tags;
    plan_options.parameter_overrides = options.parameters;
    plan_options.work_root = options.work_root;
    if (!options.seed.empty())
        plan_options.seed = std::strtoull(options.seed.c_str(), nullptr, 10);
    auto plan = plan_manifest(manifest.value(), plan_options);
    if (!plan)
        return fail(plan.error().category, plan.error().message);
    if (options.format == "json") {
        Json document = Json::object();
        document.set("manifest_id", Json(plan.value().manifest_id));
        Json cases = Json::array();
        for (const auto &case_plan : plan.value().cases) {
            Json entry = Json::object();
            entry.set("case", Json(case_plan.id));
            entry.set("benchmark", Json(case_plan.benchmark_id));
            entry.set("target", Json(case_plan.target_id));
            Json stages = Json::array();
            for (const auto &stage : case_plan.stages)
                stages.push(Json(name(stage.kind)));
            entry.set("stages", std::move(stages));
            cases.push(std::move(entry));
        }
        document.set("cases", std::move(cases));
        return emit(document.dump(true), options);
    }
    std::string text;
    for (const auto &case_plan : plan.value().cases) {
        text += case_plan.id;
        text += "  [";
        bool first = true;
        for (const auto &stage : case_plan.stages) {
            if (!first)
                text += ",";
            first = false;
            text += name(stage.kind);
        }
        text += "]\n";
    }
    text += std::to_string(plan.value().cases.size()) + " case(s)\n";
    return emit(text, options);
}

int verb_run(const Options &options) {
    if (options.arguments.empty())
        return fail(ErrorCategory::InvalidInput, "run requires a manifest path");
    EngineOptions engine_options;
    engine_options.work_root = options.work_root;
    engine_options.keep_work_directories = options.keep_work;
    Engine engine(std::move(engine_options));
    PlanOptions plan_options;
    plan_options.benchmarks = options.benchmarks;
    plan_options.targets = options.targets;
    plan_options.groups = options.groups;
    plan_options.tags = options.tags;
    plan_options.parameter_overrides = options.parameters;
    plan_options.work_root = options.work_root;
    if (!options.seed.empty())
        plan_options.seed = std::strtoull(options.seed.c_str(), nullptr, 10);
    auto session = engine.run_file(options.arguments.front(), plan_options);
    if (!session)
        return fail(session.error().category, session.error().message);
    auto store = HistoryStore::open(store_root(options));
    if (!store)
        return fail(store.error().category, store.error().message);
    if (auto committed = store.value().commit(session.value()); !committed) {
        std::cerr << "mahak: warning: session was not committed: " << committed.error().message
                  << "\n";
    }
    const SessionRecord &record = session.value();
    if (options.format == "json")
        return emit(render_session(record, options), options);
    std::cout << "session " << record.id << "\n\n";
    const int code = emit(render_session(record, options), options);
    return code != 0 ? code : (record.status == "completed" ? 0 : 3);
}

int verb_report(const Options &options) {
    if (options.session.empty())
        return fail(ErrorCategory::InvalidInput, "report requires --session <id>");
    auto session = load_session(options, options.session);
    if (!session)
        return fail(session.error().category, session.error().message);
    return emit(render_session(session.value(), options), options);
}

int verb_history(const Options &options) {
    auto store = HistoryStore::open(store_root(options));
    if (!store)
        return fail(store.error().category, store.error().message);
    auto summaries = store.value().list();
    if (!summaries)
        return fail(summaries.error().category, summaries.error().message);
    if (options.format == "json") {
        Json sessions = Json::array();
        for (const auto &summary : summaries.value()) {
            Json entry = Json::object();
            entry.set("id", Json(summary.id));
            entry.set("manifest_id", Json(summary.manifest_id));
            entry.set("status", Json(summary.status));
            entry.set("created_unix_ns", Json(static_cast<std::int64_t>(summary.created_unix_ns)));
            entry.set("run_count", Json(static_cast<std::int64_t>(summary.run_count)));
            sessions.push(std::move(entry));
        }
        return emit(sessions.dump(true), options);
    }
    std::string text;
    for (const auto &summary : summaries.value()) {
        text += summary.id + "  " + summary.manifest_id + "  " + summary.status + "  runs=" +
                std::to_string(summary.run_count) + "\n";
    }
    if (text.empty())
        text = "no sessions\n";
    return emit(text, options);
}

int verb_compare(const Options &options) {
    if (options.arguments.size() < 2)
        return fail(ErrorCategory::InvalidInput, "compare requires <baseline> <candidate>");
    auto baseline = load_session(options, options.arguments[0]);
    if (!baseline)
        return fail(baseline.error().category, baseline.error().message);
    auto candidate = load_session(options, options.arguments[1]);
    if (!candidate)
        return fail(candidate.error().category, candidate.error().message);
    ComparisonOptions comparison_options;
    auto bundle = compare_sessions(baseline.value(), candidate.value(), comparison_options);
    if (!bundle)
        return fail(bundle.error().category, bundle.error().message);
    bool regression = false;
    for (const auto &comparison : bundle.value().comparisons)
        regression = regression || comparison.result.regression;
    const std::string text = options.format == "md" || options.format == "markdown"
                                 ? render_comparison_markdown(bundle.value())
                                 : render_comparison_terminal(bundle.value());
    const int code = emit(text, options);
    if (code != 0)
        return code;
    return regression && options.fail_on_regression ? 4 : 0;
}

int verb_recommend(const Options &) {
    std::cerr << "mahak: recommendations are unavailable in this build; no advice was "
                 "produced\n";
    return 5;
}

int verb_doctor(const Options &options) {
    const EnvironmentInfo environment = probe_environment();
    std::string text;
    text += "os: " + environment.os + "\n";
    text += "kernel: " + environment.kernel + "\n";
    text += "architecture: " + environment.architecture + "\n";
    text += "cpu: " + environment.cpu_model + "\n";
    text += "cpus: logical=" + std::to_string(environment.logical_cpus) +
            " physical=" + std::to_string(environment.physical_cpus) + "\n";
    text += "affinity: " + environment.affinity + "\n";
    const PerfAvailability perf = probe_perf();
    text += std::string("perf: ") + (perf.available ? "available" : "unavailable") +
            (perf.reason.empty() ? "" : " (" + perf.reason + ")") + "\n";
    const ToolProbe compiler = probe_tool("cc");
    text += std::string("cc: ") + (compiler.available ? compiler.path : "unavailable") +
            (compiler.version.empty() ? "" : " (" + compiler.version + ")") + "\n";
    return emit(text, options);
}

Options parse_arguments(int argc, char **argv, bool &ok) {
    Options options;
    ok = true;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&](std::string &slot) {
            if (index + 1 >= argc) {
                std::cerr << "mahak: error: " << argument << " requires a value\n";
                ok = false;
                return;
            }
            slot = argv[++index];
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--version") {
            options.version = true;
        } else if (argument == "--profile")
            value(options.profile);
        else if (argument == "--store")
            value(options.store);
        else if (argument == "--format")
            value(options.format);
        else if (argument == "--out")
            value(options.out);
        else if (argument == "--session")
            value(options.session);
        else if (argument == "--work-root")
            value(options.work_root);
        else if (argument == "--metric")
            value(options.metric);
        else if (argument == "--seed")
            value(options.seed);
        else if (argument == "--keep-work")
            options.keep_work = true;
        else if (argument == "--fail-on-regression")
            options.fail_on_regression = true;
        else if (argument == "--benchmark" || argument == "--benchmarks") {
            std::string list;
            value(list);
            split_into(list, options.benchmarks);
        } else if (argument == "--target" || argument == "--targets") {
            std::string list;
            value(list);
            split_into(list, options.targets);
        } else if (argument == "--group") {
            std::string list;
            value(list);
            split_into(list, options.groups);
        } else if (argument == "--tag") {
            std::string list;
            value(list);
            split_into(list, options.tags);
        } else if (argument == "--param") {
            std::string assignment;
            value(assignment);
            const std::size_t equals = assignment.find('=');
            if (equals == std::string::npos) {
                std::cerr << "mahak: error: --param expects NAME=VALUE\n";
                ok = false;
            } else {
                options.parameters.emplace_back(assignment.substr(0, equals),
                                                assignment.substr(equals + 1));
            }
        } else if (!argument.empty() && argument.front() == '-') {
            std::cerr << "mahak: error: unknown option " << argument << "\n";
            ok = false;
        } else if (options.verb.empty()) {
            options.verb = argument;
        } else {
            options.arguments.push_back(argument);
        }
    }
    return options;
}

} // namespace

int cli_main(int argc, char **argv) {
    bool ok = true;
    Options options = parse_arguments(argc, argv, ok);
    if (!ok)
        return 2;
    if (options.version) {
        std::cout << "mahak " << mahak_version_string << " (manifest schema "
                  << mahak_manifest_schema_version << ", result schema "
                  << mahak_result_schema_version << ")\n";
        return 0;
    }
    if (options.help || options.verb.empty()) {
        print_usage(std::cout);
        return options.help ? 0 : 2;
    }
    try {
        if (options.verb == "validate")
            return verb_validate(options);
        if (options.verb == "plan")
            return verb_plan(options);
        if (options.verb == "run")
            return verb_run(options);
        if (options.verb == "report")
            return verb_report(options);
        if (options.verb == "history")
            return verb_history(options);
        if (options.verb == "compare")
            return verb_compare(options);
        if (options.verb == "recommend")
            return verb_recommend(options);
        if (options.verb == "doctor")
            return verb_doctor(options);
        std::cerr << "mahak: error: unknown verb '" << options.verb << "'\n";
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "mahak: internal error: " << error.what() << "\n";
        return 1;
    }
}

} // namespace v1
} // namespace moonstone::mahak
