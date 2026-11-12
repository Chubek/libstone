#include "novostone/CostCC/CostCC.hpp"
#include "novostone/SpeculativeIR/SpeculativeIR.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <random>
#include <thread>
#include <locale>

using namespace novostone;
namespace {
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F function, const std::string& message) {
  bool rejected = false;
  try { function(); } catch (const std::exception&) { rejected = true; }
  check(rejected, message);
}
bool fired(const query::Candidate& candidate, const std::string& name) {
  return std::any_of(candidate.rewrites().begin(), candidate.rewrites().end(), [&](const auto& r) { return r.rule == name; });
}
ccdb::Snapshot sum_loop(ccdb::Database& db) {
  ir::Builder b(db, 1);
  auto entry = b.block(), header = b.block(), body = b.block(), exit = b.block();
  auto n = b.emit(entry, ir::Op::Param);
  auto zero = b.emit(entry, ir::Op::Const), one = b.emit(entry, ir::Op::Const, {}, 1);
  b.emit(entry, ir::Op::Jump, {}, 0, {header});
  auto counter = b.emit(header, ir::Op::Phi), sum = b.emit(header, ir::Op::Phi);
  auto condition = b.emit(header, ir::Op::Ult, {counter, n});
  b.emit(header, ir::Op::Branch, {condition}, 0, {body, exit});
  auto accumulated = b.emit(body, ir::Op::Add, {sum, counter});
  auto next = b.emit(body, ir::Op::Add, {counter, one});
  b.emit(body, ir::Op::Jump, {}, 0, {header});
  b.emit(exit, ir::Op::Return, {sum});
  b.incoming(counter, entry, zero); b.incoming(counter, body, next);
  b.incoming(sum, entry, zero); b.incoming(sum, body, accumulated);
  return b.finish();
}
void ssa() {
  ccdb::Database db(ir::program()); auto source = sum_loop(db); auto f = ir::load(source);
  auto candidate = query::optimize(source);
  for (std::uint32_t n = 0; n < 200; ++n) {
    auto result = ir::evaluate(f, {n});
    check(result.value == n * (n - 1) / 2, "loop-carried sum differs from independent arithmetic reference");
    check(ir::equivalent(result, ir::evaluate(ir::load(candidate.result()), {n})), "optimized phi loop");
  }
  check(source.query("Edge").rows.size() == 4, "phi predecessor metadata is not a control-flow edge");
  check(source.query("Uses").rows.size() >= 10, "phi operands contribute to use-def queries");
  auto text = ir::dump(f);
  check(text.starts_with("novostone 2 ") && ir::dump(ir::parse(text)) == text, "v2 phi text roundtrip");
  auto bad = f; auto& phi = bad.blocks.at(2).instructions[0];
  phi.targets.pop_back(); phi.operands.pop_back();
  rejects([&] { ir::verify(bad); }, "missing incoming predecessor");
  bad = f; bad.blocks.at(2).instructions[0].targets[1] = 1;
  rejects([&] { ir::verify(bad); }, "duplicate incoming predecessor");
  bad = f; bad.blocks.at(2).instructions[0].operands[0] = bad.blocks.at(3).instructions[1].id;
  rejects([&] { ir::verify(bad); }, "backedge definition cannot supply initial phi edge");
  bad = f; std::swap(bad.blocks.at(2).instructions[0], bad.blocks.at(2).instructions[2]);
  rejects([&] { ir::verify(bad); }, "phi after ordinary instruction");
  bad = f; bad.blocks.at(1).instructions[0].op = ir::Op::Phi;
  rejects([&] { ir::verify(bad); }, "entry cannot contain phi");
  auto old_dialect = text; old_dialect.replace(10, 1, "1");
  rejects([&] { ir::parse(old_dialect); }, "v1 parser cannot silently accept v2 phi semantics");

  ccdb::Database swapping(ir::program()); ir::Builder b(swapping, 1);
  auto entry = b.block(), header = b.block(), body = b.block(), exit = b.block();
  auto n = b.emit(entry, ir::Op::Param), zero = b.emit(entry, ir::Op::Const), one = b.emit(entry, ir::Op::Const, {}, 1);
  auto ten = b.emit(entry, ir::Op::Const, {}, 10), twenty = b.emit(entry, ir::Op::Const, {}, 20), hundred = b.emit(entry, ir::Op::Const, {}, 100);
  b.emit(entry, ir::Op::Jump, {}, 0, {header});
  auto count = b.emit(header, ir::Op::Phi), a = b.emit(header, ir::Op::Phi), c = b.emit(header, ir::Op::Phi);
  auto condition = b.emit(header, ir::Op::Ult, {count, n}); b.emit(header, ir::Op::Branch, {condition}, 0, {body, exit});
  auto next = b.emit(body, ir::Op::Add, {count, one}); b.emit(body, ir::Op::Jump, {}, 0, {header});
  auto scaled = b.emit(exit, ir::Op::Mul, {a, hundred}); auto result = b.emit(exit, ir::Op::Add, {scaled, c}); b.emit(exit, ir::Op::Return, {result});
  b.incoming(count, entry, zero); b.incoming(count, body, next);
  b.incoming(a, entry, ten); b.incoming(a, body, c);
  b.incoming(c, entry, twenty); b.incoming(c, body, a);
  auto swap = ir::load(b.finish());
  for (std::uint32_t iterations = 0; iterations < 20; ++iterations)
    check(ir::evaluate(swap, {iterations}).value == (iterations % 2 ? 2010u : 1020u), "phi cycles must assign in parallel");
  ir::State initial; initial.block = swap.entry;
  auto suspended = ir::resume(swap, {7}, initial, 10000, [&](const auto& i, const auto&) { return i.id != a; });
  check(suspended.suspended && !suspended.state.phi_values.empty(), "suspended phi retains parallel edge inputs");
  check(ir::resume(swap, {7}, suspended.state).value == 2010, "resume in the middle of phi assignment");
  auto assumptions = std::make_shared<speculative::Assumptions>(); auto assumption = assumptions->assume("loop trip count");
  auto param = f.blocks.at(f.entry).instructions.front().id;
  auto version = speculative::Version::specialize(source, assumptions, {{param, 12, assumption}});
  check(version.execute({12}).outcome.value == 66, "speculative phi loop");
  check(version.execute({13}).outcome.value == 78 && version.statistics().deoptimizations == 1, "deopt into loop baseline");
}
void cfg() {
  ccdb::Database db(ir::program()); ir::Builder b(db, 1);
  auto entry = b.block(), yes = b.block(), no = b.block(), join = b.block();
  auto argument = b.emit(entry, ir::Op::Param); auto condition = b.emit(entry, ir::Op::Const, {}, 17);
  b.emit(entry, ir::Op::Branch, {condition}, 0, {yes, no});
  b.emit(yes, ir::Op::Jump, {}, 0, {join});
  auto address = b.emit(no, ir::Op::Const); auto wrong = b.emit(no, ir::Op::Const, {}, 999);
  b.emit(no, ir::Op::Store, {address, wrong}); b.emit(no, ir::Op::Jump, {}, 0, {join});
  auto merged = b.emit(join, ir::Op::Phi, {argument, wrong}, 0, {yes, no}); b.emit(join, ir::Op::Return, {merged});
  auto source = b.finish(); auto candidate = query::optimize(source);
  check(fired(candidate, "fold_branch") && fired(candidate, "phi_uniform"), "branch fold repairs phis");
  check(ir::load(candidate.result()).blocks.size() == 3, "unreachable effectful block removed");
  for (auto argument_value : {0u, 1u, UINT32_MAX}) {
    auto before = ir::evaluate(ir::load(source), {argument_value});
    check(before.value == argument_value && ir::equivalent(before, ir::evaluate(ir::load(candidate.result()), {argument_value})), "removed path was unreachable including traps");
  }
  auto checked = query::apply_checked(db, candidate, query::corpus(1, 42));
  check(checked.revision() == source.revision() + 1, "validated transactional CFG publication");
  rejects([&] { query::apply_checked(db, candidate, query::corpus(1, 42)); }, "validation does not waive stale revision checks");
  ccdb::Database same_db(ir::program()); ir::Builder same(same_db, 1);
  auto head = same.block(), tail = same.block(); auto value = same.emit(head, ir::Op::Param);
  same.emit(head, ir::Op::Branch, {value}, 0, {tail, tail}); same.emit(tail, ir::Op::Return, {value});
  auto same_source = same.finish(); auto same_candidate = query::optimize(same_source);
  check(fired(same_candidate, "fold_branch"), "identical branch arms fold without constant evidence");
  ccdb::Database cse_db(ir::program()); ir::Builder cse(cse_db, 2); auto block = cse.block();
  auto x = cse.emit(block, ir::Op::Param), y = cse.emit(block, ir::Op::Param, {}, 1);
  auto first = cse.emit(block, ir::Op::Add, {x, y}), second = cse.emit(block, ir::Op::Add, {y, x});
  auto product = cse.emit(block, ir::Op::Mul, {first, second}); cse.emit(block, ir::Op::Return, {product});
  auto cse_source = cse.finish(); auto cse_candidate = query::optimize(cse_source);
  check(fired(cse_candidate, "local_cse"), "commutative local value numbering");
  check(query::validate(ir::load(cse_source), ir::load(cse_candidate.result()), query::corpus(2, 567)).verdict == query::Verdict::Passed, "CSE bounded differential validation");
  query::Options no_queries; no_queries.query_work = 0;
  auto declined = query::optimize(cse_source, no_queries);
  check(declined.budget_exhausted() && declined.rewrites().empty() && declined.query_work() == 0, "query budget declines optimization safely");
  check(ir::dump(ir::load(declined.result())) == ir::dump(ir::load(cse_source)), "zero-work fallback keeps semantic baseline");
  query::Options tiny; tiny.query_facts = 1;
  auto bounded = query::optimize(cse_source, tiny);
  check(bounded.budget_exhausted() && bounded.stop_reason().find("fact budget") != std::string::npos, "fact budget returns verified prefix instead of failing compilation");
  check(!cse_candidate.budget_exhausted() && cse_candidate.query_work() > 0, "query work recorded for completed optimization");
}
void memory() {
  std::mt19937 random(8675309);
  for (int trial = 0; trial < 50; ++trial) {
    ccdb::Database db(ir::program()); ir::Builder b(db, 2); auto block = b.block();
    auto a = b.emit(block, ir::Op::Param), c = b.emit(block, ir::Op::Param, {}, 1);
    auto value = b.emit(block, ir::Op::Const, {}, static_cast<std::uint32_t>(random()));
    std::vector<ir::Id> values{value};
    for (int step = 0; step < 12; ++step) {
      auto address = random() % 2 ? a : c;
      if (random() % 2) b.emit(block, ir::Op::Store, {address, values[random() % values.size()]});
      else values.push_back(b.emit(block, ir::Op::Load, {address}));
    }
    // Guaranteed forwarding opportunity, after an access that must succeed.
    b.emit(block, ir::Op::Store, {a, value}); auto read = b.emit(block, ir::Op::Load, {a}); b.emit(block, ir::Op::Return, {read});
    auto source = b.finish(); auto candidate = query::optimize(source);
    check(fired(candidate, "forward_load"), "same-address store/load opportunity");
    auto f = ir::load(source), optimized = ir::load(candidate.result());
    auto dependencies = ir::memory_dependencies(f);
    check(dependencies.size() >= 14 && dependencies.front().previous == 0, "explicit ordered memory dependency chain");
    for (std::size_t i = 1; i < dependencies.size(); ++i)
      check(dependencies[i].previous == dependencies[i - 1].instruction, "memory chain predecessor");
    check(!source.query("Effect").rows.empty(), "relational effect summaries");
    for (std::uint32_t a_value = 0; a_value < 5; ++a_value) for (std::uint32_t c_value = 0; c_value < 5; ++c_value)
      for (std::size_t cells = 0; cells < 5; ++cells) {
        std::vector<std::uint32_t> heap(cells, 97);
        check(ir::equivalent(ir::evaluate(f, {a_value, c_value}, heap), ir::evaluate(optimized, {a_value, c_value}, heap)),
              "alias/trap/effect differential trial " + std::to_string(trial));
      }
  }
  ccdb::Database db(ir::program()); ir::Builder b(db); auto block = b.block();
  auto zero = b.emit(block, ir::Op::Const), one = b.emit(block, ir::Op::Const, {}, 1);
  auto forty = b.emit(block, ir::Op::Const, {}, 40), fifty = b.emit(block, ir::Op::Const, {}, 50);
  b.emit(block, ir::Op::Store, {zero, forty}); b.emit(block, ir::Op::Store, {one, fifty});
  auto load = b.emit(block, ir::Op::Load, {zero}); b.emit(block, ir::Op::Return, {load});
  auto source = b.finish(); auto candidate = query::optimize(source);
  check(fired(candidate, "forward_load"), "known-disjoint store preserves forwarding evidence");
  check(ir::evaluate(ir::load(candidate.result()), {}, {0, 0}).value == 40, "disjoint forwarding result");
  check(ir::equivalent(ir::evaluate(ir::load(source), {}, {0}), ir::evaluate(ir::load(candidate.result()), {}, {0})), "intervening store trap stays ordered");
}
void validation() {
  ccdb::Database db(ir::program()); ir::Builder b(db, 1); auto block = b.block(); auto value = b.emit(block, ir::Op::Param);
  b.emit(block, ir::Op::Return, {value}); auto source = b.finish(); auto f = ir::load(source);
  auto mutant = f; mutant.blocks.begin()->second.instructions.front().op = ir::Op::Const;
  auto report = query::validate(f, mutant, {{{0}, {}}, {{1}, {}}});
  check(report.verdict == query::Verdict::Mismatch && report.matched == 1 && report.counterexample->input.arguments == std::vector<std::uint32_t>{1}, "find and retain first counterexample");
  check(report.explain().find("arguments 1") != std::string::npos, "reproduction input in diagnostic");
  ir::Function loop{0, 1, {{1, {1, {{2, ir::Op::Jump, 0, {}, {1}}}}}}};
  auto unknown = query::validate(loop, loop, {{{}, {}}}, 10);
  check(unknown.verdict == query::Verdict::Inconclusive && unknown.inconclusive == 1 && unknown.matched == 0, "equal resource exhaustion is not proof");
  auto candidate = query::baseline(source);
  rejects([&] { query::apply_checked(db, candidate, {{{1}, {}}}, 0); }, "inconclusive candidate cannot publish");
  check(db.snapshot().same_revision(source), "validation failure retains previous artifact");
  rejects([&] { query::validate(f, f, {}); }, "empty domain cannot vacuously pass");
  rejects([&] { query::validate(f, f, {{{}, {}}}); }, "wrong-arity corpus cannot validate argument-count traps");
  auto cases = query::corpus(2, 123, 15), repeat = query::corpus(2, 123, 15);
  check(cases.size() == repeat.size(), "seeded corpus size");
  for (std::size_t i = 0; i < cases.size(); ++i)
    check(cases[i].arguments == repeat[i].arguments && cases[i].memory == repeat[i].memory, "corpus reproducibility");
  rejects([&] { query::corpus(UINT32_MAX, 1); }, "unbounded corpus rejected");
  // A mutant that preserves return values but drops a store must also fail.
  ccdb::Database effects_db(ir::program()); ir::Builder effects(effects_db); auto head = effects.block();
  auto address = effects.emit(head, ir::Op::Const), data = effects.emit(head, ir::Op::Const, {}, 3);
  effects.emit(head, ir::Op::Store, {address, data}); effects.emit(head, ir::Op::Return, {data});
  auto original = ir::load(effects.finish()), missing = original;
  missing.blocks.begin()->second.instructions.erase(missing.blocks.begin()->second.instructions.begin() + 2);
  auto effect_report = query::validate(original, missing, {{{}, {0}}});
  check(effect_report.verdict == query::Verdict::Mismatch, "write trace and heap are part of validation observations");
}
void persistence() {
  ccdb::Database db(ir::program(), [](const auto& snapshot) { ir::verify(snapshot); }); auto source = sum_loop(db);
  auto unused = db.begin(); unused.reserve(1000); unused.abort();
  auto checkpoint = db.checkpoint();
  ccdb::Database restored(ir::program(), [](const auto& snapshot) { ir::verify(snapshot); });
  auto loaded = restored.restore(restored.snapshot(), checkpoint);
  check(loaded.facts() == source.facts() && loaded.revision() == 1, "primary relations restored as fresh local revision");
  check(restored.allocate() == 1001, "checkpoint retains abandoned allocation high-water mark");
  check(ir::evaluate(ir::load(loaded), {20}).value == 190, "restored phi loop executes");
  check(!loaded.query("Reach").cached && loaded.query("Reach").cached, "derived caches recomputed after restore");
  rejects([&] { restored.restore(loaded, checkpoint + "junk"); }, "trailing checkpoint data rejected");
  rejects([&] { restored.restore(loaded, checkpoint.substr(0, checkpoint.size() - 5)); }, "truncated checkpoint rejected");
  rejects([&] { restored.restore(loaded, checkpoint, {4, 100}); }, "checkpoint byte budget");
  rejects([&] { restored.restore(loaded, checkpoint, {checkpoint.size(), 1}); }, "checkpoint fact budget");
  check(restored.snapshot().same_revision(loaded), "bad checkpoint never changes published state");
  struct Grouped : std::numpunct<char> {
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
  };
  {
    struct LocaleGuard { std::locale previous; ~LocaleGuard() { std::locale::global(previous); } } guard{std::locale()};
    std::locale::global(std::locale(std::locale::classic(), new Grouped));
    check(db.checkpoint() == checkpoint, "checkpoint and rule signatures are locale-independent");
    auto text = ir::dump(ir::load(source));
    check(ir::dump(ir::parse(text)) == text, "IR text is locale-independent");
  }
  auto changed_rules = ir::program(); changed_rules.add({{"Dead", {datalog::Term::constant(std::int64_t{123})}}, {}});
  ccdb::Database incompatible(changed_rules);
  rejects([&] { incompatible.restore(incompatible.snapshot(), checkpoint); }, "identical schema with different analysis rules rejected");
  rejects([&] { restored.restore(source, checkpoint); }, "foreign restore base rejected");
  datalog::Program strings; strings.declare("Text", {datalog::Type::Symbol});
  ccdb::Database text_db(strings); auto tx = text_db.begin();
  std::string tricky = "quote\" slash\\ newline\n embedded\0zero"; tricky.push_back('\0');
  tx.insert("Text", {tricky}); auto text_snapshot = tx.commit();
  ccdb::Database text_copy(strings); auto copied = text_copy.restore(text_copy.snapshot(), text_db.checkpoint());
  check(copied.facts() == text_snapshot.facts(), "escaped and binary symbol checkpoint roundtrip");
  // Publication validators can reject structurally malformed restored functions.
  auto corrupt = checkpoint; auto location = corrupt.find("fact \"Entry\" i 1");
  check(location != std::string::npos, "entry checkpoint fixture"); corrupt.replace(location, 16, "fact \"Entry\" i 0");
  rejects([&] { restored.restore(loaded, corrupt); }, "restore validator rejects invalid CFG");
}
void hardening() {
  using namespace datalog;
  Program p; p.declare("Base", {Type::Integer}); p.declare("Result", {Type::Integer}, true);
  p.add({{"Result", {Term::var("x")}}, {{"Base", {Term::var("x")}}}});
  auto signature = p.signature();
  check(!signature.empty() && p.signature() == signature, "stable complete program identity");
  auto malformed = p; malformed.add({{"Result", {{Term::Kind::Variable, std::int64_t{5}}}}, {}});
  rejects([&] { malformed.validate(); }, "public malformed term cannot bypass validation");
  malformed = p; malformed.add({{"Result", {{static_cast<Term::Kind>(99), std::int64_t{0}}}}, {}});
  rejects([&] { malformed.validate(); }, "invalid term discriminant rejected");
  rejects([&] { Program bad; bad.declare("Bad", {static_cast<Type>(99)}); }, "invalid schema discriminant rejected");
  auto deep = p; std::vector<Atom> body(300, Atom{"Base", {Term::var("x")}});
  deep.add({{"Result", {Term::var("x")}}, body});
  rejects([&] { deep.evaluate({{"Base", {{1LL}}}}, "Result"); }, "join depth is bounded");
  bool typed_budget = false;
  try { p.evaluate({{"Base", {{1LL}}}}, "Result", {100, 0}); } catch (const BudgetExceeded&) { typed_budget = true; }
  check(typed_budget, "resource failures have a dedicated exception type");
  rejects([&] { ir::parse("novostone 1 -18446744073709551615 1 block 1 instr 2 param 0 0 0 instr 3 return 0 1 2 0"); }, "negative unsigned arity cannot wrap to one");
  rejects([&] { ir::parse("novostone 1 0 1 block 1 instr 2 const -18446744073709551615 0 0 instr 3 return 0 1 2 0"); }, "negative unsigned immediate cannot wrap to one");
  ccdb::Database db(p); auto transaction = db.begin(); transaction.insert("Base", {1LL});
  auto moved = std::move(transaction);
  rejects([&] { transaction.preview(); }, "moved-from transaction is closed");
  auto before = moved.commit();
  auto change = db.begin(); change.insert("Base", {2LL});
  std::atomic<bool> correct = true;
  std::thread reader;
  auto after = change.commit("validator reads candidate concurrently", [&](const auto& candidate) {
    reader = std::thread([&, candidate] {
      for (int i = 0; i < 50; ++i) if (candidate.query("Result").rows.size() != 2) correct = false;
    });
  });
  reader.join();
  check(correct && before.query("Result").rows.size() == 1 && after.query("Result").rows.size() == 2, "concurrent validator cache publication");
}
}
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("expected regression suite");
    std::string suite = argv[1];
    if (suite == "ssa") ssa(); else if (suite == "cfg") cfg(); else if (suite == "memory") memory();
    else if (suite == "validation") validation(); else if (suite == "persistence") persistence(); else if (suite == "hardening") hardening();
    else throw std::invalid_argument("unknown regression suite");
    std::cout << suite << " regression regiment passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "regression failure: " << error.what() << '\n'; return 1; }
}
