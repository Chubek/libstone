#include "GateML.h"
#include <cstdlib>
#include <iostream>

namespace {
unsigned checks = 0;
void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void evaluates(const std::string& source, const std::string& expected) {
    try {
        auto result = gateml::evaluate(source);
        check(result.value.show() == expected, source + " => " + result.value.show() + ", expected " + expected);
    } catch (const gateml::Error& e) {
        throw gateml::Error(e.location, source + ": " + e.what());
    }
}
void fails(const std::string& source, const std::string& fragment, gateml::Options options = {}) {
    try { gateml::evaluate(source, options); check(false, "accepted: " + source); }
    catch (const gateml::Error& e) {
        check(std::string(e.what()).find(fragment) != std::string::npos, source + ": " + e.what());
    }
}
}
int main() {
    try {
        evaluates("1 + 2 * 3", "7");
        evaluates("10 - 3 - 2", "5");
        evaluates("let id x = x in (id 42, id true)", "(42, true)");
        evaluates("let x = 10 in let f y = x + y in let x = 99 in f 2", "12");
        evaluates("let rec fact n = if n = 0 then 1 else n * fact (n - 1) in fact 6", "720");
        evaluates("let add x y = x + y;; let inc = add 1;; inc 41", "42");
        evaluates("let rec f n = fun x -> if n = 0 then x else f (n - 1) (x + 1) in f 4 3", "7");
        evaluates("let rec f f = f in f 42", "42");
        evaluates("module Math = struct let base = 40;; let add x = base + x;; end;; Math.add 2", "42");
        evaluates("module M = struct let x = 4;; let f x = x + 1;; end;; open M;; (x, f 5)", "(4, 6)");
        evaluates("module M = struct let x = 4;; end;; open M;; let x = 9 in x", "9");
        evaluates("module M = struct let rec f n = if n = 0 then 1 else n * f (n - 1);; end;; M.f 5", "120");
        evaluates("module M = struct module N = struct let x = 42;; end;; let f y = N.x + y;; end;; M.f 0", "42");
        evaluates("false && (1 / 0 = 0)", "false");
        evaluates("true || (1 / 0 = 0)", "true");
        evaluates("(* outer (* nested *) *) [1; 2; 3] = 1 :: 2 :: 3 :: []", "true");
        evaluates("match (Some 4, [2;3]) with (Some x, h :: t) -> x + h | _ -> 0", "6");
        evaluates("compose succ succ 40", "42");
        evaluates("open List;; sum (map succ [10;20;9])", "42");
        evaluates("flip const 1 2", "2");
        evaluates("ignore (true, 1)", "()");
        evaluates("(min 4 2, max 4 2)", "(2, 4)");
        evaluates("(Int.min_int, Int.max_int + 1)", "(-9223372036854775808, -9223372036854775808)");
        evaluates("Int.pow 2 10 + Int.gcd 12 8", "1028");
        evaluates("Int.clamp 0 9 (Int.abs (-20))", "9");
        evaluates("(Int.neg 2, Int.compare 1 2)", "(-2, -1)");
        evaluates("Int.of_string (Int.to_string (-123))", "Some (-123)");
        evaluates("Int.of_string \"9223372036854775808\"", "None");
        evaluates("(Int.logand 7 3, Int.logor 4 1)", "(3, 5)");
        evaluates("(Int.logxor 7 2, Int.shift_left 1 4)", "(5, 16)");
        evaluates("Int.shift_right (-1) 63", "1");
        evaluates("(Bool.not false, Bool.to_int true)", "(true, 1)");
        evaluates("(Bool.to_string false, Bool.equal true true)", "(\"false\", true)");
        evaluates("Pair.map succ Bool.not (Pair.swap (true, 2))", "(3, false)");
        evaluates("(Pair.fst (1,2), Pair.snd (1,2))", "(1, 2)");
        evaluates("Option.map succ (Some 4)", "Some (5)");
        evaluates("Option.bind (Some 4) (fun x -> Some (x * 2))", "Some (8)");
        evaluates("(Option.is_some (Some 1), Option.is_none None)", "(true, true)");
        evaluates("Option.value 7 None + Option.get (Some 2)", "9");
        evaluates("Result.map succ (Ok 4)", "Ok (5)");
        evaluates("Result.map_error succ (Error 2)", "Error (3)");
        evaluates("Result.bind (Ok 2) (fun x -> Ok (x + 1))", "Ok (3)");
        evaluates("(Result.is_ok (Ok 1), Result.is_error (Error 2))", "(true, true)");
        evaluates("Result.value 9 (Error true)", "9");
        evaluates("Result.to_option (Ok 4)", "Some (4)");
        evaluates("String.starts_with \"he\" \"hello\" && String.ends_with \"lo\" \"hello\"", "true");
        evaluates("String.starts_with \"too long\" \"x\"", "false");
        evaluates("(String.get \"abc\" 1, String.sub \"abcd\" 1 2)", "(\"b\", \"bc\")");
        evaluates("String.is_empty \"\" && String.equal (String.of_int 42) \"42\"", "true");
        evaluates("(String.length \"abc\", String.compare \"a\" \"b\")", "(3, -1)");
        evaluates("List.map (fun x -> x * x) (List.range 1 4)", "[1; 4; 9; 16]");
        evaluates("List.filter (fun x -> x mod 2 = 0) (List.range 1 6)", "[2; 4; 6]");
        evaluates("List.sum [1;2;3] + List.product [2;3;4]", "30");
        evaluates("(List.is_empty [], List.length [1;2])", "(true, 2)");
        evaluates("List.rev [1;2;3]", "[3; 2; 1]");
        evaluates("(List.head [1;2], List.tail [1;2])", "(Some (1), Some ([2]))");
        evaluates("List.concat [[1;2]; []; [3]]", "[1; 2; 3]");
        evaluates("List.concat_map (fun x -> [x;x]) [1;2]", "[1; 1; 2; 2]");
        evaluates("List.exists (fun x -> x > 2) [1;3] && List.for_all (fun x -> x > 0) [1;3]", "true");
        evaluates("List.mem 2 [1;2;3]", "true");
        evaluates("List.find (fun x -> x > 2) [1;2;3]", "Some (3)");
        evaluates("(List.nth [1;2] 1, List.nth [1;2] (-1))", "(Some (2), None)");
        evaluates("(List.take 2 [1;2;3], List.drop 2 [1;2;3])", "([1; 2], [3])");
        evaluates("List.range 3 1", "[]");
        auto printed = gateml::evaluate("List.iter IO.print_int [1;2]; IO.print_string \"-\"; IO.print_bool true; IO.print_endline \"!\"; 42");
        check(printed.output == "12-true!\n" && printed.value.integer() == 42, "captured IO");
        check(gateml::compile("fun x -> x").type == "('a -> 'a)", "principal type");
        check(!gateml::disassemble(gateml::compile("42")).empty(), "IR listing");
        fails("1 + true", "type mismatch");
        fails("if 1 then 2 else 3", "type mismatch");
        fails("fun x -> x x", "infinite type");
        fails("missing", "unbound name");
        fails("let rec x = 1 in x", "requires a function");
        fails("[1; true]", "type mismatch");
        fails("match (1,2) with (x,x) -> x", "duplicate pattern");
        fails("match [] with h :: t -> h", "non-exhaustive");
        fails("1 / 0", "invalid division");
        fails("Int.min_int / (-1)", "invalid division");
        fails("Int.shift_left 1 64", "shift count");
        fails("String.get \"x\" 1", "out of bounds");
        fails("Option.get None", "Option.get");
        fails("Int.pow 2 (-1)", "negative exponent");
        fails("(fun x -> x) = (fun x -> x)", "equality");
        fails("9223372036854775808", "outside signed");
        fails("(* unfinished", "unterminated");
        fails("\"bad\\q\"", "escape");
        fails("let x = 1", "expected");
        fails("let x = 1 in x ;; 99", "trailing input");
        fails("module M = struct let x = 1 in x end;; 0", "module bodies");
        fails("module M = struct 42 end;; 0", "module bodies");
        fails("module M = struct 42;; end;; 0", "module bodies");
        fails("open Nope;; 1", "unknown module");
        fails("module M = struct end;; M.nope", "unbound name");
        fails("(* unfinished after (", "unterminated comment");
        fails("List.map ((*) 2) [1; true]", "type mismatch");
        fails("let f = (+) in f true", "type mismatch");
        fails("flip (+) 1 2 3", "type mismatch");
        check(gateml::compile("(+)").type == "(int -> (int -> int))", "operator value type");
        check(gateml::compile("(-)").type == "(int -> (int -> int))", "subtraction value type");
        check(gateml::compile("(not)").type == "(bool -> bool)", "not value type");
        check(gateml::compile("(^)").type == "(string -> (string -> string))", "concat value type");
        check(gateml::compile("(=)").type == "('a -> ('a -> bool))", "equality value type");
        evaluates("let f = (+) in f 1 2", "3");
        evaluates("let f = (not) in f false", "true");
        evaluates("((^) \"a\") \"b\" ^ \"c\"", "\"abc\"");
        evaluates("List.fold_left (+) 0 [1;2;3]", "6");
        evaluates("List.fold_right (+) [1;2;3] 0", "6");
        evaluates("List.map ((*) 2) [1;2;3]", "[2; 4; 6]");
        evaluates("List.filter ((>) 2) [1;2;3]", "[1]");
        evaluates("List.filter (fun y -> y > 2) [1;2;3]", "[3]");
        evaluates("List.filter ((<) 3) [1;2;3]", "[]");
        evaluates("List.filter ((<) 3) [1;2;3;4]", "[4]");
        evaluates("((<) 3) 4", "true");
        evaluates("((>) 3) 4", "false");
        evaluates("List.map ( (-) 10 ) [1;2;3]", "[9; 8; 7]");
        evaluates("List.fold_right (-) [1;2;3] 0", "2");
        evaluates("List.map (mod) [1;2]", "[<fun>; <fun>]");
        evaluates("(mod) 17 5", "2");
        evaluates("let p = (+) 10 in (p 2, p 3)", "(12, 13)");
        evaluates("let get = String.get \"abc\" in (get 0, get 2)", "(\"a\", \"c\")");
        evaluates("let sub = String.sub \"abcd\" 1 in (sub 1, sub 2)", "(\"b\", \"bc\")");
        evaluates("List.map (=) [1;2]", "[<fun>; <fun>]");
        evaluates("((=) 3) 3", "true");
        evaluates("(<>) [1] [2]", "true");
        evaluates("(&&) true false", "false");
        evaluates("(||) false true", "true");
        evaluates("(-1)", "-1");
        evaluates("(- (1 + 2))", "-3");
        evaluates("(not false)", "true");
        evaluates("(*) 6 7", "42");
        evaluates("( * ) 6 7", "42");
        evaluates("((* comment inside parentheses *) 40) + 2", "42");
        evaluates("(**) 42", "42");
        evaluates("(- (* comment *) 2)", "-2");
        fails("(&&) false (1 / 0 = 0)", "invalid division");
        fails("(||) true (1 / 0 = 0)", "invalid division");
        evaluates("let p = (fun y -> 10 - y) in p 3 + p 4", "13");
        evaluates("let p = (-) 10 in p 3", "7");
        evaluates("1 :: 2 :: 3 :: []", "[1; 2; 3]");
        evaluates("10 - 1 :: [0]", "[9; 0]");
        evaluates("1 * 2 :: [0]", "[2; 0]");
        evaluates("(1 + 1) :: [2]", "[2; 2]");
        evaluates("2 - -3", "5");
        evaluates("-2 * 3", "-6");
        evaluates("2 * -3", "-6");
        evaluates("not true || false", "false");
        evaluates("let succ x = x;; succ 5", "5");
        evaluates("let f x = x + 1;; let f x = x + 2;; f 40", "42");
        evaluates("module M = struct module N = struct let x = 42;; end;; let f y = N.x + y;; end;; open M;; f 0", "42");
        evaluates("module M = struct let x = 1;; end;; open M;; let x = 9 in x", "9");
        evaluates("module M = struct end;; 1", "1");
        evaluates("module M = struct end;; open M;; 42", "42");
        evaluates("module M = struct let x = 4;; end;; M.x + 38", "42");
        check(gateml::evaluate("String.length (\"x\" ^ \"y\")").value.integer() == 2, "string values");
        auto long_string = gateml::evaluate("\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"").value;
        check(long_string.show(128, 10) == "\"aaaaaa...", "truncation ellipsis: " + long_string.show(128, 10));
        check(long_string.show(128, 0).empty(), "zero budget stays empty");
        gateml::Options bare; bare.standard_library = false;
        check(gateml::evaluate("40 + 2", bare).value.integer() == 42, "without stdlib");
        fails("List.length []", "unbound", bare);
        bare.max_steps = 50;
        fails("let rec loop x = loop x in loop 0", "step limit", bare);
        bare.max_steps = 1000; bare.max_output_bytes = 2;
        fails("print_string \"abc\"", "output limit", bare);
        bare.max_output_bytes = 1024; bare.max_string_bytes = 4;
        fails("\"abc\" ^ \"de\"", "string allocation limit", bare);
        bare.max_string_bytes = 1024; bare.max_source_bytes = 2;
        fails("123", "source size", bare);
        bare.max_source_bytes = 1024; bare.max_depth = 10;
        fails("1+1+1+1+1+1+1+1+1+1+1+1", "depth limit", bare);
        bare = gateml::Options{}; bare.standard_library = false;
        auto limited = bare;
        limited.max_source_bytes = 100;
        fails("let rec grow n s = if n = 0 then s else grow (n-1) (s ^ s) in grow 7 \"ab\"", "string size limit", limited);
        fails("let rec grow n s = if n = 0 then s else grow (n-1) ((^) s s) in grow 7 \"ab\"", "string size limit", limited);
        limited.max_source_bytes = 1024; limited.max_string_bytes = 4;
        fails("(^) \"abc\" \"de\"", "string allocation limit", limited);
        // Calling a user closure must not recharge an unrelated string already
        // on the operand stack. The only produced string here is the 2-byte
        // concatenation; the user call returns an int.
        limited.max_string_bytes = 2;
        check(gateml::evaluate("let f x = x in ((\"a\" ^ \"b\"), f 42)", limited).value.show() == "(\"ab\", 42)", "string accounting across user calls");
        check(gateml::evaluate("let rec loop n acc = if n = 0 then acc else let x = acc + n in loop (n - 1) x in loop 10000 0", bare).value.integer() == 50005000, "tail calls across local scopes");
        check(gateml::evaluate("let rec loop n xs = if n = 0 then xs else loop (n - 1) (n :: xs) in List.length (loop 5000 [])").value.integer() == 5000, "long lists and iterative destruction");
        auto no_opt = bare; no_opt.optimize = false;
        check(gateml::evaluate("if false then 1/0 else 6*7", no_opt).value.integer() == 42, "unoptimized execution");
        std::string dag = "let x0 = 0;; ";
        for (unsigned n = 1; n <= 12; ++n) {
            auto previous = "x" + std::to_string(n - 1);
            dag += "let x" + std::to_string(n) + " = (" + previous + ", " + previous + ");; ";
        }
        auto big_display = gateml::evaluate(dag + "x12", bare);
        check(big_display.value.show(128, 100).size() <= 100, "bounded DAG display");
        check(big_display.value.show(128, 0).empty(), "zero display budget");
        fails("let rec grow n x = if n = 0 then x else grow (n - 1) (x, x) in grow 3 0", "infinite type", bare);
        auto reusable = gateml::compile("let x = 6 in x * 7");
        check(gateml::run(reusable).value.integer() == 42 && gateml::run(reusable).value.integer() == 42, "reusable program");
        auto bytes = gateml::executable("let rec f n = if n = 0 then 1 else n * f (n - 1) in f 5");
        check(bytes.size() > 4 && bytes[0] == 0x7f && bytes[1] == 'E', "MetaChain ELF pipeline");
        std::cout << checks << " checks passed\n";
    } catch (const gateml::Error& e) {
        std::cerr << e.location.file << ':' << e.location.line << ':' << e.location.column << ": " << e.what() << '\n'; return 1;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
