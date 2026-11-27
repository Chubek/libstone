#include "LibDSL.hpp"

#include <cassert>

using namespace dsl;
using namespace dsl::live;

int main()
{
  int calls = 0;
  auto root = create_peg_definition();
  auto action = [&](PEGMatch&) { ++calls; };
  auto& first = root.add_rule<"let">(action);
  auto& word = root.add_rule<"[a-z]+">(action);
  first.if_succeed = &word;
  auto& whitespace = root.add_rule<"[ \n]+">(action);
  whitespace.channel = PEGIgnoreChannel;
  auto& comment = root.add_rule<"#[^\n]*">(action);
  comment.channel = PEGIgnoreChannel;
  root.add_rule<"==">(action);
  auto& negated = root.add_rule<"[^;]">(action);
  negated.must_fail = true;
  negated.fire_on_neg = true;
  auto derived = derive_peg_definition(root);
  derived.add_rule<".">(action);
  const auto inspected = analyze_peg(derived, "let #note\n=;");
  assert(inspected.status == SyntaxStatus::complete && calls == 0);
  assert(inspected.tokens.size() == 7 && inspected.tokens[1].overlay);
  assert(inspected.tokens[2].ignored && inspected.tokens[3].ignored);
  assert(inspected.tokens.back().negated);
  assert(inspected.tokens[3].span == (SourceSpan{4, 9}));
  assert(root.parse("let").ok() && calls == 2);

  auto operators = create_peg_definition();
  operators.add_rule<"==">(action);
  const auto incomplete_operator = analyze_peg(operators, "==", 1);
  assert(incomplete_operator.status == SyntaxStatus::incomplete);
  assert(incomplete_operator.tokens[0].partial);
  const auto op_grammar = sexpr_grammar(R"((grammar (start S) (rule S ("=="))))");
  auto unfinished = analyze_cfg(op_grammar, incomplete_operator.tokens, 1);
  assert(unfinished.status == SyntaxStatus::incomplete);
  assert(unfinished.replacement == (SourceSpan{0, 1}));
  const auto bad = analyze_peg(operators, "$?==");
  assert(bad.status == SyntaxStatus::invalid && bad.position == 0);
  assert(bad.tokens.size() == 2 && bad.tokens.front().error);
  assert(bad.tokens.front().text == "$?" && bad.diagnostics.front().span.end == 2);
  assert(calls == 2);
  auto impossible = create_peg_definition();
  impossible.add_rule<"a*a">();
  assert(analyze_peg(impossible, "a").status == SyntaxStatus::invalid);
  auto appendable = create_peg_definition();
  appendable.add_rule<"a+b">();
  assert(analyze_peg(appendable, "a").status == SyntaxStatus::incomplete);
  auto zero = create_peg_definition();
  zero.add_rule<"a*">();
  assert(analyze_peg(zero, "b").status == SyntaxStatus::invalid);
  zero.parent = &zero;
  assert(analyze_peg(zero, "b").diagnostics[0].code == "peg.definition");

  const auto grammar = syntax_grammar(R"grammar(
    (grammar (start Expr)
      (production sum Expr (Expr "+" Expr) (semantic add))
      (production number Expr ((category Number)))
      (production group Expr ("(" Expr ")")))
  )grammar");
  std::vector<Token> input{
    {{0, 1}, "1", "Number", "value", false, false, false, false, {"digit"}},
    {{1, 2}, "+", "Plus", "op"},
    {{2, 3}, "2", "Number", "value"},
    {{3, 4}, "+", "Plus", "op"},
    {{4, 5}, "3", "Number", "value"},
  };
  const auto complete = analyze_cfg(grammar, input, 2, {}, 5);
  assert(complete.status == SyntaxStatus::complete && complete.tree[0].complete);
  assert(complete.tree[0].production_id == "sum");
  assert(complete.tree[0].semantic_rule == "add");
  assert(complete.replacement == (SourceSpan{2, 3}));
  assert(complete.cursor_expected == (std::vector<std::string>{"(", "<Number>"}));
  assert(complete.cursor_status == SyntaxStatus::incomplete);
  SyntaxPrefixCache cache(grammar);
  const auto cached_prefix = cache.inspect(input, 3, 5);
  assert(cached_prefix.status == SyntaxStatus::complete);
  auto editor_input = input;
  editor_input.back().text = "4";
  const auto same_prefix = cache.inspect(editor_input, 3, 5);
  assert(same_prefix.status == SyntaxStatus::complete);
  editor_input[2].kind = "not-number";
  assert(cache.inspect(editor_input, 3, 5).status == SyntaxStatus::invalid);
  cache.clear();
  input.pop_back();
  const auto partial = analyze_cfg(grammar, input, 4);
  assert(partial.status == SyntaxStatus::incomplete && !partial.tree[0].complete);
  assert(partial.expected == (std::vector<std::string>{"(", "<Number>"}));
  input.push_back({{4, 5}, "?", "error", {}, false, false, false, true});
  input.push_back({{5, 6}, "3", "Number", "value"});
  const auto recovered = analyze_cfg(grammar, input, 5);
  assert(recovered.status == SyntaxStatus::invalid && recovered.position == 4);
  assert(recovered.diagnostics[0].span == (SourceSpan{4, 5}));
  assert(recovered.tree[0].semantic_rule == "add");
  auto err = std::find_if(recovered.tree.begin(), recovered.tree.end(),
                         [](const auto& n) { return n.kind == SyntaxNodeKind::error; });
  assert(err != recovered.tree.end());
  StructuralPattern error("(and (error) (text \"?\"))");
  assert(error.match(recovered, err - recovered.tree.begin()) == PatternMatchStatus::matched);
  StructuralPattern cat("(and (category Number) (channel value) (annotation digit))");
  assert(cat.match(input[0]) == PatternMatchStatus::matched);
  StructuralPattern production("(and (production sum) (semantic add))");
  assert(production.match(recovered) == PatternMatchStatus::matched);
  const auto preorder = syntax_visit_order(recovered);
  assert(preorder.diagnostics.empty() && preorder.indices.front() == 0);
  const auto postorder = syntax_visit_order(recovered, VisitOrder::Postorder);
  assert(postorder.indices.size() == recovered.tree.size() && postorder.indices.back() == 0);
  StructuralPattern list("(list (atom add) _ (list))");
  assert(list.match(parse_sexpr("(add x ())")) == PatternMatchStatus::matched);
  StructuralPattern ast("(node sum (and (node number) (text 1)) _)");
  assert(ast.match(ASTNode("sum", std::vector<ASTNode>{
      ASTNode("number", "1"), ASTNode("number", "2")})) == PatternMatchStatus::matched);
  auto corrupt = recovered;
  corrupt.tree[0].children.push_back(0);
  assert(production.match(corrupt) == PatternMatchStatus::invalid);
  assert(!syntax_visit_order(corrupt).diagnostics.empty());

  const auto nullable = sexpr_grammar(R"((grammar (start S)
      (rule S (X X "end")) (rule X (X "x") ())))");
  const std::vector<std::string> empty;
  const auto prefix = analyze_cfg(nullable, empty);
  assert(prefix.status == SyntaxStatus::incomplete);
  assert(prefix.expected == (std::vector<std::string>{"end", "x"}));
  const std::vector<std::string> sequence{"x", "x", "end"};
  assert(nullable.parse(sequence).ok());
  assert(analyze_cfg(nullable, sequence).status == SyntaxStatus::complete);
  const auto nullable_trivia = sexpr_grammar(R"((grammar (start S)
      (rule S (A "end")) (rule A ("x" X)) (rule X ())))");
  const auto gaps = analyze_cfg(nullable_trivia, std::vector<std::string>{"x", "end"});
  assert(syntax_visit_order(gaps).diagnostics.empty());
  const auto empty_token = sexpr_grammar(R"((grammar (start S) (rule S (""))))");
  const std::vector<std::string> zero_width_token{""};
  assert(analyze_cfg(empty_token, zero_width_token, 0).cursor_status
         == SyntaxStatus::incomplete);
  assert(analyze_cfg(empty_token, zero_width_token, 1).cursor_status
         == SyntaxStatus::complete);
  const auto dead = sexpr_grammar(R"((grammar (start X) (rule X ("x" X))))");
  assert(analyze_cfg(dead, empty).status == SyntaxStatus::invalid);
  input[1].span.end = 0;
  assert(analyze_cfg(grammar, input, 0).diagnostics[0].code == "cfg.definition");

  SyntaxLimits tiny;
  tiny.states = 1;
  assert(analyze_cfg(nullable, sequence, 2, tiny).limited);
  tiny.work = 0;
  assert(analyze_peg(root, "let", 3, tiny).limited);
  std::stop_source stop;
  stop.request_stop();
  tiny.stop = stop.get_token();
  assert(analyze_cfg(nullable, empty, 0, tiny).cancelled);
  assert(cat.match(input[0], tiny) == PatternMatchStatus::cancelled);
}
