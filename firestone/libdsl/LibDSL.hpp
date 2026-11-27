/**
 * @file LibDSL.hpp
 * @brief Header-only DSL construction toolkit for modern C++ (C++20)
 *
 * ============================================================
 *  OVERVIEW
 * ============================================================
 *
 * LibDSL.hpp provides a set of composable "feature mixins" that you
 * attach to a CRTP base class to build Domain-Specific Languages entirely
 * in headers, with zero runtime overhead where possible.
 *
 * Core idea:
 *
 *   struct MyDSL : dsl::DSL<MyDSL, Feature1, Feature2, ...> { ... };
 *
 * Available features:
 *   - dsl::Pipeline       — operator| chaining (pipe stages)
 *   - dsl::Operators      — composable predicates (&, |, !)
 *   - dsl::PatternMatch   — compile-time pattern matching
 * (match/when/otherwise)
 *   - dsl::AST            — value-type AST nodes (leaf/node/dump)
 *   - dsl::Rewrite        — AST rewrite rules
 *   - dsl::ExprTemplates  — lazy expression trees with fused eval
 *   - dsl::CustomLiterals — static constexpr literal suffix registration
 *   - dsl::EnumReflection — magic_enum-compatible enum reflection and
 * utilities
 *   - SExpressions / EqualitySaturation — S-expression rules and e-graphs
 *   - Serialization / AbstractIO — user-defined codec and byte-I/O protocols
 *   - Visitors — AST-schema-derived visitor dispatch
 *   - StaticRegularExpressions — compile-time byte regexes and binary streams
 *   - Preprocessing — macros, includes, conditionals, optional Lua hooks
 *   - ContextFreeGrammar — explicit CFG productions and Earley parsing
 *   - Graphs / Terms — directed multigraphs, DOT, first-order terms and unification
 *   - TermRewriting / GraphReduction — bounded tree and shared-term reduction
 *   - Terminfo / GlobalEscapeCodes — terminal capabilities and styled output
 *   - OptParser / REPL — command schemas, editing, and interactive analysis
 *   - LanguageSynthesizer — specifications, attributes, and executable semantics
 * See firestone/docs/LibDSL.md for usage and integration notes.
 *
 * ============================================================
 *  QUICK START
 * ============================================================
 *
 * Step 1 — Pick your features:
 *
 *   struct MathDSL : dsl::DSL<MathDSL, dsl::Pipeline, dsl::Operators> {};
 * Step 2 — Use it:
 *
 *   MathDSL m;
 *   auto result = m.eval(5.0);
 *
 * Step 3 — Expose to users via a thin wrapper header (mydsl.hpp):
 *
 *   #include "LibDSL.hpp"
 *   struct MathDSL : dsl::DSL<MathDSL, dsl::Pipeline, dsl::Operators> { ... };
 *
 * Users only include mydsl.hpp — they never see LibDSL.hpp directly.
 *
 * ============================================================
 *  FEATURE REFERENCE
 * ============================================================
 *
 * --- Pipeline ---
 *
 *   Adds operator| so values can flow through a chain of transformations.
 *
 *   Usage:
 *     auto result = value | dsl::pipe(f1) | dsl::pipe(f2);
 *     // equivalent to: f2(f1(value))
 *
 *   Example:
 *     struct Proc : dsl::DSL<Proc, dsl::Pipeline> {};
 *     Proc p;
 *     auto r = p.wrap(10)
 *             | dsl::pipe([](int x){ return x * 2; })
 *             | dsl::pipe([](int x){ return x + 1; });
 *     // r == 21
 *
 * --- Operators ---
 *
 *   Adds &, |, ! for composing predicates/validators.
 *
 *   Usage:
 *     auto combined = pred_a & pred_b;   // both must pass
 *     auto either   = pred_a | pred_b;   // at least one must pass
 *     auto negated  = !pred_a;
 *
 *   Example:
 *     auto is_positive = dsl::predicate([](int x){ return x > 0; });
 *     auto is_even     = dsl::predicate([](int x){ return x % 2 == 0; });
 *     auto rule = is_positive & is_even;
 *     rule(4);   // true
 *     rule(-2);  // false
 *
 * --- PatternMatch ---
 *
 *   Compile-time dispatch table built from when<>/otherwise clauses.
 *
 *   Usage:
 *     auto table = dsl::match(
 *         dsl::when<Key1>(handler1),
 *         dsl::when<Key2>(handler2),
 *         dsl::otherwise(fallback)
 *     );
 *     auto result = table(key, args...);
 *
 *   For string patterns (CTRE-style, limited):
 *     auto table = dsl::match(
 *         dsl::when<dsl::pattern<"[0-9]+">>(handler),
 *         dsl::otherwise(fallback)
 *     );
 *
 *   Example:
 *     enum class Color { Red, Green, Blue };
 *     auto name = dsl::match(
 *         dsl::when<Color::Red>  ([](auto){ return "red";   }),
 *         dsl::when<Color::Green>([](auto){ return "green"; }),
 *         dsl::otherwise         ([](auto){ return "other"; })
 *     );
 *     name(Color::Red, 0);  // "red"
 *
 * --- AST ---
 *
 *   Value-type AST nodes. Leaves hold data; inner nodes hold children.
 *
 *   Usage:
 *     auto leaf_node  = dsl::leaf<"name">(value);
 *     auto inner_node = dsl::node<"op">(child1, child2);
 *     std::cout << node.dump();   // S-expression string
 *
 *   Example:
 *     auto expr = dsl::node<"add">(
 *         dsl::leaf<"num">(1),
 *         dsl::leaf<"num">(2)
 *     );
 *     expr.dump();  // "(add (num 1) (num 2))"
 *
 * --- Rewrite ---
 *
 *   Apply transformation rules to an AST until a fixpoint is reached.
 *
 *   Usage:
 *     auto rules = dsl::rewrite_set(
 *         dsl::rule<"name">(predicate, transformer)
 *     );
 *     auto new_tree = rules.apply(tree);
 *
 *   Example:
 *     auto rules = dsl::rewrite_set(
 *         dsl::rule<"double-neg">(
 *             [](auto& t){ return t.tag() == "not" && t.child(0).tag() ==
 * "not"; },
 *             [](auto& t){ return t.child(0).child(0); }  // remove double
 * negation
 *         )
 *     );
 *
 * --- ExprTemplates ---
 *
 *   Lazy expression trees. Operators build a tree; .eval() fuses it.
 *
 *   Usage:
 *     auto expr = a + b * c;   // no computation yet
 *     auto val  = expr.eval(); // single fused pass
 *
 *   Example:
 *     struct Vec : dsl::DSL<Vec, dsl::ExprTemplates> {
 *         std::vector<float> data;
 *     };
 *     Vec a{{1,2,3}}, b{{4,5,6}}, c{{7,8,9}};
 *     auto expr = a + b * c;
 *     auto result = expr.eval();  // {1+4*7, 2+5*8, 3+6*9} = {29, 42, 57}
 *
 * --- CustomLiterals ---
 *
 *   Register constexpr literal suffixes via a static table.
 *
 *   Usage:
 *     static constexpr auto literals = dsl::literal_set(
 *         dsl::lit<"_km">([](long double v){ return v * 1000.0L; }),
 *         dsl::lit<"_m"> ([](long double v){ return v; })
 *     );
 *     auto d = MyDSL::parse_literal("3.5_km");  // 3500.0
 *
 *   For native C++ UDL syntax (3.5_km), define operator"" in an
 *   inline namespace in your own header — see examples below.
 *
 * --- Memoization ---
 *
 *   Caches expensive pure-function results by hashable key.
 *
 *   Usage:
 *     auto f = dsl::memoize<int, int>([](int x){ return x * x; });
 *
 *   Example:
 *     auto fib = dsl::memoize<int, long long>([](int n){ return n < 2 ? n :
 * 1LL; }); auto v = fib(20);
 *
 * --- Lazy ---
 *
 *   Defers one computation until first get()/force() and caches it.
 *
 *   Usage:
 *     dsl::Lazy<int> v([]{ return 42; });
 *
 *   Example:
 *     dsl::Lazy<std::string> cfg([]{ return std::string("ready"); });
 *     auto s = cfg.get();
 *
 * --- Monadic ---
 *
 *   Optional-aware map/flat_map/filter/or_else for Maybe and std::optional.
 *
 *   Usage:
 *     auto out = dsl::Maybe<int>(5).map([](int x){ return x + 1;
 * }).or_else(0);
 *
 *   Example:
 *     std::optional<int> v = 3;
 *     auto out = dsl::map(v, [](int x){ return x * 2; });
 *
 * --- Result ---
 *
 *   Explicit success/error flow modeled as Result<T,E>.
 *
 *   Usage:
 *     auto r = dsl::Result<int,std::string>::from_ok(7);
 *
 *   Example:
 *     auto r = dsl::Result<int,std::string>::from_ok(4).map([](int x){ return
 * x+1; });
 *
 * --- CombinatorParser ---
 *
 *   Functional parser core + combinators (`|`, `&`, `*`, optional()).
 *
 *   Usage:
 *     auto p = dsl::parser([](dsl::ParsecInput&
 * in)->dsl::ExpectedResult<char>{...});
 *
 *   Example:
 *     auto seq = p1 & p2;
 *
 * --- TaskPipeline ---
 *
 *   Composes tasks with `|` and executes with shared TaskState.
 *
 *   Usage:
 *     auto chain = t1 | t2; dsl::run(state, chain);
 *
 *   Example:
 *     dsl::TaskState st; dsl::run(st, chain);
 *
 * --- EnumReflection ---
 *
 *   Static enum reflection, casts, flags, constexpr dispatch, fusion,
 *   enum-indexed containers, stream operators, and optional formatters.
 *   All magic_enum v0.9.8 public APIs are included in this single header.
 *
 *   Usage:
 *     enum class Color { Red = -10, Green = 0, Blue = 10 };
 *     static_assert(dsl::enum_name(Color::Red) == "Red");
 *     constexpr auto color = dsl::enum_cast<Color>("blue",
 * dsl::case_insensitive); constexpr auto values = dsl::enum_values<Color>();
 *
 *   The dsl::enum_reflection namespace aliases magic_enum. Customize ranges
 *   and names through dsl::customize (an alias of magic_enum::customize).
 *   The default search range is [-128, 127]; enum_name<Value>() is unbounded.
 *   See manual/ch29-enum-reflection.md for the complete API and configuration.
 *
 * ============================================================
 *  FULL EXAMPLES
 * ============================================================
 *
 * Example A — UnitsDSL (CustomLiterals):
 *
 *   struct UnitsDSL : dsl::DSL<UnitsDSL, dsl::CustomLiterals> {
 *       static constexpr auto literals = dsl::literal_set(
 *           dsl::lit<"_km">([](long double v){ return v * 1000.0L; }),
 *           dsl::lit<"_m"> ([](long double v){ return v; }),
 *           dsl::lit<"_cm">([](long double v){ return v / 100.0L; })
 *       );
 *   };
 *   inline namespace unit_literals {
 *       constexpr long double operator""_km(long double v){ return v *
 * 1000.0L; } constexpr long double operator""_m (long double v){ return v; }
 *   }
 *   // Usage:
 *   using namespace unit_literals;
 *   auto d = 3.5_km;  // 3500.0
 *
 * Example B — StateMachineDSL (Pipeline + PatternMatch):
 *
 *   struct SM : dsl::DSL<SM, dsl::Pipeline, dsl::PatternMatch> {
 *       enum class S { Idle, Running, Done };
 *       static constexpr auto transitions = dsl::match(
 *           dsl::when<S::Idle>   ([](auto e){ return
 * e=="start"?S::Running:S::Idle; }), dsl::when<S::Running>([](auto e){ return
 * e=="stop" ?S::Done   :S::Running; }), dsl::otherwise       ([](auto  ){
 * return S::Done; })
 *       );
 *       S state = S::Idle;
 *       void send(std::string_view e){ state = transitions(state, e); }
 *   };
 *   // Usage:
 *   SM sm;
 *   sm.send("start");  // state == Running
 *   sm.send("stop");   // state == Done
 *
 * Example C — VecDSL (ExprTemplates):
 *
 *   struct VecDSL : dsl::DSL<VecDSL, dsl::ExprTemplates> {
 *       std::vector<float> data;
 *   };
 *   // Usage:
 *   VecDSL a{{1,2,3}}, b{{4,5,6}};
 *   auto expr   = a + b;          // lazy
 *   auto result = expr.eval();    // {5, 7, 9}
 *
 * Example D — QueryDSL (AST + Rewrite):
 *
 *   struct QueryDSL : dsl::DSL<QueryDSL, dsl::AST, dsl::Rewrite> {
 *       auto select(auto... cols) { return
 * dsl::node<"select">(dsl::leaf<"col">(cols)...); } auto from  (auto table)  {
 * return dsl::leaf<"from">(table); }
 *   };
 *   // Usage:
 *   QueryDSL q;
 *   auto tree = q.select("name","age") | q.from("users");
 *   std::cout << tree.dump();
 *   // "(select (col name) (col age) (from users))"
 *
 * Example E — Memoization:
 *   auto sq = dsl::memoize<int,int>([](int x){ return x*x; });
 *   auto v = sq(12);
 *
 * Example F — Lazy + Monadic:
 *   dsl::Lazy<dsl::Maybe<int>> lazy([]{ return dsl::Maybe<int>(8); });
 *   auto out = lazy.get().map([](int x){ return x+1; }).or_else(0);
 *
 * Example G — Result:
 *   auto r = dsl::Result<int,std::string>::from_ok(9).map([](int x){ return
 * x*2; });
 *
 * Example H — Parser + TaskPipeline:
 *   dsl::ParsecInput in{"abc",0};
 *   auto p = dsl::parser([](dsl::ParsecInput& i)->dsl::ExpectedResult<char>{
 * return i.consume(); }); dsl::Task t{"noop", [](dsl::TaskState&){ return
 * dsl::Result<std::string,std::string>::from_ok("ok"); }};
 *
 * ============================================================
 *  DESIGN NOTES
 * ============================================================
 *
 * 1. CRTP avoids virtual dispatch — all polymorphism is compile-time.
 * 2. Features are orthogonal: adding Pipeline does not affect PatternMatch.
 * 3. NTTPs (non-type template parameters) are used for string tags and
 *    pattern strings, enabling constexpr matching with zero overhead.
 * 4. AST nodes are value types (copyable, movable) — no heap allocation
 *    for small trees (uses std::variant + std::vector internally).
 * 5. ExprTemplates use recursive template structs; .eval() is the only
 *    point where actual computation happens.
 * 6. CustomLiterals::parse_literal does a linear scan of the literal_set
 *    at runtime; for native UDL syntax prefer operator"" in your header.
 *
 * ============================================================
 *  REQUIREMENTS
 * ============================================================
 *
 *   - C++20 or later (uses concepts, NTTPs, constexpr lambdas)
 *   - No external dependencies
 *   - Header-only: just #include "LibDSL.hpp"
 * ============================================================
 */

#pragma once

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#ifndef LIBDSL_LIVE_POSIX
#if defined(__unix__) || defined(__APPLE__)
#define LIBDSL_LIVE_POSIX 1
#else
#define LIBDSL_LIVE_POSIX 0
#endif
#endif

#if LIBDSL_LIVE_POSIX
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

#if defined(LIBDSL_ENABLE_LUA)
#include <lua.hpp>
#endif

// BEGIN LIBDSL EMBEDDED MAGIC_ENUM
// Generated by scripts/update-magic-enum.py; do not edit this block.
// Source: https://github.com/Neargye/magic_enum/tree/v0.9.8
// Public-header SHA256:
// 8e39051ab6167a80897f9b67c24da77f29f108209b11f5ec47ab8d74964611d6 Upstream
// include guards and MIT notices are retained. Embedded upstream header:
// magic_enum_all.hpp
//  __  __             _        ______                          _____
// |  \/  |           (_)      |  ____|                        / ____|_     _
// | \  / | __ _  __ _ _  ___  | |__   _ __  _   _ _ __ ___   | |   _| |_ _| |_
// | |\/| |/ _` |/ _` | |/ __| |  __| | '_ \| | | | '_ ` _ \  | |  |_   _|_ _|
// | |  | | (_| | (_| | | (__  | |____| | | | |_| | | | | | | | |____|_|   |_|
// |_|  |_|\__,_|\__, |_|\___| |______|_| |_|\__,_|_| |_| |_|  \_____|
//                __/ | https://github.com/Neargye/magic_enum
//               |___/  version 0.9.8
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2019 - 2024 Daniil Goncharov <neargye@gmail.com>.
//
// Permission is hereby  granted, free of charge, to any  person obtaining a
// copy of this software and associated  documentation files (the "Software"),
// to deal in the Software  without restriction, including without  limitation
// the rights to  use, copy,  modify, merge,  publish, distribute,  sublicense,
// and/or  sell copies  of  the Software,  and  to  permit persons  to  whom
// the Software  is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS
// OR IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN
// NO EVENT  SHALL THE AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY
// CLAIM,  DAMAGES OR  OTHER LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef NEARGYE_MAGIC_ENUM_ALL_HPP
#define NEARGYE_MAGIC_ENUM_ALL_HPP

// Embedded upstream header: magic_enum.hpp
//  __  __             _        ______                          _____
// |  \/  |           (_)      |  ____|                        / ____|_     _
// | \  / | __ _  __ _ _  ___  | |__   _ __  _   _ _ __ ___   | |   _| |_ _| |_
// | |\/| |/ _` |/ _` | |/ __| |  __| | '_ \| | | | '_ ` _ \  | |  |_   _|_ _|
// | |  | | (_| | (_| | | (__  | |____| | | | |_| | | | | | | | |____|_|   |_|
// |_|  |_|\__,_|\__, |_|\___| |______|_| |_|\__,_|_| |_| |_|  \_____|
//                __/ | https://github.com/Neargye/magic_enum
//               |___/  version 0.9.8
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2019 - 2024 Daniil Goncharov <neargye@gmail.com>.
//
// Permission is hereby  granted, free of charge, to any  person obtaining a
// copy of this software and associated  documentation files (the "Software"),
// to deal in the Software  without restriction, including without  limitation
// the rights to  use, copy,  modify, merge,  publish, distribute,  sublicense,
// and/or  sell copies  of  the Software,  and  to  permit persons  to  whom
// the Software  is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS
// OR IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN
// NO EVENT  SHALL THE AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY
// CLAIM,  DAMAGES OR  OTHER LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef NEARGYE_MAGIC_ENUM_HPP
#define NEARGYE_MAGIC_ENUM_HPP

#define MAGIC_ENUM_VERSION_MAJOR 0
#define MAGIC_ENUM_VERSION_MINOR 9
#define MAGIC_ENUM_VERSION_PATCH 8

#ifndef MAGIC_ENUM_USE_STD_MODULE
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>
#endif

#if defined(MAGIC_ENUM_CONFIG_FILE)
#include MAGIC_ENUM_CONFIG_FILE
#endif

#ifndef MAGIC_ENUM_USE_STD_MODULE
#if !defined(MAGIC_ENUM_USING_ALIAS_OPTIONAL)
#include <optional>
#endif
#if !defined(MAGIC_ENUM_USING_ALIAS_STRING)
#include <string>
#endif
#if !defined(MAGIC_ENUM_USING_ALIAS_STRING_VIEW)
#include <string_view>
#endif
#endif

#if defined(MAGIC_ENUM_NO_ASSERT)
#define MAGIC_ENUM_ASSERT(...) static_cast<void> (0)
#elif !defined(MAGIC_ENUM_ASSERT)
#include <cassert>
#define MAGIC_ENUM_ASSERT(...) assert ((__VA_ARGS__))
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wenum-constexpr-conversion"
#pragma clang diagnostic ignored                                              \
    "-Wuseless-cast" // suppresses 'static_cast<char_type>('\0')' for char_type
                     // = char (common on Linux).
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored                                                \
    "-Wmaybe-uninitialized" // May be used uninitialized 'return {};'.
#pragma GCC diagnostic ignored                                                \
    "-Wuseless-cast" // suppresses 'static_cast<char_type>('\0')' for char_type
                     // = char (common on Linux).
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(                                                              \
    disable : 26495) // Variable 'static_str<N>::chars_' is uninitialized.
#pragma warning(                                                              \
    disable : 28020) // Arithmetic overflow: Using operator '-' on a 4 byte
                     // value and then casting the result to a 8 byte value.
#pragma warning(disable                                                       \
                : 26451) // The expression '0<=_Param_(1)&&_Param_(1)<=1-1' is
                         // not true at this call.
#pragma warning(disable                                                       \
                : 4514) // Unreferenced inline function has been removed.
#endif

// Checks magic_enum compiler compatibility.
#if defined(__clang__) && __clang_major__ >= 5                                \
    || defined(__GNUC__) && __GNUC__ >= 9                                     \
    || defined(_MSC_VER) && _MSC_VER >= 1910 || defined(__RESHARPER__)
#undef MAGIC_ENUM_SUPPORTED
#define MAGIC_ENUM_SUPPORTED 1
#endif

// Checks magic_enum compiler aliases compatibility.
#if defined(__clang__) && __clang_major__ >= 5                                \
    || defined(__GNUC__) && __GNUC__ >= 9                                     \
    || defined(_MSC_VER) && _MSC_VER >= 1920
#undef MAGIC_ENUM_SUPPORTED_ALIASES
#define MAGIC_ENUM_SUPPORTED_ALIASES 1
#endif

// Specify the calling convention for compilers that need it in order to get
// reliable mangled names under different compiler flags. In particular, MSVC
// allows changing the default calling convention on x86.
#if defined(__clang__) || defined(__GNUC__)
#define MAGIC_ENUM_CALLING_CONVENTION
#elif defined(_MSC_VER)
#define MAGIC_ENUM_CALLING_CONVENTION __cdecl
#endif

// Enum value must be greater or equals than MAGIC_ENUM_RANGE_MIN. By default
// MAGIC_ENUM_RANGE_MIN = -128. If need another min range for all enum types by
// default, redefine the macro MAGIC_ENUM_RANGE_MIN.
#if !defined(MAGIC_ENUM_RANGE_MIN)
#define MAGIC_ENUM_RANGE_MIN -128
#endif

// Enum value must be less or equals than MAGIC_ENUM_RANGE_MAX. By default
// MAGIC_ENUM_RANGE_MAX = 127. If need another max range for all enum types by
// default, redefine the macro MAGIC_ENUM_RANGE_MAX.
#if !defined(MAGIC_ENUM_RANGE_MAX)
#define MAGIC_ENUM_RANGE_MAX 127
#endif

// Improve ReSharper C++ intellisense performance with builtins, avoiding
// unnecessary template instantiations.
#if defined(__RESHARPER__)
#undef MAGIC_ENUM_GET_ENUM_NAME_BUILTIN
#undef MAGIC_ENUM_GET_TYPE_NAME_BUILTIN
#if __RESHARPER__ >= 20230100
#define MAGIC_ENUM_GET_ENUM_NAME_BUILTIN(V) __rscpp_enumerator_name (V)
#define MAGIC_ENUM_GET_TYPE_NAME_BUILTIN(T) __rscpp_type_name<T> ()
#else
#define MAGIC_ENUM_GET_ENUM_NAME_BUILTIN(V) nullptr
#define MAGIC_ENUM_GET_TYPE_NAME_BUILTIN(T) nullptr
#endif
#endif

namespace magic_enum
{

// If need another optional type, define the macro
// MAGIC_ENUM_USING_ALIAS_OPTIONAL.
#if defined(MAGIC_ENUM_USING_ALIAS_OPTIONAL)
MAGIC_ENUM_USING_ALIAS_OPTIONAL
#else
using std::optional;
#endif

// If need another string_view type, define the macro
// MAGIC_ENUM_USING_ALIAS_STRING_VIEW.
#if defined(MAGIC_ENUM_USING_ALIAS_STRING_VIEW)
MAGIC_ENUM_USING_ALIAS_STRING_VIEW
#else
using std::string_view;
#endif

// If need another string type, define the macro MAGIC_ENUM_USING_ALIAS_STRING.
#if defined(MAGIC_ENUM_USING_ALIAS_STRING)
MAGIC_ENUM_USING_ALIAS_STRING
#else
using std::string;
#endif

using char_type = string_view::value_type;
static_assert (std::is_same_v<string_view::value_type, string::value_type>,
               "magic_enum::customize requires same string_view::value_type "
               "and string::value_type");
static_assert (
    []
      {
        if constexpr (std::is_same_v<char_type, wchar_t>)
          {
            constexpr const char c[] = "abcdefghijklmnopqrstuvwxyz_"
                                       "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789|";
            constexpr const wchar_t wc[]
                = L"abcdefghijklmnopqrstuvwxyz_"
                  L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789|";
            static_assert (std::size (c) == std::size (wc),
                           "magic_enum::customize identifier characters are "
                           "multichars in wchar_t.");

            for (std::size_t i = 0; i < std::size (c); ++i)
              {
                if (c[i] != wc[i])
                  {
                    return false;
                  }
              }
          }
        return true;
      }(),
    "magic_enum::customize wchar_t is not compatible with ASCII.");

namespace customize
{
template <typename E, typename = void> struct enum_range;
}

namespace detail
{
template <typename E, typename = void>
constexpr inline std::size_t prefix_length_or_zero = 0;

template <typename E>
constexpr inline auto prefix_length_or_zero<
    E, std::void_t<decltype (customize::enum_range<E>::prefix_length)>>
    = std::size_t{ customize::enum_range<E>::prefix_length };
}

namespace customize
{

template <bool IsFlags = false, int Min = MAGIC_ENUM_RANGE_MIN,
          int Max = MAGIC_ENUM_RANGE_MAX, std::size_t PrefixLength = 0>
struct adl_info_holder
{
  constexpr static int max = Max;
  constexpr static int min = Min;
  constexpr static bool is_flags = IsFlags;
  constexpr static std::size_t prefix_length = PrefixLength;

  template <int min, int max>
  constexpr static adl_info_holder<IsFlags, min, max, PrefixLength>
  minmax ()
  {
    return {};
  }

  template <bool is_flag>
  constexpr static adl_info_holder<is_flag, Min, Max, PrefixLength>
  flag ()
  {
    return {};
  }

  template <std::size_t prefix_len>
  constexpr static adl_info_holder<IsFlags, Min, Max, prefix_len>
  prefix ()
  {
    return {};
  }
};

constexpr adl_info_holder<>
adl_info ()
{
  return {};
}

// Enum value must be in range [MAGIC_ENUM_RANGE_MIN, MAGIC_ENUM_RANGE_MAX]. By
// default MAGIC_ENUM_RANGE_MIN = -128, MAGIC_ENUM_RANGE_MAX = 127. If need
// another range for all enum types by default, redefine the macro
// MAGIC_ENUM_RANGE_MIN and MAGIC_ENUM_RANGE_MAX. If need another range for
// specific enum type, add specialization enum_range for necessary enum type.
template <typename E, typename> struct enum_range
{
  static constexpr int min = MAGIC_ENUM_RANGE_MIN;
  static constexpr int max = MAGIC_ENUM_RANGE_MAX;
};

template <typename E>
struct enum_range<E, decltype (void (magic_enum_define_range_adl (E{})))>
    : decltype (magic_enum_define_range_adl (E{})){};

static_assert (
    MAGIC_ENUM_RANGE_MAX > MAGIC_ENUM_RANGE_MIN,
    "MAGIC_ENUM_RANGE_MAX must be greater than MAGIC_ENUM_RANGE_MIN.");

namespace detail
{

enum class customize_tag
{
  default_tag,
  invalid_tag,
  custom_tag
};

} // namespace magic_enum::customize::detail

class customize_t : public std::pair<detail::customize_tag, string_view>
{
public:
  constexpr customize_t (string_view srt)
      : std::pair<detail::customize_tag, string_view>{
          detail::customize_tag::custom_tag, srt
        }
  {
  }
  constexpr customize_t (const char_type *srt)
      : customize_t{ string_view{ srt } }
  {
  }
  constexpr customize_t (detail::customize_tag tag)
      : std::pair<detail::customize_tag, string_view>{ tag, string_view{} }
  {
    MAGIC_ENUM_ASSERT (tag != detail::customize_tag::custom_tag);
  }
};

// Default customize.
inline constexpr auto default_tag
    = customize_t{ detail::customize_tag::default_tag };
// Invalid customize.
inline constexpr auto invalid_tag
    = customize_t{ detail::customize_tag::invalid_tag };

// If need custom names for enum, add specialization enum_name for necessary
// enum type.
template <typename E>
constexpr customize_t
enum_name (E) noexcept
{
  return default_tag;
}

// If need custom type name for enum, add specialization enum_type_name for
// necessary enum type.
template <typename E>
constexpr customize_t
enum_type_name () noexcept
{
  return default_tag;
}

} // namespace magic_enum::customize

namespace detail
{

template <typename T>
struct supported
#if defined(MAGIC_ENUM_SUPPORTED) || defined(MAGIC_ENUM_NO_CHECK_SUPPORT)
    : std::true_type
{
};
#else
    : std::false_type
{
};
#endif

template <auto V, typename E = std::decay_t<decltype (V)>,
          std::enable_if_t<std::is_enum_v<E>, int> = 0>
using enum_constant = std::integral_constant<E, V>;

template <typename... T> inline constexpr bool always_false_v = false;

template <typename T, typename = void> struct has_is_flags : std::false_type
{
};

template <typename T>
struct has_is_flags<T,
                    std::void_t<decltype (customize::enum_range<T>::is_flags)>>
    : std::bool_constant<std::is_same_v<
          bool, std::decay_t<decltype (customize::enum_range<T>::is_flags)>>>
{
};

template <typename T, typename = void>
struct range_min : std::integral_constant<int, MAGIC_ENUM_RANGE_MIN>
{
};

template <typename T>
struct range_min<T, std::void_t<decltype (customize::enum_range<T>::min)>>
    : std::integral_constant<decltype (customize::enum_range<T>::min),
                             customize::enum_range<T>::min>
{
};

template <typename T, typename = void>
struct range_max : std::integral_constant<int, MAGIC_ENUM_RANGE_MAX>
{
};

template <typename T>
struct range_max<T, std::void_t<decltype (customize::enum_range<T>::max)>>
    : std::integral_constant<decltype (customize::enum_range<T>::max),
                             customize::enum_range<T>::max>
{
};

struct str_view
{
  const char *str_ = nullptr;
  std::size_t size_ = 0;
};

template <std::uint16_t N> class static_str
{
public:
  constexpr explicit static_str (str_view str) noexcept
      : static_str{ str.str_, std::make_integer_sequence<std::uint16_t, N>{} }
  {
    MAGIC_ENUM_ASSERT (str.size_ == N);
  }

  constexpr explicit static_str (const char *const str) noexcept
      : static_str{ str, std::make_integer_sequence<std::uint16_t, N>{} }
  {
  }

  constexpr explicit static_str (string_view str) noexcept
      : static_str{ str.data (),
                    std::make_integer_sequence<std::uint16_t, N>{} }
  {
    MAGIC_ENUM_ASSERT (str.size () == N);
  }

  constexpr const char_type *
  data () const noexcept
  {
    return chars_;
  }

  constexpr std::uint16_t
  size () const noexcept
  {
    return N;
  }

  constexpr string_view
  str () const noexcept
  {
    return string_view (data (), size ());
  }

private:
  template <std::uint16_t... I>
  constexpr static_str (const char *str,
                        std::integer_sequence<std::uint16_t, I...>) noexcept
      : chars_{ static_cast<char_type> (str[I])...,
                static_cast<char_type> ('\0') }
  {
  }

  template <std::uint16_t... I>
  constexpr static_str (string_view str,
                        std::integer_sequence<std::uint16_t, I...>) noexcept
      : chars_{ str[I]..., static_cast<char_type> ('\0') }
  {
  }

  char_type chars_[static_cast<std::size_t> (N) + 1];
};

template <> class static_str<0>
{
public:
  constexpr static_str () noexcept = default;

  constexpr static_str (str_view) noexcept {}

  constexpr static_str (string_view) noexcept {}

  constexpr const char_type *
  data () const noexcept
  {
    return chars_;
  }

  constexpr std::uint16_t
  size () const noexcept
  {
    return 0;
  }

  constexpr string_view
  str () const noexcept
  {
    return string_view (data (), size ());
  }

private:
  static constexpr char_type chars_[1] = {};
};

template <typename Op = std::equal_to<>> class case_insensitive
{
  static constexpr char_type
  to_lower (char_type c) noexcept
  {
    return (c >= static_cast<char_type> ('A')
            && c <= static_cast<char_type> ('Z'))
               ? static_cast<char_type> (c
                                         + (static_cast<char_type> ('a')
                                            - static_cast<char_type> ('A')))
               : c;
  }

public:
  template <typename L, typename R>
  constexpr auto
  operator() (L lhs, R rhs) const noexcept
      -> std::enable_if_t<std::is_same_v<std::decay_t<L>, char_type>
                              && std::is_same_v<std::decay_t<R>, char_type>,
                          bool>
  {
    return Op{}(to_lower (lhs), to_lower (rhs));
  }
};

constexpr std::size_t
find (string_view str, char_type c) noexcept
{
#if defined(__clang__) && __clang_major__ < 9 && defined(__GLIBCXX__)         \
    || defined(_MSC_VER) && _MSC_VER < 1920 && !defined(__clang__)
  // https://stackoverflow.com/questions/56484834/constexpr-stdstring-viewfind-last-of-doesnt-work-on-clang-8-with-libstdc
  // https://developercommunity.visualstudio.com/content/problem/360432/vs20178-regression-c-failed-in-test.html
  constexpr bool workaround = true;
#else
  constexpr bool workaround = false;
#endif

  if constexpr (workaround)
    {
      for (std::size_t i = 0; i < str.size (); ++i)
        {
          if (str[i] == c)
            {
              return i;
            }
        }

      return string_view::npos;
    }
  else
    {
      return str.find (c);
    }
}

template <typename BinaryPredicate>
inline constexpr bool is_default_predicate_v
    = std::is_same_v<std::decay_t<BinaryPredicate>,
                     std::equal_to<string_view::value_type>>
      || std::is_same_v<std::decay_t<BinaryPredicate>, std::equal_to<>>;

template <typename BinaryPredicate>
inline constexpr bool is_nothrow_invocable_v
    = is_default_predicate_v<BinaryPredicate>
      || std::is_nothrow_invocable_r_v<bool, BinaryPredicate, char_type,
                                       char_type>;

template <typename BinaryPredicate>
constexpr bool
cmp_equal (string_view lhs, string_view rhs,
           [[maybe_unused]] BinaryPredicate
               &&p) noexcept (is_nothrow_invocable_v<BinaryPredicate>)
{
#if defined(_MSC_VER) && _MSC_VER < 1920 && !defined(__clang__)
  // https://developercommunity.visualstudio.com/content/problem/360432/vs20178-regression-c-failed-in-test.html
  // https://developercommunity.visualstudio.com/content/problem/232218/c-constexpr-string-view.html
  constexpr bool workaround = true;
#else
  constexpr bool workaround = false;
#endif

  if constexpr (!is_default_predicate_v<BinaryPredicate> || workaround)
    {
      if (lhs.size () != rhs.size ())
        {
          return false;
        }

      const auto size = lhs.size ();
      for (std::size_t i = 0; i < size; ++i)
        {
          if (!p (lhs[i], rhs[i]))
            {
              return false;
            }
        }

      return true;
    }
  else
    {
      return lhs == rhs;
    }
}

template <typename L, typename R>
constexpr bool
cmp_less (L lhs, R rhs) noexcept
{
  static_assert (std::is_integral_v<L> && std::is_integral_v<R>,
                 "magic_enum::detail::cmp_less requires integral type.");

  if constexpr (std::is_signed_v<L> == std::is_signed_v<R>)
    {
      // If same signedness (both signed or both unsigned).
      return lhs < rhs;
    }
  else if constexpr (std::is_same_v<L, bool>)
    { // bool special case
      return static_cast<R> (lhs) < rhs;
    }
  else if constexpr (std::is_same_v<R, bool>)
    { // bool special case
      return lhs < static_cast<L> (rhs);
    }
  else if constexpr (std::is_signed_v<R>)
    {
      // If 'right' is negative, then result is 'false', otherwise cast &
      // compare.
      return rhs > 0 && lhs < static_cast<std::make_unsigned_t<R>> (rhs);
    }
  else
    {
      // If 'left' is negative, then result is 'true', otherwise cast &
      // compare.
      return lhs < 0 || static_cast<std::make_unsigned_t<L>> (lhs) < rhs;
    }
}

template <typename I>
constexpr I
log2 (I value) noexcept
{
  static_assert (std::is_integral_v<I>,
                 "magic_enum::detail::log2 requires integral type.");

  if constexpr (std::is_same_v<I, bool>)
    { // bool special case
      return MAGIC_ENUM_ASSERT (false), value;
    }
  else
    {
      auto ret = I{ 0 };
      for (; value > I{ 1 }; value >>= I{ 1 }, ++ret)
        {
        }

      return ret;
    }
}

#if defined(__cpp_lib_array_constexpr) && __cpp_lib_array_constexpr >= 201603L
#define MAGIC_ENUM_ARRAY_CONSTEXPR 1
#else
template <typename T, std::size_t N, std::size_t... I>
constexpr std::array<std::remove_cv_t<T>, N>
to_array (T (&a)[N], std::index_sequence<I...>) noexcept
{
  return { { a[I]... } };
}
#endif

template <typename T>
inline constexpr bool is_enum_v
    = std::is_enum_v<T> && std::is_same_v<T, std::decay_t<T>>;

template <typename E>
constexpr auto MAGIC_ENUM_CALLING_CONVENTION
n () noexcept
{
  static_assert (is_enum_v<E>, "magic_enum::detail::n requires enum type.");

  if constexpr (supported<E>::value)
    {
#if defined(MAGIC_ENUM_GET_TYPE_NAME_BUILTIN)
      constexpr auto name_ptr = MAGIC_ENUM_GET_TYPE_NAME_BUILTIN (E);
      constexpr auto name
          = name_ptr ? str_view{ name_ptr,
                                 std::char_traits<char>::length (name_ptr) }
                     : str_view{};
#elif defined(__clang__)
      str_view name;
      if constexpr (sizeof (__PRETTY_FUNCTION__) == sizeof (__FUNCTION__))
        {
          static_assert (
              always_false_v<E>,
              "magic_enum::detail::n requires __PRETTY_FUNCTION__.");
          return str_view{};
        }
      else
        {
          name.size_ = sizeof (__PRETTY_FUNCTION__) - 36;
          name.str_ = __PRETTY_FUNCTION__ + 34;
        }
#elif defined(__GNUC__)
      auto name
          = str_view{ __PRETTY_FUNCTION__, sizeof (__PRETTY_FUNCTION__) - 1 };
      if constexpr (sizeof (__PRETTY_FUNCTION__) == sizeof (__FUNCTION__))
        {
          static_assert (
              always_false_v<E>,
              "magic_enum::detail::n requires __PRETTY_FUNCTION__.");
          return str_view{};
        }
      else if (name.str_[name.size_ - 1] == ']')
        {
          name.size_ -= 50;
          name.str_ += 49;
        }
      else
        {
          name.size_ -= 40;
          name.str_ += 37;
        }
#elif defined(_MSC_VER)
      // CLI/C++ workaround (see
      // https://github.com/Neargye/magic_enum/issues/284).
      str_view name;
      name.str_ = __FUNCSIG__;
      name.str_ += 40;
      name.size_ += sizeof (__FUNCSIG__) - 57;
#else
      auto name = str_view{};
#endif
      std::size_t p = 0;
      for (std::size_t i = name.size_; i > 0; --i)
        {
          if (name.str_[i] == ':')
            {
              p = i + 1;
              break;
            }
        }
      if (p > 0)
        {
          name.size_ -= p;
          name.str_ += p;
        }
      return name;
    }
  else
    {
      return str_view{}; // Unsupported compiler or Invalid customize.
    }
}

template <typename E>
constexpr auto
type_name () noexcept
{
  [[maybe_unused]] constexpr auto custom = customize::enum_type_name<E> ();
  static_assert (
      std::is_same_v<std::decay_t<decltype (custom)>, customize::customize_t>,
      "magic_enum::customize requires customize_t type.");
  if constexpr (custom.first == customize::detail::customize_tag::custom_tag)
    {
      constexpr auto name = custom.second;
      static_assert (!name.empty (),
                     "magic_enum::customize requires not empty string.");
      return static_str<name.size ()>{ name };
    }
  else if constexpr (custom.first
                     == customize::detail::customize_tag::invalid_tag)
    {
      return static_str<0>{};
    }
  else if constexpr (custom.first
                     == customize::detail::customize_tag::default_tag)
    {
      constexpr auto name = n<E> ();
      return static_str<name.size_>{ name };
    }
  else
    {
      static_assert (always_false_v<E>, "magic_enum::customize invalid.");
    }
}

template <typename E> inline constexpr auto type_name_v = type_name<E> ();

template <auto V>
constexpr auto MAGIC_ENUM_CALLING_CONVENTION
n () noexcept
{
  static_assert (is_enum_v<decltype (V)>,
                 "magic_enum::detail::n requires enum type.");

  if constexpr (supported<decltype (V)>::value)
    {
#if defined(MAGIC_ENUM_GET_ENUM_NAME_BUILTIN)
      constexpr auto name_ptr = MAGIC_ENUM_GET_ENUM_NAME_BUILTIN (V);
      auto name = name_ptr
                      ? str_view{ name_ptr,
                                  std::char_traits<char>::length (name_ptr) }
                      : str_view{};
#elif defined(__clang__)
      str_view name;
      if constexpr (sizeof (__PRETTY_FUNCTION__) == sizeof (__FUNCTION__))
        {
          static_assert (
              always_false_v<decltype (V)>,
              "magic_enum::detail::n requires __PRETTY_FUNCTION__.");
          return str_view{};
        }
      else
        {
          name.size_ = sizeof (__PRETTY_FUNCTION__) - 36;
          name.str_ = __PRETTY_FUNCTION__ + 34;
        }
      if (name.size_ > 22 && name.str_[0] == '(' && name.str_[1] == 'a'
          && name.str_[10] == ' ' && name.str_[22] == ':')
        {
          name.size_ -= 23;
          name.str_ += 23;
        }
      if (name.str_[0] == '(' || name.str_[0] == '-'
          || (name.str_[0] >= '0' && name.str_[0] <= '9'))
        {
          name = str_view{};
        }
#elif defined(__GNUC__)
      auto name
          = str_view{ __PRETTY_FUNCTION__, sizeof (__PRETTY_FUNCTION__) - 1 };
      if constexpr (sizeof (__PRETTY_FUNCTION__) == sizeof (__FUNCTION__))
        {
          static_assert (
              always_false_v<decltype (V)>,
              "magic_enum::detail::n requires __PRETTY_FUNCTION__.");
          return str_view{};
        }
      else if (name.str_[name.size_ - 1] == ']')
        {
          name.size_ -= 55;
          name.str_ += 54;
        }
      else
        {
          name.size_ -= 40;
          name.str_ += 37;
        }
      if (name.str_[0] == '(')
        {
          name = str_view{};
        }
#elif defined(_MSC_VER)
      str_view name;
      if ((__FUNCSIG__[5] == '_' && __FUNCSIG__[35] != '(')
          || (__FUNCSIG__[5] == 'c' && __FUNCSIG__[41] != '('))
        {
          // CLI/C++ workaround (see
          // https://github.com/Neargye/magic_enum/issues/284).
          name.str_ = __FUNCSIG__;
          name.str_ += 35;
          name.size_ = sizeof (__FUNCSIG__) - 52;
        }
#else
      auto name = str_view{};
#endif
      std::size_t p = 0;
      for (std::size_t i = name.size_; i > 0; --i)
        {
          if (name.str_[i] == ':')
            {
              p = i + 1;
              break;
            }
        }
      if (p > 0)
        {
          name.size_ -= p;
          name.str_ += p;
        }
      return name;
    }
  else
    {
      return str_view{}; // Unsupported compiler or Invalid customize.
    }
}

#if defined(_MSC_VER) && !defined(__clang__) && _MSC_VER < 1920
#define MAGIC_ENUM_VS_2017_WORKAROUND 1
#endif

#if defined(MAGIC_ENUM_VS_2017_WORKAROUND)
template <typename E, E V>
constexpr auto MAGIC_ENUM_CALLING_CONVENTION
n () noexcept
{
  static_assert (is_enum_v<E>, "magic_enum::detail::n requires enum type.");

#if defined(MAGIC_ENUM_GET_ENUM_NAME_BUILTIN)
  constexpr auto name_ptr = MAGIC_ENUM_GET_ENUM_NAME_BUILTIN (V);
  auto name = name_ptr ? str_view{ name_ptr,
                                   std::char_traits<char>::length (name_ptr) }
                       : str_view{};
#else
  // CLI/C++ workaround (see https://github.com/Neargye/magic_enum/issues/284).
  str_view name;
  name.str_ = __FUNCSIG__;
  name.size_ = sizeof (__FUNCSIG__) - 17;
  std::size_t p = 0;
  for (std::size_t i = name.size_; i > 0; --i)
    {
      if (name.str_[i] == ',' || name.str_[i] == ':')
        {
          p = i + 1;
          break;
        }
    }
  if (p > 0)
    {
      name.size_ -= p;
      name.str_ += p;
    }
  if (name.str_[0] == '(' || name.str_[0] == '-'
      || (name.str_[0] >= '0' && name.str_[0] <= '9'))
    {
      name = str_view{};
    }
  return name;
#endif
}
#endif

template <typename E, E V>
constexpr auto
enum_name () noexcept
{
  [[maybe_unused]] constexpr auto custom = customize::enum_name<E> (V);
  static_assert (
      std::is_same_v<std::decay_t<decltype (custom)>, customize::customize_t>,
      "magic_enum::customize requires customize_t type.");
  if constexpr (custom.first == customize::detail::customize_tag::custom_tag)
    {
      constexpr auto name = custom.second;
      static_assert (!name.empty (),
                     "magic_enum::customize requires not empty string.");
      return static_str<name.size ()>{ name };
    }
  else if constexpr (custom.first
                     == customize::detail::customize_tag::invalid_tag)
    {
      return static_str<0>{};
    }
  else if constexpr (custom.first
                     == customize::detail::customize_tag::default_tag)
    {
#if defined(MAGIC_ENUM_VS_2017_WORKAROUND)
      constexpr auto name = n<E, V> ();
#else
      constexpr auto name = n<V> ();
#endif
      return static_str<name.size_ - prefix_length_or_zero<E>>{
        name.str_ + prefix_length_or_zero<E>
      };
    }
  else
    {
      static_assert (always_false_v<E>, "magic_enum::customize invalid.");
    }
}

template <typename E, E V>
inline constexpr auto enum_name_v = enum_name<E, V> ();

// CWG1766: Values outside the range of the values of an enumeration
// https://reviews.llvm.org/D130058, https://reviews.llvm.org/D131307
#if defined(__clang__) && __clang_major__ >= 16
template <typename E, auto V, typename = void>
inline constexpr bool is_enum_constexpr_static_cast_valid = false;
template <typename E, auto V>
inline constexpr bool is_enum_constexpr_static_cast_valid<
    E, V, std::void_t<std::integral_constant<E, static_cast<E> (V)>>> = true;
#else
template <typename, auto>
inline constexpr bool is_enum_constexpr_static_cast_valid = true;
#endif

template <typename E, auto V>
constexpr bool
is_valid () noexcept
{
  if constexpr (is_enum_constexpr_static_cast_valid<E, V>)
    {
      constexpr E v = static_cast<E> (V);
      [[maybe_unused]] constexpr auto custom = customize::enum_name<E> (v);
      static_assert (std::is_same_v<std::decay_t<decltype (custom)>,
                                    customize::customize_t>,
                     "magic_enum::customize requires customize_t type.");
      if constexpr (custom.first
                    == customize::detail::customize_tag::custom_tag)
        {
          constexpr auto name = custom.second;
          static_assert (!name.empty (),
                         "magic_enum::customize requires not empty string.");
          return name.size () != 0;
        }
      else if constexpr (custom.first
                         == customize::detail::customize_tag::default_tag)
        {
#if defined(MAGIC_ENUM_VS_2017_WORKAROUND)
          return n<E, v> ().size_ != 0;
#else
          return n<v> ().size_ != 0;
#endif
        }
      else
        {
          return false;
        }
    }
  else
    {
      return false;
    }
}

enum class enum_subtype
{
  common,
  flags
};

template <typename E, int O, enum_subtype S,
          typename U = std::underlying_type_t<E>>
constexpr U
ualue (std::size_t i) noexcept
{
  if constexpr (std::is_same_v<U, bool>)
    { // bool special case
      static_assert (O == 0,
                     "magic_enum::detail::ualue requires valid offset.");

      return static_cast<U> (i);
    }
  else if constexpr (S == enum_subtype::flags)
    {
      return static_cast<U> (U{ 1 }
                             << static_cast<U> (static_cast<int> (i) + O));
    }
  else
    {
      return static_cast<U> (static_cast<int> (i) + O);
    }
}

template <typename E, int O, enum_subtype S,
          typename U = std::underlying_type_t<E>>
constexpr E
value (std::size_t i) noexcept
{
  return static_cast<E> (ualue<E, O, S> (i));
}

template <typename E, enum_subtype S, typename U = std::underlying_type_t<E>>
constexpr int
reflected_min () noexcept
{
  if constexpr (S == enum_subtype::flags)
    {
      return 0;
    }
  else
    {
      constexpr auto lhs = range_min<E>::value;
      constexpr auto rhs = (std::numeric_limits<U>::min) ();

      if constexpr (cmp_less (rhs, lhs))
        {
          return lhs;
        }
      else
        {
          return rhs;
        }
    }
}

template <typename E, enum_subtype S, typename U = std::underlying_type_t<E>>
constexpr int
reflected_max () noexcept
{
  if constexpr (S == enum_subtype::flags)
    {
      return std::numeric_limits<U>::digits - 1;
    }
  else
    {
      constexpr auto lhs = range_max<E>::value;
      constexpr auto rhs = (std::numeric_limits<U>::max) ();

      if constexpr (cmp_less (lhs, rhs))
        {
          return lhs;
        }
      else
        {
          return rhs;
        }
    }
}

#define MAGIC_ENUM_FOR_EACH_256(T)                                                                                                                                                                                                                                                                                                                                                                                                                                \
  T (0)                                                                                                                                                                                                                                                                                                                                                                                                                                                           \
  T (1) T (2) T (3) T (4) T (5) T (6) T (7) T (8) T (9) T (10) T (11) T (12)                                                                                                                                                                                                                                                                                                                                                                                      \
      T (13) T (14) T (15) T (16) T (17) T (18) T (19) T (20) T (21) T (                                                                                                                                                                                                                                                                                                                                                                                          \
          22) T (23) T (24) T (25) T (26) T (27) T (28) T (29) T (30) T (31)                                                                                                                                                                                                                                                                                                                                                                                      \
          T (32) T (33) T (34) T (35) T (36) T (37) T (38) T (39) T (40) T (                                                                                                                                                                                                                                                                                                                                                                                      \
              41) T (42) T (43) T (44) T (45) T (46) T (47) T (48) T (49)                                                                                                                                                                                                                                                                                                                                                                                         \
              T (50) T (51) T (52) T (53) T (54) T (55) T (56) T (57) T (58)                                                                                                                                                                                                                                                                                                                                                                                      \
                  T (59) T (60) T (61) T (62) T (63) T (64) T (65) T (66)                                                                                                                                                                                                                                                                                                                                                                                         \
                      T (67) T (68) T (69) T (70) T (71) T (72) T (73) T (74)                                                                                                                                                                                                                                                                                                                                                                                     \
                          T (75) T (76) T (77) T (78) T (79) T (80) T (81)                                                                                                                                                                                                                                                                                                                                                                                        \
                              T (82) T (83) T (84) T (85) T (86) T (87)                                                                                                                                                                                                                                                                                                                                                                                           \
                                  T (88) T (89) T (90) T (91)                                                                                                                                                                                                                                                                                                                                                                                                     \
                                      T (92) T (93) T (94) T (95) T (96) T (97) T (98) T (99) T (100) T (101) T (102) T (103) T (104) T (105) T (106) T (107) T (108) T (109) T (110) T (111) T (112) T (113) T (114) T (115) T (116) T (117) T (118) T (119) T (120) T (121) T (122) T (123) T (124) T (125) T (126) T (127) T (128) T (129) T (130) T (131) T (132) T (133) T (134) T (135) T (136) T (137) T (138) T (139) T (140) T (141) T (142) T (143) T ( \
                                          144) T (145)                                                                                                                                                                                                                                                                                                                                                                                                            \
                                          T (                                                                                                                                                                                                                                                                                                                                                                                                                     \
                                              146) T (147)                                                                                                                                                                                                                                                                                                                                                                                                        \
                                              T (                                                                                                                                                                                                                                                                                                                                                                                                                 \
                                                  148) T (149)                                                                                                                                                                                                                                                                                                                                                                                                    \
                                                  T (                                                                                                                                                                                                                                                                                                                                                                                                             \
                                                      150) T (151)                                                                                                                                                                                                                                                                                                                                                                                                \
                                                      T (                                                                                                                                                                                                                                                                                                                                                                                                         \
                                                          152) T (153)                                                                                                                                                                                                                                                                                                                                                                                            \
                                                          T (                                                                                                                                                                                                                                                                                                                                                                                                     \
                                                              154) T (155)                                                                                                                                                                                                                                                                                                                                                                                        \
                                                              T (                                                                                                                                                                                                                                                                                                                                                                                                 \
                                                                  156) T (157)                                                                                                                                                                                                                                                                                                                                                                                    \
                                                                  T (                                                                                                                                                                                                                                                                                                                                                                                             \
                                                                      158) T (159)                                                                                                                                                                                                                                                                                                                                                                                \
                                                                      T (                                                                                                                                                                                                                                                                                                                                                                                         \
                                                                          160) T (161)                                                                                                                                                                                                                                                                                                                                                                            \
                                                                          T (                                                                                                                                                                                                                                                                                                                                                                                     \
                                                                              162) T (163)                                                                                                                                                                                                                                                                                                                                                                        \
                                                                              T (                                                                                                                                                                                                                                                                                                                                                                                 \
                                                                                  164) T (165)                                                                                                                                                                                                                                                                                                                                                                    \
                                                                                  T (                                                                                                                                                                                                                                                                                                                                                                             \
                                                                                      166) T (167)                                                                                                                                                                                                                                                                                                                                                                \
                                                                                      T (                                                                                                                                                                                                                                                                                                                                                                         \
                                                                                          168) T (169)                                                                                                                                                                                                                                                                                                                                                            \
                                                                                          T (                                                                                                                                                                                                                                                                                                                                                                     \
                                                                                              170) T (171)                                                                                                                                                                                                                                                                                                                                                        \
                                                                                              T (                                                                                                                                                                                                                                                                                                                                                                 \
                                                                                                  172) T (173)                                                                                                                                                                                                                                                                                                                                                    \
                                                                                                  T (                                                                                                                                                                                                                                                                                                                                                             \
                                                                                                      174) T (175)                                                                                                                                                                                                                                                                                                                                                \
                                                                                                      T (                                                                                                                                                                                                                                                                                                                                                         \
                                                                                                          176) T (177)                                                                                                                                                                                                                                                                                                                                            \
                                                                                                          T (                                                                                                                                                                                                                                                                                                                                                     \
                                                                                                              178) T (179)                                                                                                                                                                                                                                                                                                                                        \
                                                                                                              T (180)                                                                                                                                                                                                                                                                                                                                             \
                                                                                                                  T (                                                                                                                                                                                                                                                                                                                                             \
                                                                                                                      181) T (182) T (183) T (184) T (185) T (186) T (187) T (188) T (189) T (190) T (191) T (192) T (193) T (194) T (195) T (196) T (197) T (198) T (199) T (200) T (201) T (202) T (203) T (204) T (205) T (206) T (207) T (208) T (209) T (210) T (211)                                                                                        \
                                                                                                                      T (212) T (                                                                                                                                                                                                                                                                                                                                 \
                                                                                                                          213)                                                                                                                                                                                                                                                                                                                                    \
                                                                                                                          T (214)                                                                                                                                                                                                                                                                                                                                 \
                                                                                                                              T (215)                                                                                                                                                                                                                                                                                                                             \
                                                                                                                                  T (216) T (217) T (218) T (219) T (220) T (221) T (222) T (223) T (224) T (225) T (226) T (227) T (228) T (229) T (230) T (231) T (232) T (233) T (234) T (235) T (236) T (237) T (238) T (239) T (240) T (241) T (242) T (243) T (244) T (                                                                                     \
                                                                                                                                      245) T (246)                                                                                                                                                                                                                                                                                                                \
                                                                                                                                      T (247) T (248) T (                                                                                                                                                                                                                                                                                                         \
                                                                                                                                          249) T (250)                                                                                                                                                                                                                                                                                                            \
                                                                                                                                          T (                                                                                                                                                                                                                                                                                                                     \
                                                                                                                                              251) T (252)                                                                                                                                                                                                                                                                                                        \
                                                                                                                                              T (253)                                                                                                                                                                                                                                                                                                             \
                                                                                                                                                  T (254)                                                                                                                                                                                                                                                                                                         \
                                                                                                                                                      T (255)

template <typename E, enum_subtype S, std::size_t Size, int Min, std::size_t I>
constexpr void
valid_count (bool *valid, std::size_t &count) noexcept
{
#define MAGIC_ENUM_V(O)                                                       \
  if constexpr ((I + O) < Size)                                               \
    {                                                                         \
      if constexpr (is_valid<E, ualue<E, Min, S> (I + O)> ())                 \
        {                                                                     \
          valid[I + O] = true;                                                \
          ++count;                                                            \
        }                                                                     \
    }

  MAGIC_ENUM_FOR_EACH_256 (MAGIC_ENUM_V)

  if constexpr ((I + 256) < Size)
    {
      valid_count<E, S, Size, Min, I + 256> (valid, count);
    }
#undef MAGIC_ENUM_V
}

template <std::size_t N> struct valid_count_t
{
  std::size_t count = 0;
  bool valid[N] = {};
};

template <typename E, enum_subtype S, std::size_t Size, int Min>
constexpr auto
valid_count () noexcept
{
  valid_count_t<Size> vc;
  valid_count<E, S, Size, Min, 0> (vc.valid, vc.count);
  return vc;
}

template <typename E, enum_subtype S, std::size_t Size, int Min>
constexpr auto
values () noexcept
{
  constexpr auto vc = valid_count<E, S, Size, Min> ();

  if constexpr (vc.count > 0)
    {
#if defined(MAGIC_ENUM_ARRAY_CONSTEXPR)
      std::array<E, vc.count> values = {};
#else
      E values[vc.count] = {};
#endif
      for (std::size_t i = 0, v = 0; v < vc.count; ++i)
        {
          if (vc.valid[i])
            {
              values[v++] = value<E, Min, S> (i);
            }
        }
#if defined(MAGIC_ENUM_ARRAY_CONSTEXPR)
      return values;
#else
      return to_array (values, std::make_index_sequence<vc.count>{});
#endif
    }
  else
    {
      return std::array<E, 0>{};
    }
}

template <typename E, enum_subtype S, typename U = std::underlying_type_t<E>>
constexpr auto
values () noexcept
{
  constexpr auto min = reflected_min<E, S> ();
  constexpr auto max = reflected_max<E, S> ();
  constexpr auto range_size = max - min + 1;
  static_assert (range_size > 0,
                 "magic_enum::enum_range requires valid size.");

  return values<E, S, range_size, min> ();
}

template <typename E, typename U = std::underlying_type_t<E>>
constexpr enum_subtype
subtype (std::true_type) noexcept
{
  if constexpr (std::is_same_v<U, bool>)
    { // bool special case
      return enum_subtype::common;
    }
  else if constexpr (has_is_flags<E>::value)
    {
      return customize::enum_range<E>::is_flags ? enum_subtype::flags
                                                : enum_subtype::common;
    }
  else
    {
      return enum_subtype::common;
    }
}

template <typename T>
constexpr enum_subtype
subtype (std::false_type) noexcept
{
  // For non-enum type return default common subtype.
  return enum_subtype::common;
}

template <typename E, typename D = std::decay_t<E>>
inline constexpr auto subtype_v = subtype<D> (std::is_enum<D>{});

template <typename E, enum_subtype S>
inline constexpr auto values_v = values<E, S> ();

template <typename E, enum_subtype S, typename D = std::decay_t<E>>
using values_t = decltype ((values_v<D, S>));

template <typename E, enum_subtype S>
inline constexpr auto count_v = values_v<E, S>.size ();

template <typename E, enum_subtype S, typename U = std::underlying_type_t<E>>
inline constexpr auto min_v
    = (count_v<E, S> > 0) ? static_cast<U> (values_v<E, S>.front ()) : U{ 0 };

template <typename E, enum_subtype S, typename U = std::underlying_type_t<E>>
inline constexpr auto max_v
    = (count_v<E, S> > 0) ? static_cast<U> (values_v<E, S>.back ()) : U{ 0 };

template <typename E, enum_subtype S, std::size_t... I>
constexpr auto
names (std::index_sequence<I...>) noexcept
{
  constexpr auto names = std::array<string_view, sizeof...(I)>{
    { enum_name_v<E, values_v<E, S>[I]>.str ()... }
  };
  return names;
}

template <typename E, enum_subtype S>
inline constexpr auto names_v
    = names<E, S> (std::make_index_sequence<count_v<E, S>>{});

template <typename E, enum_subtype S, typename D = std::decay_t<E>>
using names_t = decltype ((names_v<D, S>));

template <typename E, enum_subtype S, std::size_t... I>
constexpr auto
entries (std::index_sequence<I...>) noexcept
{
  constexpr auto entries = std::array<std::pair<E, string_view>, sizeof...(I)>{
    { { values_v<E, S>[I], enum_name_v<E, values_v<E, S>[I]>.str () }... }
  };
  return entries;
}

template <typename E, enum_subtype S>
inline constexpr auto entries_v
    = entries<E, S> (std::make_index_sequence<count_v<E, S>>{});

template <typename E, enum_subtype S, typename D = std::decay_t<E>>
using entries_t = decltype ((entries_v<D, S>));

template <typename E, enum_subtype S, typename U = std::underlying_type_t<E>>
constexpr bool
is_sparse () noexcept
{
  if constexpr (count_v<E, S> == 0)
    {
      return false;
    }
  else if constexpr (std::is_same_v<U, bool>)
    { // bool special case
      return false;
    }
  else
    {
      constexpr auto max
          = (S == enum_subtype::flags) ? log2 (max_v<E, S>) : max_v<E, S>;
      constexpr auto min
          = (S == enum_subtype::flags) ? log2 (min_v<E, S>) : min_v<E, S>;
      constexpr auto range_size = max - min + 1;

      return range_size != count_v<E, S>;
    }
}

template <typename E, enum_subtype S = subtype_v<E>>
inline constexpr bool is_sparse_v = is_sparse<E, S> ();

template <typename E, enum_subtype S>
struct is_reflected
#if defined(MAGIC_ENUM_NO_CHECK_REFLECTED_ENUM)
    : std::true_type
{
};
#else
    : std::bool_constant<std::is_enum_v<E> && (count_v<E, S> != 0)>
{
};
#endif

template <typename E, enum_subtype S>
inline constexpr bool is_reflected_v = is_reflected<std::decay_t<E>, S>{};

template <bool, typename R> struct enable_if_enum
{
};

template <typename R> struct enable_if_enum<true, R>
{
  using type = R;
  static_assert (
      supported<R>::value,
      "magic_enum unsupported compiler "
      "(https://github.com/Neargye/magic_enum#compiler-compatibility).");
};

template <typename T, typename R, typename BinaryPredicate = std::equal_to<>,
          typename D = std::decay_t<T>>
using enable_if_t = typename enable_if_enum<
    std::is_enum_v<D>
        && std::is_invocable_r_v<bool, BinaryPredicate, char_type, char_type>,
    R>::type;

template <typename T,
          std::enable_if_t<std::is_enum_v<std::decay_t<T>>, int> = 0>
using enum_concept = T;

template <typename T, bool = std::is_enum_v<T>>
struct is_scoped_enum : std::false_type
{
};

template <typename T>
struct is_scoped_enum<T, true>
    : std::bool_constant<!std::is_convertible_v<T, std::underlying_type_t<T>>>
{
};

template <typename T, bool = std::is_enum_v<T>>
struct is_unscoped_enum : std::false_type
{
};

template <typename T>
struct is_unscoped_enum<T, true>
    : std::bool_constant<std::is_convertible_v<T, std::underlying_type_t<T>>>
{
};

template <typename T, bool = std::is_enum_v<std::decay_t<T>>>
struct underlying_type
{
};

template <typename T>
struct underlying_type<T, true> : std::underlying_type<std::decay_t<T>>
{
};

#if defined(MAGIC_ENUM_ENABLE_HASH) || defined(MAGIC_ENUM_ENABLE_HASH_SWITCH)

template <typename Value, typename = void> struct constexpr_hash_t;

template <typename Value>
struct constexpr_hash_t<Value, std::enable_if_t<is_enum_v<Value>>>
{
  constexpr auto
  operator() (Value value) const noexcept
  {
    using U = typename underlying_type<Value>::type;
    if constexpr (std::is_same_v<U, bool>)
      { // bool special case
        return static_cast<std::size_t> (value);
      }
    else
      {
        return static_cast<U> (value);
      }
  }
  using secondary_hash = constexpr_hash_t;
};

template <typename Value>
struct constexpr_hash_t<Value,
                        std::enable_if_t<std::is_same_v<Value, string_view>>>
{
  static constexpr std::uint32_t crc_table[256]{
    0x00000000L, 0x77073096L, 0xee0e612cL, 0x990951baL, 0x076dc419L,
    0x706af48fL, 0xe963a535L, 0x9e6495a3L, 0x0edb8832L, 0x79dcb8a4L,
    0xe0d5e91eL, 0x97d2d988L, 0x09b64c2bL, 0x7eb17cbdL, 0xe7b82d07L,
    0x90bf1d91L, 0x1db71064L, 0x6ab020f2L, 0xf3b97148L, 0x84be41deL,
    0x1adad47dL, 0x6ddde4ebL, 0xf4d4b551L, 0x83d385c7L, 0x136c9856L,
    0x646ba8c0L, 0xfd62f97aL, 0x8a65c9ecL, 0x14015c4fL, 0x63066cd9L,
    0xfa0f3d63L, 0x8d080df5L, 0x3b6e20c8L, 0x4c69105eL, 0xd56041e4L,
    0xa2677172L, 0x3c03e4d1L, 0x4b04d447L, 0xd20d85fdL, 0xa50ab56bL,
    0x35b5a8faL, 0x42b2986cL, 0xdbbbc9d6L, 0xacbcf940L, 0x32d86ce3L,
    0x45df5c75L, 0xdcd60dcfL, 0xabd13d59L, 0x26d930acL, 0x51de003aL,
    0xc8d75180L, 0xbfd06116L, 0x21b4f4b5L, 0x56b3c423L, 0xcfba9599L,
    0xb8bda50fL, 0x2802b89eL, 0x5f058808L, 0xc60cd9b2L, 0xb10be924L,
    0x2f6f7c87L, 0x58684c11L, 0xc1611dabL, 0xb6662d3dL, 0x76dc4190L,
    0x01db7106L, 0x98d220bcL, 0xefd5102aL, 0x71b18589L, 0x06b6b51fL,
    0x9fbfe4a5L, 0xe8b8d433L, 0x7807c9a2L, 0x0f00f934L, 0x9609a88eL,
    0xe10e9818L, 0x7f6a0dbbL, 0x086d3d2dL, 0x91646c97L, 0xe6635c01L,
    0x6b6b51f4L, 0x1c6c6162L, 0x856530d8L, 0xf262004eL, 0x6c0695edL,
    0x1b01a57bL, 0x8208f4c1L, 0xf50fc457L, 0x65b0d9c6L, 0x12b7e950L,
    0x8bbeb8eaL, 0xfcb9887cL, 0x62dd1ddfL, 0x15da2d49L, 0x8cd37cf3L,
    0xfbd44c65L, 0x4db26158L, 0x3ab551ceL, 0xa3bc0074L, 0xd4bb30e2L,
    0x4adfa541L, 0x3dd895d7L, 0xa4d1c46dL, 0xd3d6f4fbL, 0x4369e96aL,
    0x346ed9fcL, 0xad678846L, 0xda60b8d0L, 0x44042d73L, 0x33031de5L,
    0xaa0a4c5fL, 0xdd0d7cc9L, 0x5005713cL, 0x270241aaL, 0xbe0b1010L,
    0xc90c2086L, 0x5768b525L, 0x206f85b3L, 0xb966d409L, 0xce61e49fL,
    0x5edef90eL, 0x29d9c998L, 0xb0d09822L, 0xc7d7a8b4L, 0x59b33d17L,
    0x2eb40d81L, 0xb7bd5c3bL, 0xc0ba6cadL, 0xedb88320L, 0x9abfb3b6L,
    0x03b6e20cL, 0x74b1d29aL, 0xead54739L, 0x9dd277afL, 0x04db2615L,
    0x73dc1683L, 0xe3630b12L, 0x94643b84L, 0x0d6d6a3eL, 0x7a6a5aa8L,
    0xe40ecf0bL, 0x9309ff9dL, 0x0a00ae27L, 0x7d079eb1L, 0xf00f9344L,
    0x8708a3d2L, 0x1e01f268L, 0x6906c2feL, 0xf762575dL, 0x806567cbL,
    0x196c3671L, 0x6e6b06e7L, 0xfed41b76L, 0x89d32be0L, 0x10da7a5aL,
    0x67dd4accL, 0xf9b9df6fL, 0x8ebeeff9L, 0x17b7be43L, 0x60b08ed5L,
    0xd6d6a3e8L, 0xa1d1937eL, 0x38d8c2c4L, 0x4fdff252L, 0xd1bb67f1L,
    0xa6bc5767L, 0x3fb506ddL, 0x48b2364bL, 0xd80d2bdaL, 0xaf0a1b4cL,
    0x36034af6L, 0x41047a60L, 0xdf60efc3L, 0xa867df55L, 0x316e8eefL,
    0x4669be79L, 0xcb61b38cL, 0xbc66831aL, 0x256fd2a0L, 0x5268e236L,
    0xcc0c7795L, 0xbb0b4703L, 0x220216b9L, 0x5505262fL, 0xc5ba3bbeL,
    0xb2bd0b28L, 0x2bb45a92L, 0x5cb36a04L, 0xc2d7ffa7L, 0xb5d0cf31L,
    0x2cd99e8bL, 0x5bdeae1dL, 0x9b64c2b0L, 0xec63f226L, 0x756aa39cL,
    0x026d930aL, 0x9c0906a9L, 0xeb0e363fL, 0x72076785L, 0x05005713L,
    0x95bf4a82L, 0xe2b87a14L, 0x7bb12baeL, 0x0cb61b38L, 0x92d28e9bL,
    0xe5d5be0dL, 0x7cdcefb7L, 0x0bdbdf21L, 0x86d3d2d4L, 0xf1d4e242L,
    0x68ddb3f8L, 0x1fda836eL, 0x81be16cdL, 0xf6b9265bL, 0x6fb077e1L,
    0x18b74777L, 0x88085ae6L, 0xff0f6a70L, 0x66063bcaL, 0x11010b5cL,
    0x8f659effL, 0xf862ae69L, 0x616bffd3L, 0x166ccf45L, 0xa00ae278L,
    0xd70dd2eeL, 0x4e048354L, 0x3903b3c2L, 0xa7672661L, 0xd06016f7L,
    0x4969474dL, 0x3e6e77dbL, 0xaed16a4aL, 0xd9d65adcL, 0x40df0b66L,
    0x37d83bf0L, 0xa9bcae53L, 0xdebb9ec5L, 0x47b2cf7fL, 0x30b5ffe9L,
    0xbdbdf21cL, 0xcabac28aL, 0x53b39330L, 0x24b4a3a6L, 0xbad03605L,
    0xcdd70693L, 0x54de5729L, 0x23d967bfL, 0xb3667a2eL, 0xc4614ab8L,
    0x5d681b02L, 0x2a6f2b94L, 0xb40bbe37L, 0xc30c8ea1L, 0x5a05df1bL,
    0x2d02ef8dL
  };
  constexpr std::uint32_t
  operator() (string_view value) const noexcept
  {
    auto crc = static_cast<std::uint32_t> (0xffffffffL);
    for (const auto c : value)
      {
        crc = (crc >> 8)
              ^ crc_table[(crc ^ static_cast<std::uint32_t> (c)) & 0xff];
      }
    return crc ^ 0xffffffffL;
  }

  struct secondary_hash
  {
    constexpr std::uint32_t
    operator() (string_view value) const noexcept
    {
      auto acc = static_cast<std::uint64_t> (2166136261ULL);
      for (const auto c : value)
        {
          acc = ((acc ^ static_cast<std::uint64_t> (c))
                 * static_cast<std::uint64_t> (16777619ULL))
                & (std::numeric_limits<std::uint32_t>::max) ();
        }
      return static_cast<std::uint32_t> (acc);
    }
  };
};

template <typename Hash> inline constexpr Hash hash_v{};

template <auto *GlobValues, typename Hash>
constexpr auto
calculate_cases (std::size_t Page) noexcept
{
  constexpr std::array values = *GlobValues;
  constexpr std::size_t size = values.size ();

  using switch_t
      = std::invoke_result_t<Hash, typename decltype (values)::value_type>;
  static_assert (std::is_integral_v<switch_t>
                 && !std::is_same_v<switch_t, bool>);
  const std::size_t values_to
      = (std::min)(static_cast<std::size_t> (256), size - Page);

  std::array<switch_t, 256> result{};
  auto fill = result.begin ();
  {
    auto first = values.begin () + static_cast<std::ptrdiff_t> (Page);
    auto last
        = values.begin () + static_cast<std::ptrdiff_t> (Page + values_to);
    while (first != last)
      {
        *fill++ = hash_v<Hash> (*first++);
      }
  }

  // dead cases, try to avoid case collisions
  for (switch_t last_value = result[values_to - 1];
       fill != result.end ()
       && last_value != (std::numeric_limits<switch_t>::max) ();
       *fill++ = ++last_value)
    {
    }

  {
    auto it = result.begin ();
    auto last_value = (std::numeric_limits<switch_t>::min) ();
    for (; fill != result.end (); *fill++ = last_value++)
      {
        while (last_value == *it)
          {
            ++last_value, ++it;
          }
      }
  }

  return result;
}

template <typename R, typename F, typename... Args>
constexpr R
invoke_r (F &&f, Args &&...args) noexcept (
    std::is_nothrow_invocable_r_v<R, F, Args...>)
{
  if constexpr (std::is_void_v<R>)
    {
      std::forward<F> (f) (std::forward<Args> (args)...);
    }
  else
    {
      return static_cast<R> (
          std::forward<F> (f) (std::forward<Args> (args)...));
    }
}

enum class case_call_t
{
  index,
  value
};

template <typename T = void>
inline constexpr auto default_result_type_lambda
    = [] () noexcept (std::is_nothrow_default_constructible_v<T>)
  { return T{}; };

template <>
inline constexpr auto default_result_type_lambda<void> = [] () noexcept {};

template <auto *Arr, typename Hash>
constexpr bool
has_duplicate () noexcept
{
  using value_t = std::decay_t<decltype ((*Arr)[0])>;
  using hash_value_t = std::invoke_result_t<Hash, value_t>;
  std::array<hash_value_t, Arr->size ()> hashes{};
  std::size_t size = 0;
  for (auto elem : *Arr)
    {
      hashes[size] = hash_v<Hash> (elem);
      for (auto i = size++; i > 0; --i)
        {
          if (hashes[i] < hashes[i - 1])
            {
              auto tmp = hashes[i];
              hashes[i] = hashes[i - 1];
              hashes[i - 1] = tmp;
            }
          else if (hashes[i] == hashes[i - 1])
            {
              return false;
            }
          else
            {
              break;
            }
        }
    }
  return true;
}

#define MAGIC_ENUM_CASE(val)                                                  \
  case cases[val]:                                                            \
    if constexpr ((val) + Page < size)                                        \
      {                                                                       \
        if (!pred (values[val + Page], searched))                             \
          {                                                                   \
            break;                                                            \
          }                                                                   \
        if constexpr (CallValue == case_call_t::index)                        \
          {                                                                   \
            if constexpr (std::is_invocable_r_v<                              \
                              result_t, Lambda,                               \
                              std::integral_constant<std::size_t,             \
                                                     val + Page>>)            \
              {                                                               \
                return detail::invoke_r<result_t> (                           \
                    std::forward<Lambda> (lambda),                            \
                    std::integral_constant<std::size_t, val + Page>{});       \
              }                                                               \
            else if constexpr (std::is_invocable_v<                           \
                                   Lambda, std::integral_constant<            \
                                               std::size_t, val + Page>>)     \
              {                                                               \
                MAGIC_ENUM_ASSERT (false                                      \
                                   && "magic_enum::detail::constexpr_switch " \
                                      "wrong result type.");                  \
              }                                                               \
          }                                                                   \
        else if constexpr (CallValue == case_call_t::value)                   \
          {                                                                   \
            if constexpr (std::is_invocable_r_v<                              \
                              result_t, Lambda,                               \
                              enum_constant<values[val + Page]>>)             \
              {                                                               \
                return detail::invoke_r<result_t> (                           \
                    std::forward<Lambda> (lambda),                            \
                    enum_constant<values[val + Page]>{});                     \
              }                                                               \
            else if constexpr (std::is_invocable_r_v<                         \
                                   result_t, Lambda,                          \
                                   enum_constant<values[val + Page]>>)        \
              {                                                               \
                MAGIC_ENUM_ASSERT (false                                      \
                                   && "magic_enum::detail::constexpr_switch " \
                                      "wrong result type.");                  \
              }                                                               \
          }                                                                   \
        break;                                                                \
      }                                                                       \
    else                                                                      \
      [[fallthrough]];

template <auto *GlobValues, case_call_t CallValue, std::size_t Page = 0,
          typename Hash = constexpr_hash_t<
              typename std::decay_t<decltype (*GlobValues)>::value_type>,
          typename BinaryPredicate = std::equal_to<>, typename Lambda,
          typename ResultGetterType>
constexpr decltype (auto)
constexpr_switch (
    Lambda &&lambda,
    typename std::decay_t<decltype (*GlobValues)>::value_type searched,
    ResultGetterType &&def, BinaryPredicate &&pred = {})
{
  using result_t = std::invoke_result_t<ResultGetterType>;
  using hash_t = std::conditional_t<has_duplicate<GlobValues, Hash> (), Hash,
                                    typename Hash::secondary_hash>;
  static_assert (
      has_duplicate<GlobValues, hash_t> (),
      "magic_enum::detail::constexpr_switch duplicated hash found, please "
      "report it: https://github.com/Neargye/magic_enum/issues.");
  constexpr std::array values = *GlobValues;
  constexpr std::size_t size = values.size ();
  constexpr std::array cases = calculate_cases<GlobValues, hash_t> (Page);

  switch (hash_v<hash_t> (searched))
    {
      MAGIC_ENUM_FOR_EACH_256 (MAGIC_ENUM_CASE)
    default:
      if constexpr (size > 256 + Page)
        {
          return constexpr_switch<GlobValues, CallValue, Page + 256, Hash> (
              std::forward<Lambda> (lambda), searched,
              std::forward<ResultGetterType> (def));
        }
      break;
    }
  return def ();
}

#undef MAGIC_ENUM_CASE

#endif

} // namespace magic_enum::detail

// Checks is magic_enum supported compiler.
inline constexpr bool is_magic_enum_supported = detail::supported<void>::value;

template <typename T> using Enum = detail::enum_concept<T>;

// Returns true if T is an Unscoped enumeration type.
// Provides the member constant value which is equal to true, if T is an
// [Unscoped
// enumeration](https://en.cppreference.com/w/cpp/language/enum#Unscoped_enumeration)
// type. Otherwise, value is equal to false.
template <typename T> struct is_unscoped_enum : detail::is_unscoped_enum<T>
{
};

template <typename T>
inline constexpr bool is_unscoped_enum_v = is_unscoped_enum<T>::value;

// Returns true if T is a Scoped enumeration type.
// Provides the member constant value which is equal to true, if T is an
// [Scoped
// enumeration](https://en.cppreference.com/w/cpp/language/enum#Scoped_enumerations)
// type. Otherwise, value is equal to false.
template <typename T> struct is_scoped_enum : detail::is_scoped_enum<T>
{
};

template <typename T>
inline constexpr bool is_scoped_enum_v = is_scoped_enum<T>::value;

// If T is a complete enumeration type, provides a member typedef type that
// names the underlying type of T. Otherwise, if T is not an enumeration type,
// there is no member type. Otherwise (T is an incomplete enumeration type),
// the program is ill-formed.
template <typename T> struct underlying_type : detail::underlying_type<T>
{
};

template <typename T>
using underlying_type_t = typename underlying_type<T>::type;

template <auto V> using enum_constant = detail::enum_constant<V>;

// Returns type name of enum.
template <typename E>
[[nodiscard]] constexpr auto
enum_type_name () noexcept -> detail::enable_if_t<E, string_view>
{
  constexpr string_view name = detail::type_name_v<std::decay_t<E>>.str ();
  static_assert (!name.empty (),
                 "magic_enum::enum_type_name enum type does not have a name.");

  return name;
}

// Returns number of enum values.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_count () noexcept -> detail::enable_if_t<E, std::size_t>
{
  return detail::count_v<std::decay_t<E>, S>;
}

// Returns enum value at specified index.
// No bounds checking is performed: the behavior is undefined if index >=
// number of enum values.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_value (std::size_t index) noexcept
    -> detail::enable_if_t<E, std::decay_t<E>>
{
  using D = std::decay_t<E>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  if constexpr (detail::is_sparse_v<D, S>)
    {
      return MAGIC_ENUM_ASSERT (index < detail::count_v<D, S>),
             detail::values_v<D, S>[index];
    }
  else
    {
      constexpr auto min = (S == detail::enum_subtype::flags)
                               ? detail::log2 (detail::min_v<D, S>)
                               : detail::min_v<D, S>;

      return MAGIC_ENUM_ASSERT (index < detail::count_v<D, S>),
             detail::value<D, min, S> (index);
    }
}

// Returns enum value at specified index.
template <typename E, std::size_t I,
          detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_value () noexcept -> detail::enable_if_t<E, std::decay_t<E>>
{
  using D = std::decay_t<E>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");
  static_assert (I < detail::count_v<D, S>,
                 "magic_enum::enum_value out of range.");

  return enum_value<D, S> (I);
}

// Returns std::array with enum values, sorted by enum value.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_values () noexcept -> detail::enable_if_t<E, detail::values_t<E, S>>
{
  using D = std::decay_t<E>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  return detail::values_v<D, S>;
}

// Returns integer value from enum value.
template <typename E>
[[nodiscard]] constexpr auto
enum_integer (E value) noexcept -> underlying_type_t<E>
{
  return static_cast<underlying_type_t<E>> (value);
}

// Returns underlying value from enum value.
template <typename E>
[[nodiscard]] constexpr auto
enum_underlying (E value) noexcept -> underlying_type_t<E>
{
  return static_cast<underlying_type_t<E>> (value);
}

// Returns index in enum values from enum value.
// Returns optional with index.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_index (E value) noexcept -> detail::enable_if_t<E, optional<std::size_t>>
{
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  if constexpr (detail::is_sparse_v<D, S>
                || (S == detail::enum_subtype::flags))
    {
#if defined(MAGIC_ENUM_ENABLE_HASH)
      return detail::constexpr_switch<&detail::values_v<D, S>,
                                      detail::case_call_t::index> (
          [] (std::size_t i) { return optional<std::size_t>{ i }; }, value,
          detail::default_result_type_lambda<optional<std::size_t>>);
#else
      for (std::size_t i = 0; i < detail::count_v<D, S>; ++i)
        {
          if (enum_value<D, S> (i) == value)
            {
              return i;
            }
        }
      return {}; // Invalid value or out of range.
#endif
    }
  else
    {
      const auto v = static_cast<U> (value);
      if (v >= detail::min_v<D, S> && v <= detail::max_v<D, S>)
        {
          return static_cast<std::size_t> (v - detail::min_v<D, S>);
        }
      return {}; // Invalid value or out of range.
    }
}

// Returns index in enum values from enum value.
// Returns optional with index.
template <detail::enum_subtype S, typename E>
[[nodiscard]] constexpr auto
enum_index (E value) noexcept -> detail::enable_if_t<E, optional<std::size_t>>
{
  using D = std::decay_t<E>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  return enum_index<D, S> (value);
}

// Returns index in enum values from static storage enum variable.
template <auto V, detail::enum_subtype S
                  = detail::subtype_v<std::decay_t<decltype (V)>>>
[[nodiscard]] constexpr auto
enum_index () noexcept -> detail::enable_if_t<decltype (V), std::size_t>
{
  using D = std::decay_t<decltype (V)>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");
  constexpr auto index = enum_index<D, S> (V);
  static_assert (index,
                 "magic_enum::enum_index enum value does not have a index.");

  return *index;
}

// Returns name from static storage enum variable.
// This version is much lighter on the compile times and is not restricted to
// the enum_range limitation.
template <auto V>
[[nodiscard]] constexpr auto
enum_name () noexcept -> detail::enable_if_t<decltype (V), string_view>
{
  constexpr string_view name
      = detail::enum_name_v<std::decay_t<decltype (V)>, V>.str ();
  static_assert (!name.empty (),
                 "magic_enum::enum_name enum value does not have a name.");

  return name;
}

// Returns name from enum value.
// If enum value does not have name or value out of range, returns empty
// string.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_name (E value) noexcept -> detail::enable_if_t<E, string_view>
{
  using D = std::decay_t<E>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  if (const auto i = enum_index<D, S> (value))
    {
      return detail::names_v<D, S>[*i];
    }
  return detail::static_str<0>{}.str ();
}

// Returns name from enum value.
// If enum value does not have name or value out of range, returns empty
// string.
template <detail::enum_subtype S, typename E>
[[nodiscard]] constexpr auto
enum_name (E value) -> detail::enable_if_t<E, string_view>
{
  using D = std::decay_t<E>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  return enum_name<D, S> (value);
}

// Returns std::array with names, sorted by enum value.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_names () noexcept -> detail::enable_if_t<E, detail::names_t<E, S>>
{
  using D = std::decay_t<E>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  return detail::names_v<D, S>;
}

// Returns std::array with pairs (value, name), sorted by enum value.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_entries () noexcept -> detail::enable_if_t<E, detail::entries_t<E, S>>
{
  using D = std::decay_t<E>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  return detail::entries_v<D, S>;
}

// Allows you to write magic_enum::enum_cast<foo>("bar",
// magic_enum::case_insensitive);
inline constexpr auto case_insensitive = detail::case_insensitive<>{};

// Returns enum value from integer value.
// Returns optional with enum value.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_cast (underlying_type_t<E> value) noexcept
    -> detail::enable_if_t<E, optional<std::decay_t<E>>>
{
  using D = std::decay_t<E>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  if constexpr (detail::is_sparse_v<D, S>
                || (S == detail::enum_subtype::flags))
    {
#if defined(MAGIC_ENUM_ENABLE_HASH)
      return detail::constexpr_switch<&detail::values_v<D, S>,
                                      detail::case_call_t::value> (
          [] (D v) { return optional<D>{ v }; }, static_cast<D> (value),
          detail::default_result_type_lambda<optional<D>>);
#else
      for (std::size_t i = 0; i < detail::count_v<D, S>; ++i)
        {
          if (value
              == static_cast<underlying_type_t<D>> (enum_value<D, S> (i)))
            {
              return static_cast<D> (value);
            }
        }
      return {}; // Invalid value or out of range.
#endif
    }
  else
    {
      if (value >= detail::min_v<D, S> && value <= detail::max_v<D, S>)
        {
          return static_cast<D> (value);
        }
      return {}; // Invalid value or out of range.
    }
}

// Returns enum value from name.
// Returns optional with enum value.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>,
          typename BinaryPredicate = std::equal_to<>>
[[nodiscard]] constexpr auto
enum_cast (string_view value,
           [[maybe_unused]] BinaryPredicate p
           = {}) noexcept (detail::is_nothrow_invocable_v<BinaryPredicate>)
    -> detail::enable_if_t<E, optional<std::decay_t<E>>, BinaryPredicate>
{
  using D = std::decay_t<E>;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

#if defined(MAGIC_ENUM_ENABLE_HASH)
  if constexpr (detail::is_default_predicate_v<BinaryPredicate>)
    {
      return detail::constexpr_switch<&detail::names_v<D, S>,
                                      detail::case_call_t::index> (
          [] (std::size_t i)
            { return optional<D>{ detail::values_v<D, S>[i] }; }, value,
          detail::default_result_type_lambda<optional<D>>,
          [&p] (string_view lhs, string_view rhs)
            { return detail::cmp_equal (lhs, rhs, p); });
    }
#endif
  for (std::size_t i = 0; i < detail::count_v<D, S>; ++i)
    {
      if (detail::cmp_equal (value, detail::names_v<D, S>[i], p))
        {
          return enum_value<D, S> (i);
        }
    }
  return {}; // Invalid value or out of range.
}

// Returns true if enum contains value with such value.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_contains (E value) noexcept -> detail::enable_if_t<E, bool>
{
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;

  return static_cast<bool> (enum_cast<D, S> (static_cast<U> (value)));
}

// Returns true if enum contains value with such value.
template <detail::enum_subtype S, typename E>
[[nodiscard]] constexpr auto
enum_contains (E value) noexcept -> detail::enable_if_t<E, bool>
{
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;

  return static_cast<bool> (enum_cast<D, S> (static_cast<U> (value)));
}

// Returns true if enum contains value with such integer value.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_contains (underlying_type_t<E> value) noexcept
    -> detail::enable_if_t<E, bool>
{
  using D = std::decay_t<E>;

  return static_cast<bool> (enum_cast<D, S> (value));
}

// Returns true if enum contains enumerator with such name.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>,
          typename BinaryPredicate = std::equal_to<>>
[[nodiscard]] constexpr auto
enum_contains (string_view value,
               BinaryPredicate p
               = {}) noexcept (detail::is_nothrow_invocable_v<BinaryPredicate>)
    -> detail::enable_if_t<E, bool, BinaryPredicate>
{
  using D = std::decay_t<E>;

  return static_cast<bool> (enum_cast<D, S> (value, std::move (p)));
}

// Returns true if the enum integer value is in the range of values that can be
// reflected.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_reflected (underlying_type_t<E> value) noexcept
    -> detail::enable_if_t<E, bool>
{
  using D = std::decay_t<E>;

  if constexpr (detail::is_reflected_v<D, S>)
    {
      constexpr auto min = detail::reflected_min<E, S> ();
      constexpr auto max = detail::reflected_max<E, S> ();
      return value >= min && value <= max;
    }
  else
    {
      return false;
    }
}

// Returns true if the enum value is in the range of values that can be
// reflected.
template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_reflected (E value) noexcept -> detail::enable_if_t<E, bool>
{
  using D = std::decay_t<E>;

  return enum_reflected<D, S> (static_cast<underlying_type_t<D>> (value));
}

// Returns true if the enum value is in the range of values that can be
// reflected.
template <detail::enum_subtype S, typename E>
[[nodiscard]] constexpr auto
enum_reflected (E value) noexcept -> detail::enable_if_t<E, bool>
{
  using D = std::decay_t<E>;

  return enum_reflected<D, S> (value);
}

template <bool AsFlags = true>
inline constexpr auto as_flags
    = AsFlags ? detail::enum_subtype::flags : detail::enum_subtype::common;

template <bool AsCommon = true>
inline constexpr auto as_common
    = AsCommon ? detail::enum_subtype::common : detail::enum_subtype::flags;

namespace bitwise_operators
{

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E
operator~(E rhs) noexcept
{
  return static_cast<E> (~static_cast<underlying_type_t<E>> (rhs));
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E
operator| (E lhs, E rhs) noexcept
{
  return static_cast<E> (static_cast<underlying_type_t<E>> (lhs)
                         | static_cast<underlying_type_t<E>> (rhs));
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E
operator& (E lhs, E rhs) noexcept
{
  return static_cast<E> (static_cast<underlying_type_t<E>> (lhs)
                         & static_cast<underlying_type_t<E>> (rhs));
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E
operator^ (E lhs, E rhs) noexcept
{
  return static_cast<E> (static_cast<underlying_type_t<E>> (lhs)
                         ^ static_cast<underlying_type_t<E>> (rhs));
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E &
operator|= (E &lhs, E rhs) noexcept
{
  return lhs = (lhs | rhs);
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E &
operator&= (E &lhs, E rhs) noexcept
{
  return lhs = (lhs & rhs);
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E &
operator^= (E &lhs, E rhs) noexcept
{
  return lhs = (lhs ^ rhs);
}

} // namespace magic_enum::bitwise_operators

} // namespace magic_enum

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#undef MAGIC_ENUM_GET_ENUM_NAME_BUILTIN
#undef MAGIC_ENUM_GET_TYPE_NAME_BUILTIN
#undef MAGIC_ENUM_VS_2017_WORKAROUND
#undef MAGIC_ENUM_ARRAY_CONSTEXPR
#undef MAGIC_ENUM_FOR_EACH_256

#endif // NEARGYE_MAGIC_ENUM_HPP
// Embedded upstream header: magic_enum_containers.hpp
//  __  __             _        ______                          _____
// |  \/  |           (_)      |  ____|                        / ____|_     _
// | \  / | __ _  __ _ _  ___  | |__   _ __  _   _ _ __ ___   | |   _| |_ _| |_
// | |\/| |/ _` |/ _` | |/ __| |  __| | '_ \| | | | '_ ` _ \  | |  |_   _|_ _|
// | |  | | (_| | (_| | | (__  | |____| | | | |_| | | | | | | | |____|_|   |_|
// |_|  |_|\__,_|\__, |_|\___| |______|_| |_|\__,_|_| |_| |_|  \_____|
//                __/ | https://github.com/Neargye/magic_enum
//               |___/  version 0.9.8
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2019 - 2024 Daniil Goncharov <neargye@gmail.com>.
// Copyright (c) 2022 - 2023 Bela Schaum <schaumb@gmail.com>.
//
// Permission is hereby  granted, free of charge, to any  person obtaining a
// copy of this software and associated  documentation files (the "Software"),
// to deal in the Software  without restriction, including without  limitation
// the rights to  use, copy,  modify, merge,  publish, distribute,  sublicense,
// and/or  sell copies  of  the Software,  and  to  permit persons  to  whom
// the Software  is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS
// OR IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN
// NO EVENT  SHALL THE AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY
// CLAIM,  DAMAGES OR  OTHER LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef NEARGYE_MAGIC_ENUM_CONTAINERS_HPP
#define NEARGYE_MAGIC_ENUM_CONTAINERS_HPP

// magic_enum.hpp is already embedded above.

#if !defined(MAGIC_ENUM_NO_EXCEPTION)                                         \
    && (defined(__cpp_exceptions) || defined(__EXCEPTIONS)                    \
        || defined(_CPPUNWIND))
#ifndef MAGIC_ENUM_USE_STD_MODULE
#include <stdexcept>
#endif
#define MAGIC_ENUM_THROW(...) throw (__VA_ARGS__)
#else
#ifndef MAGIC_ENUM_USE_STD_MODULE
#include <cstdlib>
#endif
#define MAGIC_ENUM_THROW(...) std::abort ()
#endif

namespace magic_enum::containers
{

namespace detail
{

template <typename T, typename = void>
inline constexpr bool is_transparent_v{};

template <typename T>
inline constexpr bool
    is_transparent_v<T, std::void_t<typename T::is_transparent>>{ true };

template <typename Eq = std::equal_to<>, typename T1, typename T2>
constexpr bool
equal (T1 &&t1, T2 &&t2, Eq &&eq = {})
{
  auto first1 = t1.begin ();
  auto last1 = t1.end ();
  auto first2 = t2.begin ();
  auto last2 = t2.end ();

  for (; first1 != last1; ++first1, ++first2)
    {
      if (first2 == last2 || !eq (*first1, *first2))
        {
          return false;
        }
    }
  return first2 == last2;
}

template <typename Cmp = std::less<>, typename T1, typename T2>
constexpr bool
lexicographical_compare (T1 &&t1, T2 &&t2, Cmp &&cmp = {}) noexcept
{
  auto first1 = t1.begin ();
  auto last1 = t1.end ();
  auto first2 = t2.begin ();
  auto last2 = t2.end ();

  // copied from std::lexicographical_compare
  for (; (first1 != last1) && (first2 != last2); ++first1, (void)++first2)
    {
      if (cmp (*first1, *first2))
        {
          return true;
        }
      if (cmp (*first2, *first1))
        {
          return false;
        }
    }
  return (first1 == last1) && (first2 != last2);
}

template <typename T>
constexpr std::size_t
popcount (T x) noexcept
{
  std::size_t c = 0;
  while (x > 0)
    {
      c += x & 1;
      x >>= 1;
    }
  return c;
}

template <typename Cmp = std::less<>, typename ForwardIt, typename E>
constexpr ForwardIt
lower_bound (ForwardIt first, ForwardIt last, E &&e, Cmp &&comp = {})
{
  auto count = std::distance (first, last);
  for (auto it = first; count > 0;)
    {
      auto step = count / 2;
      std::advance (it, step);
      if (comp (*it, e))
        {
          first = ++it;
          count -= step + 1;
        }
      else
        {
          count = step;
        }
    }
  return first;
}

template <typename Cmp = std::less<>, typename BidirIt, typename E>
constexpr auto
equal_range (BidirIt begin, BidirIt end, E &&e, Cmp &&comp = {})
{
  const auto first = lower_bound (begin, end, e, comp);
  return std::pair{ first, lower_bound (std::make_reverse_iterator (end),
                                        std::make_reverse_iterator (first), e,
                                        [&comp] (auto &&lhs, auto &&rhs)
                                          { return comp (rhs, lhs); })
                               .base () };
}

template <typename E = void, typename Cmp = std::less<E>, typename = void>
class indexing
{
  [[nodiscard]] static constexpr auto
  get_indices () noexcept
  {
    // reverse result index mapping
    std::array<std::size_t, enum_count<E> ()> rev_res{};

    // std::iota
    for (std::size_t i = 0; i < enum_count<E> (); ++i)
      {
        rev_res[i] = i;
      }

    constexpr auto orig_values = enum_values<E> ();
    constexpr Cmp cmp{};

    // ~std::sort
    for (std::size_t i = 0; i < enum_count<E> (); ++i)
      {
        for (std::size_t j = i + 1; j < enum_count<E> (); ++j)
          {
            if (cmp (orig_values[rev_res[j]], orig_values[rev_res[i]]))
              {
                auto tmp = rev_res[i];
                rev_res[i] = rev_res[j];
                rev_res[j] = tmp;
              }
          }
      }

    std::array<E, enum_count<E> ()> sorted_values{};
    // reverse the sorted indices
    std::array<std::size_t, enum_count<E> ()> res{};
    for (std::size_t i = 0; i < enum_count<E> (); ++i)
      {
        res[rev_res[i]] = i;
        sorted_values[i] = orig_values[rev_res[i]];
      }

    return std::pair{ sorted_values, res };
  }

  static constexpr auto indices = get_indices ();

public:
  [[nodiscard]] static constexpr const E *
  begin () noexcept
  {
    return indices.first.data ();
  }

  [[nodiscard]] static constexpr const E *
  end () noexcept
  {
    return indices.first.data () + indices.first.size ();
  }

  [[nodiscard]] static constexpr const E *
  it (std::size_t i) noexcept
  {
    return indices.first.data () + i;
  }

  [[nodiscard]] static constexpr optional<std::size_t>
  at (E val) noexcept
  {
    if (auto i = enum_index (val))
      {
        return indices.second[*i];
      }
    return {};
  }
};

template <typename E, typename Cmp>
class indexing<E, Cmp,
               std::enable_if_t<std::is_enum_v<std::decay_t<E>>
                                && (std::is_same_v<Cmp, std::less<E>>
                                    || std::is_same_v<Cmp, std::less<>>)>>
{
  static constexpr auto &values = enum_values<E> ();

public:
  [[nodiscard]] static constexpr const E *
  begin () noexcept
  {
    return values.data ();
  }

  [[nodiscard]] static constexpr const E *
  end () noexcept
  {
    return values.data () + values.size ();
  }

  [[nodiscard]] static constexpr const E *
  it (std::size_t i) noexcept
  {
    return values.data () + i;
  }

  [[nodiscard]] static constexpr optional<std::size_t>
  at (E val) noexcept
  {
    return enum_index (val);
  }
};

template <typename Cmp> struct indexing<void, Cmp, void>
{
  using is_transparent = std::true_type;

  template <typename E>
  [[nodiscard]] static constexpr optional<std::size_t>
  at (E val) noexcept
  {
    return indexing<E, Cmp>::at (val);
  }
};

template <typename E = void, typename Cmp = std::less<>, typename = void>
struct name_sort_impl
{
  [[nodiscard]] constexpr bool
  operator() (E e1, E e2) const noexcept
  {
    return Cmp{}(enum_name (e1), enum_name (e2));
  }
};

template <typename Cmp> struct name_sort_impl<void, Cmp>
{
  using is_transparent = std::true_type;

  template <typename C = Cmp, typename = void> struct FullCmp : C
  {
  };

  template <typename C>
  struct FullCmp<
      C, std::enable_if_t<!std::is_invocable_v<C, string_view, string_view>
                          && std::is_invocable_v<C, char_type, char_type>>>
  {
    [[nodiscard]] constexpr bool
    operator() (string_view s1, string_view s2) const noexcept
    {
      return lexicographical_compare<C> (s1, s2);
    }
  };

  template <typename E1, typename E2>
  [[nodiscard]] constexpr std::enable_if_t<
      // at least one of need to be an enum type
      (std::is_enum_v<std::decay_t<E1>> || std::is_enum_v<std::decay_t<E2>>) &&
          // if both is enum, only accept if the same enum
          (!std::is_enum_v<std::decay_t<E1>>
           || !std::is_enum_v<std::decay_t<E2>> || std::is_same_v<E1, E2>)
          &&
          // is invocable with comparator
          (std::is_invocable_r_v<
              bool, FullCmp<>,
              std::conditional_t<std::is_enum_v<std::decay_t<E1>>, string_view,
                                 E1>,
              std::conditional_t<std::is_enum_v<std::decay_t<E2>>, string_view,
                                 E2>>),
      bool>
  operator() (E1 e1, E2 e2) const noexcept
  {
    using D1 = std::decay_t<E1>;
    using D2 = std::decay_t<E2>;
    constexpr FullCmp<> cmp{};

    if constexpr (std::is_enum_v<D1> && std::is_enum_v<D2>)
      {
        return cmp (enum_name (e1), enum_name (e2));
      }
    else if constexpr (std::is_enum_v<D1>)
      {
        return cmp (enum_name (e1), e2);
      }
    else /* if constexpr (std::is_enum_v<D2>) */
      {
        return cmp (e1, enum_name (e2));
      }
  }
};

struct raw_access_t
{
};

template <typename Parent, typename Iterator, typename Getter,
          typename Predicate>
struct FilteredIterator
{
  Parent parent;
  Iterator first;
  Iterator last;
  Iterator current;
  Getter getter;
  Predicate predicate;

  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = std::remove_reference_t<
      std::invoke_result_t<Getter, Parent, Iterator>>;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type *;
  using reference = value_type &;

  constexpr FilteredIterator () noexcept = default;
  constexpr FilteredIterator (const FilteredIterator &) = default;
  constexpr FilteredIterator &operator= (const FilteredIterator &) = default;
  constexpr FilteredIterator (FilteredIterator &&) noexcept = default;
  constexpr FilteredIterator &operator= (FilteredIterator &&) noexcept
      = default;

  template <typename OtherParent, typename OtherIterator,
            typename = std::enable_if_t<
                std::is_convertible_v<OtherParent, Parent>
                && std::is_convertible_v<OtherIterator, Iterator>> *>
  constexpr explicit FilteredIterator (
      const FilteredIterator<OtherParent, OtherIterator, Getter, Predicate>
          &other)
      : parent (other.parent), first (other.first), last (other.last),
        current (other.current), getter (other.getter),
        predicate (other.predicate)
  {
  }

  constexpr FilteredIterator (Parent p, Iterator begin, Iterator end,
                              Iterator curr, Getter get = {},
                              Predicate pred = {})
      : parent (p), first (std::move (begin)), last (std::move (end)),
        current (std::move (curr)), getter{ std::move (get) },
        predicate{ std::move (pred) }
  {
    if (current == first && !predicate (parent, current))
      {
        ++*this;
      }
  }

  [[nodiscard]] constexpr reference
  operator* () const
  {
    return getter (parent, current);
  }

  [[nodiscard]] constexpr pointer
  operator->() const
  {
    return std::addressof (**this);
  }

  constexpr FilteredIterator &
  operator++ ()
  {
    do
      {
        ++current;
      }
    while (current != last && !predicate (parent, current));
    return *this;
  }

  [[nodiscard]] constexpr FilteredIterator
  operator++ (int)
  {
    FilteredIterator cp = *this;
    ++*this;
    return cp;
  }

  constexpr FilteredIterator &
  operator-- ()
  {
    do
      {
        --current;
      }
    while (current != first && !predicate (parent, current));
    return *this;
  }

  [[nodiscard]] constexpr FilteredIterator
  operator-- (int)
  {
    FilteredIterator cp = *this;
    --*this;
    return cp;
  }

  [[nodiscard]] friend constexpr bool
  operator== (const FilteredIterator &lhs, const FilteredIterator &rhs)
  {
    return lhs.current == rhs.current;
  }

  [[nodiscard]] friend constexpr bool
  operator!= (const FilteredIterator &lhs, const FilteredIterator &rhs)
  {
    return lhs.current != rhs.current;
  }
};

} // namespace detail

template <typename E = void> using name_less = detail::name_sort_impl<E>;

template <typename E = void>
using name_greater = detail::name_sort_impl<E, std::greater<>>;

using name_less_case_insensitive = detail::name_sort_impl<
    void, magic_enum::detail::case_insensitive<std::less<>>>;

using name_greater_case_insensitive = detail::name_sort_impl<
    void, magic_enum::detail::case_insensitive<std::greater<>>>;

template <typename E = void> using default_indexing = detail::indexing<E>;

template <typename Cmp = std::less<>>
using comparator_indexing = detail::indexing<void, Cmp>;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//                                                          ARRAY //
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename E, typename V, typename Index = default_indexing<E>>
struct array
{
  static_assert (std::is_enum_v<E>);
  static_assert (std::is_trivially_constructible_v<Index>);
  static_assert (enum_count<E> () > 0
                 && Index::at (enum_values<E> ().front ()));

  using index_type = Index;
  using container_type = std::array<V, enum_count<E> ()>;

  using value_type = typename container_type::value_type;
  using size_type = typename container_type::size_type;
  using difference_type = typename container_type::difference_type;
  using reference = typename container_type::reference;
  using const_reference = typename container_type::const_reference;
  using pointer = typename container_type::pointer;
  using const_pointer = typename container_type::const_pointer;
  using iterator = typename container_type::iterator;
  using const_iterator = typename container_type::const_iterator;
  using reverse_iterator = typename container_type::reverse_iterator;
  using const_reverse_iterator =
      typename container_type::const_reverse_iterator;

  constexpr reference
  at (E pos)
  {
    if (auto index = index_type::at (pos))
      {
        return a[*index];
      }
    MAGIC_ENUM_THROW (std::out_of_range (
        "magic_enum::containers::array::at Unrecognized position"));
  }

  constexpr const_reference
  at (E pos) const
  {
    if (auto index = index_type::at (pos))
      {
        return a[*index];
      }
    MAGIC_ENUM_THROW (std::out_of_range (
        "magic_enum::containers::array::at: Unrecognized position"));
  }

  [[nodiscard]] constexpr reference
  operator[] (E pos)
  {
    auto i = index_type::at (pos);
    return MAGIC_ENUM_ASSERT (i), a[*i];
  }

  [[nodiscard]] constexpr const_reference
  operator[] (E pos) const
  {
    auto i = index_type::at (pos);
    return MAGIC_ENUM_ASSERT (i), a[*i];
  }

  [[nodiscard]] constexpr reference
  front () noexcept
  {
    return a.front ();
  }

  [[nodiscard]] constexpr const_reference
  front () const noexcept
  {
    return a.front ();
  }

  [[nodiscard]] constexpr reference
  back () noexcept
  {
    return a.back ();
  }

  [[nodiscard]] constexpr const_reference
  back () const noexcept
  {
    return a.back ();
  }

  [[nodiscard]] constexpr pointer
  data () noexcept
  {
    return a.data ();
  }

  [[nodiscard]] constexpr const_pointer
  data () const noexcept
  {
    return a.data ();
  }

  [[nodiscard]] constexpr iterator
  begin () noexcept
  {
    return a.begin ();
  }

  [[nodiscard]] constexpr const_iterator
  begin () const noexcept
  {
    return a.begin ();
  }

  [[nodiscard]] constexpr const_iterator
  cbegin () const noexcept
  {
    return a.cbegin ();
  }

  [[nodiscard]] constexpr iterator
  end () noexcept
  {
    return a.end ();
  }

  [[nodiscard]] constexpr const_iterator
  end () const noexcept
  {
    return a.end ();
  }

  [[nodiscard]] constexpr const_iterator
  cend () const noexcept
  {
    return a.cend ();
  }

  [[nodiscard]] constexpr iterator
  rbegin () noexcept
  {
    return a.rbegin ();
  }

  [[nodiscard]] constexpr const_iterator
  rbegin () const noexcept
  {
    return a.rbegin ();
  }

  [[nodiscard]] constexpr const_iterator
  crbegin () const noexcept
  {
    return a.crbegin ();
  }

  [[nodiscard]] constexpr iterator
  rend () noexcept
  {
    return a.rend ();
  }

  [[nodiscard]] constexpr const_iterator
  rend () const noexcept
  {
    return a.rend ();
  }

  [[nodiscard]] constexpr const_iterator
  crend () const noexcept
  {
    return a.crend ();
  }

  [[nodiscard]] constexpr bool
  empty () const noexcept
  {
    return a.empty ();
  }

  [[nodiscard]] constexpr size_type
  size () const noexcept
  {
    return a.size ();
  }

  [[nodiscard]] constexpr size_type
  max_size () const noexcept
  {
    return a.max_size ();
  }

  constexpr void
  fill (const V &value)
  {
    for (auto &v : a)
      {
        v = value;
      }
  }

  constexpr void
  swap (array &other) noexcept (std::is_nothrow_swappable_v<V>)
  {
    for (std::size_t i = 0; i < a.size (); ++i)
      {
        auto v = std::move (other.a[i]);
        other.a[i] = std::move (a[i]);
        a[i] = std::move (v);
      }
  }

  [[nodiscard]] friend constexpr bool
  operator== (const array &a1, const array &a2)
  {
    return detail::equal (a1, a2);
  }

  [[nodiscard]] friend constexpr bool
  operator!= (const array &a1, const array &a2)
  {
    return !detail::equal (a1, a2);
  }

  [[nodiscard]] friend constexpr bool
  operator< (const array &a1, const array &a2)
  {
    return detail::lexicographical_compare (a1, a2);
  }

  [[nodiscard]] friend constexpr bool
  operator<= (const array &a1, const array &a2)
  {
    return !detail::lexicographical_compare (a2, a1);
  }

  [[nodiscard]] friend constexpr bool
  operator> (const array &a1, const array &a2)
  {
    return detail::lexicographical_compare (a2, a1);
  }

  [[nodiscard]] friend constexpr bool
  operator>= (const array &a1, const array &a2)
  {
    return !detail::lexicographical_compare (a1, a2);
  }

  container_type a;
};

namespace detail
{

template <typename E, typename T, std::size_t N, std::size_t... I>
constexpr array<E, std::remove_cv_t<T>>
to_array_impl (T (&a)[N], std::index_sequence<I...>)
{
  return { { a[I]... } };
}

template <typename E, typename T, std::size_t N, std::size_t... I>
constexpr array<E, std::remove_cv_t<T>>
to_array_impl (T (&&a)[N], std::index_sequence<I...>)
{
  return { { std::move (a[I])... } };
}

} // namespace detail

template <typename E, typename T, std::size_t N>
constexpr std::enable_if_t<(enum_count<E> () == N),
                           array<E, std::remove_cv_t<T>>>
to_array (T (&a)[N])
{
  return detail::to_array_impl<E> (a, std::make_index_sequence<N>{});
}

template <typename E, typename T, std::size_t N>
constexpr std::enable_if_t<(enum_count<E> () == N),
                           array<E, std::remove_cv_t<T>>>
to_array (T (&&a)[N])
{
  return detail::to_array_impl<E> (std::move (a),
                                   std::make_index_sequence<N>{});
}

template <typename E, typename... Ts>
constexpr std::enable_if_t<
    (enum_count<E> () == sizeof...(Ts)),
    array<E, std::remove_cv_t<std::common_type_t<Ts...>>>>
make_array (Ts &&...ts)
{
  return { { std::forward<Ts> (ts)... } };
}

inline constexpr detail::raw_access_t raw_access{};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//                                                         BITSET //
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename E, typename Index = default_indexing<E>> class bitset
{
  // LibDSL: keep optional string facilities dependent on E.
  using string
      = std::conditional_t<std::is_enum_v<E>, magic_enum::string, void>;
  using string_view
      = std::conditional_t<std::is_enum_v<E>, magic_enum::string_view, void>;
  static_assert (std::is_enum_v<E>);
  static_assert (std::is_trivially_constructible_v<Index>);
  static_assert (enum_count<E> () > 0
                 && Index::at (enum_values<E> ().front ()));

  using base_type = std::conditional_t<
      enum_count<E> () <= 8, std::uint_least8_t,
      std::conditional_t<
          enum_count<E> () <= 16, std::uint_least16_t,
          std::conditional_t<enum_count<E> () <= 32, std::uint_least32_t,
                             std::uint_least64_t>>>;

  static constexpr std::size_t bits_per_base = sizeof (base_type) * 8;
  static constexpr std::size_t base_type_count
      = (enum_count<E> () > 0 ? (enum_count<E> () - 1) / bits_per_base + 1
                              : 0);
  static constexpr std::size_t not_interested
      = base_type_count * bits_per_base - enum_count<E> ();
  static constexpr base_type last_value_max
      = (base_type{ 1 } << (bits_per_base - not_interested)) - 1;

  template <typename parent_t = bitset *> class reference_impl
  {
    friend class bitset;

    parent_t parent;
    std::size_t num_index;
    base_type bit_index;

    constexpr reference_impl (parent_t p, std::size_t i) noexcept
        : reference_impl (p,
                          std::pair{ i / bits_per_base,
                                     base_type{ 1 } << (i % bits_per_base) })
    {
    }

    constexpr reference_impl (parent_t p,
                              std::pair<std::size_t, base_type> i) noexcept
        : parent (p), num_index (std::get<0> (i)), bit_index (std::get<1> (i))
    {
    }

  public:
    constexpr reference_impl &
    operator= (bool v) noexcept
    {
      if (v)
        {
          parent->a[num_index] |= bit_index;
        }
      else
        {
          parent->a[num_index] &= ~bit_index;
        }
      return *this;
    }

    constexpr reference_impl &
    operator= (const reference_impl &v) noexcept
    {
      if (this == &v)
        {
          return *this;
        }
      *this = static_cast<bool> (v);
      return *this;
    }

    [[nodiscard]] constexpr
    operator bool () const noexcept
    {
      return (parent->a[num_index] & bit_index) > 0;
    }

    [[nodiscard]] constexpr bool
    operator~() const noexcept
    {
      return !static_cast<bool> (*this);
    }

    constexpr reference_impl &
    flip () noexcept
    {
      *this = ~*this;
      return *this;
    }
  };

  template <typename T>
  [[nodiscard]] constexpr T
  to_ (detail::raw_access_t) const
  {
    T res{};
    T flag{ 1 };
    for (std::size_t i = 0; i < size (); ++i, flag <<= 1)
      {
        if (const_reference{ this, i })
          {
            if (i >= sizeof (T) * 8)
              {
                MAGIC_ENUM_THROW (std::overflow_error (
                    "magic_enum::containers::bitset::to: Cannot represent "
                    "enum in this type"));
              }
            res |= flag;
          }
      }
    return res;
  }

public:
  using index_type = Index;
  using container_type = std::array<base_type, base_type_count>;
  using reference = reference_impl<>;
  using const_reference = reference_impl<const bitset *>;

  constexpr explicit bitset (detail::raw_access_t = raw_access) noexcept
      : a{ {} }
  {
  }

  constexpr explicit bitset (detail::raw_access_t, unsigned long long val)
      : a{ {} }
  {
    unsigned long long bit{ 1 };
    for (std::size_t i = 0; i < (sizeof (val) * 8); ++i, bit <<= 1)
      {
        if ((val & bit) > 0)
          {
            if (i >= enum_count<E> ())
              {
                MAGIC_ENUM_THROW (std::out_of_range (
                    "magic_enum::containers::bitset::constructor: Upper bit "
                    "set in raw number"));
              }

            reference{ this, i } = true;
          }
      }
  }

  constexpr explicit bitset (detail::raw_access_t, string_view sv,
                             std::size_t pos = 0,
                             std::size_t n = string_view::npos,
                             char_type zero = static_cast<char_type> ('0'),
                             char_type one = static_cast<char_type> ('1'))
      : a{ {} }
  {
    std::size_t i = 0;
    for (auto c : sv.substr (pos, n))
      {
        if (c == one)
          {
            if (i >= enum_count<E> ())
              {
                MAGIC_ENUM_THROW (std::out_of_range (
                    "magic_enum::containers::bitset::constructor: Upper bit "
                    "set in raw string"));
              }
            reference{ this, i } = true;
          }
        else if (c != zero)
          {
            MAGIC_ENUM_THROW (std::invalid_argument (
                "magic_enum::containers::bitset::constructor: Unrecognized "
                "character in raw string"));
          }
        ++i;
      }
  }

  constexpr explicit bitset (detail::raw_access_t, const char_type *str,
                             std::size_t n = ~std::size_t{ 0 },
                             char_type zero = static_cast<char_type> ('0'),
                             char_type one = static_cast<char_type> ('1'))
      : bitset (
            string_view{
                str,
                (std::min)(std::char_traits<char_type>::length (str), n) },
            0, n, zero, one)
  {
  }

  constexpr bitset (std::initializer_list<E> starters) : a{ {} }
  {
    if constexpr (magic_enum::detail::subtype_v<E>
                  == magic_enum::detail::enum_subtype::flags)
      {
        for (auto &f : starters)
          {
            *this |= bitset (f);
          }
      }
    else
      {
        for (auto &f : starters)
          {
            set (f);
          }
      }
  }
  template <
      typename V,
      std::enable_if_t<std::is_same_v<V, E>
                           && magic_enum::detail::subtype_v<V>
                                  == magic_enum::detail::enum_subtype::flags,
                       int> = 0>
  constexpr explicit bitset (V starter) : a{ {} }
  {
    auto u = enum_underlying (starter);
    for (E v : enum_values<E> ())
      {
        if (auto ul = enum_underlying (v); (ul & u) != 0)
          {
            u &= ~ul;
            (*this)[v] = true;
          }
      }
    if (u != 0)
      {
        MAGIC_ENUM_THROW (std::invalid_argument (
            "magic_enum::containers::bitset::constructor: Unrecognized enum "
            "value in flag"));
      }
  }

  template <typename Cmp = std::equal_to<>>
  constexpr explicit bitset (string_view sv, Cmp &&cmp = {},
                             char_type sep = static_cast<char_type> ('|'))
  {
    for (std::size_t to = 0;
         (to = magic_enum::detail::find (sv, sep)) != string_view::npos;
         sv.remove_prefix (to + 1))
      {
        if (auto v = enum_cast<E> (sv.substr (0, to), cmp))
          {
            set (*v);
          }
        else
          {
            MAGIC_ENUM_THROW (std::invalid_argument (
                "magic_enum::containers::bitset::constructor: Unrecognized "
                "enum value in string"));
          }
      }
    if (!sv.empty ())
      {
        if (auto v = enum_cast<E> (sv, cmp))
          {
            set (*v);
          }
        else
          {
            MAGIC_ENUM_THROW (std::invalid_argument (
                "magic_enum::containers::bitset::constructor: Unrecognized "
                "enum value in string"));
          }
      }
  }

  [[nodiscard]] friend constexpr bool
  operator== (const bitset &lhs, const bitset &rhs) noexcept
  {
    return detail::equal (lhs.a, rhs.a);
  }

  [[nodiscard]] friend constexpr bool
  operator!= (const bitset &lhs, const bitset &rhs) noexcept
  {
    return !detail::equal (lhs.a, rhs.a);
  }

  [[nodiscard]] constexpr bool
  operator[] (E pos) const
  {
    auto i = index_type::at (pos);
    return MAGIC_ENUM_ASSERT (i),
           static_cast<bool> (const_reference (this, *i));
  }

  [[nodiscard]] constexpr reference
  operator[] (E pos)
  {
    auto i = index_type::at (pos);
    return MAGIC_ENUM_ASSERT (i), reference{ this, *i };
  }

  constexpr bool
  test (E pos) const
  {
    if (auto i = index_type::at (pos))
      {
        return static_cast<bool> (const_reference (this, *i));
      }
    MAGIC_ENUM_THROW (std::out_of_range (
        "magic_enum::containers::bitset::test: Unrecognized position"));
  }

  [[nodiscard]] constexpr bool
  all () const noexcept
  {
    if constexpr (base_type_count == 0)
      {
        return true;
      }

    for (std::size_t i = 0; i < base_type_count - (not_interested > 0); ++i)
      {
        auto check = ~a[i];
        if (check)
          {
            return false;
          }
      }

    if constexpr (not_interested > 0)
      {
        return a[base_type_count - 1] == last_value_max;
      }
  }

  [[nodiscard]] constexpr bool
  any () const noexcept
  {
    for (auto &v : a)
      {
        if (v > 0)
          {
            return true;
          }
      }
    return false;
  }

  [[nodiscard]] constexpr bool
  none () const noexcept
  {
    return !any ();
  }

  [[nodiscard]] constexpr std::size_t
  count () const noexcept
  {
    std::size_t c = 0;
    for (auto &v : a)
      {
        c += detail::popcount (v);
      }
    return c;
  }

  [[nodiscard]] constexpr std::size_t
  size () const noexcept
  {
    return enum_count<E> ();
  }

  [[nodiscard]] constexpr std::size_t
  max_size () const noexcept
  {
    return enum_count<E> ();
  }

  constexpr bitset &
  operator&= (const bitset &other) noexcept
  {
    for (std::size_t i = 0; i < base_type_count; ++i)
      {
        a[i] &= other.a[i];
      }
    return *this;
  }

  constexpr bitset &
  operator|= (const bitset &other) noexcept
  {
    for (std::size_t i = 0; i < base_type_count; ++i)
      {
        a[i] |= other.a[i];
      }
    return *this;
  }

  constexpr bitset &
  operator^= (const bitset &other) noexcept
  {
    for (std::size_t i = 0; i < base_type_count; ++i)
      {
        a[i] ^= other.a[i];
      }
    return *this;
  }

  [[nodiscard]] constexpr bitset
  operator~() const noexcept
  {
    bitset res;
    for (std::size_t i = 0; i < base_type_count - (not_interested > 0); ++i)
      {
        res.a[i] = ~a[i];
      }

    if constexpr (not_interested > 0)
      {
        res.a[base_type_count - 1] = ~a[base_type_count - 1] & last_value_max;
      }
    return res;
  }

  constexpr bitset &
  set () noexcept
  {
    for (std::size_t i = 0; i < base_type_count - (not_interested > 0); ++i)
      {
        a[i] = ~base_type{ 0 };
      }

    if constexpr (not_interested > 0)
      {
        a[base_type_count - 1] = last_value_max;
      }
    return *this;
  }

  constexpr bitset &
  set (E pos, bool value = true)
  {
    if (auto i = index_type::at (pos))
      {
        reference{ this, *i } = value;
        return *this;
      }
    MAGIC_ENUM_THROW (std::out_of_range (
        "magic_enum::containers::bitset::set: Unrecognized position"));
  }

  constexpr bitset &
  reset () noexcept
  {
    return *this = bitset{};
  }

  constexpr bitset &
  reset (E pos)
  {
    if (auto i = index_type::at (pos))
      {
        reference{ this, *i } = false;
        return *this;
      }
    MAGIC_ENUM_THROW (std::out_of_range (
        "magic_enum::containers::bitset::reset: Unrecognized position"));
  }

  constexpr bitset &
  flip () noexcept
  {
    return *this = ~*this;
  }

  [[nodiscard]] friend constexpr bitset
  operator& (const bitset &lhs, const bitset &rhs) noexcept
  {
    bitset cp = lhs;
    cp &= rhs;
    return cp;
  }

  [[nodiscard]] friend constexpr bitset
  operator| (const bitset &lhs, const bitset &rhs) noexcept
  {
    bitset cp = lhs;
    cp |= rhs;
    return cp;
  }

  [[nodiscard]] friend constexpr bitset
  operator^ (const bitset &lhs, const bitset &rhs) noexcept
  {
    bitset cp = lhs;
    cp ^= rhs;
    return cp;
  }

  template <typename V = E>
  [[nodiscard]] constexpr explicit
  operator std::enable_if_t<magic_enum::detail::subtype_v<V>
                                == magic_enum::detail::enum_subtype::flags,
                            E> () const
  {
    E res{};
    for (const auto &e : enum_values<E> ())
      {
        if (test (e))
          {
            res |= e;
          }
      }
    return res;
  }

  [[nodiscard]] string
  to_string (char_type sep = static_cast<char_type> ('|')) const
  {
    string name;

    for (const auto &e : enum_values<E> ())
      {
        if (test (e))
          {
            if (!name.empty ())
              {
                name.append (1, sep);
              }
            auto n = enum_name (e);
            name.append (n.data (), n.size ());
          }
      }
    return name;
  }

  [[nodiscard]] string
  to_string (detail::raw_access_t,
             char_type zero = static_cast<char_type> ('0'),
             char_type one = static_cast<char_type> ('1')) const
  {
    string name;
    name.reserve (size ());
    for (std::size_t i = 0; i < size (); ++i)
      {
        name.append (1, const_reference{ this, i } ? one : zero);
      }
    return name;
  }

  [[nodiscard]] constexpr unsigned long long
  to_ullong (detail::raw_access_t raw) const
  {
    return to_<unsigned long long> (raw);
  }

  [[nodiscard]] constexpr unsigned long long
  to_ulong (detail::raw_access_t raw) const
  {
    return to_<unsigned long> (raw);
  }

  friend std::ostream &
  operator<< (std::ostream &o, const bitset &bs)
  {
    return o << bs.to_string ();
  }

  friend std::istream &
  operator>> (std::istream &i, bitset &bs)
  {
    string s;
    if (i >> s; !s.empty ())
      {
        bs = bitset (string_view{ s });
      }
    return i;
  }

private:
  container_type a;
};

template <typename V, int = 0> explicit bitset (V starter) -> bitset<V>;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//                                                           SET //
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename E, typename Cmp = std::less<E>> class set
{
  using index_type = detail::indexing<E, Cmp>;
  struct Getter
  {
    constexpr const E &
    operator() (const set *, const E *p) const noexcept
    {
      return *p;
    }
  };
  struct Predicate
  {
    constexpr bool
    operator() (const set *h, const E *e) const noexcept
    {
      return h->a[*e];
    }
  };

public:
  using container_type = bitset<E, index_type>;
  using key_type = E;
  using value_type = E;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using key_compare = Cmp;
  using value_compare = Cmp;
  using reference = value_type &;
  using const_reference = const value_type &;
  using pointer = value_type *;
  using const_pointer = const value_type *;
  using iterator
      = detail::FilteredIterator<const set *, const E *, Getter, Predicate>;
  using const_iterator
      = detail::FilteredIterator<const set *, const E *, Getter, Predicate>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  constexpr set () noexcept = default;

  template <typename InputIt> constexpr set (InputIt first, InputIt last)
  {
    while (first != last)
      {
        insert (*first++);
      }
  }

  constexpr set (std::initializer_list<E> ilist)
  {
    for (auto e : ilist)
      {
        insert (e);
      }
  }
  template <
      typename V,
      std::enable_if_t<std::is_same_v<V, E>
                           && magic_enum::detail::subtype_v<V>
                                  == magic_enum::detail::enum_subtype::flags,
                       int> = 0>
  constexpr explicit set (V starter)
  {
    auto u = enum_underlying (starter);
    for (E v : enum_values<E> ())
      {
        if ((enum_underlying (v) & u) != 0)
          {
            insert (v);
          }
      }
  }

  constexpr set (const set &) noexcept = default;
  constexpr set (set &&) noexcept = default;

  constexpr set &operator= (const set &) noexcept = default;
  constexpr set &operator= (set &&) noexcept = default;
  constexpr set &
  operator= (std::initializer_list<E> ilist)
  {
    for (auto e : ilist)
      {
        insert (e);
      }
  }

  constexpr const_iterator
  begin () const noexcept
  {
    return const_iterator{ this, index_type::begin (), index_type::end (),
                           index_type::begin () };
  }

  constexpr const_iterator
  end () const noexcept
  {
    return const_iterator{ this, index_type::begin (), index_type::end (),
                           index_type::end () };
  }

  constexpr const_iterator
  cbegin () const noexcept
  {
    return begin ();
  }

  constexpr const_iterator
  cend () const noexcept
  {
    return end ();
  }

  constexpr const_reverse_iterator
  rbegin () const noexcept
  {
    return { end () };
  }

  constexpr const_reverse_iterator
  rend () const noexcept
  {
    return { begin () };
  }

  constexpr const_reverse_iterator
  crbegin () const noexcept
  {
    return rbegin ();
  }

  constexpr const_reverse_iterator
  crend () const noexcept
  {
    return rend ();
  }

  [[nodiscard]] constexpr bool
  empty () const noexcept
  {
    return s == 0;
  }

  [[nodiscard]] constexpr size_type
  size () const noexcept
  {
    return s;
  }

  [[nodiscard]] constexpr size_type
  max_size () const noexcept
  {
    return a.max_size ();
  }

  constexpr void
  clear () noexcept
  {
    a.reset ();
    s = 0;
  }

  constexpr std::pair<iterator, bool>
  insert (const value_type &value) noexcept
  {
    if (auto i = index_type::at (value))
      {
        typename container_type::reference ref = a[value];
        bool r = !ref;
        if (r)
          {
            ref = true;
            ++s;
          }

        return { iterator{ this, index_type::begin (), index_type::end (),
                           index_type::it (*i) },
                 r };
      }
    return { end (), false };
  }

  constexpr std::pair<iterator, bool>
  insert (value_type &&value) noexcept
  {
    return insert (value);
  }

  constexpr iterator
  insert (const_iterator, const value_type &value) noexcept
  {
    return insert (value).first;
  }

  constexpr iterator
  insert (const_iterator hint, value_type &&value) noexcept
  {
    return insert (hint, value);
  }

  template <typename InputIt>
  constexpr void
  insert (InputIt first, InputIt last) noexcept
  {
    while (first != last)
      {
        insert (*first++);
      }
  }

  constexpr void
  insert (std::initializer_list<value_type> ilist) noexcept
  {
    for (auto v : ilist)
      {
        insert (v);
      }
  }

  template <typename... Args>
  constexpr std::pair<iterator, bool>
  emplace (Args &&...args) noexcept
  {
    return insert ({ std::forward<Args> (args)... });
  }

  template <typename... Args>
  constexpr iterator
  emplace_hint (const_iterator, Args &&...args) noexcept
  {
    return emplace (std::forward<Args> (args)...).first;
  }

  constexpr iterator
  erase (const_iterator pos) noexcept
  {
    erase (*pos++);
    return pos;
  }

  constexpr iterator
  erase (const_iterator first, const_iterator last) noexcept
  {
    while ((first = erase (first)) != last)
      {
      }
    return first;
  }

  constexpr size_type
  erase (const key_type &key) noexcept
  {
    typename container_type::reference ref = a[key];
    bool res = ref;
    if (res)
      {
        --s;
      }
    ref = false;
    return res;
  }

  template <typename K, typename KC = key_compare>
  constexpr std::enable_if_t<detail::is_transparent_v<KC>, size_type>
  erase (K &&x) noexcept
  {
    size_type c = 0;
    for (auto [first, last] = detail::equal_range (
             index_type::begin (), index_type::end (), x, key_compare{});
         first != last;)
      {
        c += erase (*first++);
      }
    return c;
  }

  void
  swap (set &other) noexcept
  {
    set cp = *this;
    *this = other;
    other = cp;
  }

  [[nodiscard]] constexpr size_type
  count (const key_type &key) const noexcept
  {
    return index_type::at (key) && a[key];
  }

  template <typename K, typename KC = key_compare>
  [[nodiscard]] constexpr std::enable_if_t<detail::is_transparent_v<KC>,
                                           size_type>
  count (const K &x) const
  {
    size_type c = 0;
    for (auto [first, last] = detail::equal_range (
             index_type::begin (), index_type::end (), x, key_compare{});
         first != last; ++first)
      {
        c += count (*first);
      }
    return c;
  }

  [[nodiscard]] constexpr const_iterator
  find (const key_type &key) const noexcept
  {
    if (auto i = index_type::at (key); i && a.test (key))
      {
        return const_iterator{ this, index_type::begin (), index_type::end (),
                               index_type::it (*i) };
      }
    return end ();
  }

  template <typename K, typename KC = key_compare>
  [[nodiscard]] constexpr std::enable_if_t<detail::is_transparent_v<KC>,
                                           const_iterator>
  find (const K &x) const
  {
    for (auto [first, last] = detail::equal_range (
             index_type::begin (), index_type::end (), x, key_compare{});
         first != last; ++first)
      {
        if (a.test (*first))
          {
            return find (*first);
          }
      }
    return end ();
  }

  [[nodiscard]] constexpr bool
  contains (const key_type &key) const noexcept
  {
    return count (key);
  }

  template <typename K, typename KC = key_compare>
  [[nodiscard]] constexpr std::enable_if_t<detail::is_transparent_v<KC>, bool>
  contains (const K &x) const noexcept
  {
    return count (x) > 0;
  }

  [[nodiscard]] constexpr std::pair<const_iterator, const_iterator>
  equal_range (const key_type &key) const noexcept
  {
    return { lower_bound (key), upper_bound (key) };
  }

  template <typename K, typename KC = key_compare>
  [[nodiscard]] constexpr std::enable_if_t<
      detail::is_transparent_v<KC>, std::pair<const_iterator, const_iterator>>
  equal_range (const K &x) const noexcept
  {
    return { lower_bound (x), upper_bound (x) };
  }

  [[nodiscard]] constexpr const_iterator
  lower_bound (const key_type &key) const noexcept
  {
    if (auto i = index_type::at (key))
      {
        auto it = const_iterator{ this, index_type::begin (),
                                  index_type::end (), index_type::it (*i) };
        return a.test (key) ? it : std::next (it);
      }
    return end ();
  }

  template <typename K, typename KC = key_compare>
  [[nodiscard]] constexpr std::enable_if_t<detail::is_transparent_v<KC>,
                                           const_iterator>
  lower_bound (const K &x) const noexcept
  {
    auto [first, last] = detail::equal_range (
        index_type::begin (), index_type::end (), x, key_compare{});
    return first != last ? lower_bound (*first) : end ();
  }

  [[nodiscard]] constexpr const_iterator
  upper_bound (const key_type &key) const noexcept
  {
    if (auto i = index_type::at (key))
      {
        return std::next (const_iterator{ this, index_type::begin (),
                                          index_type::end (),
                                          index_type::it (*i) });
      }
    return end ();
  }

  template <typename K, typename KC = key_compare>
  [[nodiscard]] constexpr std::enable_if_t<detail::is_transparent_v<KC>,
                                           const_iterator>
  upper_bound (const K &x) const noexcept
  {
    auto [first, last] = detail::equal_range (
        index_type::begin (), index_type::end (), x, key_compare{});
    return first != last ? upper_bound (*std::prev (last)) : end ();
  }

  [[nodiscard]] constexpr key_compare
  key_comp () const
  {
    return {};
  }

  [[nodiscard]] constexpr value_compare
  value_comp () const
  {
    return {};
  }

  [[nodiscard]] constexpr friend bool
  operator== (const set &lhs, const set &rhs) noexcept
  {
    return lhs.a == rhs.a;
  }

  [[nodiscard]] constexpr friend bool
  operator!= (const set &lhs, const set &rhs) noexcept
  {
    return lhs.a != rhs.a;
  }

  [[nodiscard]] constexpr friend bool
  operator< (const set &lhs, const set &rhs) noexcept
  {
    if (lhs.s < rhs.s)
      {
        return true;
      }
    if (rhs.s < lhs.s)
      {
        return false;
      }

    for (auto it = index_type::begin (); it != index_type::end (); ++it)
      {
        if (auto c = rhs.contains (*it); c != lhs.contains (*it))
          {
            return c;
          }
      }
    return false;
  }

  [[nodiscard]] constexpr friend bool
  operator<= (const set &lhs, const set &rhs) noexcept
  {
    return !(rhs < lhs);
  }

  [[nodiscard]] constexpr friend bool
  operator> (const set &lhs, const set &rhs) noexcept
  {
    return rhs < lhs;
  }

  [[nodiscard]] constexpr friend bool
  operator>= (const set &lhs, const set &rhs) noexcept
  {
    return !(lhs < rhs);
  }

  template <typename Pred>
  size_type
  erase_if (Pred pred)
  {
    auto old_size = size ();
    for (auto i = begin (), last = end (); i != last;)
      {
        if (pred (*i))
          {
            i = erase (i);
          }
        else
          {
            ++i;
          }
      }
    return old_size - size ();
  }

private:
  container_type a;
  std::size_t s = 0;
};

template <typename V, int = 0> explicit set (V starter) -> set<V>;

template <auto I, typename E, typename V, typename Index>
constexpr std::enable_if_t<
    (std::is_integral_v<decltype (I)> && I < enum_count<E> ()), V &>
get (array<E, V, Index> &a) noexcept
{
  return a.a[I];
}

template <auto I, typename E, typename V, typename Index>
constexpr std::enable_if_t<
    (std::is_integral_v<decltype (I)> && I < enum_count<E> ()), V &&>
get (array<E, V, Index> &&a) noexcept
{
  return std::move (a.a[I]);
}

template <auto I, typename E, typename V, typename Index>
constexpr std::enable_if_t<
    (std::is_integral_v<decltype (I)> && I < enum_count<E> ()), const V &>
get (const array<E, V, Index> &a) noexcept
{
  return a.a[I];
}

template <auto I, typename E, typename V, typename Index>
constexpr std::enable_if_t<
    (std::is_integral_v<decltype (I)> && I < enum_count<E> ()), const V &&>
get (const array<E, V, Index> &&a) noexcept
{
  return std::move (a.a[I]);
}

template <auto Enum, typename E, typename V, typename Index>
constexpr std::enable_if_t<
    std::is_same_v<decltype (Enum), E> && enum_contains (Enum), V &>
get (array<E, V, Index> &a)
{
  return a[Enum];
}

template <auto Enum, typename E, typename V, typename Index>
constexpr std::enable_if_t<
    std::is_same_v<decltype (Enum), E> && enum_contains (Enum), V &&>
get (array<E, V, Index> &&a)
{
  return std::move (a[Enum]);
}

template <auto Enum, typename E, typename V, typename Index>
constexpr std::enable_if_t<
    std::is_same_v<decltype (Enum), E> && enum_contains (Enum), const V &>
get (const array<E, V, Index> &a)
{
  return a[Enum];
}

template <auto Enum, typename E, typename V, typename Index>
constexpr std::enable_if_t<
    std::is_same_v<decltype (Enum), E> && enum_contains (Enum), const V &&>
get (const array<E, V, Index> &&a)
{
  return std::move (a[Enum]);
}

} // namespace magic_enum::containers

#endif // NEARGYE_MAGIC_ENUM_CONTAINERS_HPP
// Embedded upstream header: magic_enum_flags.hpp
//  __  __             _        ______                          _____
// |  \/  |           (_)      |  ____|                        / ____|_     _
// | \  / | __ _  __ _ _  ___  | |__   _ __  _   _ _ __ ___   | |   _| |_ _| |_
// | |\/| |/ _` |/ _` | |/ __| |  __| | '_ \| | | | '_ ` _ \  | |  |_   _|_ _|
// | |  | | (_| | (_| | | (__  | |____| | | | |_| | | | | | | | |____|_|   |_|
// |_|  |_|\__,_|\__, |_|\___| |______|_| |_|\__,_|_| |_| |_|  \_____|
//                __/ | https://github.com/Neargye/magic_enum
//               |___/  version 0.9.8
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2019 - 2024 Daniil Goncharov <neargye@gmail.com>.
//
// Permission is hereby  granted, free of charge, to any  person obtaining a
// copy of this software and associated  documentation files (the "Software"),
// to deal in the Software  without restriction, including without  limitation
// the rights to  use, copy,  modify, merge,  publish, distribute,  sublicense,
// and/or  sell copies  of  the Software,  and  to  permit persons  to  whom
// the Software  is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS
// OR IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN
// NO EVENT  SHALL THE AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY
// CLAIM,  DAMAGES OR  OTHER LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef NEARGYE_MAGIC_ENUM_FLAGS_HPP
#define NEARGYE_MAGIC_ENUM_FLAGS_HPP

// magic_enum.hpp is already embedded above.

#if defined(__clang__)
#pragma clang diagnostic push
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored                                                \
    "-Wmaybe-uninitialized" // May be used uninitialized 'return {};'.
#elif defined(_MSC_VER)
#pragma warning(push)
#endif

namespace magic_enum
{

namespace detail
{

template <typename E, enum_subtype S, typename U = std::underlying_type_t<E>>
constexpr U
values_ors () noexcept
{
  static_assert (S == enum_subtype::flags,
                 "magic_enum::detail::values_ors requires valid subtype.");

  auto ors = U{ 0 };
  for (std::size_t i = 0; i < count_v<E, S>; ++i)
    {
      ors |= static_cast<U> (values_v<E, S>[i]);
    }

  return ors;
}

} // namespace magic_enum::detail

// Returns name from enum-flags value.
// If enum-flags value does not have name or value out of range, returns empty
// string.
template <typename E>
[[nodiscard]] auto
enum_flags_name (E value, char_type sep = static_cast<char_type> ('|'))
    -> detail::enable_if_t<E, string>
{
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;
  constexpr auto S = detail::enum_subtype::flags;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  string name;
  auto check_value = U{ 0 };
  for (std::size_t i = 0; i < detail::count_v<D, S>; ++i)
    {
      if (const auto v = static_cast<U> (enum_value<D, S> (i));
          (static_cast<U> (value) & v) != 0)
        {
          if (const auto n = detail::names_v<D, S>[i]; !n.empty ())
            {
              check_value |= v;
              if (!name.empty ())
                {
                  name.append (1, sep);
                }
              name.append (n.data (), n.size ());
            }
          else
            {
              return {}; // Value out of range.
            }
        }
    }

  if (check_value != 0 && check_value == static_cast<U> (value))
    {
      return name;
    }
  return {}; // Invalid value or out of range.
}

// Returns enum-flags value from integer value.
// Returns optional with enum-flags value.
template <typename E>
[[nodiscard]] constexpr auto
enum_flags_cast (underlying_type_t<E> value) noexcept
    -> detail::enable_if_t<E, optional<std::decay_t<E>>>
{
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;
  constexpr auto S = detail::enum_subtype::flags;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  if constexpr (detail::count_v<D, S> == 0)
    {
      static_cast<void> (value);
      return {}; // Empty enum.
    }
  else
    {
      if constexpr (detail::is_sparse_v<D, S>)
        {
          auto check_value = U{ 0 };
          for (std::size_t i = 0; i < detail::count_v<D, S>; ++i)
            {
              if (const auto v = static_cast<U> (enum_value<D, S> (i));
                  (value & v) != 0)
                {
                  check_value |= v;
                }
            }

          if (check_value != 0 && check_value == value)
            {
              return static_cast<D> (value);
            }
        }
      else
        {
          constexpr auto min = detail::min_v<D, S>;
          constexpr auto max = detail::values_ors<D, S> ();

          if (value >= min && value <= max)
            {
              return static_cast<D> (value);
            }
        }
      return {}; // Invalid value or out of range.
    }
}

// Returns enum-flags value from name.
// Returns optional with enum-flags value.
template <typename E, typename BinaryPredicate = std::equal_to<>>
[[nodiscard]] constexpr auto
enum_flags_cast (
    string_view value,
    [[maybe_unused]] char_type sep = static_cast<char_type> ('|'),
    [[maybe_unused]] BinaryPredicate p
    = {}) noexcept (detail::is_nothrow_invocable_v<BinaryPredicate>)
    -> detail::enable_if_t<E, optional<std::decay_t<E>>, BinaryPredicate>
{
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;
  constexpr auto S = detail::enum_subtype::flags;
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

  if constexpr (detail::count_v<D, S> == 0)
    {
      static_cast<void> (value);
      return {}; // Empty enum.
    }
  else
    {
      auto result = U{ 0 };
      while (!value.empty ())
        {
          const auto d = detail::find (value, sep);
          const auto s
              = (d == string_view::npos) ? value : value.substr (0, d);
          auto f = U{ 0 };
          for (std::size_t i = 0; i < detail::count_v<D, S>; ++i)
            {
              if (detail::cmp_equal (s, detail::names_v<D, S>[i], p))
                {
                  f = static_cast<U> (enum_value<D, S> (i));
                  result |= f;
                  break;
                }
            }
          if (f == U{ 0 })
            {
              return {}; // Invalid value or out of range.
            }
          value.remove_prefix ((d == string_view::npos) ? value.size ()
                                                        : d + 1);
        }

      if (result != U{ 0 })
        {
          return static_cast<D> (result);
        }
      return {}; // Invalid value or out of range.
    }
}

// Returns true if enum-flags contains value with such value.
template <typename E>
[[nodiscard]] constexpr auto
enum_flags_contains (E value) noexcept -> detail::enable_if_t<E, bool>
{
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;

  return static_cast<bool> (enum_flags_cast<D> (static_cast<U> (value)));
}

// Returns true if enum-flags contains value with such integer value.
template <typename E>
[[nodiscard]] constexpr auto
enum_flags_contains (underlying_type_t<E> value) noexcept
    -> detail::enable_if_t<E, bool>
{
  using D = std::decay_t<E>;

  return static_cast<bool> (enum_flags_cast<D> (value));
}

// Returns true if enum-flags contains enumerator with such name.
template <typename E, typename BinaryPredicate = std::equal_to<>>
[[nodiscard]] constexpr auto
enum_flags_contains (
    string_view value, char_type sep = static_cast<char_type> ('|'),
    BinaryPredicate p
    = {}) noexcept (detail::is_nothrow_invocable_v<BinaryPredicate>)
    -> detail::enable_if_t<E, bool, BinaryPredicate>
{
  using D = std::decay_t<E>;

  return static_cast<bool> (enum_flags_cast<D> (value, sep, std::move (p)));
}

// Returns true if `flags set` contains `flag`.
// Note: If `flag` equals 0, it returns false, as 0 is not a flag.
template <typename E>
constexpr auto
enum_flags_test (E flags, E flag) noexcept -> detail::enable_if_t<E, bool>
{
  using U = underlying_type_t<E>;

  return static_cast<U> (flag)
         && ((static_cast<U> (flags) & static_cast<U> (flag))
             == static_cast<U> (flag));
}

// Returns true if `lhs flags set` and `rhs flags set` have common flags.
// Note: If `lhs flags set` or `rhs flags set` equals 0, it returns false, as 0
// is not a flag, and therfore cannot have any matching flag.
template <typename E>
constexpr auto
enum_flags_test_any (E lhs, E rhs) noexcept -> detail::enable_if_t<E, bool>
{
  using U = underlying_type_t<E>;

  return (static_cast<U> (lhs) & static_cast<U> (rhs)) != 0;
}

} // namespace magic_enum

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif // NEARGYE_MAGIC_ENUM_FLAGS_HPP
// Embedded upstream header: magic_enum_format.hpp
//  __  __             _        ______                          _____
// |  \/  |           (_)      |  ____|                        / ____|_     _
// | \  / | __ _  __ _ _  ___  | |__   _ __  _   _ _ __ ___   | |   _| |_ _| |_
// | |\/| |/ _` |/ _` | |/ __| |  __| | '_ \| | | | '_ ` _ \  | |  |_   _|_ _|
// | |  | | (_| | (_| | | (__  | |____| | | | |_| | | | | | | | |____|_|   |_|
// |_|  |_|\__,_|\__, |_|\___| |______|_| |_|\__,_|_| |_| |_|  \_____|
//                __/ | https://github.com/Neargye/magic_enum
//               |___/  version 0.9.8
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2019 - 2024 Daniil Goncharov <neargye@gmail.com>.
//
// Permission is hereby  granted, free of charge, to any  person obtaining a
// copy of this software and associated  documentation files (the "Software"),
// to deal in the Software  without restriction, including without  limitation
// the rights to  use, copy,  modify, merge,  publish, distribute,  sublicense,
// and/or  sell copies  of  the Software,  and  to  permit persons  to  whom
// the Software  is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS
// OR IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN
// NO EVENT  SHALL THE AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY
// CLAIM,  DAMAGES OR  OTHER LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef NEARGYE_MAGIC_ENUM_FORMAT_HPP
#define NEARGYE_MAGIC_ENUM_FORMAT_HPP

// magic_enum.hpp is already embedded above.
// magic_enum_flags.hpp is already embedded above.

namespace magic_enum::detail
{

template <typename E,
          std::enable_if_t<std::is_enum_v<std::decay_t<E>>, int> = 0>
std::string
format_as (E e)
{
  using D = std::decay_t<E>;
  static_assert (std::is_same_v<char, magic_enum::string_view::value_type>,
                 "magic_enum::formatter requires string_view::value_type type "
                 "same as char.");
  if constexpr (magic_enum::detail::supported<D>::value)
    {
      if constexpr (magic_enum::detail::subtype_v<D>
                    == magic_enum::detail::enum_subtype::flags)
        {
          if (const auto name = magic_enum::enum_flags_name<D> (e);
              !name.empty ())
            {
              return { name.data (), name.size () };
            }
        }
      else
        {
          if (const auto name = magic_enum::enum_name<D> (e); !name.empty ())
            {
              return { name.data (), name.size () };
            }
        }
    }
  return std::to_string (magic_enum::enum_integer<D> (e));
}

} // namespace magic_enum::format

// LibDSL: formatting is optional on C++20 standard libraries.
#ifndef MAGIC_ENUM_USE_STD_MODULE
#if __has_include(<format>)
#include <format>
#endif
#endif

#if defined(__cpp_lib_format)

#ifndef MAGIC_ENUM_USE_STD_MODULE
#include <format>
#endif

template <typename E>
struct std::formatter<E,
                      std::enable_if_t<std::is_enum_v<std::decay_t<E>>, char>>
    : std::formatter<std::string_view, char>
{
  template <class FormatContext>
  auto
  format (E e, FormatContext &ctx) const
  {
    return std::formatter<std::string_view, char>::format (
        magic_enum::detail::format_as<E> (e), ctx);
  }
};

#endif

#if defined(FMT_VERSION)

template <typename E>
struct fmt::formatter<E,
                      std::enable_if_t<std::is_enum_v<std::decay_t<E>>, char>>
    : fmt::formatter<std::string_view, char>
{
  template <class FormatContext>
  auto
  format (E e, FormatContext &ctx) const
  {
    return fmt::formatter<std::string_view, char>::format (
        magic_enum::detail::format_as<E> (e), ctx);
  }
};

#endif

#endif // NEARGYE_MAGIC_ENUM_FORMAT_HPP
// Embedded upstream header: magic_enum_fuse.hpp
//  __  __             _        ______                          _____
// |  \/  |           (_)      |  ____|                        / ____|_     _
// | \  / | __ _  __ _ _  ___  | |__   _ __  _   _ _ __ ___   | |   _| |_ _| |_
// | |\/| |/ _` |/ _` | |/ __| |  __| | '_ \| | | | '_ ` _ \  | |  |_   _|_ _|
// | |  | | (_| | (_| | | (__  | |____| | | | |_| | | | | | | | |____|_|   |_|
// |_|  |_|\__,_|\__, |_|\___| |______|_| |_|\__,_|_| |_| |_|  \_____|
//                __/ | https://github.com/Neargye/magic_enum
//               |___/  version 0.9.8
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2019 - 2024 Daniil Goncharov <neargye@gmail.com>.
//
// Permission is hereby  granted, free of charge, to any  person obtaining a
// copy of this software and associated  documentation files (the "Software"),
// to deal in the Software  without restriction, including without  limitation
// the rights to  use, copy,  modify, merge,  publish, distribute,  sublicense,
// and/or  sell copies  of  the Software,  and  to  permit persons  to  whom
// the Software  is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS
// OR IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN
// NO EVENT  SHALL THE AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY
// CLAIM,  DAMAGES OR  OTHER LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef NEARGYE_MAGIC_ENUM_FUSE_HPP
#define NEARGYE_MAGIC_ENUM_FUSE_HPP

// magic_enum.hpp is already embedded above.

namespace magic_enum
{

namespace detail
{

template <typename E>
constexpr optional<std::uintmax_t>
fuse_one_enum (optional<std::uintmax_t> hash, E value) noexcept
{
  if (hash)
    {
      if (const auto index = enum_index (value))
        {
          return (*hash << log2 ((enum_count<E> () << 1) - 1)) | *index;
        }
    }
  return {};
}

template <typename E>
constexpr optional<std::uintmax_t>
fuse_enum (E value) noexcept
{
  return fuse_one_enum (0, value);
}

template <typename E, typename... Es>
constexpr optional<std::uintmax_t>
fuse_enum (E head, Es... tail) noexcept
{
  return fuse_one_enum (fuse_enum (tail...), head);
}

template <typename... Es>
constexpr auto
typesafe_fuse_enum (Es... values) noexcept
{
  enum class enum_fuse_t : std::uintmax_t;
  const auto fuse = fuse_enum (values...);
  if (fuse)
    {
      return optional<enum_fuse_t>{ static_cast<enum_fuse_t> (*fuse) };
    }
  return optional<enum_fuse_t>{};
}

} // namespace magic_enum::detail

// Returns a bijective mix of several enum values. This can be used to emulate
// 2D switch/case statements.
template <typename... Es>
[[nodiscard]] constexpr auto
enum_fuse (Es... values) noexcept
{
  static_assert ((std::is_enum_v<std::decay_t<Es>> && ...),
                 "magic_enum::enum_fuse requires enum type.");
  static_assert (sizeof...(Es) >= 2,
                 "magic_enum::enum_fuse requires at least 2 values.");
  static_assert ((detail::log2 (enum_count<std::decay_t<Es>> () + 1) + ...)
                     <= (sizeof (std::uintmax_t) * 8),
                 "magic_enum::enum_fuse does not work for large enums");
#if defined(MAGIC_ENUM_NO_TYPESAFE_ENUM_FUSE)
  const auto fuse = detail::fuse_enum<std::decay_t<Es>...> (values...);
#else
  const auto fuse
      = detail::typesafe_fuse_enum<std::decay_t<Es>...> (values...);
#endif
  return MAGIC_ENUM_ASSERT (fuse), fuse;
}

} // namespace magic_enum

#endif // NEARGYE_MAGIC_ENUM_FUSE_HPP
// Embedded upstream header: magic_enum_iostream.hpp
//  __  __             _        ______                          _____
// |  \/  |           (_)      |  ____|                        / ____|_     _
// | \  / | __ _  __ _ _  ___  | |__   _ __  _   _ _ __ ___   | |   _| |_ _| |_
// | |\/| |/ _` |/ _` | |/ __| |  __| | '_ \| | | | '_ ` _ \  | |  |_   _|_ _|
// | |  | | (_| | (_| | | (__  | |____| | | | |_| | | | | | | | |____|_|   |_|
// |_|  |_|\__,_|\__, |_|\___| |______|_| |_|\__,_|_| |_| |_|  \_____|
//                __/ | https://github.com/Neargye/magic_enum
//               |___/  version 0.9.8
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2019 - 2024 Daniil Goncharov <neargye@gmail.com>.
//
// Permission is hereby  granted, free of charge, to any  person obtaining a
// copy of this software and associated  documentation files (the "Software"),
// to deal in the Software  without restriction, including without  limitation
// the rights to  use, copy,  modify, merge,  publish, distribute,  sublicense,
// and/or  sell copies  of  the Software,  and  to  permit persons  to  whom
// the Software  is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS
// OR IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN
// NO EVENT  SHALL THE AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY
// CLAIM,  DAMAGES OR  OTHER LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef NEARGYE_MAGIC_ENUM_IOSTREAM_HPP
#define NEARGYE_MAGIC_ENUM_IOSTREAM_HPP

// magic_enum.hpp is already embedded above.
// magic_enum_flags.hpp is already embedded above.

#ifndef MAGIC_ENUM_USE_STD_MODULE
#include <iosfwd>
#endif

namespace magic_enum
{

namespace ostream_operators
{

template <typename Char, typename Traits, typename E,
          detail::enable_if_t<E, int> = 0>
std::basic_ostream<Char, Traits> &
operator<< (std::basic_ostream<Char, Traits> &os, E value)
{
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;

  if constexpr (detail::supported<D>::value)
    {
      if constexpr (detail::subtype_v<D> == detail::enum_subtype::flags)
        {
          if (const auto name = enum_flags_name<D> (value); !name.empty ())
            {
              for (const auto c : name)
                {
                  os.put (c);
                }
              return os;
            }
        }
      else
        {
          if (const auto name = enum_name<D> (value); !name.empty ())
            {
              for (const auto c : name)
                {
                  os.put (c);
                }
              return os;
            }
        }
    }
  return (os << static_cast<U> (value));
}

template <typename Char, typename Traits, typename E,
          detail::enable_if_t<E, int> = 0>
std::basic_ostream<Char, Traits> &
operator<< (std::basic_ostream<Char, Traits> &os, optional<E> value)
{
  return value ? (os << *value) : os;
}

} // namespace magic_enum::ostream_operators

namespace istream_operators
{

template <typename Char, typename Traits, typename E,
          detail::enable_if_t<E, int> = 0>
std::basic_istream<Char, Traits> &
operator>> (std::basic_istream<Char, Traits> &is, E &value)
{
  using D = std::decay_t<E>;

  std::basic_string<Char, Traits> s;
  is >> s;
  if constexpr (detail::supported<D>::value)
    {
      if constexpr (detail::subtype_v<D> == detail::enum_subtype::flags)
        {
          if (const auto v = enum_flags_cast<D> (s))
            {
              value = *v;
            }
          else
            {
              is.setstate (std::basic_ios<Char>::failbit);
            }
        }
      else
        {
          if (const auto v = enum_cast<D> (s))
            {
              value = *v;
            }
          else
            {
              is.setstate (std::basic_ios<Char>::failbit);
            }
        }
    }
  else
    {
      is.setstate (std::basic_ios<Char>::failbit);
    }
  return is;
}

} // namespace magic_enum::istream_operators

namespace iostream_operators
{

using magic_enum::ostream_operators::operator<<;
using magic_enum::istream_operators::operator>>;

} // namespace magic_enum::iostream_operators

} // namespace magic_enum

#endif // NEARGYE_MAGIC_ENUM_IOSTREAM_HPP
// Embedded upstream header: magic_enum_switch.hpp
//  __  __             _        ______                          _____
// |  \/  |           (_)      |  ____|                        / ____|_     _
// | \  / | __ _  __ _ _  ___  | |__   _ __  _   _ _ __ ___   | |   _| |_ _| |_
// | |\/| |/ _` |/ _` | |/ __| |  __| | '_ \| | | | '_ ` _ \  | |  |_   _|_ _|
// | |  | | (_| | (_| | | (__  | |____| | | | |_| | | | | | | | |____|_|   |_|
// |_|  |_|\__,_|\__, |_|\___| |______|_| |_|\__,_|_| |_| |_|  \_____|
//                __/ | https://github.com/Neargye/magic_enum
//               |___/  version 0.9.8
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2019 - 2024 Daniil Goncharov <neargye@gmail.com>.
//
// Permission is hereby  granted, free of charge, to any  person obtaining a
// copy of this software and associated  documentation files (the "Software"),
// to deal in the Software  without restriction, including without  limitation
// the rights to  use, copy,  modify, merge,  publish, distribute,  sublicense,
// and/or  sell copies  of  the Software,  and  to  permit persons  to  whom
// the Software  is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS
// OR IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN
// NO EVENT  SHALL THE AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY
// CLAIM,  DAMAGES OR  OTHER LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef NEARGYE_MAGIC_ENUM_SWITCH_HPP
#define NEARGYE_MAGIC_ENUM_SWITCH_HPP

// magic_enum.hpp is already embedded above.

namespace magic_enum
{

namespace detail
{

struct default_result_type
{
};

template <typename T> struct identity
{
  using type = T;
};

struct nonesuch
{
};

template <typename F, typename V, bool = std::is_invocable_v<F, V>>
struct invoke_result : identity<nonesuch>
{
};

template <typename F, typename V>
struct invoke_result<F, V, true> : std::invoke_result<F, V>
{
};

template <typename F, typename V>
using invoke_result_t = typename invoke_result<F, V>::type;

template <typename E, enum_subtype S, typename F, std::size_t... I>
constexpr auto
common_invocable (std::index_sequence<I...>) noexcept
{
  static_assert (std::is_enum_v<E>,
                 "magic_enum::detail::invocable_index requires enum type.");

  if constexpr (count_v<E, S> == 0)
    {
      return identity<nonesuch>{};
    }
  else
    {
      return std::common_type<
          invoke_result_t<F, enum_constant<values_v<E, S>[I]>>...>{};
    }
}

template <typename E, enum_subtype S, typename Result, typename F>
constexpr auto
result_type () noexcept
{
  static_assert (std::is_enum_v<E>,
                 "magic_enum::detail::result_type requires enum type.");

  constexpr auto seq = std::make_index_sequence<count_v<E, S>>{};
  using R = typename decltype (common_invocable<E, S, F> (seq))::type;
  if constexpr (std::is_same_v<Result, default_result_type>)
    {
      if constexpr (std::is_same_v<R, nonesuch>)
        {
          return identity<void>{};
        }
      else
        {
          return identity<R>{};
        }
    }
  else
    {
      if constexpr (std::is_convertible_v<R, Result>)
        {
          return identity<Result>{};
        }
      else if constexpr (std::is_convertible_v<Result, R>)
        {
          return identity<R>{};
        }
      else
        {
          return identity<nonesuch>{};
        }
    }
}

template <typename E, enum_subtype S, typename Result, typename F,
          typename D = std::decay_t<E>,
          typename R =
              typename decltype (result_type<D, S, Result, F> ())::type>
using result_t
    = std::enable_if_t<std::is_enum_v<D> && !std::is_same_v<R, nonesuch>, R>;

#if !defined(MAGIC_ENUM_ENABLE_HASH) && !defined(MAGIC_ENUM_ENABLE_HASH_SWITCH)

template <typename T = void>
inline constexpr auto default_result_type_lambda
    = [] () noexcept (std::is_nothrow_default_constructible_v<T>)
  { return T{}; };

template <>
inline constexpr auto default_result_type_lambda<void> = [] () noexcept {};

template <std::size_t I, std::size_t End, typename R, typename E,
          enum_subtype S, typename F, typename Def>
constexpr decltype (auto)
constexpr_switch_impl (F &&f, E value, Def &&def)
{
  if constexpr (I < End)
    {
      constexpr auto v = enum_constant<enum_value<E, I, S> ()>{};
      if (value == v)
        {
          if constexpr (std::is_invocable_r_v<R, F, decltype (v)>)
            {
              return static_cast<R> (std::forward<F> (f) (v));
            }
          else
            {
              return def ();
            }
        }
      else
        {
          return constexpr_switch_impl<I + 1, End, R, E, S> (
              std::forward<F> (f), value, std::forward<Def> (def));
        }
    }
  else
    {
      return def ();
    }
}

template <typename R, typename E, enum_subtype S, typename F, typename Def>
constexpr decltype (auto)
constexpr_switch (F &&f, E value, Def &&def)
{
  static_assert (is_enum_v<E>,
                 "magic_enum::detail::constexpr_switch requires enum type.");

  if constexpr (count_v<E, S> == 0)
    {
      return def ();
    }
  else
    {
      return constexpr_switch_impl<0, count_v<E, S>, R, E, S> (
          std::forward<F> (f), value, std::forward<Def> (def));
    }
}
#endif

} // namespace magic_enum::detail

template <typename Result = detail::default_result_type, typename E,
          detail::enum_subtype S = detail::subtype_v<E>, typename F,
          typename R = detail::result_t<E, S, Result, F>>
constexpr decltype (auto)
enum_switch (F &&f, E value)
{
  using D = std::decay_t<E>;
  static_assert (std::is_enum_v<D>,
                 "magic_enum::enum_switch requires enum type.");
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

#if defined(MAGIC_ENUM_ENABLE_HASH) || defined(MAGIC_ENUM_ENABLE_HASH_SWITCH)
  return detail::constexpr_switch<&detail::values_v<D, S>,
                                  detail::case_call_t::value> (
      std::forward<F> (f), value, detail::default_result_type_lambda<R>);
#else
  return detail::constexpr_switch<R, D, S> (
      std::forward<F> (f), value, detail::default_result_type_lambda<R>);
#endif
}

template <typename Result = detail::default_result_type,
          detail::enum_subtype S, typename E, typename F,
          typename R = detail::result_t<E, S, Result, F>>
constexpr decltype (auto)
enum_switch (F &&f, E value)
{
  return enum_switch<Result, E, S> (std::forward<F> (f), value);
}

template <typename Result, typename E,
          detail::enum_subtype S = detail::subtype_v<E>, typename F,
          typename R = detail::result_t<E, S, Result, F>>
constexpr decltype (auto)
enum_switch (F &&f, E value, Result &&result)
{
  using D = std::decay_t<E>;
  static_assert (std::is_enum_v<D>,
                 "magic_enum::enum_switch requires enum type.");
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");

#if defined(MAGIC_ENUM_ENABLE_HASH) || defined(MAGIC_ENUM_ENABLE_HASH_SWITCH)
  return detail::constexpr_switch<&detail::values_v<D, S>,
                                  detail::case_call_t::value> (
      std::forward<F> (f), value,
      [&result] () -> R { return std::forward<Result> (result); });
#else
  return detail::constexpr_switch<R, D, S> (
      std::forward<F> (f), value,
      [&result] () -> R { return std::forward<Result> (result); });
#endif
}

template <typename Result, detail::enum_subtype S, typename E, typename F,
          typename R = detail::result_t<E, S, Result, F>>
constexpr decltype (auto)
enum_switch (F &&f, E value, Result &&result)
{
  return enum_switch<Result, E, S> (std::forward<F> (f), value,
                                    std::forward<Result> (result));
}

} // namespace magic_enum

template <>
struct std::common_type<magic_enum::detail::nonesuch,
                        magic_enum::detail::nonesuch>
    : magic_enum::detail::identity<magic_enum::detail::nonesuch>
{
};

template <typename T>
struct std::common_type<T, magic_enum::detail::nonesuch>
    : magic_enum::detail::identity<T>
{
};

template <typename T>
struct std::common_type<magic_enum::detail::nonesuch, T>
    : magic_enum::detail::identity<T>
{
};

#endif // NEARGYE_MAGIC_ENUM_SWITCH_HPP
// Embedded upstream header: magic_enum_utility.hpp
//  __  __             _        ______                          _____
// |  \/  |           (_)      |  ____|                        / ____|_     _
// | \  / | __ _  __ _ _  ___  | |__   _ __  _   _ _ __ ___   | |   _| |_ _| |_
// | |\/| |/ _` |/ _` | |/ __| |  __| | '_ \| | | | '_ ` _ \  | |  |_   _|_ _|
// | |  | | (_| | (_| | | (__  | |____| | | | |_| | | | | | | | |____|_|   |_|
// |_|  |_|\__,_|\__, |_|\___| |______|_| |_|\__,_|_| |_| |_|  \_____|
//                __/ | https://github.com/Neargye/magic_enum
//               |___/  version 0.9.8
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2019 - 2024 Daniil Goncharov <neargye@gmail.com>.
//
// Permission is hereby  granted, free of charge, to any  person obtaining a
// copy of this software and associated  documentation files (the "Software"),
// to deal in the Software  without restriction, including without  limitation
// the rights to  use, copy,  modify, merge,  publish, distribute,  sublicense,
// and/or  sell copies  of  the Software,  and  to  permit persons  to  whom
// the Software  is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS
// OR IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN
// NO EVENT  SHALL THE AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY
// CLAIM,  DAMAGES OR  OTHER LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef NEARGYE_MAGIC_ENUM_UTILITY_HPP
#define NEARGYE_MAGIC_ENUM_UTILITY_HPP

// magic_enum.hpp is already embedded above.

namespace magic_enum
{

namespace detail
{

template <typename E, enum_subtype S, typename F, std::size_t... I>
constexpr auto
for_each (F &&f, std::index_sequence<I...>)
{
  constexpr bool has_void_return
      = (std::is_void_v<
             std::invoke_result_t<F, enum_constant<values_v<E, S>[I]>>>
         || ...);
  constexpr bool all_same_return
      = (std::is_same_v<
             std::invoke_result_t<F, enum_constant<values_v<E, S>[0]>>,
             std::invoke_result_t<F, enum_constant<values_v<E, S>[I]>>>
         && ...);

  if constexpr (has_void_return)
    {
      (f (enum_constant<values_v<E, S>[I]> {}), ...);
    }
  else if constexpr (all_same_return)
    {
      return std::array{ f (enum_constant<values_v<E, S>[I]> {})... };
    }
  else
    {
      return std::tuple{ f (enum_constant<values_v<E, S>[I]> {})... };
    }
}

template <typename E, enum_subtype S, typename F, std::size_t... I>
constexpr bool
all_invocable (std::index_sequence<I...>)
{
  if constexpr (count_v<E, S> == 0)
    {
      return false;
    }
  else
    {
      return (std::is_invocable_v<F, enum_constant<values_v<E, S>[I]>> && ...);
    }
}

} // namespace magic_enum::detail

template <typename E, detail::enum_subtype S = detail::subtype_v<E>,
          typename F, detail::enable_if_t<E, int> = 0>
constexpr auto
enum_for_each (F &&f)
{
  using D = std::decay_t<E>;
  static_assert (std::is_enum_v<D>,
                 "magic_enum::enum_for_each requires enum type.");
  static_assert (
      detail::is_reflected_v<D, S>,
      "magic_enum requires enum implementation and valid max and min.");
  constexpr auto sep = std::make_index_sequence<detail::count_v<D, S>>{};

  if constexpr (detail::all_invocable<D, S, F> (sep))
    {
      return detail::for_each<D, S> (std::forward<F> (f), sep);
    }
  else
    {
      static_assert (
          detail::always_false_v<D>,
          "magic_enum::enum_for_each requires invocable of all enum value.");
    }
}

template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_next_value (E value, std::ptrdiff_t n = 1) noexcept
    -> detail::enable_if_t<E, optional<std::decay_t<E>>>
{
  using D = std::decay_t<E>;
  constexpr std::ptrdiff_t count = detail::count_v<D, S>;

  if (const auto i = enum_index<D, S> (value))
    {
      const std::ptrdiff_t index = (static_cast<std::ptrdiff_t> (*i) + n);
      if (index >= 0 && index < count)
        {
          return enum_value<D, S> (static_cast<std::size_t> (index));
        }
    }
  return {};
}

template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_next_value_circular (E value, std::ptrdiff_t n = 1) noexcept
    -> detail::enable_if_t<E, std::decay_t<E>>
{
  using D = std::decay_t<E>;
  constexpr std::ptrdiff_t count = detail::count_v<D, S>;

  if (const auto i = enum_index<D, S> (value))
    {
      const std::ptrdiff_t index
          = ((((static_cast<std::ptrdiff_t> (*i) + n) % count) + count)
             % count);
      if (index >= 0 && index < count)
        {
          return enum_value<D, S> (static_cast<std::size_t> (index));
        }
    }
  return MAGIC_ENUM_ASSERT (false), value;
}

template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_prev_value (E value, std::ptrdiff_t n = 1) noexcept
    -> detail::enable_if_t<E, optional<std::decay_t<E>>>
{
  using D = std::decay_t<E>;
  constexpr std::ptrdiff_t count = detail::count_v<D, S>;

  if (const auto i = enum_index<D, S> (value))
    {
      const std::ptrdiff_t index = (static_cast<std::ptrdiff_t> (*i) - n);
      if (index >= 0 && index < count)
        {
          return enum_value<D, S> (static_cast<std::size_t> (index));
        }
    }
  return {};
}

template <typename E, detail::enum_subtype S = detail::subtype_v<E>>
[[nodiscard]] constexpr auto
enum_prev_value_circular (E value, std::ptrdiff_t n = 1) noexcept
    -> detail::enable_if_t<E, std::decay_t<E>>
{
  using D = std::decay_t<E>;
  constexpr std::ptrdiff_t count = detail::count_v<D, S>;

  if (const auto i = enum_index<D, S> (value))
    {
      const std::ptrdiff_t index
          = ((((static_cast<std::ptrdiff_t> (*i) - n) % count) + count)
             % count);
      if (index >= 0 && index < count)
        {
          return enum_value<D, S> (static_cast<std::size_t> (index));
        }
    }
  return MAGIC_ENUM_ASSERT (false), value;
}

} // namespace magic_enum

#endif // NEARGYE_MAGIC_ENUM_UTILITY_HPP

#endif // NEARGYE_MAGIC_ENUM_ALL_HPP
// END LIBDSL EMBEDDED MAGIC_ENUM

namespace dsl
{

// ============================================================
//  Enum reflection: magic_enum-compatible public API
// ============================================================

/// Complete magic_enum API, also available directly through ::magic_enum.
namespace enum_reflection = ::magic_enum;
namespace customize = ::magic_enum::customize;
namespace containers = ::magic_enum::containers;
namespace bitwise_operators = ::magic_enum::bitwise_operators;
namespace iostream_operators = ::magic_enum::iostream_operators;
namespace istream_operators = ::magic_enum::istream_operators;
namespace ostream_operators = ::magic_enum::ostream_operators;

using ::magic_enum::as_common;
using ::magic_enum::as_flags;
using ::magic_enum::case_insensitive;
using ::magic_enum::Enum;
using ::magic_enum::enum_cast;
using ::magic_enum::enum_constant;
using ::magic_enum::enum_contains;
using ::magic_enum::enum_count;
using ::magic_enum::enum_entries;
using ::magic_enum::enum_flags_cast;
using ::magic_enum::enum_flags_contains;
using ::magic_enum::enum_flags_name;
using ::magic_enum::enum_flags_test;
using ::magic_enum::enum_flags_test_any;
using ::magic_enum::enum_for_each;
using ::magic_enum::enum_fuse;
using ::magic_enum::enum_index;
using ::magic_enum::enum_integer;
using ::magic_enum::enum_name;
using ::magic_enum::enum_names;
using ::magic_enum::enum_next_value;
using ::magic_enum::enum_next_value_circular;
using ::magic_enum::enum_prev_value;
using ::magic_enum::enum_prev_value_circular;
using ::magic_enum::enum_reflected;
using ::magic_enum::enum_switch;
using ::magic_enum::enum_type_name;
using ::magic_enum::enum_underlying;
using ::magic_enum::enum_value;
using ::magic_enum::enum_values;
using ::magic_enum::is_magic_enum_supported;
using ::magic_enum::is_scoped_enum;
using ::magic_enum::is_scoped_enum_v;
using ::magic_enum::is_unscoped_enum;
using ::magic_enum::is_unscoped_enum_v;
using ::magic_enum::underlying_type;
using ::magic_enum::underlying_type_t;

/// Whether an enum uses flags reflection (false for non-enum types).
template <typename T>
struct is_flags
    : std::bool_constant<
          []
            {
              if constexpr (std::is_enum_v<std::decay_t<T>>)
                return ::magic_enum::detail::subtype_v<std::decay_t<T>>
                       == as_flags<>;
              else
                return false;
            }()>
{
};

template <typename T> inline constexpr bool is_flags_v = is_flags<T>::value;

/**
 * @brief Feature tag for enum-reflection integrations.
 *
 * Like PatternMatch, the feature uses free functions in namespace dsl.
 * All reflection helpers are also usable without a DSL base class.
 *
 *   struct Tokens : dsl::DSL<Tokens, dsl::EnumReflection> {};
 */
struct EnumReflection
{
  template <typename Derived> struct Mixin
  {
  };
};

// ============================================================
//  Utility: fixed-length string for use as NTTP
// ============================================================

/**
 * @brief Compile-time fixed-length string, usable as a non-type template
 * parameter.
 *
 * Allows string literals to be used as template arguments:
 *   dsl::leaf<"myTag">(value)
 *   dsl::pattern<"[0-9]+">()
 *
 * @tparam N Length of the string including null terminator.
 */
template <std::size_t N> struct FixedString
{
  char data[N]{};
  constexpr FixedString (const char (&s)[N]) { std::copy_n (s, N, data); }
  constexpr std::string_view
  view () const
  {
    return { data, N - 1 };
  }
  constexpr bool operator== (const FixedString &) const = default;
};

// ============================================================
//  Forward declarations
// ============================================================

struct Pipeline;
struct Operators;
struct PatternMatch;
struct AST;
struct Rewrite;
struct ExprTemplates;
struct CustomLiterals;
struct Memoization;
struct LazyFeature;
struct Monadic;
struct ResultFeature;
struct CombinatorParser;
struct TaskPipeline;

// ============================================================
//  CRTP base: DSL<Derived, Features...>
// ============================================================

/**
 * @brief CRTP base class for all user-defined DSLs.
 *
 * Inherit from this to compose features:
 *
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::Pipeline, dsl::Operators> { ... };
 *
 * The Derived type gets access to all methods provided by each Feature.
 * Features are mixed in via multiple inheritance through FeatureMixin<>.
 *
 * @tparam Derived  The user's DSL class (CRTP pattern).
 * @tparam Features Zero or more feature tags (Pipeline, Operators, etc.).
 */
template <typename Derived, typename... Features>
struct DSL : Features::template Mixin<Derived>...
{
protected:
  constexpr Derived &
  self () noexcept
  {
    return static_cast<Derived &> (*this);
  }
  constexpr const Derived &
  self () const noexcept
  {
    return static_cast<const Derived &> (*this);
  }
};

// ============================================================
//  Predicate wrapper (used by Operators)
// ============================================================

/**
 * @brief Wraps a callable as a composable predicate.
 *
 * Supports &, |, ! composition.
 *
 * @tparam F Callable type: F(auto) -> bool
 *
 * Example:
 *   auto pos  = dsl::predicate([](int x){ return x > 0; });
 *   auto even = dsl::predicate([](int x){ return x % 2 == 0; });
 *   auto rule = pos & even;
 *   rule(4);   // true
 *   rule(3);   // false (odd)
 *   rule(-2);  // false (negative)
 */
template <typename F> struct Predicate
{
  F fn;
  explicit constexpr Predicate (F f) : fn (std::move (f)) {}

  template <typename T>
  constexpr bool
  operator() (T &&v) const
  {
    return fn (std::forward<T> (v));
  }

  /// Conjunction: both predicates must pass.
  template <typename G>
  constexpr auto
  operator& (Predicate<G> other) const
  {
    return dsl::Predicate{ [a = fn, b = other.fn] (auto &&v)
                        { return a (v) && b (v); } };
  }

  /// Disjunction: at least one predicate must pass.
  template <typename G>
  constexpr auto
  operator| (Predicate<G> other) const
  {
    return dsl::Predicate{ [a = fn, b = other.fn] (auto &&v)
                        { return a (v) || b (v); } };
  }

  /// Negation.
  constexpr auto
  operator!() const
  {
    return dsl::Predicate{ [a = fn] (auto &&v) { return !a (v); } };
  }
};

/**
 * @brief Factory for Predicate<F>.
 *
 * Example:
 *   auto is_even = dsl::predicate([](int x){ return x % 2 == 0; });
 */
template <typename F>
constexpr auto
predicate (F &&f)
{
  return Predicate<std::decay_t<F>>{ std::forward<F> (f) };
}

// ============================================================
//  Pipeline stage wrapper
// ============================================================

/**
 * @brief Wraps a callable as a pipeline stage for use with operator|.
 *
 * @tparam F Callable: F(T) -> U
 *
 * Example:
 *   auto double_it = dsl::pipe([](int x){ return x * 2; });
 *   auto result = 5 | double_it;  // 10
 */
template <typename F> struct PipeStage
{
  F fn;
  explicit constexpr PipeStage (F f) : fn (std::move (f)) {}
};

/**
 * @brief Factory for PipeStage<F>.
 *
 * Example:
 *   auto stage = dsl::pipe([](int x){ return x + 1; });
 *   auto r = 4 | stage;  // 5
 */
template <typename F>
constexpr auto
pipe (F &&f)
{
  return PipeStage<std::decay_t<F>>{ std::forward<F> (f) };
}

/// operator| for any value and a PipeStage.
template <typename T, typename F>
constexpr auto
operator| (T &&val, PipeStage<F> stage)
{
  return stage.fn (std::forward<T> (val));
}

// ============================================================
//  Feature: Pipeline
// ============================================================

/**
 * @brief Feature tag: adds pipeline (operator|) support to a DSL.
 *
 * Provides a wrap() method to lift a value into the pipeline.
 *
 * Usage:
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::Pipeline> {};
 *   MyDSL m;
 *   auto r = m.wrap(10)
 *            | dsl::pipe([](int x){ return x * 2; })
 *            | dsl::pipe([](int x){ return x - 1; });
 *   // r == 19
 */
struct Pipeline
{
  template <typename Derived> struct Mixin
  {
    /**
     * @brief Lifts a value into the pipeline.
     * @param v Value to wrap.
     * @return v unchanged; subsequent | stages transform it.
     */
    template <typename T>
    static constexpr T
    wrap (T &&v)
    {
      return std::forward<T> (v);
    }
  };
};

// ============================================================
//  Feature: Operators
// ============================================================

/**
 * @brief Feature tag: adds predicate composition helpers to a DSL.
 *
 * Provides make_pred() as a named factory inside the DSL.
 *
 * Usage:
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::Operators> {};
 *   auto rule = MyDSL::make_pred([](int x){ return x > 0; })
 *             & MyDSL::make_pred([](int x){ return x < 100; });
 *   rule(42);  // true
 *   rule(200); // false
 */
struct Operators
{
  template <typename Derived> struct Mixin
  {
    /**
     * @brief Creates a composable Predicate from a callable.
     * @param f Callable: f(T) -> bool
     */
    template <typename F>
    static constexpr auto
    make_pred (F &&f)
    {
      return dsl::predicate (std::forward<F> (f));
    }
  };
};

// ============================================================
//  PatternMatch: when / otherwise / match
// ============================================================

/**
 * @brief Compile-time string pattern tag for use with when<>.
 *
 * Wraps a FixedString so it can be used as a template argument
 * to distinguish pattern-based dispatch from value-based dispatch.
 *
 * @tparam S The pattern string (e.g., "[0-9]+").
 *
 * Example:
 *   dsl::when<dsl::pattern<"[0-9]+">>(handler)
 */
template <FixedString S> struct pattern
{
  static constexpr std::string_view value = S.view ();

  /**
   * @brief Minimal regex-like match: anchored prefix/suffix, literal chars,
   * [class], *, +.
   *
   * Supported syntax (subset):
   *   .       any character
   *   [abc]   character class
   *   [a-z]   range in class
   *   *       zero or more of previous
   *   +       one or more of previous
   *   ^       (ignored, match is always anchored at start)
   *   $       (ignored, match is always anchored at end)
   *
   * @param input String to test.
   * @return true if the entire input matches the pattern.
   */
  static constexpr bool
  matches (std::string_view input)
  {
    return match_here (S.view (), input);
  }

private:
  static constexpr bool
  match_class (std::string_view cls, char c)
  {
    for (std::size_t i = 0; i < cls.size ();)
      {
        if (i + 2 < cls.size () && cls[i + 1] == '-')
          {
            if (c >= cls[i] && c <= cls[i + 2])
              return true;
            i += 3;
          }
        else
          {
            if (cls[i] == c)
              return true;
            ++i;
          }
      }
    return false;
  }

  static constexpr bool
  match_one (char pat, std::string_view cls, char c)
  {
    if (pat == '[')
      return match_class (cls, c);
    if (pat == '.')
      return true;
    return pat == c;
  }

  static constexpr bool
  match_here (std::string_view pat, std::string_view str)
  {
    if (pat.empty ())
      return str.empty ();
    if (pat[0] == '^')
      return match_here (pat.substr (1), str);
    if (pat == "$")
      return str.empty ();

    // Parse class [...]
    std::string_view cls;
    std::size_t pat_advance = 1;
    char first = pat[0];
    if (first == '[')
      {
        auto end = pat.find (']', 1);
        if (end == std::string_view::npos)
          return false;
        cls = pat.substr (1, end - 1);
        pat_advance = end + 1;
      }

    // Check for quantifier
    char quant = 0;
    if (pat_advance < pat.size ()
        && (pat[pat_advance] == '*' || pat[pat_advance] == '+'))
      quant = pat[pat_advance++];

    auto rest = pat.substr (pat_advance);

    if (quant == '*')
      {
        // Try matching zero occurrences first, then one or more
        if (match_here (rest, str))
          return true;
        while (!str.empty () && match_one (first, cls, str[0]))
          {
            str.remove_prefix (1);
            if (match_here (rest, str))
              return true;
          }
        return false;
      }
    if (quant == '+')
      {
        // One or more
        if (str.empty () || !match_one (first, cls, str[0]))
          return false;
        str.remove_prefix (1);
        if (match_here (rest, str))
          return true;
        while (!str.empty () && match_one (first, cls, str[0]))
          {
            str.remove_prefix (1);
            if (match_here (rest, str))
              return true;
          }
        return false;
      }

    // Exactly one
    if (str.empty () || !match_one (first, cls, str[0]))
      return false;
    return match_here (rest, str.substr (1));
  }
};

// --- when<Key> clause ---

/**
 * @brief A single dispatch clause for dsl::match().
 *
 * Two specializations:
 *   when<EnumOrIntegralValue>(handler)  — value-based dispatch
 *   when<dsl::pattern<"...">>(handler)  — pattern-based dispatch
 *
 * @tparam Key  The value or pattern to match against.
 * @tparam F    Handler callable.
 */
template <auto Key, typename F> struct WhenClause
{
  F handler;
  using handler_type_or_void = F;
  explicit constexpr WhenClause (F f) : handler (std::move (f)) {}

  template <typename K, typename R, typename... Args>
  constexpr bool
  try_invoke (K key, std::optional<R> &out,
              Args &&...args) const
  {
    auto invoke = [&]
      {
        if constexpr (std::is_void_v<
                          std::invoke_result_t<const F &, K &, Args...>>)
          {
            std::invoke (handler, key, std::forward<Args> (args)...);
            out.emplace ();
          }
        else
          out.emplace (
              std::invoke (handler, key, std::forward<Args> (args)...));
      };
    using KeyT = std::remove_cvref_t<decltype (Key)>;
    if constexpr (requires { KeyT::matches (std::string_view{}); })
      {
        if constexpr (std::convertible_to<K, std::string_view>)
          {
            if (KeyT::matches (std::string_view (key)))
              {
                invoke ();
                return true;
              }
          }
      }
    else if constexpr (std::equality_comparable_with<K, decltype (Key)>)
      {
        if (key == Key)
          {
            invoke ();
            return true;
          }
      }
    return false;
  }
};

/**
 * @brief Factory for a when<Key> clause.
 *
 * Example (value-based):
 *   dsl::when<Color::Red>([](auto key){ return "red"; })
 *
 * Example (pattern-based):
 *   dsl::when<dsl::pattern<"[0-9]+">>(handler)
 */
template <auto Key, typename F>
constexpr auto
when (F &&f)
{
  return WhenClause<Key, std::decay_t<F>>{ std::forward<F> (f) };
}

/// Type-based pattern syntax: when<pattern<"...">>(handler).
template <typename Pattern, typename F>
  requires requires { Pattern::matches (std::string_view{}); }
constexpr auto
when (F &&f)
{
  return when<Pattern{}> (std::forward<F> (f));
}

// --- otherwise clause ---

/**
 * @brief Fallback clause for dsl::match() when no when<> matches.
 *
 * @tparam F Handler callable.
 *
 * Example:
 *   dsl::otherwise([](auto key){ return "unknown"; })
 */
template <typename F> struct OtherwiseClause
{
  F handler;
  using handler_type_or_void = F;
  explicit constexpr OtherwiseClause (F f) : handler (std::move (f)) {}

  template <typename K, typename R, typename... Args>
  constexpr bool
  try_invoke (K, std::optional<R> &,
              Args &&...) const
  {
    return false;
  }
};

/**
 * @brief Factory for an otherwise clause.
 */
template <typename F>
constexpr auto
otherwise (F &&f)
{
  return OtherwiseClause<std::decay_t<F>>{ std::forward<F> (f) };
}

// --- MatchTable ---

/**
 * @brief Dispatch table built from when<>/otherwise clauses.
 *
 * Created by dsl::match(). Call operator()(key, args...) to dispatch.
 * Handlers may all return void, or values with a common result type.
 *
 * @tparam Clauses  Tuple of WhenClause and OtherwiseClause types.
 *
 * Example:
 *   auto table = dsl::match(
 *       dsl::when<1>([](auto){ return "one"; }),
 *       dsl::when<2>([](auto){ return "two"; }),
 *       dsl::otherwise([](auto){ return "other"; })
 *   );
 *   table(1);  // "one"
 *   table(99); // "other"
 */
template <typename... Clauses> struct MatchTable
{
  std::tuple<Clauses...> clauses;
  explicit constexpr MatchTable (Clauses... cs) : clauses (std::move (cs)...)
  {
  }

  template <typename Key, typename... Args>
  constexpr auto
  operator() (Key key, Args &&...args) const
  {
    using RetT = std::common_type_t<std::invoke_result_t<
        const typename Clauses::handler_type_or_void &, Key &, Args...>...>;
    // Try each when<> clause in order
    using StoredT
        = std::conditional_t<std::is_void_v<RetT>, std::monostate, RetT>;
    std::optional<StoredT> result;
    bool has_otherwise = false;
    bool matched = std::apply (
        [&] (const auto &...c)
          {
            return (c.try_invoke (key, result, std::forward<Args> (args)...)
                    || ...);
          },
        clauses);
    if (!matched)
      {
        // Find and invoke the otherwise clause
        std::apply (
            [&] (const auto &...c)
              {
                ((
                     [&]
                       {
                         using Cls = std::remove_cvref_t<decltype (c)>;
                         if constexpr (std::is_same_v<
                                           Cls, OtherwiseClause<
                                                    typename Cls::
                                                        handler_type_or_void>>)
                           {
                             has_otherwise = true;
                             if (!result)
                               {
                                 if constexpr (std::is_void_v<RetT>)
                                   {
                                     std::invoke (
                                         c.handler, key,
                                         std::forward<Args> (args)...);
                                     result.emplace ();
                                   }
                                 else
                                   result.emplace (std::invoke (
                                       c.handler, key,
                                       std::forward<Args> (args)...));
                               }
                           }
                       }()),
                 ...);
              },
            clauses);
      }
    if (!result && !has_otherwise)
      throw std::runtime_error (
          "dsl::match: no clause matched and no otherwise");
    if constexpr (!std::is_void_v<RetT>)
      return std::move (*result);
  }
};

// Deduction guide
template <typename... Cs> MatchTable (Cs...) -> MatchTable<Cs...>;

/**
 * @brief Builds a MatchTable from when<>/otherwise clauses.
 *
 * Example:
 *   auto t = dsl::match(
 *       dsl::when<0>([](auto){ return "zero"; }),
 *       dsl::otherwise([](auto){ return "nonzero"; })
 *   );
 *   t(0);  // "zero"
 *   t(5);  // "nonzero"
 */
template <typename... Clauses>
constexpr auto
match (Clauses &&...cs)
{
  return MatchTable<std::decay_t<Clauses>...>{ std::forward<Clauses> (cs)... };
}

// ============================================================
//  Feature: PatternMatch
// ============================================================

/**
 * @brief Feature tag: adds pattern matching helpers to a DSL.
 *
 * Exposes dsl::match / dsl::when / dsl::otherwise as static methods
 * on the derived class for convenience.
 *
 * Usage:
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::PatternMatch> {
 *       static constexpr auto table = dsl::match(
 *           dsl::when<1>([](auto){ return "one"; }),
 *           dsl::otherwise([](auto){ return "?"; })
 *       );
 *   };
 */
struct PatternMatch
{
  template <typename Derived> struct Mixin
  {
    // Feature presence marker; no additional methods needed since
    // dsl::match/when/otherwise are free functions in the dsl:: namespace.
  };
};

// ============================================================
//  AST: leaf / node / ASTNode
// ============================================================

/**
 * @brief A value-type AST node.
 *
 * Each node has a tag (compile-time string) and either:
 *   - A leaf value (string representation), or
 *   - A list of child nodes.
 *
 * Nodes are copyable, movable, and support dump() for S-expression output.
 */
class ASTNode
{
public:
  ASTNode () : tag_ (""), is_leaf_ (true) {}

  /// Construct a leaf node.
  ASTNode (std::string_view tag, std::string value)
      : tag_ (tag), value_ (std::move (value)), is_leaf_ (true)
  {
  }

  /// Construct an inner node with children.
  ASTNode (std::string_view tag, std::vector<ASTNode> children)
      : tag_ (tag), children_ (std::move (children)), is_leaf_ (false)
  {
  }

  /// @return The tag string of this node.
  std::string_view
  tag () const
  {
    return tag_;
  }

  /// @return true if this is a leaf (no children).
  bool
  is_leaf () const
  {
    return is_leaf_;
  }

  /// @return The leaf value (empty string if not a leaf).
  const std::string &
  value () const
  {
    return value_;
  }

  /// @return The children of this node (empty if leaf).
  const std::vector<ASTNode> &
  children () const
  {
    return children_;
  }

  /// @return Mutable reference to children.
  std::vector<ASTNode> &
  children ()
  {
    return children_;
  }

  /// @return The i-th child. Throws if out of range.
  const ASTNode &
  child (std::size_t i) const
  {
    return children_.at (i);
  }
  ASTNode &
  child (std::size_t i)
  {
    return children_.at (i);
  }

  /// @return Number of children.
  std::size_t
  size () const
  {
    return children_.size ();
  }

  /**
   * @brief Produces an S-expression string representation.
   *
   * Leaf:  "(tag value)"
   * Inner: "(tag child1 child2 ...)"
   *
   * Example:
   *   node<"add">(leaf<"num">(1), leaf<"num">(2)).dump()
   *   // => "(add (num 1) (num 2))"
   */
  std::string
  dump () const
  {
    std::ostringstream os;
    dump_impl (os);
    return os.str ();
  }

  bool
  operator== (const ASTNode &other) const
  {
    if (tag_ != other.tag_ || is_leaf_ != other.is_leaf_)
      return false;
    if (is_leaf_)
      return value_ == other.value_;
    return children_ == other.children_;
  }

private:
  void
  dump_impl (std::ostringstream &os) const
  {
    os << '(' << tag_;
    if (is_leaf_)
      {
        if (!value_.empty ())
          os << ' ' << value_;
      }
    else
      {
        for (auto &c : children_)
          {
            os << ' ';
            c.dump_impl (os);
          }
      }
    os << ')';
  }

  std::string tag_;
  std::string value_;
  std::vector<ASTNode> children_;
  bool is_leaf_ = true;
};

/**
 * @brief Creates a leaf AST node with the given tag and value.
 *
 * @tparam Tag Compile-time string tag.
 * @param val  The value (converted to string via ostringstream).
 * @return ASTNode leaf.
 *
 * Example:
 *   auto n = dsl::leaf<"num">(42);
 *   n.dump();  // "(num 42)"
 */
template <FixedString Tag, typename T>
ASTNode
leaf (T &&val)
{
  std::ostringstream os;
  os << std::forward<T> (val);
  return ASTNode{ Tag.view (), os.str () };
}

/// Overload for string_view values (avoids extra stream).
template <FixedString Tag>
ASTNode
leaf (std::string_view val)
{
  return ASTNode{ Tag.view (), std::string (val) };
}

/// Overload for const char* values.
template <FixedString Tag>
ASTNode
leaf (const char *val)
{
  return ASTNode{ Tag.view (), std::string (val) };
}

/**
 * @brief Creates an inner AST node with the given tag and children.
 *
 * @tparam Tag Compile-time string tag.
 * @param children  Variadic list of ASTNode children.
 * @return ASTNode inner node.
 *
 * Example:
 *   auto n = dsl::node<"add">(dsl::leaf<"x">(1), dsl::leaf<"y">(2));
 *   n.dump();  // "(add (x 1) (y 2))"
 */
template <FixedString Tag, typename... Children>
  requires (std::convertible_to<Children, ASTNode> && ...)
ASTNode
node (Children &&...children)
{
  std::vector<ASTNode> v;
  v.reserve (sizeof...(children));
  (v.emplace_back (std::forward<Children> (children)), ...);
  return ASTNode{ Tag.view (), std::move (v) };
}

// ============================================================
//  Feature: AST
// ============================================================

/**
 * @brief Feature tag: adds AST construction helpers to a DSL.
 *
 * Provides make_leaf() and make_node() as member functions.
 *
 * Usage:
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::AST> {};
 *   MyDSL m;
 *   auto tree = dsl::node<"root">(dsl::leaf<"val">(42));
 */
struct AST
{
  template <typename Derived> struct Mixin
  {
    /**
     * @brief Creates a leaf node (convenience wrapper).
     */
    template <FixedString Tag, typename T>
    static ASTNode
    make_leaf (T &&val)
    {
      return dsl::leaf<Tag> (std::forward<T> (val));
    }

    /**
     * @brief Creates an inner node (convenience wrapper).
     */
    template <FixedString Tag, typename... Children>
      requires (std::convertible_to<Children, ASTNode> && ...)
    static ASTNode
    make_node (Children &&...children)
    {
      return dsl::node<Tag> (std::forward<Children> (children)...);
    }
  };
};

// ============================================================
//  Rewrite: rule / rewrite_set
// ============================================================

/**
 * @brief A single rewrite rule: predicate + transformer.
 *
 * @tparam Tag   Name of the rule (for debugging/logging).
 * @tparam Pred  Callable: Pred(const ASTNode&) -> bool
 * @tparam Trans Callable: Trans(const ASTNode&) -> ASTNode
 *
 * Example:
 *   auto r = dsl::rule<"elim-double-neg">(
 *       [](const ASTNode& n){ return n.tag()=="not" &&
 * n.child(0).tag()=="not"; },
 *       [](const ASTNode& n){ return n.child(0).child(0); }
 *   );
 */
template <FixedString Tag, typename Pred, typename Trans> struct RewriteRule
{
  Pred pred;
  Trans trans;
  static constexpr std::string_view name = Tag.view ();

  constexpr RewriteRule (Pred p, Trans t)
      : pred (std::move (p)), trans (std::move (t))
  {
  }

  /**
   * @brief Attempts to apply this rule to a node.
   * @param n The node to test and potentially transform.
   * @return The transformed node if pred matched, or std::nullopt.
   */
  std::optional<ASTNode>
  try_apply (const ASTNode &n) const
  {
    if (pred (n))
      return trans (n);
    return std::nullopt;
  }
};

/**
 * @brief Factory for RewriteRule.
 *
 * @tparam Tag  Rule name (FixedString NTTP).
 * @param pred  Predicate: pred(const ASTNode&) -> bool
 * @param trans Transformer: trans(const ASTNode&) -> ASTNode
 */
template <FixedString Tag, typename Pred, typename Trans>
constexpr auto
rule (Pred &&p, Trans &&t)
{
  return RewriteRule<Tag, std::decay_t<Pred>, std::decay_t<Trans>>{
    std::forward<Pred> (p), std::forward<Trans> (t)
  };
}

/**
 * @brief A set of rewrite rules applied repeatedly until fixpoint.
 *
 * @tparam Rules  Variadic list of RewriteRule types.
 *
 * Example:
 *   auto rules = dsl::rewrite_set(
 *       dsl::rule<"r1">(pred1, trans1),
 *       dsl::rule<"r2">(pred2, trans2)
 *   );
 *   auto optimized = rules.apply(tree);
 */
template <typename... Rules> struct RewriteSet
{
  std::tuple<Rules...> rules;
  std::size_t max_iterations = 100;

  explicit constexpr RewriteSet (Rules... rs) : rules (std::move (rs)...) {}

  /**
   * @brief Applies all rules bottom-up until no rule fires or max iterations
   * reached.
   * @param root The AST to rewrite.
   * @return The rewritten AST.
   */
  ASTNode
  apply (ASTNode root) const
  {
    for (std::size_t i = 0; i < max_iterations; ++i)
      {
        auto [changed, result] = rewrite_once (root);
        if (!changed)
          return result;
        root = std::move (result);
      }
    return root;
  }

private:
  /// Single bottom-up pass: rewrite children first, then try rules on the
  /// node.
  std::pair<bool, ASTNode>
  rewrite_once (const ASTNode &n) const
  {
    bool any_changed = false;

    // Recursively rewrite children
    std::vector<ASTNode> new_children;
    if (!n.is_leaf ())
      {
        new_children.reserve (n.children ().size ());
        for (auto &child : n.children ())
          {
            auto [changed, rewritten] = rewrite_once (child);
            any_changed |= changed;
            new_children.push_back (std::move (rewritten));
          }
      }

    ASTNode current
        = n.is_leaf () ? n : ASTNode{ n.tag (), std::move (new_children) };

    // Try each rule on the current node
    std::apply (
        [&] (const auto &...r)
          {
            (
                [&]
                  {
                    if (auto result = r.try_apply (current))
                      {
                        current = std::move (*result);
                        any_changed = true;
                      }
                  }(),
                ...);
          },
        rules);

    return { any_changed, current };
  }
};

/**
 * @brief Factory for RewriteSet.
 *
 * Example:
 *   auto rs = dsl::rewrite_set(rule1, rule2, rule3);
 *   auto tree2 = rs.apply(tree1);
 */
template <typename... Rules>
constexpr auto
rewrite_set (Rules &&...rs)
{
  return RewriteSet<std::decay_t<Rules>...>{ std::forward<Rules> (rs)... };
}

// ============================================================
//  Feature: Rewrite
// ============================================================

/**
 * @brief Feature tag: adds rewrite capabilities to a DSL.
 *
 * Provides a rewrite() convenience method.
 *
 * Usage:
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::AST, dsl::Rewrite> {
 *       static inline auto rules = dsl::rewrite_set(...);
 *   };
 *   MyDSL m;
 *   auto optimized = m.rewrite(tree);
 */
struct Rewrite
{
  template <typename Derived> struct Mixin
  {
    /**
     * @brief Applies the Derived::rules rewrite set to the given tree.
     * @param tree The AST to rewrite.
     * @return Rewritten AST.
     *
     * Requires that Derived defines a static `rules` member of type
     * RewriteSet.
     */
    ASTNode
    rewrite (const ASTNode &tree) const
      requires requires { Derived::rules; }
    {
      return Derived::rules.apply (tree);
    }
  };
};

// ============================================================
//  ExprTemplates: lazy expression trees
// ============================================================

/**
 * @brief Tag type indicating a terminal (leaf) in an expression template tree.
 */
struct ExprTerminal
{
};

/**
 * @brief Binary expression node in an expression template tree.
 *
 * @tparam Op  Operation tag (e.g., std::plus<>, std::multiplies<>).
 * @tparam L   Left operand type.
 * @tparam R   Right operand type.
 *
 * Evaluation is deferred until .eval() is called.
 */
template <typename Op, typename L, typename R> struct BinExpr
{
  L lhs;
  R rhs;

  constexpr BinExpr (L l, R r) : lhs (std::move (l)), rhs (std::move (r)) {}

  /**
   * @brief Evaluates the expression tree recursively.
   *
   * For terminal nodes, returns the stored value.
   * For binary nodes, evaluates both sides and applies Op.
   */
  constexpr auto
  eval () const
  {
    auto l = eval_operand (lhs);
    auto r = eval_operand (rhs);
    return Op{}(l, r);
  }

private:
  template <typename T>
  static constexpr auto
  eval_operand (const T &operand)
  {
    if constexpr (requires { operand.eval (); })
      {
        return operand.eval ();
      }
    else if constexpr (requires { operand.expr_value (); })
      {
        return operand.expr_value ();
      }
    else
      {
        return operand;
      }
  }
};

/**
 * @brief Unary expression node.
 *
 * @tparam Op  Operation tag (e.g., std::negate<>).
 * @tparam E   Operand type.
 */
template <typename Op, typename E> struct UnaryExpr
{
  E operand;

  explicit constexpr UnaryExpr (E e) : operand (std::move (e)) {}

  constexpr auto
  eval () const
  {
    if constexpr (requires { operand.eval (); })
      {
        return Op{}(operand.eval ());
      }
    else if constexpr (requires { operand.expr_value (); })
      {
        return Op{}(operand.expr_value ());
      }
    else
      {
        return Op{}(operand);
      }
  }
};

// ============================================================
//  Feature: ExprTemplates
// ============================================================

/**
 * @brief Feature tag: adds expression template operators to a DSL.
 *
 * The derived class must provide an expr_value() method that returns
 * the underlying value for evaluation.
 *
 * Provides: operator+, operator-, operator*, operator/ (binary)
 *           operator- (unary negation)
 *
 * Usage:
 *   struct Scalar : dsl::DSL<Scalar, dsl::ExprTemplates> {
 *       double val;
 *       double expr_value() const { return val; }
 *   };
 *   Scalar a{3.0}, b{4.0};
 *   auto expr = a + b * a;   // lazy: no computation
 *   double r = expr.eval();  // 3.0 + 4.0*3.0 = 15.0
 */
struct ExprTemplates
{
  template <typename Derived> struct Mixin
  {
    /// operator+ (binary)
    template <typename R>
    friend constexpr auto
    operator+ (const Derived &lhs, const R &rhs)
    {
      return BinExpr<std::plus<>, Derived, R>{ lhs, rhs };
    }

    /// operator- (binary)
    template <typename R>
    friend constexpr auto
    operator- (const Derived &lhs, const R &rhs)
    {
      return BinExpr<std::minus<>, Derived, R>{ lhs, rhs };
    }

    /// operator* (binary)
    template <typename R>
    friend constexpr auto
    operator* (const Derived &lhs, const R &rhs)
    {
      return BinExpr<std::multiplies<>, Derived, R>{ lhs, rhs };
    }

    /// operator/ (binary)
    template <typename R>
    friend constexpr auto
    operator/ (const Derived &lhs, const R &rhs)
    {
      return BinExpr<std::divides<>, Derived, R>{ lhs, rhs };
    }

    /// Unary negation
    friend constexpr auto
    operator- (const Derived &e)
    {
      return UnaryExpr<std::negate<>, Derived>{ e };
    }
  };
};

// Also allow BinExpr on the left side of further operations:

template <typename Op1, typename L1, typename R1, typename R>
constexpr auto
operator+ (const BinExpr<Op1, L1, R1> &lhs, const R &rhs)
{
  return BinExpr<std::plus<>, BinExpr<Op1, L1, R1>, R>{ lhs, rhs };
}

template <typename Op1, typename L1, typename R1, typename R>
constexpr auto
operator- (const BinExpr<Op1, L1, R1> &lhs, const R &rhs)
{
  return BinExpr<std::minus<>, BinExpr<Op1, L1, R1>, R>{ lhs, rhs };
}

template <typename Op1, typename L1, typename R1, typename R>
constexpr auto
operator* (const BinExpr<Op1, L1, R1> &lhs, const R &rhs)
{
  return BinExpr<std::multiplies<>, BinExpr<Op1, L1, R1>, R>{ lhs, rhs };
}

template <typename Op1, typename L1, typename R1, typename R>
constexpr auto
operator/ (const BinExpr<Op1, L1, R1> &lhs, const R &rhs)
{
  return BinExpr<std::divides<>, BinExpr<Op1, L1, R1>, R>{ lhs, rhs };
}

template <typename Op, typename L, typename R>
constexpr auto
operator- (const BinExpr<Op, L, R> &expr)
{
  return UnaryExpr<std::negate<>, BinExpr<Op, L, R>>{ expr };
}

template <typename Op, typename E>
constexpr auto
operator- (const UnaryExpr<Op, E> &expr)
{
  return UnaryExpr<std::negate<>, UnaryExpr<Op, E>>{ expr };
}

template <typename Op, typename E, typename R>
constexpr auto
operator+ (const UnaryExpr<Op, E> &lhs, const R &rhs)
{
  return BinExpr<std::plus<>, UnaryExpr<Op, E>, R>{ lhs, rhs };
}

template <typename Op, typename E, typename R>
constexpr auto
operator- (const UnaryExpr<Op, E> &lhs, const R &rhs)
{
  return BinExpr<std::minus<>, UnaryExpr<Op, E>, R>{ lhs, rhs };
}

template <typename Op, typename E, typename R>
constexpr auto
operator* (const UnaryExpr<Op, E> &lhs, const R &rhs)
{
  return BinExpr<std::multiplies<>, UnaryExpr<Op, E>, R>{ lhs, rhs };
}

template <typename Op, typename E, typename R>
constexpr auto
operator/ (const UnaryExpr<Op, E> &lhs, const R &rhs)
{
  return BinExpr<std::divides<>, UnaryExpr<Op, E>, R>{ lhs, rhs };
}

// ============================================================
//  CustomLiterals: literal suffix registration
// ============================================================

/**
 * @brief A single literal suffix entry.
 *
 * @tparam Suffix  The suffix string (e.g., "_km").
 * @tparam F       Callable: F(long double) -> T
 */
template <FixedString Suffix, typename F> struct LiteralEntry
{
  F handler;
  static constexpr std::string_view suffix = Suffix.view ();

  explicit constexpr LiteralEntry (F f) : handler (std::move (f)) {}
};

/**
 * @brief Factory for LiteralEntry.
 *
 * @tparam Suffix  The suffix string (FixedString NTTP).
 * @param f        Handler: f(long double) -> T
 *
 * Example:
 *   dsl::lit<"_km">([](long double v){ return v * 1000.0L; })
 */
template <FixedString Suffix, typename F>
constexpr auto
lit (F &&f)
{
  return LiteralEntry<Suffix, std::decay_t<F>>{ std::forward<F> (f) };
}

/**
 * @brief A set of literal suffix entries for runtime parsing.
 *
 * @tparam Entries  Variadic list of LiteralEntry types.
 *
 * Example:
 *   constexpr auto lits = dsl::literal_set(
 *       dsl::lit<"_km">([](long double v){ return v * 1000.0L; }),
 *       dsl::lit<"_m"> ([](long double v){ return v; })
 *   );
 *   auto val = lits.parse("3.5_km");  // 3500.0L
 */
template <typename... Entries> struct LiteralSet
{
  std::tuple<Entries...> entries;

  explicit constexpr LiteralSet (Entries... es) : entries (std::move (es)...)
  {
  }

  /**
   * @brief Parses a string like "3.5_km" by finding the matching suffix.
   *
   * @param input  The full literal string (e.g., "3.5_km").
   * @return The result of applying the matched handler to the numeric part.
   * @throws std::runtime_error if no suffix matches.
   */
  long double
  parse (std::string_view input) const
  {
    long double result = 0.0L;
    bool found = false;

    std::apply (
        [&] (const auto &...e)
          {
            (
                [&]
                  {
                    if (found)
                      return;
                    auto suf = e.suffix;
                    if (input.size () > suf.size ()
                        && input.substr (input.size () - suf.size ()) == suf)
                      {
                        auto num_str
                            = input.substr (0, input.size () - suf.size ());
                        long double num = std::stold (std::string (num_str));
                        result = e.handler (num);
                        found = true;
                      }
                  }(),
                ...);
          },
        entries);

    if (!found)
      {
        throw std::runtime_error (
            std::string ("dsl::LiteralSet::parse: no suffix matched for '")
            + std::string (input) + "'");
      }
    return result;
  }
};

/**
 * @brief Factory for LiteralSet.
 *
 * Example:
 *   auto ls = dsl::literal_set(
 *       dsl::lit<"_km">([](long double v){ return v * 1000.0L; }),
 *       dsl::lit<"_cm">([](long double v){ return v / 100.0L; })
 *   );
 */
template <typename... Entries>
constexpr auto
literal_set (Entries &&...es)
{
  return LiteralSet<std::decay_t<Entries>...>{ std::forward<Entries> (es)... };
}

// ============================================================
//  Feature: CustomLiterals
// ============================================================

/**
 * @brief Feature tag: adds literal parsing support to a DSL.
 *
 * The derived class must define a static `literals` member of type LiteralSet.
 *
 * Usage:
 *   struct UnitsDSL : dsl::DSL<UnitsDSL, dsl::CustomLiterals> {
 *       static constexpr auto literals = dsl::literal_set(
 *           dsl::lit<"_km">([](long double v){ return v * 1000.0L; }),
 *           dsl::lit<"_m"> ([](long double v){ return v; })
 *       );
 *   };
 *   auto val = UnitsDSL::parse_literal("5.0_km");  // 5000.0
 */
struct CustomLiterals
{
  template <typename Derived> struct Mixin
  {
    /**
     * @brief Parses a literal string using the Derived::literals set.
     * @param input  String like "3.5_km".
     * @return Parsed value.
     */
    static long double
    parse_literal (std::string_view input)
      requires requires { Derived::literals; }
    {
      return Derived::literals.parse (input);
    }
  };
};

/**
 * @brief Feature tag: adds cache invalidation helper for memoized DSL state.
 *
 * The derived DSL is expected to define `memo_cache` (typically an
 * unordered_map). clear_cache() erases all entries. Thread-safety is
 * intentionally not provided.
 */
struct Memoization
{
  template <typename Derived> struct Mixin
  {
    /// Clears all cached entries stored in `Derived::memo_cache`.
    void
    clear_cache ()
    {
      static_cast<Derived &> (*this).memo_cache.clear ();
    }
  };
};
/** @brief Feature tag for lazy-evaluation related DSL integrations. */
struct LazyFeature
{
  template <typename Derived> struct Mixin
  {
  };
};
/** @brief Feature tag for monadic combinator integrations. */
struct Monadic
{
  template <typename Derived> struct Mixin
  {
  };
};
/** @brief Feature tag for Result-based error-flow integrations. */
struct ResultFeature
{
  template <typename Derived> struct Mixin
  {
  };
};
/** @brief Feature tag for combinatory parser integrations. */
struct CombinatorParser
{
  template <typename Derived> struct Mixin
  {
  };
};
/** @brief Feature tag for task-pipeline integrations. */
struct TaskPipeline
{
  template <typename Derived> struct Mixin
  {
  };
};

/**
 * @brief Wraps a callable with a memoization cache keyed by the first
 * argument.
 *
 * This helper is suitable for expensive pure functions where repeated calls
 * with the same key should return cached results. The key type must be
 * hashable by `std::hash<Key>`. This helper is intentionally not thread-safe.
 *
 * @tparam Key Cache key type.
 * @tparam Value Cached value type.
 * @tparam Fn Callable type with signature compatible with `Value(const Key&)`.
 * @param fn Callable to memoize.
 * @return Callable object that performs memoized lookup.
 *
 * Example:
 *   auto fib = dsl::memoize<int, long long>([](int n){ return n < 2 ? n : 1LL;
 * });
 */
template <typename Key, typename Value, typename Fn> class MemoizedCallable
{
public:
  /// Constructs a memoized callable wrapper.
  explicit MemoizedCallable (Fn fn) : fn_ (std::move (fn)) {}
  /// Evaluates or returns cached value.
  Value
  operator() (const Key &key)
  {
    if (auto it = cache_.find (key); it != cache_.end ())
      return it->second;
    auto value = fn_ (key);
    cache_.emplace (key, value);
    return value;
  }
  /// Clears the memoization cache.
  void
  clear_cache ()
  {
    cache_.clear ();
  }

private:
  Fn fn_;
  std::unordered_map<Key, Value> cache_{};
};
template <typename Key, typename Value, typename Fn>
auto
memoize (Fn &&fn)
{
  return MemoizedCallable<Key, Value, std::decay_t<Fn>>{ std::forward<Fn> (
      fn) };
}

/**
 * @brief Deferred single-shot computation wrapper.
 *
 * Lazy<T> stores a callable and evaluates it only on first access through
 * get()/force(). The computed value is cached for subsequent reads.
 * Unlike ExprTemplates, this models one deferred computation, not an
 * expression tree. This type is not thread-safe by default.
 *
 * @tparam T Value produced by the deferred computation.
 *
 * Example:
 *   dsl::Lazy<std::string> cfg([]{ return std::string("loaded"); });
 *   auto& v = cfg.get();
 */
template <typename T> class Lazy
{
public:
  /// Creates lazy value from producer callable.
  explicit Lazy (std::function<T ()> producer)
      : producer_ (std::move (producer))
  {
  }
  /// Computes the value on first access and returns cached reference.
  T &
  get ()
  {
    if (!value_)
      value_ = producer_ ();
    return *value_;
  }
  /// Alias of get().
  T &
  force ()
  {
    return get ();
  }
  /// Indicates whether value has already been computed.
  bool
  is_computed () const noexcept
  {
    return value_.has_value ();
  }

private:
  std::function<T ()> producer_;
  std::optional<T> value_{};
};

/**
 * @brief Optional-like wrapper with monadic method chaining.
 *
 * This type wraps std::optional semantics and adds fluent
 * map/flat_map/filter/or_else. Monadic laws (left identity/right
 * identity/associativity) apply when map/flat_map functions are pure and
 * side-effect free.
 *
 * @tparam T Contained value type.
 *
 * Example:
 *   auto out = dsl::Maybe<int>(4).map([](int x){ return x + 1; }).or_else(0);
 */
template <typename T> class Maybe
{
public:
  /// Constructs empty Maybe.
  Maybe () = default;
  /// Constructs Maybe with a value.
  Maybe (T value) : value_ (std::move (value)) {}
  /// Constructs Maybe from std::optional.
  Maybe (std::optional<T> value) : value_ (std::move (value)) {}
  /// Checks whether a value is present.
  bool
  has_value () const noexcept
  {
    return value_.has_value ();
  }
  /// Returns underlying std::optional.
  const std::optional<T> &
  as_optional () const
  {
    return value_;
  }
  /// Applies function to contained value, if present.
  /// Reference results are copied into the returned Maybe.
  template <typename Fn>
  auto
  map (Fn &&fn) const
      -> Maybe<std::decay_t<decltype (fn (std::declval<const T &> ()))>>
  {
    using U = std::decay_t<decltype (fn (*value_))>;
    if (!value_)
      return Maybe<U>{};
    return Maybe<U>{ fn (*value_) };
  }
  /// Applies function returning Maybe-like type, if value is present.
  template <typename Fn>
  auto
  flat_map (Fn &&fn) const -> decltype (fn (std::declval<const T &> ()))
  {
    using R = decltype (fn (*value_));
    if (!value_)
      return R{};
    return fn (*value_);
  }
  /// Retains value only if predicate passes.
  template <typename Pred>
  Maybe
  filter (Pred &&pred) const
  {
    if (value_ && pred (*value_))
      return *this;
    return Maybe{};
  }
  /// Returns contained value or fallback.
  T
  or_else (T fallback) const
  {
    return value_.value_or (std::move (fallback));
  }

private:
  std::optional<T> value_{};
};
template <typename Opt, typename Fn>
auto
map (const Opt &opt, Fn &&fn) -> std::optional<std::decay_t<decltype (fn (*opt))>>
{
  if (!opt)
    return std::nullopt;
  return fn (*opt);
}
template <typename Opt, typename Fn>
auto
flat_map (const Opt &opt, Fn &&fn) -> decltype (fn (*opt))
{
  using R = decltype (fn (*opt));
  if (!opt)
    return R{};
  return fn (*opt);
}
template <typename Opt, typename Pred>
auto
filter (const Opt &opt, Pred &&pred) -> Opt
{
  if (opt && pred (*opt))
    return opt;
  return std::nullopt;
}
template <typename Opt, typename T>
T
or_else (const Opt &opt, T fallback)
{
  return opt.value_or (std::move (fallback));
}

template <typename T, typename E> class Result
{
public:
  /// Constructs an Ok result.
  Result (T value) : data_ (std::in_place_index<0>, std::move (value)) {}
  /// Constructs an Err result (disabled when T == E to avoid ambiguity).
  Result (E error)
    requires (!std::same_as<T, E>)
      : data_ (std::in_place_index<1>, std::move (error))
  {
  }
  /// True when value branch is active.
  bool
  is_ok () const
  {
    return data_.index () == 0;
  }
  /// True when error branch is active.
  bool
  is_err () const
  {
    return data_.index () == 1;
  }
  /// Returns value or throws if Err.
  T
  unwrap () const
  {
    if (!is_ok ())
      throw std::runtime_error ("dsl::Result::unwrap called on Err");
    return std::get<0> (data_);
  }
  /// Returns the error or throws if Ok (mirrors D Result.unwrap_err).
  E
  unwrap_err () const
  {
    if (is_ok ())
      throw std::runtime_error ("dsl::Result::unwrap_err called on Ok");
    return std::get<1> (data_);
  }
  /// Returns value or fallback when Err.
  T
  unwrap_or (T fallback) const
  {
    return is_ok () ? std::get<0> (data_) : std::move (fallback);
  }
  /// Maps Ok value with function.
  template <typename Fn>
  auto
  map (Fn &&fn) const
      -> Result<std::decay_t<decltype (fn (std::declval<const T &> ()))>, E>
  {
    using U = std::decay_t<decltype (fn (std::declval<const T &> ()))>;
    if (is_ok ())
      return Result<U, E>{ fn (std::get<0> (data_)) };
    return Result<U, E>::from_err (std::get<1> (data_));
  }
  /// Chains operation producing another Result.
  template <typename Fn>
  auto
  and_then (Fn &&fn) const -> decltype (fn (std::declval<const T &> ()))
  {
    using R = decltype (fn (std::declval<const T &> ()));
    if (is_ok ())
      return fn (std::get<0> (data_));
    return R::from_err (std::get<1> (data_));
  }
  /**
   * @brief Transforms the error payload while keeping success values
   * unchanged.
   *
   * @tparam Fn Error transformer callable.
   * @param fn Function applied only when this is an Err.
   * @return Result<T, E2> with transformed error type.
   */
  template <typename Fn>
  auto
  map_err (Fn &&fn) const
      -> Result<T, std::decay_t<decltype (fn (std::declval<const E &> ()))>>
  {
    using E2 = std::decay_t<decltype (fn (std::declval<const E &> ()))>;
    if (is_ok ())
      return Result<T, E2>::from_ok (std::get<0> (data_));
    return Result<T, E2>::from_err (fn (std::get<1> (data_)));
  }
  /// Factory for Ok value.
  static Result
  from_ok (T value)
  {
    return Result{ std::move (value) };
  }
  /// Factory for Err value.
  static Result
  from_err (E error)
  {
    return Result (std::in_place_index<1>, std::move (error));
  }

private:
  Result (std::in_place_index_t<1>, E error)
      : data_ (std::in_place_index<1>, std::move (error))
  {
  }
  std::variant<T, E> data_;
};
template <typename T> struct OkWrap
{
  T value;
};
template <typename E> struct ErrWrap
{
  E error;
};
template <typename T>
OkWrap<T>
Ok (T value)
{
  return { std::move (value) };
}
template <typename E>
ErrWrap<E>
Err (E error)
{
  return { std::move (error) };
}

/**
 * @brief Mutable parser input stream for combinatory parsers.
 *
 * ParsecInput tracks source text and cursor position and provides primitive
 * operations used by low-level parser callables.
 */
struct ParsecInput
{
  std::string_view source{};
  std::size_t pos{ 0 };
  char
  peek () const
  {
    return pos < source.size () ? source[pos] : '\0';
  }
  bool
  eof () const
  {
    return pos >= source.size ();
  }
  char
  consume ()
  {
    return eof () ? '\0' : source[pos++];
  }
  std::string_view
  get_span (std::size_t start) const
  {
    return source.substr (start, pos - start);
  }
};

inline Result<std::string, std::string>
load_parse_input_file (const std::string &path)
{
  std::ifstream in (path, std::ios::in | std::ios::binary);
  if (!in)
    return Result<std::string, std::string>::from_err (
        "dsl::load_parse_input_file: cannot open '" + path + "'");
  std::ostringstream ss;
  ss << in.rdbuf ();
  if (!in.good () && !in.eof ())
    return Result<std::string, std::string>::from_err (
        "dsl::load_parse_input_file: read failed for '" + path + "'");
  return Result<std::string, std::string>::from_ok (ss.str ());
}
enum class ParseFailureKind
{
  Soft,
  Committed
};

struct ParseError
{
  std::size_t pos{ 0 };
  ParseFailureKind kind{ ParseFailureKind::Soft };
  std::vector<std::string> expected{};
};

template <typename T> struct ExpectedResult
{
  using value_type = T;

  std::optional<T> value{};
  ParseError error{};

  ExpectedResult () = default;
  ExpectedResult (std::nullopt_t) : value (std::nullopt) {}
  ExpectedResult (const T &v) : value (v) {}
  ExpectedResult (T &&v) : value (std::move (v)) {}

  static ExpectedResult
  failure (std::size_t pos, ParseFailureKind kind = ParseFailureKind::Soft,
           std::vector<std::string> expected = {})
  {
    ExpectedResult out{};
    out.error = ParseError{ pos, kind, std::move (expected) };
    return out;
  }

  explicit
  operator bool () const
  {
    return value.has_value ();
  }
  T &
  operator* ()
  {
    return *value;
  }
  const T &
  operator* () const
  {
    return *value;
  }
};

/**
 * @brief Functional parser wrapper around `ExpectedResult<T>(ParsecInput&)`.
 *
 * @tparam T Parse output type.
 */
template <typename T> struct Parser
{
  std::function<ExpectedResult<T> (ParsecInput &)> parse;
  ExpectedResult<T>
  operator() (ParsecInput &input) const
  {
    return parse (input);
  }
};

inline void
merge_error (ParseError &dst, const ParseError &src)
{
  if (src.pos > dst.pos)
    {
      dst = src;
      return;
    }
  if (src.pos < dst.pos)
    return;
  if (src.kind == ParseFailureKind::Committed)
    dst.kind = ParseFailureKind::Committed;
  for (const auto &label : src.expected)
    {
      if (std::find (dst.expected.begin (), dst.expected.end (), label)
          == dst.expected.end ())
        dst.expected.push_back (label);
    }
}

template <typename T>
ExpectedResult<T>
fail_expected (const ParsecInput &in, std::string label,
               ParseFailureKind kind = ParseFailureKind::Soft)
{
  return ExpectedResult<T>::failure (in.pos, kind, { std::move (label) });
}

template <typename Fn>
auto
parser (Fn &&fn)
{
  using Ret = decltype (fn (std::declval<ParsecInput &> ()));
  using T = typename Ret::value_type;
  return Parser<T>{ std::forward<Fn> (fn) };
}

template <typename T>
Parser<std::vector<T>>
operator* (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<std::vector<T>>
        {
          std::vector<T> out;
          ParseError best_err{};
          while (true)
            {
              std::size_t save = in.pos;
              auto r = p (in);
              if (!r)
                {
                  merge_error (best_err, r.error);
                  if (r.error.kind == ParseFailureKind::Committed)
                    return ExpectedResult<std::vector<T>>::failure (
                        best_err.pos, best_err.kind, best_err.expected);
                  in.pos = save;
                  break;
                }
              if (in.pos == save)
                return fail_expected<std::vector<T>> (
                    in, "<repetition must consume input>",
                    ParseFailureKind::Committed);
              out.push_back (std::move (*r));
            }
          return out;
        });
}
template <typename A, typename B>
Parser<std::pair<A, B>>
operator& (const Parser<A> &a, const Parser<B> &b)
{
  return parser (
      [a, b] (ParsecInput &in) -> ExpectedResult<std::pair<A, B>>
        {
          auto save = in.pos;
          auto ra = a (in);
          if (!ra)
            return ExpectedResult<std::pair<A, B>>::failure (
                ra.error.pos, ra.error.kind, ra.error.expected);
          auto rb = b (in);
          if (!rb)
            {
              bool consumed = in.pos != save;
              in.pos = save;
              auto kind = rb.error.kind;
              if (consumed)
                kind = ParseFailureKind::Committed;
              return ExpectedResult<std::pair<A, B>>::failure (
                  rb.error.pos, kind, rb.error.expected);
            }
          return std::make_pair (std::move (*ra), std::move (*rb));
        });
}
template <typename T>
Parser<T>
operator| (const Parser<T> &a, const Parser<T> &b)
{
  return parser (
      [a, b] (ParsecInput &in) -> ExpectedResult<T>
        {
          auto save = in.pos;
          auto ra = a (in);
          if (ra)
            return ra;
          if (ra.error.kind == ParseFailureKind::Committed)
            return ra;
          in.pos = save;
          auto rb = b (in);
          if (!rb)
            merge_error (rb.error, ra.error);
          return rb;
        });
}
template <typename T>
Parser<std::optional<T>>
optional (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<std::optional<T>>
        {
          auto save = in.pos;
          auto r = p (in);
          if (!r)
            {
              if (r.error.kind == ParseFailureKind::Committed)
                return ExpectedResult<std::optional<T>>::failure (
                    r.error.pos, r.error.kind, r.error.expected);
              in.pos = save;
              return std::optional<T>{};
            }
          return std::optional<T>{ std::move (*r) };
        });
}

template <typename T>
Parser<T>
try_parse (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<T>
        {
          auto save = in.pos;
          auto r = p (in);
          if (!r)
            {
              in.pos = save;
              r.error.kind = ParseFailureKind::Soft;
            }
          return r;
        });
}

template <typename T>
Parser<T>
labeled (const Parser<T> &p, std::string expected)
{
  return parser (
      [p,
       expected = std::move (expected)] (ParsecInput &in) -> ExpectedResult<T>
        {
          auto r = p (in);
          if (!r)
            r.error.expected = { expected };
          return r;
        });
}

template <typename T> struct ParseOutcome
{
  std::optional<T> value{};
  ParseError error{};
};

template <typename T>
ParseOutcome<T>
run_parser (const Parser<T> &p, std::string_view source)
{
  ParsecInput in{ source, 0 };
  auto r = p (in);
  if (!r)
    return { std::nullopt, r.error };
  if (!in.eof ())
    {
      ParseError trailing{ in.pos,
                           ParseFailureKind::Committed,
                           { "<end-of-input>" } };
      return { std::nullopt, trailing };
    }
  return { std::move (r.value), {} };
}

inline Parser<char>
ch (char c)
{
  return labeled (parser (
                      [c] (ParsecInput &in) -> ExpectedResult<char>
                        {
                          if (!in.eof () && in.peek () == c)
                            return in.consume ();
                          return fail_expected<char> (in, std::string (1, c));
                        }),
                  std::string (1, c));
}

inline Parser<char>
satisfy (std::function<bool (char)> pred, std::string label)
{
  return labeled (parser (
                      [pred = std::move (pred),
                       label] (ParsecInput &in) -> ExpectedResult<char>
                        {
                          if (!in.eof () && pred (in.peek ()))
                            return in.consume ();
                          return fail_expected<char> (in, label);
                        }),
                  std::move (label));
}

enum class TaskPolicy
{
  StopOnError,
  ContinueOnError
};
/**
 * @brief Shared mutable state for task-pipeline execution.
 */
struct TaskState
{
  std::unordered_map<std::string, Result<std::string, std::string>> results{};
  bool suspended{ false };
};
/**
 * @brief Pipeline task unit, either internal callable or external command
 * adapter.
 */
struct Task
{
  std::string name{};
  std::function<Result<std::string, std::string> (TaskState &)> run_fn{};
  Result<std::string, std::string>
  run (TaskState &state) const
  {
    return run_fn (state);
  }
};
/**
 * @brief Sequence of tasks composed by operator|.
 */
struct TaskChain
{
  std::vector<Task> tasks{};
  TaskPolicy policy{ TaskPolicy::StopOnError };
};
inline TaskChain
operator| (Task lhs, Task rhs)
{
  return TaskChain{ { std::move (lhs), std::move (rhs) },
                    TaskPolicy::StopOnError };
}
inline TaskChain
operator| (TaskChain lhs, Task rhs)
{
  lhs.tasks.push_back (std::move (rhs));
  return lhs;
}
template <typename... Args>
TaskState &
run (TaskState &state, const TaskChain &chain, Args &&...)
{
  for (const auto &task : chain.tasks)
    {
      auto result = task.run (state);
      state.results.insert_or_assign (task.name, result);
      if (result.is_err () && chain.policy == TaskPolicy::StopOnError)
        break;
      if (state.suspended)
        break;
    }
  return state;
}
inline void
suspend (TaskState &state)
{
  state.suspended = true;
}
template <typename EventFn>
void
upon (TaskState &state, EventFn &&event, Task task)
{
  if (event ())
    state.results.insert_or_assign (task.name, task.run (state));
}

/**
 * @brief Parser stage metadata passed to semantic hooks.
 */
struct ParserStage
{
  std::size_t begin{};
  std::size_t end{};
  std::string_view span{};
};

/**
 * @brief Runs a parser and invokes semantic action with matched span.
 *
 * @tparam T Parser output type.
 * @tparam ActionFn Action callable type.
 * @param p Parser to execute.
 * @param input Parse input stream.
 * @param semantics Map of production id to matched content.
 * @param action Semantic action callback.
 * @return Optional action result; empty when parse fails.
 */
template <typename T, typename ActionFn>
auto
parse_with_action (
    const Parser<T> &p, ParsecInput &input,
    const std::unordered_map<std::string, std::string> &semantics,
    ActionFn &&action)
    -> std::optional<decltype (action (semantics, ParserStage{}))>
{
  std::size_t begin = input.pos;
  auto r = p (input);
  if (!r)
    return std::nullopt;
  ParserStage stage{ begin, input.pos, input.get_span (begin) };
  return action (semantics, stage);
}

/**
 * @brief Production registration entry for parser table-pass semantics.
 *
 * @tparam Id Compile-time production identifier.
 * @tparam Fn Handler callable.
 */
template <FixedString Id, typename Fn> struct ProductionHandler
{
  Fn fn;
};

/**
 * @brief Registers a production handler keyed by compile-time string ID.
 *
 * @tparam Id Compile-time production identifier.
 * @tparam Fn Handler callable type.
 * @param fn Production handler.
 * @return ProductionHandler object.
 */
template <FixedString Id, typename Fn>
auto
production (Fn &&fn)
{
  return ProductionHandler<Id, std::decay_t<Fn>>{ std::forward<Fn> (fn) };
}

// ============================================================
//  PEG: definitions, rules, channels, matching, combinators
// ============================================================
//
// This section is a purely additive extension: it layers a PEG
// (Parsing Expression Grammar) subsystem on top of the existing
// pattern-matching (`dsl::pattern`) and parser-combinator
// (`dsl::Parser` / `ParsecInput`) machinery above. No existing type,
// function, or layout is modified.

struct PEGDefinition;
struct PEGRule;
struct PEGMatch;
struct PEGParseResult;
struct PEGMatcher;

/**
 * @brief A named logical stream that PEG matches are routed into.
 *
 * Channels suppress or categorize matches without changing the grammar:
 * the built-in @IGNORE channel is used for whitespace/comments, while
 * user-defined channels (created with dsl::new_peg_channel) group matches
 * by category and enable filtering in the PEG matcher.
 */
struct PEGChannel
{
  std::string name{ "@DEFAULT" };

  constexpr bool operator== (const PEGChannel &) const = default;

  /// @return true if this is the built-in @IGNORE channel.
  bool
  is_ignore () const noexcept
  {
    return name == "@IGNORE";
  }
};

/// Built-in channel for ignorable input (whitespace, comments, ...).
inline const PEGChannel PEGIgnoreChannel{ "@IGNORE" };

/// Built-in channel for ordinary (semantic) input.
inline const PEGChannel PEGDefaultChannel{ "@DEFAULT" };

/**
 * @brief Creates a custom PEG channel.
 *
 * Custom channel names must begin with '@'.
 *
 * @param name Channel name, e.g. "@COMMENTS".
 * @throws std::invalid_argument if the name is empty or lacks the '@'
 * prefix.
 */
inline PEGChannel
new_peg_channel (std::string_view name)
{
  if (name.empty () || name.front () != '@')
    throw std::invalid_argument (
        "dsl::new_peg_channel: channel names must begin with '@'");
  return PEGChannel{ std::string (name) };
}

/**
 * @brief Match record handed to PEG semantic actions.
 *
 * `value` is a view into the parsed source text and is only valid for the
 * duration of the parse that produced it.
 */
struct PEGMatch
{
  std::string_view value{};   ///< matched text
  std::size_t begin{ 0 };     ///< start offset in source
  std::size_t end{ 0 };       ///< one-past-end offset in source
  PEGRule *rule{ nullptr };   ///< rule that produced this match
  PEGChannel channel{};       ///< resolved channel of the rule
  void *user_data{ nullptr }; ///< optional extension point

  std::size_t line{ 1 };   ///< 1-based line of `begin`
  std::size_t column{ 1 }; ///< 1-based column of `begin`
  bool ignored{ false };   ///< true if routed to @IGNORE

  /// @return length of the matched text (end - begin).
  std::size_t
  length () const noexcept
  {
    return end - begin;
  }
};

/// Signature of a PEG semantic action.
using PEGSemanticAction = std::function<void (PEGMatch &)>;

namespace peg_detail
{

/// Runtime-compiled matcher for the dsl::pattern regex subset:
/// literals, '.', [class] (with ranges and ^ negation), and */+/?/?!
/// quantifiers. Matching is an unanchored greedy prefix match.
struct CompiledPattern
{
  struct Atom
  {
    bool any{ false };      ///< '.' wildcard
    bool is_class{ false }; ///< [...]
    bool negate{ false };   ///< [^...]
    char ch{ 0 };           ///< literal character
    std::string cls{};      ///< class body, e.g. "a-z0-9"
    char quant{ 0 };        ///< 0, '*', '+', '?', '!'
  };

  std::vector<Atom> atoms{};
  bool valid{ true };
  std::string error{};

  static bool
  class_test (std::string_view cls, bool negate, char c)
  {
    bool hit = false;
    for (std::size_t i = 0; i < cls.size ();)
      {
        if (i + 2 < cls.size () && cls[i + 1] == '-')
          {
            if (c >= cls[i] && c <= cls[i + 2])
              hit = true;
            i += 3;
          }
        else
          {
            if (cls[i] == c)
              hit = true;
            ++i;
          }
      }
    return negate ? !hit : hit;
  }

  static bool
  atom_test (const Atom &a, char c)
  {
    if (a.any)
      return true;
    if (a.is_class)
      return class_test (a.cls, a.negate, c);
    return a.ch == c;
  }

  static CompiledPattern
  compile (std::string_view pat)
  {
    CompiledPattern out;
    std::size_t i = 0;
    while (i < pat.size ())
      {
        // '^'/'$' anchors are accepted and ignored: PEG matching is
        // implicitly anchored at the current position.
        if (pat[i] == '^' || pat[i] == '$')
          {
            ++i;
            continue;
          }
        Atom a;
        if (pat[i] == '.')
          {
            a.any = true;
            ++i;
          }
        else if (pat[i] == '[')
          {
            auto close = pat.find (']', i + 1);
            if (close == std::string_view::npos)
              {
                out.valid = false;
                out.error = "unterminated character class";
                return out;
              }
            a.is_class = true;
            std::string body{ pat.substr (i + 1, close - i - 1) };
            if (!body.empty () && body.front () == '^')
              {
                a.negate = true;
                body.erase (body.begin ());
              }
            a.cls = std::move (body);
            i = close + 1;
          }
        else
          {
            a.ch = pat[i];
            ++i;
          }
        if (i < pat.size ()
            && (pat[i] == '*' || pat[i] == '+' || pat[i] == '?'
                || pat[i] == '!'))
          a.quant = pat[i++];
        out.atoms.push_back (std::move (a));
      }
    return out;
  }

  /// Greedy prefix match starting at `start`; returns match end offset.
  std::optional<std::size_t>
  match_at (std::string_view text, std::size_t start) const
  {
    if (!valid)
      return std::nullopt;
    std::size_t pos = start;
    for (const auto &a : atoms)
      {
        auto avail = [&] { return pos < text.size (); };
        switch (a.quant)
          {
          case '*':
            while (avail () && atom_test (a, text[pos]))
              ++pos;
            break;
          case '+':
            {
              std::size_t n = 0;
              while (avail () && atom_test (a, text[pos]))
                {
                  ++pos;
                  ++n;
                }
              if (n == 0)
                return std::nullopt;
              break;
            }
          case '?':
            if (avail () && atom_test (a, text[pos]))
              ++pos;
            break;
          case '!':
            // Must-fail atom: zero-width negative lookahead.
            if (avail () && atom_test (a, text[pos]))
              return std::nullopt;
            break;
          default:
            if (!avail () || !atom_test (a, text[pos]))
              return std::nullopt;
            ++pos;
            break;
          }
      }
    return pos;
  }
};

inline void
line_col_at (std::string_view text, std::size_t pos, std::size_t &line,
             std::size_t &col)
{
  line = 1;
  col = 1;
  for (std::size_t i = 0; i < pos && i < text.size (); ++i)
    {
      if (text[i] == '\n')
        {
          ++line;
          col = 1;
        }
      else
        ++col;
    }
}

/// A lightweight match-arm consumed by PEGMatcher::operator<<.
struct PEGArm
{
  PEGRule *rule{ nullptr }; ///< nullptr => wildcard
  PEGSemanticAction action{};
  bool is_wildcard{ false };
  bool is_channel{ false }; ///< match any rule on `channel`
  PEGChannel channel{};
};

} // namespace peg_detail

/**
 * @brief A single PEG rule: a compiled pattern plus mutable behavior.
 *
 * Rules are created via PEGDefinition::add_rule<"...">() and remain
 * mutable afterwards: the semantic action, channel, and the guarded
 * (must_fail / if_succeed) behavior may be adjusted at any time.
 *
 * The rule is invocable with a callback to build a matcher arm:
 *   matcher << rule([](dsl::PEGMatch& m){ ... });
 */
struct PEGRule
{
  std::string pattern_text{};
  peg_detail::CompiledPattern compiled{};
  PEGSemanticAction semantic_action{};
  PEGChannel channel{ "@DEFAULT" };
  bool must_fail{ false };        ///< succeed only when the pattern fails
  bool fire_on_neg{ false };      ///< run the action even on negated success
  PEGRule *if_succeed{ nullptr }; ///< rule to attempt when pattern matches
  std::size_t order{ 0 };         ///< declaration order (PEG precedence)

  /// Attempts this rule at `pos`. Returns the match end offset, or
  /// std::nullopt on failure. For must_fail rules, success consumes one
  /// character so tokenizing parses always make progress.
  std::optional<std::size_t>
  try_match (std::string_view text, std::size_t pos) const
  {
    if (must_fail)
      {
        if (compiled.match_at (text, pos))
          return std::nullopt;
        return pos < text.size () ? std::optional<std::size_t> (pos + 1)
                                  : std::nullopt;
      }
    return compiled.match_at (text, pos);
  }

  /// Builds a match-arm binding this rule to a callback.
  template <typename Fn>
  peg_detail::PEGArm
  operator() (Fn &&fn) const
  {
    peg_detail::PEGArm arm;
    arm.rule = const_cast<PEGRule *> (this);
    arm.action = std::forward<Fn> (fn);
    return arm;
  }
};

/**
 * @brief Diagnostics produced by PEGDefinition::parse().
 */
struct PEGParseResult
{
  bool success{ true };
  std::size_t offset{ 0 };
  std::size_t line{ 1 };
  std::size_t column{ 1 };
  std::string message{};

  bool
  ok () const noexcept
  {
    return success;
  }
  bool
  failed () const noexcept
  {
    return !success;
  }
  std::size_t
  error_offset () const noexcept
  {
    return offset;
  }
  std::size_t
  error_line () const noexcept
  {
    return line;
  }
  std::size_t
  error_column () const noexcept
  {
    return column;
  }
  /// @return human-readable failure description (empty on success).
  std::string_view
  error_message () const noexcept
  {
    return message;
  }
};

/**
 * @brief A PEG grammar: an ordered set of rules plus channels and
 * optional inheritance metadata.
 *
 * Create a root grammar with dsl::create_peg_definition(), or extend an
 * existing one with dsl::derive_peg_definition(parent). Derived grammars
 * see parent rules first (parent rules precede child rules at any
 * decision point), and may add new rules or shadow parent behavior
 * without mutating the parent.
 */
struct PEGDefinition
{
  const PEGDefinition *parent{ nullptr };

  /// Adds a rule from a compile-time pattern string, with an optional
  /// semantic action. Returns a reference to the stored rule; the
  /// reference stays valid as long as rules are only appended (and this
  /// definition is alive).
  template <FixedString S, typename Fn>
  PEGRule &
  add_rule (Fn &&action)
  {
    return emplace_rule (S.view (),
                         PEGSemanticAction (std::forward<Fn> (action)));
  }

  template <FixedString S>
  PEGRule &
  add_rule ()
  {
    return emplace_rule (S.view (), PEGSemanticAction{});
  }

  /// @return all rules visible at this level: parent rules first, then
  /// local rules, preserving PEG ordered-choice precedence.
  std::vector<const PEGRule *>
  rules () const
  {
    std::vector<const PEGRule *> out;
    if (parent)
      out = parent->rules ();
    for (const auto &r : local_rules_)
      out.push_back (&r);
    return out;
  }

  /// Inspect only this definition's rules, without traversing parent links.
  const std::deque<PEGRule> &
  local_rules () const noexcept
  {
    return local_rules_;
  }

  /**
   * @brief Parses `text` token by token using ordered choice over all
   * visible rules, invoking semantic actions as matches occur.
   *
   * Matches routed to @IGNORE are recognized and skipped silently.
   * Parsing succeeds when the whole input is consumed.
   */
  PEGParseResult
  parse (std::string_view text) const
  {
    PEGParseResult result;
    auto all = rules ();
    std::size_t pos = 0;
    while (pos < text.size ())
      {
        bool matched = false;
        for (const PEGRule *r : all)
          {
            auto end = r->try_match (text, pos);
            if (!end)
              continue;
            const bool negated = r->must_fail;
            if (!negated && *end == pos)
              {
                // Zero-width positive match would loop forever.
                result.success = false;
                result.offset = pos;
                peg_detail::line_col_at (text, pos, result.line,
                                         result.column);
                result.message
                    = "PEG rule matched empty input; refusing to loop";
                return result;
              }
            matched = true;
            if (!negated || r->fire_on_neg)
              {
                PEGMatch m;
                m.value = text.substr (pos, *end - pos);
                m.begin = pos;
                m.end = *end;
                m.rule = const_cast<PEGRule *> (r);
                m.channel = r->channel;
                m.ignored = r->channel.is_ignore ();
                peg_detail::line_col_at (text, pos, m.line, m.column);
                if (r->semantic_action)
                  r->semantic_action (m);
              }
            if (r->if_succeed)
              {
                if (auto e2 = r->if_succeed->try_match (text, pos))
                  {
                    PEGMatch m;
                    m.value = text.substr (pos, *e2 - pos);
                    m.begin = pos;
                    m.end = *e2;
                    m.rule = r->if_succeed;
                    m.channel = r->if_succeed->channel;
                    m.ignored = r->if_succeed->channel.is_ignore ();
                    peg_detail::line_col_at (text, pos, m.line, m.column);
                    if (r->if_succeed->semantic_action)
                      r->if_succeed->semantic_action (m);
                  }
              }
            pos = *end;
            break; // ordered choice: first success wins
          }
        if (!matched)
          {
            result.success = false;
            result.offset = pos;
            peg_detail::line_col_at (text, pos, result.line, result.column);
            result.message
                = "no PEG rule matched at offset " + std::to_string (pos);
            return result;
          }
      }
    result.offset = pos;
    return result;
  }

private:
  PEGRule &
  emplace_rule (std::string_view pat, PEGSemanticAction action)
  {
    PEGRule r;
    r.pattern_text = std::string (pat);
    r.compiled = peg_detail::CompiledPattern::compile (pat);
    if (!r.compiled.valid)
      throw std::invalid_argument ("dsl::PEGDefinition::add_rule: "
                                   + r.compiled.error + " in pattern '"
                                   + r.pattern_text + "'");
    r.semantic_action = std::move (action);
    r.order = next_order_++;
    local_rules_.push_back (std::move (r));
    return local_rules_.back ();
  }

  // The public rule references and success guards must survive appends.
  std::deque<PEGRule> local_rules_{};
  std::size_t next_order_{ 0 };
};

/// Creates a root PEG definition (parent == nullptr).
inline PEGDefinition
create_peg_definition ()
{
  return PEGDefinition{};
}

/// Derives a PEG definition from `parent`; parent rules stay visible.
inline PEGDefinition
derive_peg_definition (const PEGDefinition &parent)
{
  PEGDefinition d;
  d.parent = &parent;
  return d;
}

/**
 * @brief Fluent, stream-style PEG pattern matcher.
 *
 * Created with dsl::peg_new_matcher(peg, text). Arms are consumed with
 * operator<<; the first arm whose rule matches at the current position
 * wins (PEG ordered choice). The wildcard arm fires when no preceding
 * arm matches, consuming one character by default.
 *
 * Channel filters (only/include/exclude) restrict which rules the rule
 * arms will consider; ignored-channel matches are skipped automatically
 * before any arm is consulted.
 *
 *   auto matcher = dsl::peg_new_matcher(peg, text);
 *   while (!matcher.is_spent())
 *       matcher << ident(cb) << number(cb) << matcher.wildcard(cb);
 *   matcher.close();
 */
struct PEGMatcher
{
  const PEGDefinition *def{ nullptr };
  std::string_view text{};
  std::size_t pos{ 0 };

  bool
  is_spent () const noexcept
  {
    return pos >= text.size ();
  }
  std::size_t
  position () const noexcept
  {
    return pos;
  }
  /// No-op for API symmetry; releases nothing (nothing is held).
  void
  close () noexcept
  {
  }

  /// Restricts matching to a single channel.
  PEGMatcher &
  only (std::string_view channel)
  {
    mode_ = FilterMode::Only;
    channels_.clear ();
    channels_.push_back (std::string (channel));
    return *this;
  }
  /// Adds a channel to the accepted set.
  PEGMatcher &
  include (std::string_view channel)
  {
    if (mode_ == FilterMode::Exclude)
      mode_ = FilterMode::Only;
    channels_.push_back (std::string (channel));
    return *this;
  }
  /// Rejects rules on the given channel.
  PEGMatcher &
  exclude (std::string_view channel)
  {
    if (mode_ != FilterMode::Exclude)
      {
        mode_ = FilterMode::Exclude;
        channels_.clear ();
      }
    channels_.push_back (std::string (channel));
    return *this;
  }
  bool
  channel_allowed (std::string_view name) const
  {
    if (mode_ == FilterMode::All)
      return true;
    bool found = std::find (channels_.begin (), channels_.end (), name)
                 != channels_.end ();
    return mode_ == FilterMode::Only ? found : !found;
  }

  /// Wildcard arm: fires when no rule arm matches at the position.
  template <typename Fn>
  peg_detail::PEGArm
  wildcard (Fn &&fn)
  {
    peg_detail::PEGArm arm;
    arm.is_wildcard = true;
    arm.action = std::forward<Fn> (fn);
    return arm;
  }

  /// Enqueues one match arm. Arms accumulate across chained << calls
  /// until one of them fires at the current position; the first arm
  /// whose rule matches wins (ordered choice). @IGNORE matches are
  /// skipped automatically before arms are consulted.
  PEGMatcher &
  operator<< (const peg_detail::PEGArm &arm)
  {
    arms_.push_back (arm);
    step ();
    return *this;
  }

private:
  /// Tries the accumulated arms once at the current position.
  void
  step ()
  {
    if (arms_.empty () || is_spent ())
      return;
    skip_ignored ();
    if (is_spent ())
      {
        arms_.clear ();
        return;
      }
    // Non-wildcard arms in order: first match wins.
    for (std::size_t i = 0; i < arms_.size (); ++i)
      {
        const auto &arm = arms_[i];
        if (arm.is_wildcard)
          continue;
        if (arm.is_channel)
          {
            for (const PEGRule *r : def->rules ())
              {
                if (r->channel.name != arm.channel.name)
                  continue;
                if (auto end = r->try_match (text, pos))
                  {
                    fire (arm.action, r, pos, *end);
                    arms_.clear ();
                    return;
                  }
              }
            continue;
          }
        if (!arm.rule || !channel_allowed (arm.rule->channel.name))
          continue;
        if (auto end = arm.rule->try_match (text, pos))
          {
            fire (arm.action, arm.rule, pos, *end);
            arms_.clear ();
            return;
          }
      }
    // No rule arm matched: fire the first wildcard arm, if any.
    for (std::size_t i = 0; i < arms_.size (); ++i)
      {
        if (!arms_[i].is_wildcard)
          continue;
        PEGMatch m;
        m.value = text.substr (pos, 1);
        m.begin = pos;
        m.end = pos + 1;
        m.rule = nullptr;
        peg_detail::line_col_at (text, pos, m.line, m.column);
        if (arms_[i].action)
          arms_[i].action (m);
        ++pos; // wildcard consumes one character
        arms_.clear ();
        return;
      }
    // Nothing fired yet: keep the accumulated arms; the next << may
    // provide one that matches at this position.
  }

  void
  skip_ignored ()
  {
    bool progressed = true;
    while (progressed && !is_spent ())
      {
        progressed = false;
        for (const PEGRule *r : def->rules ())
          {
            if (!r->channel.is_ignore ())
              continue;
            if (auto end = r->try_match (text, pos))
              {
                if (*end == pos)
                  continue;
                pos = *end;
                progressed = true;
                break;
              }
          }
      }
  }

  void
  fire (const PEGSemanticAction &action, const PEGRule *rule,
        std::size_t begin, std::size_t end)
  {
    PEGMatch m;
    m.value = text.substr (begin, end - begin);
    m.begin = begin;
    m.end = end;
    m.rule = const_cast<PEGRule *> (rule);
    m.channel = rule->channel;
    m.ignored = rule->channel.is_ignore ();
    peg_detail::line_col_at (text, begin, m.line, m.column);
    pos = end;
    if (action)
      action (m);
    else if (rule->semantic_action)
      rule->semantic_action (m);
  }

  enum class FilterMode
  {
    All,
    Only,
    Exclude
  };
  FilterMode mode_{ FilterMode::All };
  std::vector<std::string> channels_{};
  std::vector<peg_detail::PEGArm> arms_{};
  static constexpr std::size_t npos = static_cast<std::size_t> (-1);
};

/// Creates a fluent PEG matcher over `text` for the grammar `peg`.
inline PEGMatcher
peg_new_matcher (const PEGDefinition &peg, std::string_view text)
{
  PEGMatcher m;
  m.def = &peg;
  m.text = text;
  return m;
}

// ============================================================
//  PEG parser combinators
// ============================================================
//
// Thin, additive wrappers over the existing dsl::Parser machinery with
// strict PEG semantics: ordered choice (operator| already behaves
// ordered), repetition that never commits on partial input, and
// lookahead operators.

namespace peg_detail
{

/// Converts any dsl::Parser<T> into a Parser<PEGMatch> that reports the
/// consumed span. The output value is discarded; the span is kept.
template <typename T>
Parser<PEGMatch>
as_span_parser (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<PEGMatch>
        {
          std::size_t begin = in.pos;
          auto r = p (in);
          if (!r)
            return ExpectedResult<PEGMatch>::failure (
                r.error.pos, r.error.kind, r.error.expected);
          PEGMatch m;
          m.begin = begin;
          m.end = in.pos;
          m.value = in.get_span (begin);
          return m;
        });
}

} // namespace peg_detail

/// PEG sequence: parse a, then b, then c... Spans concatenate.
template <typename A, typename B, typename... Rest>
auto
peg_seq (const Parser<A> &a, const Parser<B> &b, const Rest &...rest)
{
  auto ab = parser (
      [a, b] (ParsecInput &in) -> ExpectedResult<PEGMatch>
        {
          std::size_t begin = in.pos;
          auto ra = a (in);
          if (!ra)
            return ExpectedResult<PEGMatch>::failure (
                ra.error.pos, ra.error.kind, ra.error.expected);
          auto rb = b (in);
          if (!rb)
            {
              bool consumed = in.pos != begin;
              in.pos = begin;
              auto kind = rb.error.kind;
              if (consumed)
                kind = ParseFailureKind::Committed;
              return ExpectedResult<PEGMatch>::failure (rb.error.pos, kind,
                                                        rb.error.expected);
            }
          PEGMatch m;
          m.begin = begin;
          m.end = in.pos;
          m.value = in.get_span (begin);
          return m;
        });
  if constexpr (sizeof...(rest) == 0)
    return ab;
  else
    return peg_seq (ab, rest...);
}

/// PEG ordered choice: first successful alternative wins; later
/// alternatives are not tried at that decision point.
template <typename A, typename B, typename... Rest>
auto
peg_choice (const Parser<A> &a, const Parser<B> &b, const Rest &...rest)
{
  auto ab = try_parse (a) | try_parse (b);
  if constexpr (sizeof...(rest) == 0)
    return ab;
  else
    return peg_choice (ab, rest...);
}

/// PEG optional: a? (never fails, never commits).
template <typename T>
auto
peg_opt (const Parser<T> &p)
{
  return optional (try_parse (p));
}

/// PEG zero-or-more: a* (greedy; partial failures backtrack fully).
template <typename T>
auto
peg_many (const Parser<T> &p)
{
  return *try_parse (p);
}

/// PEG one-or-more: a+ (greedy; fails if the first iteration fails).
template <typename T>
auto
peg_many1 (const Parser<T> &p)
{
  return p & *try_parse (p);
}

/// PEG positive lookahead: &a — succeeds if a matches, consumes nothing.
template <typename T>
auto
peg_and (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<bool>
        {
          auto save = in.pos;
          auto r = p (in);
          in.pos = save;
          if (!r)
            return ExpectedResult<bool>::failure (
                r.error.pos, ParseFailureKind::Soft, r.error.expected);
          return true;
        });
}

/// PEG negative lookahead: !a — succeeds if a fails, consumes nothing.
template <typename T>
auto
peg_not (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<bool>
        {
          auto save = in.pos;
          auto r = p (in);
          in.pos = save;
          if (r)
            return fail_expected<bool> (in, "negative lookahead");
          return true;
        });
}

/// Adapts a PEGRule into a Parser<PEGMatch> honoring must_fail.
inline Parser<PEGMatch>
peg_rule (const PEGRule &rule)
{
  return parser (
      [&rule] (ParsecInput &in) -> ExpectedResult<PEGMatch>
        {
          std::size_t begin = in.pos;
          auto end = rule.try_match (in.source, begin);
          if (!end)
            return fail_expected<PEGMatch> (in, rule.pattern_text);
          in.pos = *end;
          PEGMatch m;
          m.begin = begin;
          m.end = *end;
          m.value = in.source.substr (begin, *end - begin);
          m.rule = const_cast<PEGRule *> (&rule);
          m.channel = rule.channel;
          m.ignored = rule.channel.is_ignore ();
          peg_detail::line_col_at (in.source, begin, m.line, m.column);
          return m;
        });
}

/// Assigns a channel to a parser's PEGMatch result.
template <typename T>
auto
peg_channel (PEGChannel channel, const Parser<T> &p)
{
  return parser (
      [channel = std::move (channel),
       p] (ParsecInput &in) -> ExpectedResult<PEGMatch>
        {
          std::size_t begin = in.pos;
          auto r = p (in);
          if (!r)
            return ExpectedResult<PEGMatch>::failure (
                r.error.pos, r.error.kind, r.error.expected);
          PEGMatch m;
          m.begin = begin;
          m.end = in.pos;
          m.value = in.get_span (begin);
          m.channel = channel;
          m.ignored = channel.is_ignore ();
          peg_detail::line_col_at (in.source, begin, m.line, m.column);
          return m;
        });
}

/// Overload accepting the channel name directly (must begin with '@').
template <typename T>
auto
peg_channel (std::string_view name, const Parser<T> &p)
{
  return peg_channel (new_peg_channel (name), p);
}

// ============================================================
//  S-expressions and equality saturation
// ============================================================

struct SExpressions
{
  template <class Derived> struct Mixin
  {
  };
};
struct EqualitySaturation
{
  template <class Derived> struct Mixin
  {
  };
};
struct Serialization
{
  template <class Derived> struct Mixin
  {
  };
};
struct AbstractIO
{
  template <class Derived> struct Mixin
  {
  };
};
struct Visitors
{
  template <class Derived> struct Mixin
  {
  };
};
struct StaticRegularExpressions
{
  template <class Derived> struct Mixin
  {
  };
};
struct Preprocessing
{
  template <class Derived> struct Mixin
  {
  };
};
struct ContextFreeGrammar
{
  template <class Derived> struct Mixin
  {
  };
};

/// Atoms and lists form the interchange language for rules and grammars.
/// Quoted atoms are data, even when their text begins with '?'.
struct SExpr
{
  bool is_atom = true;
  std::string text;
  std::vector<SExpr> items;
  bool quoted = false;
  static SExpr
  atom (std::string text, bool quoted = false)
  {
    if (text.empty ())
      quoted = true;
    for (unsigned char c : text)
      quoted |= c <= ' ' || c == '(' || c == ')' || c == '"' || c == ';'
                || c == '\\';
    return { true, std::move (text), {}, quoted };
  }
  static SExpr
  list (std::vector<SExpr> items)
  {
    return { false, {}, std::move (items), false };
  }
  bool operator== (const SExpr &) const = default;
  std::string
  dump () const
  {
    if (!is_atom)
      {
        std::string out = "(";
        for (std::size_t i = 0; i < items.size (); ++i)
          {
            if (i)
              out += ' ';
            out += items[i].dump ();
          }
        return out + ')';
      }
    bool quote = quoted || text.empty ();
    for (unsigned char c : text)
      quote |= c <= ' ' || c == '(' || c == ')' || c == '"' || c == ';'
               || c == '\\';
    if (!quote)
      return text;
    std::string out = "\"";
    for (char c : text)
      {
        switch (c)
          {
          case '\n':
            out += "\\n";
            break;
          case '\r':
            out += "\\r";
            break;
          case '\t':
            out += "\\t";
            break;
          case '"':
            out += "\\\"";
            break;
          case '\\':
            out += "\\\\";
            break;
          default:
            out += c;
          }
      }
    return out + '"';
  }
};
namespace sexpr_detail
{
struct Reader
{
  std::string_view source;
  std::size_t pos = 0;
  std::size_t nodes_left = (std::numeric_limits<std::size_t>::max) ();
  std::size_t max_depth = 512;
  void
  skip ()
  {
    while (pos < source.size ())
      {
        if (static_cast<unsigned char> (source[pos]) <= ' ')
          ++pos;
        else if (source[pos] == ';')
          {
            while (pos < source.size () && source[pos] != '\n')
              ++pos;
          }
        else
          break;
      }
  }
  [[noreturn]] void
  error (std::string_view message) const
  {
    throw std::invalid_argument ("S-expression at " + std::to_string (pos)
                                 + ": " + std::string (message));
  }
  SExpr
  read (std::size_t depth = 0)
  {
    if (depth > max_depth)
      error ("nesting limit exceeded");
    if (nodes_left == 0)
      error ("node limit exceeded");
    --nodes_left;
    skip ();
    if (pos == source.size ())
      error ("expected expression");
    if (source[pos] == ')')
      error ("unexpected ')'");
    if (source[pos] == '(')
      {
        ++pos;
        std::vector<SExpr> items;
        for (;;)
          {
            skip ();
            if (pos == source.size ())
              error ("unterminated list");
            if (source[pos] == ')')
              {
                ++pos;
                return SExpr::list (std::move (items));
              }
            items.push_back (read (depth + 1));
          }
      }
    if (source[pos] == '"')
      {
        ++pos;
        std::string text;
        while (pos < source.size ())
          {
            char c = source[pos++];
            if (c == '"')
              return SExpr::atom (std::move (text), true);
            if (c == '\\')
              {
                if (pos == source.size ())
                  error ("unfinished escape");
                c = source[pos++];
                switch (c)
                  {
                  case 'n':
                    c = '\n';
                    break;
                  case 'r':
                    c = '\r';
                    break;
                  case 't':
                    c = '\t';
                    break;
                  case '\\':
                  case '"':
                    break;
                  default:
                    error ("unknown escape");
                  }
              }
            text += c;
          }
        error ("unterminated quoted atom");
      }
    auto begin = pos;
    while (pos < source.size ()
           && static_cast<unsigned char> (source[pos]) > ' '
           && source[pos] != '(' && source[pos] != ')' && source[pos] != ';')
      ++pos;
    return SExpr::atom (std::string (source.substr (begin, pos - begin)));
  }
};
}
inline std::vector<SExpr>
parse_sexpressions (std::string_view text)
{
  sexpr_detail::Reader reader{ text };
  std::vector<SExpr> out;
  reader.skip ();
  while (reader.pos < text.size ())
    {
      out.push_back (reader.read ());
      reader.skip ();
    }
  return out;
}
inline SExpr
parse_sexpr (std::string_view text)
{
  auto expressions = parse_sexpressions (text);
  if (expressions.size () != 1)
    throw std::invalid_argument ("expected exactly one S-expression");
  return std::move (expressions.front ());
}
/// Resource limits for specifications received from outside the application.
struct SExprLimits
{
  std::size_t max_bytes = 1024 * 1024;
  std::size_t max_nodes = 65536;
  std::size_t max_depth = 128;
};

inline SExpr
parse_bounded_sexpr (std::string_view text, SExprLimits limits = {})
{
  if (text.size () > limits.max_bytes)
    throw std::invalid_argument ("S-expression byte limit exceeded");
  sexpr_detail::Reader reader{
    text, 0, limits.max_nodes, std::min<std::size_t> (512, limits.max_depth)
  };
  auto result = reader.read ();
  reader.skip ();
  if (reader.pos != text.size ())
    reader.error ("expected exactly one S-expression");
  return result;
}
inline SExpr
ast_to_sexpr (const ASTNode &node)
{
  std::vector<SExpr> items{ SExpr::atom (std::string (node.tag ())) };
  if (node.is_leaf ())
    items.push_back (SExpr::atom (node.value ()));
  else
    for (const auto &child : node.children ())
      items.push_back (ast_to_sexpr (child));
  return SExpr::list (std::move (items));
}
inline ASTNode
sexpr_to_ast (const SExpr &expression)
{
  if (expression.is_atom)
    return ASTNode ("atom", expression.text);
  if (expression.items.empty () || !expression.items[0].is_atom)
    throw std::invalid_argument ("AST S-expression requires an atom head");
  if (expression.items.size () == 2 && expression.items[1].is_atom)
    return ASTNode (expression.items[0].text, expression.items[1].text);
  std::vector<ASTNode> children;
  for (std::size_t i = 1; i < expression.items.size (); ++i)
    children.push_back (sexpr_to_ast (expression.items[i]));
  return ASTNode (expression.items[0].text, std::move (children));
}

using EClassId = std::size_t;
/// Versioned, bounded S-expression bytecode. Lengths are little-endian u32.
inline std::vector<std::uint8_t> encode_sexpr (const SExpr &term)
{
  std::vector<std::uint8_t> out{ 'M', 'S', 'X', 1 };
  std::function<void (const SExpr &, unsigned)> put = [&] (const SExpr &e, unsigned depth) {
    if (depth > 512) throw std::invalid_argument ("S-expression nesting limit exceeded");
    auto n = e.is_atom ? e.text.size () : e.items.size ();
    if (n > 0xffffffffu) throw std::invalid_argument ("S-expression too large");
    out.push_back (e.is_atom ? (e.quoted ? 1 : 0) : 2);
    for (unsigned i = 0; i < 4; ++i) out.push_back (static_cast<std::uint8_t> (n >> (8 * i)));
    if (e.is_atom) out.insert (out.end (), e.text.begin (), e.text.end ());
    else for (auto &child : e.items) put (child, depth + 1);
  };
  put (term, 0);
  return out;
}
inline SExpr decode_sexpr (const std::vector<std::uint8_t> &bytes)
{
  if (bytes.size () < 4 || bytes[0] != 'M' || bytes[1] != 'S' || bytes[2] != 'X' || bytes[3] != 1)
    throw std::invalid_argument ("invalid S-expression bytecode header");
  std::size_t pos = 4;
  std::function<SExpr (unsigned)> get = [&] (unsigned depth) -> SExpr {
    if (depth > 512 || bytes.size () - pos < 5) throw std::invalid_argument ("truncated or deeply nested S-expression bytecode");
    auto tag = bytes[pos++];
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= std::uint32_t (bytes[pos++]) << (8 * i);
    if (tag > 2 || n > bytes.size () - pos) throw std::invalid_argument ("invalid S-expression bytecode length or tag");
    if (tag != 2)
      { std::string text (bytes.begin () + pos, bytes.begin () + pos + n); pos += n; return SExpr::atom (std::move (text), tag == 1); }
    if (n > (bytes.size () - pos) / 5) throw std::invalid_argument ("invalid S-expression bytecode list length");
    std::vector<SExpr> items;
    for (std::uint32_t i = 0; i < n; ++i) items.push_back (get (depth + 1));
    return SExpr::list (std::move (items));
  };
  auto result = get (0);
  if (pos != bytes.size ()) throw std::invalid_argument ("trailing S-expression bytecode");
  return result;
}

using SExprBindings = std::unordered_map<std::string, SExpr>;
inline bool match_sexpr (const SExpr &pattern, const SExpr &subject, SExprBindings &bindings)
{
  if (pattern.is_atom && !pattern.quoted && pattern.text.size () > 1 && pattern.text[0] == '?')
    { auto [p, added] = bindings.emplace (pattern.text, subject); return added || p->second == subject; }
  if (pattern.is_atom || subject.is_atom) return pattern == subject;
  if (pattern.items.size () != subject.items.size ()) return false;
  for (std::size_t i = 0; i < pattern.items.size (); ++i)
    if (!match_sexpr (pattern.items[i], subject.items[i], bindings)) return false;
  return true;
}
inline SExpr substitute_sexpr (const SExpr &term, const SExprBindings &bindings)
{
  if (term.is_atom)
    { auto p = bindings.find (term.text); return !term.quoted && p != bindings.end () ? p->second : term; }
  std::vector<SExpr> items;
  for (auto &child : term.items) items.push_back (substitute_sexpr (child, bindings));
  return SExpr::list (std::move (items));
}
using ESubstitution = std::unordered_map<std::string, EClassId>;
struct EMatch
{
  EClassId root;
  ESubstitution bindings;
};
struct ERewriteRule
{
  std::string name;
  SExpr lhs, rhs;
};
inline bool
is_pattern_variable (const SExpr &term)
{
  return term.is_atom && !term.quoted && term.text.size () > 1
         && term.text[0] == '?';
}
inline void
validate_eterm (const SExpr &term, std::unordered_set<std::string> &variables)
{
  if (is_pattern_variable (term))
    {
      variables.insert (term.text);
      return;
    }
  if (term.is_atom)
    return;
  if (term.items.empty () || !term.items[0].is_atom
      || is_pattern_variable (term.items[0]))
    throw std::invalid_argument (
        "e-graph term requires a literal operator head");
  for (std::size_t i = 1; i < term.items.size (); ++i)
    validate_eterm (term.items[i], variables);
}
/// Syntax: (rewrite name lhs rhs), or (= lhs rhs). Variables are ?name.
inline ERewriteRule
sexpr_rule (const SExpr &expression)
{
  if (expression.is_atom || expression.items.empty ()
      || !expression.items[0].is_atom)
    throw std::invalid_argument ("expected S-expression rewrite rule");
  const auto &items = expression.items;
  ERewriteRule rule;
  if (items[0].text == "rewrite" && items.size () == 4 && items[1].is_atom)
    rule = { items[1].text, items[2], items[3] };
  else if ((items[0].text == "=" || items[0].text == "=>")
           && items.size () == 3)
    rule = { items[0].text, items[1], items[2] };
  else
    throw std::invalid_argument (
        "rule syntax is (rewrite name lhs rhs) or (= lhs rhs)");
  std::unordered_set<std::string> left, right;
  validate_eterm (rule.lhs, left);
  validate_eterm (rule.rhs, right);
  for (const auto &variable : right)
    if (!left.contains (variable))
      throw std::invalid_argument ("unbound RHS variable " + variable);
  return rule;
}
inline ERewriteRule
sexpr_rule (std::string_view source)
{
  return sexpr_rule (parse_sexpr (source));
}
inline std::vector<ERewriteRule>
sexpr_rules (std::string_view source)
{
  std::vector<ERewriteRule> rules;
  for (const auto &expression : parse_sexpressions (source))
    rules.push_back (sexpr_rule (expression));
  return rules;
}
struct SaturationLimits
{
  std::size_t iterations = 30, nodes = 100000, matches = 100000;
};
enum class SaturationStop
{
  Saturated,
  IterationLimit,
  NodeLimit,
  MatchLimit
};
struct SaturationResult
{
  SaturationStop stop = SaturationStop::IterationLimit;
  std::size_t iterations = 0, applications = 0;
  bool
  saturated () const
  {
    return stop == SaturationStop::Saturated;
  }
};

/// Hash-consed e-nodes, union-find e-classes, and congruence rebuilding.
/// A rule inserts its RHS and merges it with the matched class; it never
/// deletes alternatives. extract() chooses a minimum-node finite
/// representative.
class EGraph
{
public:
  struct ENode
  {
    std::string op;
    bool atom = false, quoted = false;
    std::vector<EClassId> children;
  };

private:
  struct EClass
  {
    EClassId parent;
    std::vector<ENode> nodes;
  };
  struct NodeLimit
  {
  };
  std::vector<EClass> classes_;
  std::unordered_map<std::string, EClassId> hash_;
  bool dirty_ = false;
  std::size_t revision_ = 0,
              budget_ = std::numeric_limits<std::size_t>::max ();
  std::string
  key (ENode &node)
  {
    std::string out = std::string (node.atom ? "a" : "n")
                      + (node.quoted ? "q" : "u")
                      + std::to_string (node.op.size ()) + ":" + node.op;
    for (auto &child : node.children)
      {
        child = find (child);
        out += ":" + std::to_string (child);
      }
    return out;
  }
  EClassId
  insert (ENode node)
  {
    rebuild ();
    auto encoded = key (node);
    if (auto it = hash_.find (encoded); it != hash_.end ())
      return find (it->second);
    if (hash_.size () >= budget_)
      throw NodeLimit{};
    auto id = classes_.size ();
    classes_.push_back ({ id, { std::move (node) } });
    hash_[std::move (encoded)] = id;
    ++revision_;
    return id;
  }
  std::vector<ESubstitution>
  match_term (const SExpr &term, EClassId root, ESubstitution bindings,
              std::size_t limit)
  {
    root = find (root);
    if (is_pattern_variable (term))
      {
        auto it = bindings.find (term.text);
        if (it != bindings.end () && find (it->second) != root)
          return {};
        bindings[term.text] = root;
        return { std::move (bindings) };
      }
    std::vector<ESubstitution> out;
    for (const auto &node : classes_[root].nodes)
      {
        if (node.atom != term.is_atom)
          continue;
        const auto &head = term.is_atom ? term : term.items[0];
        if (node.op != head.text || node.quoted != head.quoted)
          continue;
        if (term.is_atom)
          {
            out.push_back (bindings);
          }
        else if (node.children.size () + 1 == term.items.size ())
          {
            std::vector<ESubstitution> partial{ bindings };
            for (std::size_t i = 0;
                 i < node.children.size () && !partial.empty (); ++i)
              {
                std::vector<ESubstitution> next;
                for (const auto &b : partial)
                  {
                    auto found = match_term (term.items[i + 1],
                                             node.children[i], b, limit);
                    for (auto &item : found)
                      {
                        next.push_back (std::move (item));
                        if (next.size () >= limit)
                          break;
                      }
                    if (next.size () >= limit)
                      break;
                  }
                partial = std::move (next);
              }
            for (auto &item : partial)
              {
                out.push_back (std::move (item));
                if (out.size () >= limit)
                  break;
              }
          }
        if (out.size () >= limit)
          break;
      }
    return out;
  }
  EClassId
  instantiate (const SExpr &term, const ESubstitution &bindings)
  {
    if (is_pattern_variable (term))
      return find (bindings.at (term.text));
    if (term.is_atom)
      return insert ({ term.text, true, term.quoted, {} });
    ENode node{ term.items[0].text, false, term.items[0].quoted, {} };
    for (std::size_t i = 1; i < term.items.size (); ++i)
      node.children.push_back (instantiate (term.items[i], bindings));
    return insert (std::move (node));
  }

public:
  EClassId
  find (EClassId id)
  {
    if (id >= classes_.size ())
      throw std::out_of_range ("invalid e-class ID");
    auto root = id;
    while (classes_[root].parent != root)
      root = classes_[root].parent;
    while (classes_[id].parent != id)
      {
        auto next = classes_[id].parent;
        classes_[id].parent = root;
        id = next;
      }
    return root;
  }
  bool
  unite (EClassId a, EClassId b)
  {
    a = find (a);
    b = find (b);
    if (a == b)
      return false;
    if (a > b)
      std::swap (a, b);
    classes_[b].parent = a;
    auto &target = classes_[a].nodes;
    for (auto &node : classes_[b].nodes)
      target.push_back (std::move (node));
    classes_[b].nodes.clear ();
    dirty_ = true;
    ++revision_;
    return true;
  }
  void
  rebuild ()
  {
    if (!dirty_)
      return;
    std::vector<std::pair<EClassId, ENode>> nodes;
    for (EClassId i = 0; i < classes_.size (); ++i)
      for (const auto &node : classes_[i].nodes)
        nodes.emplace_back (i, node);
    bool changed;
    do
      {
        changed = false;
        hash_.clear ();
        for (auto &[owner, node] : nodes)
          {
            auto encoded = key (node);
            if (auto it = hash_.find (encoded); it != hash_.end ())
              changed |= unite (owner, it->second);
            else
              hash_[std::move (encoded)] = find (owner);
          }
      }
    while (changed);
    for (auto &cls : classes_)
      cls.nodes.clear ();
    hash_.clear ();
    for (auto &[owner, node] : nodes)
      {
        auto encoded = key (node);
        if (hash_.contains (encoded))
          continue;
        auto root = find (owner);
        hash_[std::move (encoded)] = root;
        classes_[root].nodes.push_back (std::move (node));
      }
    dirty_ = false;
  }
  EClassId
  add (const SExpr &expression)
  {
    std::unordered_set<std::string> variables;
    validate_eterm (expression, variables);
    if (!variables.empty ())
      throw std::invalid_argument (
          "cannot add an unbound pattern to an e-graph");
    return instantiate (expression, {});
  }
  EClassId
  add (std::string_view expression)
  {
    return add (parse_sexpr (expression));
  }
  EClassId
  add (const ASTNode &tree)
  {
    return add (ast_to_sexpr (tree));
  }
  bool
  equivalent (EClassId a, EClassId b)
  {
    rebuild ();
    return find (a) == find (b);
  }
  template <class F>
  bool
  when_equal (EClassId a, EClassId b, F &&rewriter)
  {
    if (!equivalent (a, b))
      return false;
    std::invoke (std::forward<F> (rewriter), *this, find (a));
    return true;
  }
  std::size_t
  node_count ()
  {
    rebuild ();
    return hash_.size ();
  }
  std::size_t
  class_count ()
  {
    rebuild ();
    std::size_t count = 0;
    for (EClassId i = 0; i < classes_.size (); ++i)
      count += find (i) == i;
    return count;
  }
  std::vector<EMatch>
  matches (const SExpr &pattern, std::size_t limit = 100000)
  {
    std::unordered_set<std::string> variables;
    validate_eterm (pattern, variables);
    rebuild ();
    std::vector<EMatch> out;
    if (!limit)
      return out;
    for (EClassId i = 0; i < classes_.size (); ++i)
      if (find (i) == i)
        {
          for (auto &bindings :
               match_term (pattern, i, {}, limit - out.size ()))
            {
              out.push_back ({ i, std::move (bindings) });
              if (out.size () == limit)
                return out;
            }
        }
    return out;
  }
  SaturationResult
  saturate (const std::vector<ERewriteRule> &rules,
            SaturationLimits limits = {},
            std::function<void (const ERewriteRule &, const EMatch &)> on_match
            = {})
  {
    // Validate manually constructed rules as well as parsed rules.
    for (const auto &rule : rules)
      sexpr_rule (
          SExpr::list ({ SExpr::atom ("rewrite"), SExpr::atom (rule.name),
                         rule.lhs, rule.rhs }));
    auto old_budget = budget_;
    budget_ = limits.nodes;
    struct Restore
    {
      std::size_t &value;
      std::size_t old;
      ~Restore () { value = old; }
    } restore{ budget_, old_budget };
    SaturationResult report;
    if (node_count () > limits.nodes)
      {
        report.stop = SaturationStop::NodeLimit;
        return report;
      }
    for (std::size_t iteration = 0; iteration < limits.iterations; ++iteration)
      {
        rebuild ();
        auto before = revision_;
        ++report.iterations;
        for (const auto &rule : rules)
          {
            auto remaining = limits.matches - report.applications;
            auto found = matches (
                rule.lhs, remaining == std::numeric_limits<std::size_t>::max ()
                              ? remaining
                              : remaining + 1);
            for (const auto &match : found)
              {
                if (report.applications == limits.matches)
                  {
                    report.stop = SaturationStop::MatchLimit;
                    return report;
                  }
                try
                  {
                    auto rhs = instantiate (rule.rhs, match.bindings);
                    unite (match.root, rhs);
                  }
                catch (NodeLimit &)
                  {
                    report.stop = SaturationStop::NodeLimit;
                    rebuild ();
                    return report;
                  }
                ++report.applications;
                if (on_match)
                  on_match (rule, match);
              }
          }
        rebuild ();
        if (revision_ == before)
          {
            report.stop = SaturationStop::Saturated;
            return report;
          }
      }
    return report;
  }
  SExpr
  extract (EClassId id)
  {
    rebuild ();
    id = find (id);
    const auto infinity = std::numeric_limits<std::size_t>::max ();
    std::vector<std::size_t> costs (classes_.size (), infinity),
        choice (classes_.size (), infinity);
    bool changed;
    do
      {
        changed = false;
        for (EClassId i = 0; i < classes_.size (); ++i)
          if (find (i) == i)
            {
              for (std::size_t j = 0; j < classes_[i].nodes.size (); ++j)
                {
                  const auto &node = classes_[i].nodes[j];
                  std::size_t cost = 1;
                  for (auto child : node.children)
                    {
                      auto c = costs[find (child)];
                      if (c == infinity || c >= infinity - cost)
                        {
                          cost = infinity;
                          break;
                        }
                      cost += c;
                    }
                  if (cost < costs[i])
                    {
                      costs[i] = cost;
                      choice[i] = j;
                      changed = true;
                    }
                }
            }
      }
    while (changed);
    if (choice[id] == infinity)
      throw std::runtime_error ("e-class has no finite expression");
    std::function<SExpr (EClassId)> build = [&] (EClassId root)
      {
        root = find (root);
        const auto &node = classes_[root].nodes[choice[root]];
        if (node.atom)
          return SExpr::atom (node.op, node.quoted);
        std::vector<SExpr> items{ SExpr::atom (node.op, node.quoted) };
        for (auto child : node.children)
          items.push_back (build (child));
        return SExpr::list (std::move (items));
      };
    return build (id);
  }
  ASTNode
  extract_ast (EClassId id)
  {
    return sexpr_to_ast (extract (id));
  }
};

// ============================================================
//  User-defined serialization and I/O protocols
// ============================================================

/// Backends choose syntax, operations, return types and error handling.
/// These adapters borrow a backend; its lifetime must cover the adapter's use.
template <class B, class T, class Output>
concept SerializerBackend
    = requires (B &backend, const T &value, Output &output) {
        backend.serialize (value, output);
      };
template <class B, class T, class Input>
concept DeserializerBackend = requires (B &backend, Input &input) {
  backend.template deserialize<T> (input);
};
template <class Backend> struct Serializer
{
  Backend &backend;
  explicit Serializer (Backend &backend) : backend (backend) {}
  template <class T, class Output>
    requires SerializerBackend<Backend, T, Output>
  decltype (auto)
  serialize (const T &value, Output &output)
  {
    return backend.serialize (value, output);
  }
};
template <class Backend> struct Deserializer
{
  Backend &backend;
  explicit Deserializer (Backend &backend) : backend (backend) {}
  template <class T, class Input>
    requires DeserializerBackend<Backend, T, Input>
  decltype (auto)
  deserialize (Input &input)
  {
    return backend.template deserialize<T> (input);
  }
};
template <class B>
concept IOBackend = requires (B &backend, std::span<std::byte> out,
                              std::span<const std::byte> in) {
  { backend.read (out) } -> std::convertible_to<std::size_t>;
  { backend.write (in) } -> std::convertible_to<std::size_t>;
};
/// Byte protocol: read/write return transferred counts. Zero read is EOF;
/// short reads/writes are supported. seek/tell are optional backend
/// operations.
template <IOBackend Backend> struct IO
{
  Backend &backend;
  explicit IO (Backend &backend) : backend (backend) {}
  std::size_t
  read (std::span<std::byte> bytes)
  {
    auto n = static_cast<std::size_t> (backend.read (bytes));
    if (n > bytes.size ())
      throw std::runtime_error ("I/O backend returned an invalid read count");
    return n;
  }
  std::size_t
  write (std::span<const std::byte> bytes)
  {
    auto n = static_cast<std::size_t> (backend.write (bytes));
    if (n > bytes.size ())
      throw std::runtime_error ("I/O backend returned an invalid write count");
    return n;
  }
  void
  read_exact (std::span<std::byte> bytes)
  {
    while (!bytes.empty ())
      {
        auto n = read (bytes);
        if (!n)
          throw std::runtime_error ("unexpected I/O EOF");
        bytes = bytes.subspan (n);
      }
  }
  void
  write_all (std::span<const std::byte> bytes)
  {
    while (!bytes.empty ())
      {
        auto n = write (bytes);
        if (!n)
          throw std::runtime_error ("I/O write made no progress");
        bytes = bytes.subspan (n);
      }
  }
  void
  seek (std::size_t position)
    requires requires { backend.seek (position); }
  {
    backend.seek (position);
  }
  auto
  tell ()
    requires requires { backend.tell (); }
  {
    return backend.tell ();
  }
};

// ============================================================
//  Visitors derived from AST schemas
// ============================================================
template <FixedString Tag> struct ASTTag
{
  static constexpr auto value = Tag.view ();
};
template <FixedString Tag, class F> struct VisitorCase
{
  static constexpr auto tag = Tag.view ();
  F fn;
  template <class Node>
  void
  invoke (Node &node)
  {
    if constexpr (std::invocable<F &, ASTTag<Tag>, Node &>)
      std::invoke (fn, ASTTag<Tag>{}, node);
    else
      std::invoke (fn, node);
  }
};
template <FixedString Tag, class F>
auto
on (F &&fn)
{
  return VisitorCase<Tag, std::decay_t<F>>{ std::forward<F> (fn) };
}
enum class VisitOrder
{
  Preorder,
  Postorder
};
template <class... Cases> class ASTVisitor
{
  std::tuple<Cases...> cases_;
  std::vector<std::string> tags_;
  void
  collect (const ASTNode &node)
  {
    if (std::find (tags_.begin (), tags_.end (), node.tag ()) == tags_.end ())
      tags_.emplace_back (node.tag ());
    for (const auto &child : node.children ())
      collect (child);
  }

public:
  ASTVisitor (const ASTNode &schema, Cases... cases)
      : cases_ (std::move (cases)...)
  {
    collect (schema);
    for (const auto &tag : tags_)
      if (!((tag == Cases::tag) || ...))
        throw std::invalid_argument ("visitor lacks a handler for " + tag);
  }
  template <class Node>
  void
  visit (Node &node)
  {
    if (std::find (tags_.begin (), tags_.end (), node.tag ()) == tags_.end ())
      throw std::invalid_argument ("node tag is outside visitor schema");
    bool invoked = false;
    std::apply (
        [&] (auto &...entry)
          {
            ((
                 [&]
                   {
                     if (!invoked
                         && node.tag ()
                                == std::remove_reference_t<
                                    decltype (entry)>::tag)
                       {
                         entry.invoke (node);
                         invoked = true;
                       }
                   }()),
             ...);
          },
        cases_);
  }
  template <class Node>
  void
  walk (Node &node, VisitOrder order = VisitOrder::Preorder)
  {
    if (order == VisitOrder::Preorder)
      visit (node);
    for (auto &child : node.children ())
      walk (child, order);
    if (order == VisitOrder::Postorder)
      visit (node);
  }
};
/// Runtime AST -> template-generated dispatch, with runtime schema validation.
template <class... Cases>
auto
derive_visitor (const ASTNode &schema, Cases &&...cases)
{
  return ASTVisitor<std::decay_t<Cases>...> (schema,
                                             std::forward<Cases> (cases)...);
}
/// A structural, compile-time AST of tags, usable without runtime AST
/// allocation.
template <FixedString Tag, class... Children> struct ASTSchema
{
  static constexpr auto tag = Tag.view ();
  using children = std::tuple<Children...>;
};
template <FixedString Tag, class... Children>
constexpr auto
ast_schema (Children...)
{
  return ASTSchema<Tag, Children...>{};
}
namespace visitor_detail
{
template <class Schema, class... Cases>
constexpr bool
covered ()
{
  constexpr bool own = ((Schema::tag == Cases::tag) || ...);
  return own &&
         []<std::size_t... I> (std::index_sequence<I...>)
           {
             return (
                 covered<std::tuple_element_t<I, typename Schema::children>,
                         Cases...> ()
                 && ...);
           }(std::make_index_sequence<
               std::tuple_size_v<typename Schema::children>>{});
}
template <class Schema>
ASTNode
tree ()
{
  return []<std::size_t... I> (std::index_sequence<I...>)
    {
      return ASTNode (
          Schema::tag,
          std::vector<ASTNode>{ tree<
              std::tuple_element_t<I, typename Schema::children>> ()... });
    }(std::make_index_sequence<
        std::tuple_size_v<typename Schema::children>>{});
}
}
template <class Schema, class... Cases>
auto
compile_visitor (Schema, Cases &&...cases)
{
  static_assert (visitor_detail::covered<Schema, std::decay_t<Cases>...> (),
                 "visitor must handle every schema tag");
  return derive_visitor (visitor_detail::tree<Schema> (),
                         std::forward<Cases> (cases)...);
}
template <class... Cases>
auto
compile_visitor (const ASTNode &schema, Cases &&...cases)
{
  return derive_visitor (schema, std::forward<Cases> (cases)...);
}

// ============================================================
//  Compile-time byte regular expressions and binary streams
// ============================================================

struct RegexMatch
{
  std::size_t begin = 0, end = 0;
  std::size_t
  size () const
  {
    return end - begin;
  }
};
namespace byte_regex_detail
{
enum class Op
{
  Byte,
  Any,
  Class,
  Split,
  Empty,
  Begin,
  End,
  Accept
};
struct State
{
  Op op = Op::Empty;
  int x = -1, y = -1;
  unsigned char byte = 0;
  std::array<bool, 256> chars{};
};
template <std::size_t Capacity> struct Program
{
  std::array<State, Capacity> states{};
  std::size_t size = 0;
  int start = 0;
  bool valid = true;
};
struct Fragment
{
  int start;
  std::vector<std::pair<int, bool>> ends;
};
template <std::size_t Capacity> struct Compiler
{
  std::string_view source;
  std::size_t pos = 0;
  Program<Capacity> program;
  constexpr int
  emit (State state)
  {
    if (program.size == Capacity)
      {
        program.valid = false;
        return 0;
      }
    program.states[program.size] = state;
    return static_cast<int> (program.size++);
  }
  constexpr void
  patch (const Fragment &fragment, int destination)
  {
    for (auto [index, second] : fragment.ends)
      (second ? program.states[index].y : program.states[index].x)
          = destination;
  }
  constexpr Fragment
  one (State state)
  {
    auto index = emit (state);
    return { index, { { index, false } } };
  }
  static constexpr int
  hex (char c)
  {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  }
  constexpr unsigned char
  escaped ()
  {
    if (pos == source.size ())
      {
        program.valid = false;
        return 0;
      }
    auto c = source[pos++];
    switch (c)
      {
      case 'n':
        return '\n';
      case 'r':
        return '\r';
      case 't':
        return '\t';
      case '0':
        return 0;
      case 'x':
        {
          if (pos + 2 > source.size () || hex (source[pos]) < 0
              || hex (source[pos + 1]) < 0)
            {
              program.valid = false;
              return 0;
            }
          auto byte = hex (source[pos]) * 16 + hex (source[pos + 1]);
          pos += 2;
          return static_cast<unsigned char> (byte);
        }
      default:
        return static_cast<unsigned char> (c);
      }
  }
  constexpr Fragment
  atom ()
  {
    if (pos == source.size ())
      return one ({ Op::Empty });
    char c = source[pos++];
    if (c == '(')
      {
        auto fragment = expression ();
        if (pos == source.size () || source[pos++] != ')')
          program.valid = false;
        return fragment;
      }
    State state;
    if (c == '[')
      {
        state.op = Op::Class;
        bool negate = pos < source.size () && source[pos] == '^';
        if (negate)
          ++pos;
        bool any = false;
        while (pos < source.size () && source[pos] != ']')
          {
            unsigned char first = static_cast<unsigned char> (source[pos++]);
            if (first == '\\')
              first = escaped ();
            unsigned char last = first;
            if (pos + 1 < source.size () && source[pos] == '-'
                && source[pos + 1] != ']')
              {
                ++pos;
                last = static_cast<unsigned char> (source[pos++]);
                if (last == '\\')
                  last = escaped ();
              }
            if (first > last)
              program.valid = false;
            for (unsigned value = first; value <= last; ++value)
              state.chars[value] = true;
            any = true;
          }
        if (!any || pos == source.size () || source[pos++] != ']')
          program.valid = false;
        if (negate)
          for (auto &value : state.chars)
            value = !value;
      }
    else if (c == '.')
      state.op = Op::Any;
    else if (c == '^')
      state.op = Op::Begin;
    else if (c == '$')
      state.op = Op::End;
    else
      {
        if (c == '*' || c == '+' || c == '?' || c == ')')
          program.valid = false;
        state.op = Op::Byte;
        state.byte = c == '\\' ? escaped () : static_cast<unsigned char> (c);
      }
    return one (state);
  }
  constexpr Fragment
  repeated ()
  {
    auto fragment = atom ();
    if (pos < source.size ()
        && (source[pos] == '*' || source[pos] == '+' || source[pos] == '?'))
      {
        auto quant = source[pos++];
        auto split = emit ({ Op::Split, fragment.start, -1 });
        if (quant == '?')
          {
            fragment.ends.emplace_back (split, true);
            return { split, std::move (fragment.ends) };
          }
        patch (fragment, split);
        return { quant == '+' ? fragment.start : split, { { split, true } } };
      }
    return fragment;
  }
  constexpr Fragment
  sequence ()
  {
    auto result = one ({ Op::Empty });
    while (pos < source.size () && source[pos] != ')' && source[pos] != '|')
      {
        auto next = repeated ();
        patch (result, next.start);
        result.ends = std::move (next.ends);
      }
    return result;
  }
  constexpr Fragment
  expression ()
  {
    auto left = sequence ();
    while (pos < source.size () && source[pos] == '|')
      {
        ++pos;
        auto right = sequence ();
        auto split = emit ({ Op::Split, left.start, right.start });
        left.ends.insert (left.ends.end (), right.ends.begin (),
                          right.ends.end ());
        left.start = split;
      }
    return left;
  }
  constexpr auto
  compile ()
  {
    auto result = expression ();
    patch (result, emit ({ Op::Accept }));
    program.start = result.start;
    program.valid &= pos == source.size ();
    return program;
  }
};
}
/// Thompson NFA compiled from a string template parameter. Operates on all 256
/// byte values, including NUL. Supports grouping, alternation, . [] [^]
/// ranges,
/// * + ?, ^ $, and escaped literals / \xHH / \0 / \n / \r / \t. No captures.
template <FixedString Source> struct StaticRegex
{
  static constexpr auto program
      = byte_regex_detail::Compiler<Source.view ().size () * 4
                                    + 8>{ Source.view () }
            .compile ();
  static_assert (program.valid, "invalid static byte regular expression");
  static std::optional<std::size_t>
  prefix (std::span<const std::byte> bytes, std::size_t start = 0)
  {
    using byte_regex_detail::Op;
    if (start > bytes.size ())
      return std::nullopt;
    std::vector<int> active, next;
    std::vector<bool> seen (program.size);
    auto closure = [&] (auto &&self, int index, std::size_t pos,
                        std::vector<int> &target) -> void
      {
        if (index < 0 || seen[index])
          return;
        seen[index] = true;
        const auto &state = program.states[index];
        if (state.op == Op::Split)
          {
            self (self, state.x, pos, target);
            self (self, state.y, pos, target);
          }
        else if (state.op == Op::Empty || (state.op == Op::Begin && pos == 0)
                 || (state.op == Op::End && pos == bytes.size ()))
          self (self, state.x, pos, target);
        else if (state.op != Op::Begin && state.op != Op::End)
          target.push_back (index);
      };
    closure (closure, program.start, start, active);
    std::optional<std::size_t> best;
    for (auto pos = start;; ++pos)
      {
        for (int index : active)
          if (program.states[index].op == Op::Accept)
            best = pos;
        if (pos == bytes.size () || active.empty ())
          break;
        next.clear ();
        std::fill (seen.begin (), seen.end (), false);
        auto byte = std::to_integer<unsigned char> (bytes[pos]);
        for (int index : active)
          {
            const auto &state = program.states[index];
            if (state.op == Op::Any
                || (state.op == Op::Byte && state.byte == byte)
                || (state.op == Op::Class && state.chars[byte]))
              closure (closure, state.x, pos + 1, next);
          }
        active.swap (next);
      }
    return best;
  }
  static std::optional<RegexMatch>
  search (std::span<const std::byte> bytes, std::size_t start = 0)
  {
    for (auto i = start; i <= bytes.size (); ++i)
      if (auto end = prefix (bytes, i))
        return RegexMatch{ i, *end };
    return std::nullopt;
  }
  static bool
  matches (std::span<const std::byte> bytes)
  {
    auto end = prefix (bytes);
    return end && *end == bytes.size ();
  }
  static bool
  matches (std::string_view text)
  {
    return matches (std::as_bytes (std::span (text.data (), text.size ())));
  }
};

enum class ByteOrder
{
  Little,
  Big
};
/// Buffered binary stream, with regex-token reads and explicit byte ordering.
/// Views returned by read/next remain valid until the stream's storage
/// changes.
struct BinaryStream
{
  std::vector<std::byte> bytes;
  std::size_t pos = 0;
  BinaryStream () = default;
  explicit BinaryStream (std::span<const std::byte> input)
      : bytes (input.begin (), input.end ())
  {
  }
  bool
  eof () const
  {
    return pos >= bytes.size ();
  }
  std::span<const std::byte>
  read (std::size_t count)
  {
    if (pos > bytes.size () || count > bytes.size () - pos)
      throw std::out_of_range ("binary stream EOF");
    auto view = std::span<const std::byte> (bytes).subspan (pos, count);
    pos += count;
    return view;
  }
  void
  write (std::span<const std::byte> input)
  {
    // Stage the input so appending a view into this stream is well-defined.
    std::vector<std::byte> copy (input.begin (), input.end ());
    bytes.insert (bytes.end (), copy.begin (), copy.end ());
  }
  template <FixedString Regex>
  std::optional<std::span<const std::byte>>
  next ()
  {
    auto end = StaticRegex<Regex>::prefix (bytes, pos);
    if (!end)
      return std::nullopt;
    if (*end == pos)
      throw std::runtime_error ("binary token regex matched empty input");
    return read (*end - pos);
  }
  template <std::unsigned_integral T>
  T
  read_integer (ByteOrder order = ByteOrder::Little)
  {
    auto input = read (sizeof (T));
    T value = 0;
    for (std::size_t i = 0; i < sizeof (T); ++i)
      {
        auto shift = order == ByteOrder::Little ? i : sizeof (T) - 1 - i;
        value |= static_cast<T> (std::to_integer<unsigned char> (input[i]))
                 << (shift * 8);
      }
    return value;
  }
  template <std::unsigned_integral T>
  void
  write_integer (T value, ByteOrder order = ByteOrder::Little)
  {
    for (std::size_t i = 0; i < sizeof (T); ++i)
      {
        auto shift = order == ByteOrder::Little ? i : sizeof (T) - 1 - i;
        bytes.push_back (
            static_cast<std::byte> ((value >> (shift * 8)) & 255));
      }
  }
  template <class Backend>
  static BinaryStream
  load (IO<Backend> &input, std::size_t limit = 64 * 1024 * 1024)
  {
    BinaryStream result;
    std::array<std::byte, 4096> buffer;
    for (;;)
      {
        auto n = input.read (buffer);
        if (!n)
          return result;
        if (n > limit - result.bytes.size ())
          throw std::length_error ("binary stream input limit exceeded");
        result.write (std::span<const std::byte> (buffer).first (n));
      }
  }
  template <class Backend>
  void
  save (IO<Backend> &output) const
  {
    output.write_all (bytes);
  }
};

// ============================================================
//  Explicit context-free grammars (Earley recognition and AST construction)
// ============================================================

struct CFGSymbol
{
  bool terminal = false;
  std::string name;
  static CFGSymbol
  token (std::string text)
  {
    return { true, std::move (text) };
  }
  static CFGSymbol
  nonterminal (std::string name)
  {
    return { false, std::move (name) };
  }
};
struct CFGRule
{
  std::string lhs;
  std::vector<CFGSymbol> rhs;
};
struct CFGParseResult
{
  std::optional<ASTNode> tree;
  std::size_t position = 0;
  std::vector<std::string> expected;
  bool
  ok () const
  {
    return tree.has_value ();
  }
};
/// Grammar terminals are tokens, not characters. The user's lexer chooses the
/// token syntax. Handles epsilon, left recursion and unordered alternatives;
/// for ambiguous grammars, returns the first discovered derivation.
struct CFG
{
  std::string start;
  std::vector<CFGRule> productions;
  void
  add (std::string lhs, std::vector<CFGSymbol> rhs)
  {
    productions.push_back ({ std::move (lhs), std::move (rhs) });
  }
  void
  validate () const
  {
    std::unordered_set<std::string> names;
    for (const auto &rule : productions)
      {
        if (rule.lhs.empty ())
          throw std::invalid_argument ("empty CFG nonterminal name");
        names.insert (rule.lhs);
      }
    if (!names.contains (start))
      throw std::invalid_argument ("undefined CFG start symbol");
    for (const auto &rule : productions)
      for (const auto &symbol : rule.rhs)
        if (!symbol.terminal && !names.contains (symbol.name))
          throw std::invalid_argument ("undefined CFG nonterminal "
                                       + symbol.name);
  }
  CFGParseResult
  parse (const std::vector<std::string> &tokens) const
  {
    validate ();
    auto rules = productions;
    rules.insert (rules.begin (),
                  { "$start", { CFGSymbol::nonterminal (start) } });
    struct Item
    {
      std::size_t rule, dot, origin;
      std::vector<ASTNode> children;
    };
    std::vector<std::vector<Item>> chart (tokens.size () + 1);
    auto insert = [&] (std::size_t position, Item item)
      {
        for (const auto &old : chart[position])
          if (old.rule == item.rule && old.dot == item.dot
              && old.origin == item.origin)
            return;
        chart[position].push_back (std::move (item));
      };
    insert (0, { 0, 0, 0, {} });
    for (std::size_t pos = 0; pos <= tokens.size (); ++pos)
      {
        for (std::size_t index = 0; index < chart[pos].size (); ++index)
          {
            auto item = chart[pos][index];
            const auto &rule = rules[item.rule];
            if (item.dot == rule.rhs.size ())
              {
                auto tree = ASTNode (rule.lhs, item.children);
                for (std::size_t k = 0; k < chart[item.origin].size (); ++k)
                  {
                    auto waiting = chart[item.origin][k];
                    const auto &parent = rules[waiting.rule];
                    if (waiting.dot < parent.rhs.size ()
                        && !parent.rhs[waiting.dot].terminal
                        && parent.rhs[waiting.dot].name == rule.lhs)
                      {
                        ++waiting.dot;
                        waiting.children.push_back (tree);
                        insert (pos, std::move (waiting));
                      }
                  }
              }
            else
              {
                const auto &symbol = rule.rhs[item.dot];
                if (symbol.terminal)
                  {
                    if (pos < tokens.size () && symbol.name == tokens[pos])
                      {
                        ++item.dot;
                        item.children.emplace_back ("token", tokens[pos]);
                        insert (pos + 1, std::move (item));
                      }
                  }
                else
                  {
                    for (std::size_t i = 1; i < rules.size (); ++i)
                      if (rules[i].lhs == symbol.name)
                        insert (pos, { i, 0, pos, {} });
                    // Nullable completions can precede newly inserted waiting
                    // items.
                    for (std::size_t k = 0; k < chart[pos].size (); ++k)
                      {
                        auto completed = chart[pos][k];
                        const auto &child = rules[completed.rule];
                        if (completed.origin == pos
                            && completed.dot == child.rhs.size ()
                            && child.lhs == symbol.name)
                          {
                            auto advanced = item;
                            ++advanced.dot;
                            advanced.children.emplace_back (
                                child.lhs, completed.children);
                            insert (pos, std::move (advanced));
                          }
                      }
                  }
              }
          }
      }
    CFGParseResult result;
    for (const auto &item : chart.back ())
      if (item.rule == 0 && item.dot == 1 && item.origin == 0)
        {
          result.tree = item.children[0];
          result.position = tokens.size ();
          return result;
        }
    for (std::size_t pos = 0; pos <= tokens.size (); ++pos)
      if (!chart[pos].empty ())
        result.position = pos;
    for (const auto &item : chart[result.position])
      {
        const auto &rule = rules[item.rule];
        if (item.dot < rule.rhs.size () && rule.rhs[item.dot].terminal)
          {
            const auto &name = rule.rhs[item.dot].name;
            if (std::find (result.expected.begin (), result.expected.end (),
                           name)
                == result.expected.end ())
              result.expected.push_back (name);
          }
      }
    if (result.position < tokens.size () && result.expected.empty ())
      result.expected.push_back ("<end-of-input>");
    return result;
  }
};
template <FixedString Text> struct CFGTerminal
{
  static CFGSymbol
  symbol ()
  {
    return CFGSymbol::token (std::string (Text.view ()));
  }
};
template <FixedString Name> struct CFGNonterminal
{
  static CFGSymbol
  symbol ()
  {
    return CFGSymbol::nonterminal (std::string (Name.view ()));
  }
};
template <FixedString LHS, class... Symbols> struct CFGProduction
{
  static CFGRule
  rule ()
  {
    return { std::string (LHS.view ()), { Symbols::symbol ()... } };
  }
};
template <FixedString Start, class... Productions>
CFG
make_cfg (Productions...)
{
  CFG grammar{ std::string (Start.view ()), { Productions::rule ()... } };
  grammar.validate ();
  return grammar;
}
/// (grammar (start Expr) (rule Expr (Expr "+" Term) (Term)) ...)
/// Quoted symbols are terminals; unquoted symbols are nonterminals; () is
/// epsilon.
inline CFG
sexpr_grammar (const SExpr &expression)
{
  if (expression.is_atom || expression.items.empty ()
      || expression.items[0].text != "grammar")
    throw std::invalid_argument ("expected (grammar ...) S-expression");
  CFG grammar;
  for (std::size_t i = 1; i < expression.items.size (); ++i)
    {
      const auto &part = expression.items[i];
      const auto &items = part.items;
      if (part.is_atom || items.empty ())
        throw std::invalid_argument ("invalid grammar declaration");
      if (items[0].text == "start" && items.size () == 2 && items[1].is_atom)
        grammar.start = items[1].text;
      else if (items[0].text == "rule" && items.size () >= 3
               && items[1].is_atom)
        {
          for (std::size_t j = 2; j < items.size (); ++j)
            {
              if (items[j].is_atom)
                throw std::invalid_argument ("CFG alternative must be a list");
              std::vector<CFGSymbol> symbols;
              for (const auto &symbol : items[j].items)
                {
                  if (!symbol.is_atom)
                    throw std::invalid_argument ("CFG symbol must be an atom");
                  symbols.push_back ({ symbol.quoted, symbol.text });
                }
              grammar.add (items[1].text, std::move (symbols));
            }
        }
      else
        throw std::invalid_argument ("invalid grammar declaration");
    }
  grammar.validate ();
  return grammar;
}
inline CFG
sexpr_grammar (std::string_view source)
{
  return sexpr_grammar (parse_sexpr (source));
}

// ============================================================
//  Built-in preprocessing, with user-supplied include and scripting backends
// ============================================================

namespace preprocess_detail
{
inline bool
identifier_start (char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool
identifier_char (char c)
{
  return identifier_start (c) || (c >= '0' && c <= '9');
}
inline std::string_view
trim (std::string_view s)
{
  while (!s.empty () && static_cast<unsigned char> (s.front ()) <= ' ')
    s.remove_prefix (1);
  while (!s.empty () && static_cast<unsigned char> (s.back ()) <= ' ')
    s.remove_suffix (1);
  return s;
}
inline std::string_view
word (std::string_view &s)
{
  s = trim (s);
  std::size_t end = 0;
  while (end < s.size () && identifier_char (s[end]))
    ++end;
  auto result = s.substr (0, end);
  s.remove_prefix (end);
  return result;
}
}
struct PreprocessorMacro
{
  std::string replacement;
  std::vector<std::string> parameters;
  bool function = false;
};
struct Preprocessor
{
  std::unordered_map<std::string, PreprocessorMacro> macros;
  std::function<std::string (std::string_view)> include;
  std::function<std::string (std::string_view, std::string_view)> script;
  std::size_t max_expansion_depth = 100, max_include_depth = 32;
  void
  define (std::string name, std::string replacement)
  {
    macros[std::move (name)] = { std::move (replacement), {}, false };
  }
  void
  define_function (std::string name, std::vector<std::string> parameters,
                   std::string replacement)
  {
    macros[std::move (name)]
        = { std::move (replacement), std::move (parameters), true };
  }

private:
  bool in_comment_ = false;
  std::string
  substitute (std::string_view source,
              const std::unordered_map<std::string, std::string> &values)
  {
    std::string out;
    for (std::size_t i = 0; i < source.size ();)
      {
        if (source[i] == '"' || source[i] == '\'')
          {
            auto quote = source[i];
            out += source[i++];
            while (i < source.size ())
              {
                auto c = source[i++];
                out += c;
                if (c == '\\' && i < source.size ())
                  out += source[i++];
                else if (c == quote)
                  break;
              }
          }
        else if (preprocess_detail::identifier_start (source[i]))
          {
            auto begin = i++;
            while (i < source.size ()
                   && preprocess_detail::identifier_char (source[i]))
              ++i;
            auto name = std::string (source.substr (begin, i - begin));
            auto found = values.find (name);
            out += found == values.end () ? name : found->second;
          }
        else
          out += source[i++];
      }
    return out;
  }
  std::string
  expand_impl (std::string_view source,
               std::unordered_set<std::string> disabled, std::size_t depth)
  {
    if (depth > max_expansion_depth)
      throw std::runtime_error ("macro expansion depth exceeded");
    std::string out;
    for (std::size_t i = 0; i < source.size ();)
      {
        if (in_comment_)
          {
            auto end = source.find ("*/", i);
            if (end == source.npos)
              {
                out += source.substr (i);
                break;
              }
            out += source.substr (i, end + 2 - i);
            i = end + 2;
            in_comment_ = false;
          }
        else if (source.substr (i, 2) == "//")
          {
            out += source.substr (i);
            break;
          }
        else if (source.substr (i, 2) == "/*")
          {
            in_comment_ = true;
            out += "/*";
            i += 2;
          }
        else if (source[i] == '"' || source[i] == '\'')
          {
            auto quote = source[i];
            out += source[i++];
            while (i < source.size ())
              {
                auto c = source[i++];
                out += c;
                if (c == '\\' && i < source.size ())
                  out += source[i++];
                else if (c == quote)
                  break;
              }
          }
        else if (preprocess_detail::identifier_start (source[i]))
          {
            auto begin = i++;
            while (i < source.size ()
                   && preprocess_detail::identifier_char (source[i]))
              ++i;
            auto name = std::string (source.substr (begin, i - begin));
            auto found = macros.find (name);
            if (found == macros.end () || disabled.contains (name))
              {
                out += name;
                continue;
              }
            auto macro = found->second;
            auto replacement = macro.replacement;
            if (macro.function)
              {
                auto cursor = i;
                while (cursor < source.size () && source[cursor] <= ' ')
                  ++cursor;
                if (cursor == source.size () || source[cursor] != '(')
                  {
                    out += name;
                    continue;
                  }
                ++cursor;
                auto arg_begin = cursor;
                std::size_t nesting = 1;
                std::vector<std::string> arguments;
                while (cursor < source.size () && nesting)
                  {
                    char c = source[cursor++];
                    if (c == '"' || c == '\'')
                      {
                        while (cursor < source.size ())
                          {
                            char q = source[cursor++];
                            if (q == '\\' && cursor < source.size ())
                              ++cursor;
                            else if (q == c)
                              break;
                          }
                      }
                    else if (c == '(')
                      ++nesting;
                    else if (c == ')')
                      {
                        if (!--nesting)
                          arguments.emplace_back (
                              preprocess_detail::trim (source.substr (
                                  arg_begin, cursor - arg_begin - 1)));
                      }
                    else if (c == ',' && nesting == 1)
                      {
                        arguments.emplace_back (
                            preprocess_detail::trim (source.substr (
                                arg_begin, cursor - arg_begin - 1)));
                        arg_begin = cursor;
                      }
                  }
                if (nesting)
                  throw std::invalid_argument (
                      "unterminated macro invocation");
                if (macro.parameters.empty () && arguments.size () == 1
                    && arguments[0].empty ())
                  arguments.clear ();
                if (arguments.size () != macro.parameters.size ())
                  throw std::invalid_argument (
                      "macro argument count mismatch for " + name);
                std::unordered_map<std::string, std::string> parameters;
                for (std::size_t a = 0; a < arguments.size (); ++a)
                  parameters[macro.parameters[a]]
                      = expand_impl (arguments[a], disabled, depth + 1);
                replacement = substitute (replacement, parameters);
                i = cursor;
              }
            disabled.insert (name);
            out += expand_impl (replacement, disabled, depth + 1);
            disabled.erase (name);
          }
        else
          out += source[i++];
      }
    return out;
  }
  struct Condition
  {
    std::string_view source;
    std::size_t pos = 0;
    const Preprocessor &owner;
    std::unordered_set<std::string> disabled;
    void
    skip ()
    {
      while (pos < source.size () && source[pos] <= ' ')
        ++pos;
    }
    bool
    take (std::string_view text)
    {
      skip ();
      if (source.substr (pos, text.size ()) != text)
        return false;
      pos += text.size ();
      return true;
    }
    long long
    primary ()
    {
      skip ();
      if (take ("!"))
        return !primary ();
      if (take ("("))
        {
          auto value = either ();
          if (!take (")"))
            throw std::invalid_argument ("missing ')' in #if");
          return value;
        }
      auto rest = source.substr (pos);
      auto name = preprocess_detail::word (rest);
      if (!name.empty () && preprocess_detail::identifier_start (name[0]))
        {
          pos = source.size () - rest.size ();
          if (name == "defined")
            {
              auto paren = take ("(");
              rest = source.substr (pos);
              auto id = preprocess_detail::word (rest);
              pos = source.size () - rest.size ();
              if (id.empty () || (paren && !take (")")))
                throw std::invalid_argument ("invalid defined expression");
              return owner.macros.contains (std::string (id));
            }
          auto found = owner.macros.find (std::string (name));
          if (found == owner.macros.end () || found->second.function
              || disabled.contains (std::string (name)))
            return 0;
          auto next = disabled;
          next.insert (std::string (name));
          return Condition{ found->second.replacement, 0, owner,
                            std::move (next) }
              .evaluate ();
        }
      auto begin = pos;
      if (pos < source.size () && (source[pos] == '-' || source[pos] == '+'))
        ++pos;
      while (pos < source.size ()
             && preprocess_detail::identifier_char (source[pos]))
        ++pos;
      if (begin == pos)
        throw std::invalid_argument ("expected integer or identifier in #if");
      auto integer = source.substr (begin, pos - begin);
      auto digit
          = !integer.empty () && (integer[0] == '-' || integer[0] == '+') ? 1u
                                                                          : 0u;
      int radix
          = integer.size () > digit + 1 && integer[digit] == '0'
                    && (integer[digit + 1] == 'x' || integer[digit + 1] == 'X')
                ? 16
                : 10;
      std::size_t used;
      auto value = std::stoll (std::string (integer), &used, radix);
      if (used != pos - begin)
        throw std::invalid_argument ("invalid #if integer");
      return value;
    }
    long long
    compare ()
    {
      auto a = primary ();
      if (take ("=="))
        return a == primary ();
      if (take ("!="))
        return a != primary ();
      if (take ("<="))
        return a <= primary ();
      if (take (">="))
        return a >= primary ();
      if (take ("<"))
        return a < primary ();
      if (take (">"))
        return a > primary ();
      return a;
    }
    long long
    both ()
    {
      auto a = compare ();
      while (take ("&&"))
        {
          auto b = compare ();
          a = a && b;
        }
      return a;
    }
    long long
    either ()
    {
      auto a = both ();
      while (take ("||"))
        {
          auto b = both ();
          a = a || b;
        }
      return a;
    }
    long long
    evaluate ()
    {
      auto value = either ();
      skip ();
      if (pos != source.size ())
        throw std::invalid_argument ("unsupported #if expression");
      return value;
    }
  };

public:
  std::string
  expand (std::string_view source)
  {
    return expand_impl (source, {}, 0);
  }
  /// Object/function macros (no token-pasting/stringification), includes,
  /// #if/ifdef/ifndef/elif/else/endif, #undef, #error and #lua/#script.
  /// Conditions accept integer constants, defined, !, comparisons, && and ||.
  std::string
  process (std::string_view source, std::size_t depth = 0)
  {
    if (depth > max_include_depth)
      throw std::runtime_error ("preprocessor include depth exceeded");
    std::string joined;
    for (std::size_t i = 0; i < source.size (); ++i)
      {
        if (source[i] == '\\' && i + 1 < source.size ()
            && source[i + 1] == '\n')
          ++i;
        else
          joined += source[i];
      }
    source = joined;
    struct Branch
    {
      bool parent, taken, active, seen_else = false;
    };
    std::vector<Branch> branches;
    std::string output;
    auto active = [&] { return branches.empty () || branches.back ().active; };
    for (std::size_t begin = 0; begin < source.size ();)
      {
        auto end = source.find ('\n', begin);
        bool newline = end != source.npos;
        if (!newline)
          end = source.size ();
        auto line = source.substr (begin, end - begin);
        begin = end + (newline ? 1 : 0);
        auto directive = preprocess_detail::trim (line);
        if (in_comment_ || directive.empty () || directive[0] != '#')
          {
            if (active ())
              {
                output += expand (line);
                if (newline)
                  output += '\n';
              }
            else
              {
                std::unordered_set<std::string> disabled;
                for (const auto &[name, macro] : macros)
                  {
                    (void)macro;
                    disabled.insert (name);
                  }
                expand_impl (line, std::move (disabled), 0);
              }
            continue;
          }
        directive.remove_prefix (1);
        auto command = preprocess_detail::word (directive);
        directive = preprocess_detail::trim (directive);
        if (command == "if" || command == "ifdef" || command == "ifndef")
          {
            bool parent = active (), value = false;
            if (parent)
              {
                if (command == "if")
                  value
                      = Condition{ directive, 0, *this, {} }.evaluate () != 0;
                else
                  {
                    value = macros.contains (std::string (directive));
                    if (command == "ifndef")
                      value = !value;
                  }
              }
            branches.push_back ({ parent, value, parent && value });
          }
        else if (command == "else" || command == "elif")
          {
            if (branches.empty () || branches.back ().seen_else)
              throw std::invalid_argument (
                  "unmatched or duplicate conditional branch");
            auto &branch = branches.back ();
            bool value
                = command == "else"
                  || (branch.parent && !branch.taken
                      && Condition{ directive, 0, *this, {} }.evaluate ());
            branch.active = branch.parent && !branch.taken && value;
            branch.taken |= value;
            branch.seen_else = command == "else";
          }
        else if (command == "endif")
          {
            if (branches.empty ())
              throw std::invalid_argument ("unmatched #endif");
            branches.pop_back ();
          }
        else if (!active ())
          continue;
        else if (command == "define")
          {
            auto name = preprocess_detail::word (directive);
            if (name.empty ()
                || !preprocess_detail::identifier_start (name[0]))
              throw std::invalid_argument ("invalid macro name");
            if (!directive.empty () && directive.front () == '(')
              {
                auto close = directive.find (')');
                if (close == directive.npos)
                  throw std::invalid_argument (
                      "unterminated macro parameters");
                auto params = directive.substr (1, close - 1);
                std::vector<std::string> parameters;
                while (!preprocess_detail::trim (params).empty ())
                  {
                    auto comma = params.find (',');
                    auto param
                        = preprocess_detail::trim (params.substr (0, comma));
                    auto check = param;
                    auto id = preprocess_detail::word (check);
                    if (id.empty ()
                        || !preprocess_detail::identifier_start (id[0])
                        || !preprocess_detail::trim (check).empty ()
                        || std::find (parameters.begin (), parameters.end (),
                                      id)
                               != parameters.end ())
                      throw std::invalid_argument ("invalid macro parameter");
                    parameters.emplace_back (id);
                    if (comma == params.npos)
                      break;
                    params.remove_prefix (comma + 1);
                    if (preprocess_detail::trim (params).empty ())
                      throw std::invalid_argument (
                          "trailing comma in macro parameters");
                  }
                define_function (std::string (name), std::move (parameters),
                                 std::string (preprocess_detail::trim (
                                     directive.substr (close + 1))));
              }
            else
              define (std::string (name),
                      std::string (preprocess_detail::trim (directive)));
          }
        else if (command == "undef")
          macros.erase (std::string (directive));
        else if (command == "include")
          {
            auto expanded_path = expand (directive);
            auto path = preprocess_detail::trim (expanded_path);
            if (path.size () < 2
                || !((path.front () == '"' && path.back () == '"')
                     || (path.front () == '<' && path.back () == '>'))
                || !include)
              throw std::invalid_argument ("#include requires a resolver and "
                                           "a quoted or angle-bracket path");
            output += process (include (path.substr (1, path.size () - 2)),
                               depth + 1);
            if (newline && !output.empty () && output.back () != '\n')
              output += '\n';
          }
        else if (command == "lua" || command == "script")
          {
            if (!script)
              throw std::invalid_argument (
                  "script directive requires a user script backend");
            auto language = command == "lua"
                                ? std::string_view ("lua")
                                : preprocess_detail::word (directive);
            output += expand (
                script (language, preprocess_detail::trim (directive)));
            if (newline)
              output += '\n';
          }
        else if (command == "error")
          throw std::runtime_error (std::string (directive));
        else
          throw std::invalid_argument ("unknown preprocessor directive "
                                       + std::string (command));
      }
    if (!branches.empty ())
      throw std::invalid_argument ("unterminated preprocessor conditional");
    return output;
  }
};
#if defined(LIBDSL_ENABLE_LUA)
/// Link the user's Lua library and define LIBDSL_ENABLE_LUA to enable this
/// bridge. The caller owns the Lua state. Each directive must return a string.
inline auto
lua_script_hook (lua_State *state)
{
  return
      [state] (std::string_view language, std::string_view code) -> std::string
        {
          if (!state || language != "lua")
            throw std::invalid_argument ("invalid Lua script backend");
          int top = lua_gettop (state);
          struct Restore
          {
            lua_State *state;
            int top;
            ~Restore () { lua_settop (state, top); }
          } restore{ state, top };
          if (luaL_loadbuffer (state, code.data (), code.size (),
                               "LibDSL preprocessor")
              || lua_pcall (state, 0, 1, 0))
            throw std::runtime_error (lua_tostring (state, -1)
                                          ? lua_tostring (state, -1)
                                          : "Lua preprocessing failed");
          std::size_t size = 0;
          const char *value = lua_tolstring (state, -1, &size);
          if (!value)
            throw std::runtime_error (
                "Lua preprocessing must return a string");
          return std::string (value, size);
        };
}
#endif

// ============================================================
//  Utility concepts
// ============================================================

/// Concept: T has a feature F mixed in.
template <typename T, typename F>
concept HasFeature = std::derived_from<T, typename F::template Mixin<T>>;

/// Concept: T is a DSL with Pipeline support.
template <typename T>
concept Pipeable = HasFeature<T, Pipeline>;

/// Concept: T is a DSL with ExprTemplates support.
template <typename T>
concept ExprLike = HasFeature<T, ExprTemplates>
                   && requires (const T &t) { t.expr_value (); };

/// Concept: T is a DSL with CustomLiterals support.
template <typename T>
concept HasLiterals
    = HasFeature<T, CustomLiterals> && requires { T::literals; };

/// Concept: T is a DSL with Rewrite support.
template <typename T>
concept Rewritable = HasFeature<T, Rewrite> && requires { T::rules; };

struct Graphs { template <class Derived> struct Mixin {}; };
struct Terms { template <class Derived> struct Mixin {}; };
struct TermRewriting { template <class Derived> struct Mixin {}; };
struct GraphReduction { template <class Derived> struct Mixin {}; };

using GraphNodeId = std::size_t;
using GraphEdgeId = std::size_t;

inline std::string dot_quote(std::string_view text)
{
  std::string result = "\"";
  for (unsigned char character : text)
    switch (character)
      {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += '\t'; break;
      default:
        if (character < 32 || character == 127)
          throw std::invalid_argument("unsupported control character in DOT label");
        result += static_cast<char>(character);
      }
  return result + '"';
}

template <class NodeValue = std::string, class EdgeValue = std::string>
class DirectedGraph
{
public:
  struct Edge { GraphNodeId source, target; EdgeValue value; };
private:
  struct Node
  {
    NodeValue value;
    std::vector<GraphEdgeId> incoming, outgoing;
  };
  std::vector<std::optional<Node>> nodes_;
  std::vector<std::optional<Edge>> edges_;
  void require_node(GraphNodeId id) const
  {
    if (!contains(id)) throw std::out_of_range("invalid graph node");
  }
public:
  bool contains(GraphNodeId id) const { return id < nodes_.size() && nodes_[id].has_value(); }
  bool contains_edge(GraphEdgeId id) const { return id < edges_.size() && edges_[id].has_value(); }
  GraphNodeId add_node(NodeValue value)
  {
    nodes_.push_back(Node{std::move(value), {}, {}});
    return nodes_.size() - 1;
  }
  GraphEdgeId add_edge(GraphNodeId source, GraphNodeId target, EdgeValue value = {})
  {
    require_node(source); require_node(target);
    auto id = edges_.size();
    edges_.push_back(Edge{source, target, std::move(value)});
    nodes_[source]->outgoing.push_back(id);
    nodes_[target]->incoming.push_back(id);
    return id;
  }
  const NodeValue& value(GraphNodeId id) const { require_node(id); return nodes_[id]->value; }
  void set_value(GraphNodeId id, NodeValue value) { require_node(id); nodes_[id]->value = std::move(value); }
  const Edge& edge(GraphEdgeId id) const
  {
    if (!contains_edge(id)) throw std::out_of_range("invalid graph edge");
    return *edges_[id];
  }
  std::vector<GraphNodeId> nodes() const
  {
    std::vector<GraphNodeId> result;
    for (GraphNodeId id = 0; id < nodes_.size(); ++id) if (contains(id)) result.push_back(id);
    return result;
  }
  std::vector<GraphEdgeId> edges() const
  {
    std::vector<GraphEdgeId> result;
    for (GraphEdgeId id = 0; id < edges_.size(); ++id) if (contains_edge(id)) result.push_back(id);
    return result;
  }
  std::size_t node_count() const { return nodes().size(); }
  std::size_t edge_count() const { return edges().size(); }
  std::vector<GraphEdgeId> incoming(GraphNodeId id) const { require_node(id); return nodes_[id]->incoming; }
  std::vector<GraphEdgeId> outgoing(GraphNodeId id) const { require_node(id); return nodes_[id]->outgoing; }
  std::vector<GraphNodeId> successors(GraphNodeId id) const
  {
    std::vector<GraphNodeId> result;
    for (auto link : outgoing(id)) result.push_back(edge(link).target);
    return result;
  }
  std::vector<GraphNodeId> predecessors(GraphNodeId id) const
  {
    std::vector<GraphNodeId> result;
    for (auto link : incoming(id)) result.push_back(edge(link).source);
    return result;
  }
  void remove_edge(GraphEdgeId id)
  {
    const auto& link = edge(id);
    std::erase(nodes_[link.source]->outgoing, id);
    std::erase(nodes_[link.target]->incoming, id);
    edges_[id].reset();
  }
  void remove_node(GraphNodeId id)
  {
    require_node(id);
    for (auto link : outgoing(id)) remove_edge(link);
    for (auto link : incoming(id)) remove_edge(link);
    nodes_[id].reset();
  }
  void redirect(GraphNodeId from, GraphNodeId to)
  {
    require_node(from); require_node(to);
    if (from == to) return;
    for (auto link : incoming(from))
      {
        edges_[link]->target = to;
        nodes_[to]->incoming.push_back(link);
      }
    nodes_[from]->incoming.clear();
  }
  std::vector<GraphNodeId> breadth_first(GraphNodeId root) const
  {
    require_node(root);
    std::vector<GraphNodeId> result{root};
    std::unordered_set<GraphNodeId> seen{root};
    for (std::size_t index = 0; index < result.size(); ++index)
      for (auto child : successors(result[index]))
        if (seen.insert(child).second) result.push_back(child);
    return result;
  }
  std::vector<GraphNodeId> depth_first(GraphNodeId root, bool postorder = false) const
  {
    require_node(root);
    std::vector<GraphNodeId> result;
    std::vector<std::pair<GraphNodeId, bool>> pending{{root, false}};
    std::unordered_set<GraphNodeId> seen;
    while (!pending.empty())
      {
        auto [id, expanded] = pending.back(); pending.pop_back();
        if (expanded) { result.push_back(id); continue; }
        if (!seen.insert(id).second) continue;
        if (postorder) pending.emplace_back(id, true); else result.push_back(id);
        auto children = successors(id);
        for (auto child = children.rbegin(); child != children.rend(); ++child)
          pending.emplace_back(*child, false);
      }
    return result;
  }
  std::vector<GraphNodeId> topological_sort() const
  {
    std::vector<std::size_t> degrees(nodes_.size());
    std::vector<GraphNodeId> result;
    auto live = nodes();
    for (auto id : live)
      {
        degrees[id] = nodes_[id]->incoming.size();
        if (!degrees[id]) result.push_back(id);
      }
    for (std::size_t index = 0; index < result.size(); ++index)
      for (auto child : successors(result[index]))
        if (!--degrees[child]) result.push_back(child);
    if (result.size() != live.size()) throw std::invalid_argument("graph contains a cycle");
    return result;
  }
  std::optional<std::vector<GraphNodeId>> shortest_path(GraphNodeId source, GraphNodeId target) const
  {
    require_node(source); require_node(target);
    std::vector<GraphNodeId> pending{source};
    std::unordered_map<GraphNodeId, GraphNodeId> parents{{source, source}};
    for (std::size_t index = 0; index < pending.size(); ++index)
      {
        auto id = pending[index];
        if (id == target)
          {
            std::vector<GraphNodeId> path{target};
            while (path.back() != source) path.push_back(parents.at(path.back()));
            std::reverse(path.begin(), path.end());
            return path;
          }
        for (auto child : successors(id))
          if (parents.emplace(child, id).second) pending.push_back(child);
      }
    return std::nullopt;
  }
  std::vector<std::vector<GraphNodeId>> strongly_connected_components() const
  {
    std::unordered_set<GraphNodeId> seen;
    std::vector<GraphNodeId> order;
    for (auto root : nodes())
      {
        if (seen.contains(root)) continue;
        std::vector<std::pair<GraphNodeId, bool>> pending{{root, false}};
        while (!pending.empty())
          {
            auto [id, expanded] = pending.back(); pending.pop_back();
            if (expanded) { order.push_back(id); continue; }
            if (!seen.insert(id).second) continue;
            pending.emplace_back(id, true);
            auto children = successors(id);
            for (auto child = children.rbegin(); child != children.rend(); ++child)
              pending.emplace_back(*child, false);
          }
      }
    seen.clear();
    std::vector<std::vector<GraphNodeId>> result;
    for (auto root = order.rbegin(); root != order.rend(); ++root)
      {
        if (!seen.insert(*root).second) continue;
        std::vector<GraphNodeId> component{*root};
        for (std::size_t index = 0; index < component.size(); ++index)
          for (auto parent : predecessors(component[index]))
            if (seen.insert(parent).second) component.push_back(parent);
        result.push_back(std::move(component));
      }
    return result;
  }
  template <class NodeLabel, class EdgeLabel>
  std::string to_dot(NodeLabel node_label, EdgeLabel edge_label, std::string_view name = "G") const
  {
    std::string result = "digraph " + dot_quote(name) + " {\n";
    for (auto id : nodes())
      result += "  n" + std::to_string(id) + " [label=" + dot_quote(node_label(value(id))) + "];\n";
    for (auto id : edges())
      {
        const auto& link = edge(id);
        result += "  n" + std::to_string(link.source) + " -> n" + std::to_string(link.target)
                  + " [label=" + dot_quote(edge_label(link.value)) + "];\n";
      }
    return result + "}\n";
  }
  std::string to_dot(std::string_view name = "G") const
  {
    auto label = [](const auto& value) { std::ostringstream stream; stream << value; return stream.str(); };
    return to_dot(label, label, name);
  }
};

struct Term
{
  std::string symbol;
  std::vector<Term> arguments;
  bool is_variable = false;
  static Term variable(std::string name)
  {
    if (name.empty() || name.find_first_of(" \t\r\n()\";\\?") != std::string::npos)
      throw std::invalid_argument("invalid term variable name");
    for (unsigned char character : name)
      if (character <= 32 || character == 127) throw std::invalid_argument("invalid term variable name");
    return {std::move(name), {}, true};
  }
  static Term function(std::string symbol, std::vector<Term> arguments = {})
  { return {std::move(symbol), std::move(arguments), false}; }
  bool operator==(const Term&) const = default;
  void validate() const
  {
    if (is_variable)
      {
        variable(symbol);
        if (!arguments.empty()) throw std::invalid_argument("variable cannot have arguments");
      }
    for (const auto& child : arguments) child.validate();
  }
  std::unordered_set<std::string> variables() const
  {
    std::unordered_set<std::string> result;
    if (is_variable) result.insert(symbol);
    for (const auto& child : arguments)
      { auto names = child.variables(); result.insert(names.begin(), names.end()); }
    return result;
  }
  bool ground() const { return variables().empty(); }
  std::size_t size() const
  {
    std::size_t result = 1;
    for (const auto& child : arguments) result += child.size();
    return result;
  }
  const Term& at(std::span<const std::size_t> position) const
  {
    const Term* result = this;
    for (auto index : position) result = &result->arguments.at(index);
    return *result;
  }
  Term replaced(std::span<const std::size_t> position, const Term& replacement) const
  {
    if (position.empty()) return replacement;
    Term result = *this;
    result.arguments.at(position.front()) = arguments.at(position.front()).replaced(position.subspan(1), replacement);
    return result;
  }
  SExpr sexpr() const
  {
    validate();
    if (is_variable) return SExpr::atom("?" + symbol);
    auto head = SExpr::atom(symbol, !symbol.empty() && symbol.front() == '?');
    if (arguments.empty()) return head;
    std::vector<SExpr> items{head};
    for (const auto& child : arguments) items.push_back(child.sexpr());
    return SExpr::list(std::move(items));
  }
  std::string dump() const { return sexpr().dump(); }
};

inline Term term_from_sexpr(const SExpr& expression)
{
  if (expression.is_atom)
    {
      if (!expression.quoted && !expression.text.empty() && expression.text.front() == '?')
        return Term::variable(expression.text.substr(1));
      return Term::function(expression.text);
    }
  if (expression.items.empty() || !expression.items.front().is_atom
      || (!expression.items.front().quoted && !expression.items.front().text.empty()
          && expression.items.front().text.front() == '?'))
    throw std::invalid_argument("term requires a literal function symbol");
  std::vector<Term> arguments;
  for (std::size_t index = 1; index < expression.items.size(); ++index)
    arguments.push_back(term_from_sexpr(expression.items[index]));
  return Term::function(expression.items.front().text, std::move(arguments));
}
inline Term parse_term(std::string_view source) { return term_from_sexpr(parse_sexpr(source)); }

using TermSubstitution = std::unordered_map<std::string, Term>;

inline std::optional<TermSubstitution> match_term(const Term& pattern, const Term& subject)
{
  pattern.validate(); subject.validate();
  TermSubstitution bindings;
  std::function<bool(const Term&, const Term&)> match = [&](const Term& current, const Term& value) {
    if (current.is_variable)
      {
        auto [entry, inserted] = bindings.emplace(current.symbol, value);
        return inserted || entry->second == value;
      }
    if (value.is_variable || current.symbol != value.symbol || current.arguments.size() != value.arguments.size())
      return false;
    for (std::size_t index = 0; index < current.arguments.size(); ++index)
      if (!match(current.arguments[index], value.arguments[index])) return false;
    return true;
  };
  if (!match(pattern, subject)) return std::nullopt;
  return bindings;
}

inline Term substitute(const Term& term, const TermSubstitution& bindings)
{
  if (term.is_variable)
    {
      auto found = bindings.find(term.symbol);
      return found == bindings.end() ? term : found->second;
    }
  Term result = term;
  for (auto& child : result.arguments) child = substitute(child, bindings);
  return result;
}

inline std::optional<TermSubstitution> unify_terms(Term left, Term right)
{
  left.validate(); right.validate();
  TermSubstitution bindings;
  std::vector<std::pair<Term, Term>> pending{{std::move(left), std::move(right)}};
  while (!pending.empty())
    {
      auto [first, second] = std::move(pending.back()); pending.pop_back();
      first = substitute(first, bindings); second = substitute(second, bindings);
      if (first == second) continue;
      if (!first.is_variable && second.is_variable) std::swap(first, second);
      if (first.is_variable)
        {
          if (second.variables().contains(first.symbol)) return std::nullopt;
          TermSubstitution binding{{first.symbol, second}};
          for (auto& entry : bindings) entry.second = substitute(entry.second, binding);
          bindings[first.symbol] = second;
        }
      else
        {
          if (first.symbol != second.symbol || first.arguments.size() != second.arguments.size()) return std::nullopt;
          for (std::size_t index = 0; index < first.arguments.size(); ++index)
            pending.emplace_back(first.arguments[index], second.arguments[index]);
        }
    }
  return bindings;
}

struct TermRule
{
  std::string name;
  Term lhs, rhs;
  void validate() const
  {
    lhs.validate(); rhs.validate();
    if (lhs.is_variable) throw std::invalid_argument("rewrite left side cannot be a variable");
    auto available = lhs.variables();
    for (const auto& variable : rhs.variables())
      if (!available.contains(variable)) throw std::invalid_argument("unbound rewrite variable");
  }
};
inline TermRule term_rule(std::string_view source)
{
  auto parsed = sexpr_rule(source);
  TermRule result{parsed.name, term_from_sexpr(parsed.lhs), term_from_sexpr(parsed.rhs)};
  result.validate();
  return result;
}
enum class RewriteStrategy { Outermost, Innermost };
enum class ReductionStop { NormalForm, StepLimit };
struct TermRewriteStep
{
  Term term;
  std::size_t rule_index;
  std::vector<std::size_t> position;
};
struct TermReductionResult
{
  Term term;
  std::size_t steps;
  ReductionStop stop;
  bool normal_form() const { return stop == ReductionStop::NormalForm; }
};

class TermRewriteSystem
{
  std::vector<TermRule> rules_;
public:
  void add_rule(TermRule rule) { rule.validate(); rules_.push_back(std::move(rule)); }
  void add_rule(std::string_view source) { add_rule(term_rule(source)); }
  const std::vector<TermRule>& rules() const { return rules_; }
  std::optional<TermRewriteStep> rewrite_once(const Term& term, RewriteStrategy strategy = RewriteStrategy::Outermost) const
  {
    term.validate();
    std::vector<std::size_t> position;
    std::optional<TermRewriteStep> result;
    auto at_root = [&](const Term& current) {
      for (std::size_t index = 0; index < rules_.size(); ++index)
        if (auto bindings = match_term(rules_[index].lhs, current))
          {
            auto replacement = substitute(rules_[index].rhs, *bindings);
            result = TermRewriteStep{term.replaced(position, replacement), index, position};
            return true;
          }
      return false;
    };
    std::function<bool(const Term&)> visit = [&](const Term& current) {
      if (strategy == RewriteStrategy::Outermost && at_root(current)) return true;
      for (std::size_t index = 0; index < current.arguments.size(); ++index)
        {
          position.push_back(index);
          if (visit(current.arguments[index])) return true;
          position.pop_back();
        }
      return strategy == RewriteStrategy::Innermost && at_root(current);
    };
    visit(term);
    return result;
  }
  TermReductionResult normalize(Term term, std::size_t max_steps = 1000,
                                RewriteStrategy strategy = RewriteStrategy::Outermost) const
  {
    std::size_t steps = 0;
    while (auto next = rewrite_once(term, strategy))
      {
        if (steps == max_steps) return {std::move(term), steps, ReductionStop::StepLimit};
        term = std::move(next->term); ++steps;
      }
    return {std::move(term), steps, ReductionStop::NormalForm};
  }
};

struct TermGraphLabel
{
  std::string symbol;
  bool is_variable = false;
  bool operator==(const TermGraphLabel&) const = default;
};
struct GraphRewriteStep { GraphNodeId root, redex, replacement; std::size_t rule_index; };
struct GraphReductionResult
{
  GraphNodeId root;
  std::size_t steps;
  ReductionStop stop;
  bool normal_form() const { return stop == ReductionStop::NormalForm; }
};

class TermGraph
{
  DirectedGraph<TermGraphLabel, std::size_t> graph_;
  using Bindings = std::unordered_map<std::string, GraphNodeId>;
  static std::string node_key(const TermGraphLabel& label, const std::vector<GraphNodeId>& children)
  {
    std::string key = (label.is_variable ? "v" : "f") + std::to_string(label.symbol.size()) + ":" + label.symbol;
    for (auto child : children) key += ":" + std::to_string(child);
    return key;
  }
  bool equal_nodes(GraphNodeId left, GraphNodeId right) const
  {
    std::unordered_map<GraphNodeId, std::unordered_set<GraphNodeId>> seen;
    std::vector<std::pair<GraphNodeId, GraphNodeId>> pending{{left, right}};
    while (!pending.empty())
      {
        auto [first, second] = pending.back(); pending.pop_back();
        if (first == second || !seen[first].insert(second).second) continue;
        if (graph_.value(first) != graph_.value(second)) return false;
        auto first_children = graph_.successors(first), second_children = graph_.successors(second);
        if (first_children.size() != second_children.size()) return false;
        for (std::size_t index = 0; index < first_children.size(); ++index)
          pending.emplace_back(first_children[index], second_children[index]);
      }
    return true;
  }
  bool match(const Term& pattern, GraphNodeId id, Bindings& bindings) const
  {
    if (pattern.is_variable)
      {
        auto [entry, inserted] = bindings.emplace(pattern.symbol, id);
        return inserted || equal_nodes(entry->second, id);
      }
    auto label = graph_.value(id);
    auto children = graph_.successors(id);
    if (label.is_variable || label.symbol != pattern.symbol || children.size() != pattern.arguments.size()) return false;
    for (std::size_t index = 0; index < children.size(); ++index)
      if (!match(pattern.arguments[index], children[index], bindings)) return false;
    return true;
  }
  GraphNodeId instantiate(const Term& term, const Bindings& bindings)
  {
    if (term.is_variable) return bindings.at(term.symbol);
    std::vector<GraphNodeId> children;
    for (const auto& child : term.arguments) children.push_back(instantiate(child, bindings));
    return add_node(term.symbol, children);
  }
  struct Candidate { GraphNodeId id; std::size_t rule; Bindings bindings; };
  std::optional<Candidate> find(GraphNodeId root, const TermRewriteSystem& system, RewriteStrategy strategy) const
  {
    for (auto id : graph_.depth_first(root, strategy == RewriteStrategy::Innermost))
      for (std::size_t index = 0; index < system.rules().size(); ++index)
        {
          Bindings bindings;
          if (match(system.rules()[index].lhs, id, bindings)) return Candidate{id, index, std::move(bindings)};
        }
    return std::nullopt;
  }
  GraphRewriteStep apply(GraphNodeId root, const TermRewriteSystem& system, const Candidate& candidate)
  {
    auto replacement = instantiate(system.rules()[candidate.rule].rhs, candidate.bindings);
    graph_.redirect(candidate.id, replacement);
    graph_.remove_node(candidate.id);
    return {root == candidate.id ? replacement : root, candidate.id, replacement, candidate.rule};
  }
public:
  const DirectedGraph<TermGraphLabel, std::size_t>& graph() const { return graph_; }
  GraphNodeId add_node(std::string symbol, const std::vector<GraphNodeId>& children = {})
  {
    for (auto child : children) graph_.value(child);
    auto id = graph_.add_node({std::move(symbol), false});
    for (std::size_t index = 0; index < children.size(); ++index) graph_.add_edge(id, children[index], index);
    return id;
  }
  GraphNodeId add(const Term& term, bool share = true)
  {
    term.validate();
    std::unordered_map<std::string, GraphNodeId> interned;
    if (share)
      for (auto id : graph_.nodes()) interned.emplace(node_key(graph_.value(id), graph_.successors(id)), id);
    std::function<GraphNodeId(const Term&)> insert = [&](const Term& current) {
      std::vector<GraphNodeId> children;
      for (const auto& child : current.arguments) children.push_back(insert(child));
      TermGraphLabel label{current.symbol, current.is_variable};
      auto key = share ? node_key(label, children) : std::string{};
      if (share)
        if (auto found = interned.find(key); found != interned.end()) return found->second;
      auto id = graph_.add_node(label);
      for (std::size_t index = 0; index < children.size(); ++index) graph_.add_edge(id, children[index], index);
      if (share) interned.emplace(std::move(key), id);
      return id;
    };
    return insert(term);
  }
  GraphNodeId add(std::string_view source, bool share = true) { return add(parse_term(source), share); }
  Term to_term(GraphNodeId root) const
  {
    auto label = graph_.value(root);
    if (label.is_variable) return Term::variable(label.symbol);
    std::vector<Term> arguments;
    for (auto child : graph_.successors(root)) arguments.push_back(to_term(child));
    return Term::function(label.symbol, std::move(arguments));
  }
  std::optional<GraphRewriteStep> rewrite_once(GraphNodeId root, const TermRewriteSystem& system,
                                               RewriteStrategy strategy = RewriteStrategy::Outermost)
  {
    auto candidate = find(root, system, strategy);
    if (!candidate) return std::nullopt;
    return apply(root, system, *candidate);
  }
  GraphReductionResult reduce(GraphNodeId root, const TermRewriteSystem& system, std::size_t max_steps = 1000,
                               RewriteStrategy strategy = RewriteStrategy::Outermost)
  {
    std::size_t steps = 0;
    while (auto candidate = find(root, system, strategy))
      {
        if (steps == max_steps) return {root, steps, ReductionStop::StepLimit};
        root = apply(root, system, *candidate).root; ++steps;
      }
    return {root, steps, ReductionStop::NormalForm};
  }
  void collect_garbage(const std::vector<GraphNodeId>& roots)
  {
    std::unordered_set<GraphNodeId> reachable;
    for (auto root : roots)
      { auto visited = graph_.depth_first(root); reachable.insert(visited.begin(), visited.end()); }
    for (auto id : graph_.nodes()) if (!reachable.contains(id)) graph_.remove_node(id);
  }
  std::string to_dot(std::string_view name = "TermGraph") const
  {
    return graph_.to_dot([](const TermGraphLabel& label) { return label.is_variable ? "?" + label.symbol : label.symbol; },
                         [](std::size_t index) { return std::to_string(index); }, name);
  }
};


// BEGIN LIBDSL LIVE FACULTIES
// Notice for terminal code derived from TTyPack:
// MIT License
//
// Copyright (c) 2026 Chubak Bidpaa <chubakbidpaa@riseup.net>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Shared services for live language tools.
// Integration: include <memory_resource> with the header's standard includes.
namespace live
{
namespace support_detail
{
struct TextHash
{
  using is_transparent = void;

  std::size_t operator()(std::string_view value) const noexcept
  {
    return std::hash<std::string_view>{}(value);
  }
};

struct TextEqual
{
  using is_transparent = void;

  bool operator()(std::string_view left, std::string_view right) const noexcept
  {
    return left == right;
  }
};

inline std::pmr::memory_resource* resource_or_default(std::pmr::memory_resource* resource) noexcept
{
  return resource ? resource : std::pmr::get_default_resource();
}

// Treat CRLF as one break, and recognize standalone CR and LF as well.
// Keep an entry after a trailing break so EOF maps to a real position.
inline std::pmr::vector<std::size_t>
line_starts(std::string_view text, std::pmr::memory_resource* resource)
{
  std::pmr::vector<std::size_t> starts(resource);
  starts.push_back(0);
  for (std::size_t i = 0; i < text.size();)
    {
      const auto byte = text[i++];
      if (byte == '\r' && i < text.size() && text[i] == '\n')
        ++i;
      if (byte == '\r' || byte == '\n')
        starts.push_back(i);
    }
  return starts;
}

// Entries may safely publish views into their strings (including short
// strings) because subsequent insertions move only the owning pointers.
struct SourceBuffer
{
  const std::pmr::string name;
  const std::pmr::string text;
  const std::pmr::vector<std::size_t> lines;

  SourceBuffer(std::string_view name_, std::string_view text_,
               std::pmr::memory_resource* resource)
      : name(name_, resource), text(text_, resource),
        lines(line_starts(text, resource))
  {
  }
};

// Type tags are process-local implementation details and are never persisted.
// Equality is based on the exact function signature, not argument conversions.
template <typename Signature> inline constexpr char callback_signature_tag = 0;

struct CallbackEntry
{
  const void* signature;
  std::shared_ptr<const void> callable;

  template <typename Signature>
  const std::function<Signature>* get() const noexcept
  {
    return signature == &callback_signature_tag<Signature>
               ? static_cast<const std::function<Signature>*>(callable.get())
               : nullptr;
  }
};
} // namespace support_detail

using SourceId = std::size_t;
using OriginId = std::size_t;
using NodeId = std::size_t;

// Offsets are half-open byte offsets. Source zero permits an anonymous span
// in an adapter that only has the input buffer, not a SourceManager.
struct SourceSpan
{
  std::size_t begin = 0;
  std::size_t end = 0;
  SourceId source = 0;

  constexpr bool ordered() const noexcept { return begin <= end; }
  constexpr bool empty() const noexcept { return begin == end; }
  constexpr bool operator==(const SourceSpan&) const = default;
};

struct SourceLocation
{
  SourceId source = 0;
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;

  constexpr bool operator==(const SourceLocation&) const = default;
};

enum class Severity { note, warning, error };

struct DiagnosticNote
{
  std::string message;
  SourceSpan span{};
};

// Replace span with replacement. An empty span represents an insertion;
// an empty replacement represents a deletion.
struct FixIt
{
  SourceSpan span{};
  std::string replacement;
};

// Code is an application-defined stable machine-readable key. The message
// and notes are data, not terminal escape sequences or format strings.
struct Diagnostic
{
  Severity severity = Severity::error;
  std::string code;
  std::string message;
  SourceSpan span{};
  std::vector<DiagnosticNote> notes{};
  OriginId origin = 0;
  std::vector<FixIt> fixits{};
  NodeId node = 0;
};

// A borrowed resource must outlive its users. arena() instead owns a
// monotonic resource (but still borrows the upstream resource, if supplied).
// Copy the Storage to share ownership; bare PMR allocators don't keep it alive.
class Storage
{
  std::shared_ptr<std::pmr::memory_resource> owner_;
  std::pmr::memory_resource* resource_;

public:
  explicit Storage(std::pmr::memory_resource* resource = nullptr) noexcept
      : resource_(support_detail::resource_or_default(resource))
  {
  }

  static Storage arena(std::pmr::memory_resource* upstream = nullptr)
  {
    Storage result;
    result.owner_ = std::make_shared<std::pmr::monotonic_buffer_resource>(
        support_detail::resource_or_default(upstream));
    result.resource_ = result.owner_.get();
    return result;
  }

  // Owns the PMR resource, not these bytes. The buffer must be exclusively
  // available to this arena and outlive its last owner and allocation.
  // Exhaustion throws std::bad_alloc: it never falls back to the heap for
  // allocations from the resource.
  static Storage fixed(std::span<std::byte> buffer)
  {
    Storage result;
    result.owner_ = std::make_shared<std::pmr::monotonic_buffer_resource>(
        buffer.data(), buffer.size_bytes(), std::pmr::null_memory_resource());
    result.resource_ = result.owner_.get();
    return result;
  }

  // Share explicit ownership of the input bytes, too. The resource is
  // destroyed before the holder releases its reference to the array.
  template <std::size_t N>
  static Storage fixed(std::shared_ptr<std::array<std::byte, N>> buffer)
  {
    if (!buffer)
      throw std::invalid_argument("dsl::live: null fixed storage buffer");
    struct Arena
    {
      std::shared_ptr<std::array<std::byte, N>> buffer;
      std::pmr::monotonic_buffer_resource resource;

      explicit Arena(std::shared_ptr<std::array<std::byte, N>> bytes)
          : buffer(std::move(bytes)),
            resource(buffer->data(), N, std::pmr::null_memory_resource())
      {
      }
    };
    auto holder = std::make_shared<Arena>(std::move(buffer));
    Storage result;
    result.owner_ = std::shared_ptr<std::pmr::memory_resource>(holder, &holder->resource);
    result.resource_ = result.owner_.get();
    return result;
  }

  // Preserve backing storage in both objects even when one is an rvalue.
  Storage(const Storage&) noexcept = default;
  Storage& operator=(const Storage&) noexcept = default;

  std::pmr::memory_resource* resource() const noexcept { return resource_; }

  template <typename T>
  std::pmr::polymorphic_allocator<T> allocator() const noexcept
  {
    return std::pmr::polymorphic_allocator<T>{resource_};
  }
};

// Own immutable source snapshots. IDs are local to this manager; zero is
// reserved for an anonymous source. Registering a new revision gives a new ID.
// Text/name views survive subsequent insertions and are valid until destruction.
class SourceManager
{
  Storage storage_;
  using Buffer = support_detail::SourceBuffer;
  std::pmr::vector<std::shared_ptr<const Buffer>> sources_;

  const Buffer& get(SourceId id) const
  {
    if (!contains(id))
      throw std::out_of_range("dsl::live: unknown source ID");
    return *sources_[id - 1];
  }

public:
  explicit SourceManager(Storage storage = Storage{})
      : storage_(storage), sources_(storage_.resource())
  {
  }
  explicit SourceManager(std::pmr::memory_resource* resource)
      : SourceManager(Storage(resource))
  {
  }
  SourceManager(const SourceManager&) = delete;
  SourceManager& operator=(const SourceManager&) = delete;

  SourceId add(std::string_view name, std::string_view text)
  {
    sources_.push_back(std::allocate_shared<Buffer>(
        storage_.allocator<Buffer>(), name, text, storage_.resource()));
    return sources_.size();
  }

  std::size_t size() const noexcept { return sources_.size(); }
  bool contains(SourceId id) const noexcept { return id != 0 && id <= size(); }
  bool contains(SourceSpan span) const noexcept
  {
    return contains(span.source) && span.ordered()
           && span.end <= sources_[span.source - 1]->text.size();
  }
  std::string_view name(SourceId id) const { return get(id).name; }
  std::string_view text(SourceId id) const { return get(id).text; }

  std::string_view slice(SourceSpan span) const
  {
    if (!contains(span))
      throw std::out_of_range("dsl::live: invalid source span");
    return text(span.source).substr(span.begin, span.end - span.begin);
  }

  // Locations use one-based lines and byte columns. EOF is a valid location.
  SourceLocation locate(SourceId id, std::size_t offset) const
  {
    const auto& source = get(id);
    if (offset > source.text.size())
      throw std::out_of_range("dsl::live: source offset outside text");
    const auto next = std::upper_bound(source.lines.begin(), source.lines.end(), offset);
    return {id, offset, static_cast<std::size_t>(next - source.lines.begin()),
            offset - *(next - 1) + 1};
  }
};

enum class OriginKind { source, include, expansion, generated };

struct Origin
{
  OriginKind kind = OriginKind::source;
  SourceSpan span{};
  OriginId parent = 0;
  std::optional<SourceSpan> definition{};
  std::string_view label{};
};

// Origins form a forest of append-only chains. A definition span can point
// at a macro body while span is the occurrence that triggered its expansion.
// The copied labels in views returned by at() survive subsequent insertions.
class OriginTable
{
  struct Entry
  {
    std::pmr::string label;
    Origin origin;

    Entry(Origin value, std::pmr::memory_resource* resource)
        : label(value.label, resource), origin(value)
    {
      origin.label = label;
    }
  };
  Storage storage_;
  std::pmr::vector<std::shared_ptr<const Entry>> origins_;

public:
  explicit OriginTable(Storage storage = Storage{})
      : storage_(storage), origins_(storage_.resource())
  {
  }
  explicit OriginTable(std::pmr::memory_resource* resource)
      : OriginTable(Storage(resource))
  {
  }
  OriginTable(const OriginTable&) = delete;
  OriginTable& operator=(const OriginTable&) = delete;

  std::size_t size() const noexcept { return origins_.size(); }
  bool contains(OriginId id) const noexcept { return id != 0 && id <= size(); }

  OriginId add(Origin origin)
  {
    if ((origin.parent && !contains(origin.parent)) || !origin.span.ordered()
        || (origin.definition && !origin.definition->ordered()))
      throw std::invalid_argument("dsl::live: invalid origin parent or span");
    origins_.push_back(std::allocate_shared<Entry>(
        storage_.allocator<Entry>(), origin, storage_.resource()));
    return origins_.size();
  }

  Origin at(OriginId id) const
  {
    if (!contains(id))
      throw std::out_of_range("dsl::live: unknown origin ID");
    return origins_[id - 1]->origin;
  }

  // Order is newest to oldest. Zero represents an absent chain.
  // Fail explicitly rather than returning a silently truncated trace.
  std::vector<Origin> trace(OriginId id, std::size_t max_depth = 256) const
  {
    if (id && !contains(id))
      throw std::out_of_range("dsl::live: unknown origin ID");
    std::vector<Origin> result;
    while (id)
      {
        if (result.size() >= max_depth)
          throw std::length_error("dsl::live: origin trace depth exceeded");
        result.push_back(at(id));
        id = result.back().parent;
      }
    return result;
  }
};

struct NodeInfo
{
  SourceSpan span{};
  OriginId origin = 0;
};

template <typename T>
struct LocatedNode
{
  T value;
  const NodeId id;
  const SourceSpan span;
  const OriginId origin;
};

using LocatedSExpr = LocatedNode<SExpr>;
using LocatedAST = LocatedNode<ASTNode>;

// Stable, append-only logical node IDs, local to this table. Zero denotes
// no node. Attach metadata to a native value without altering its layout.
class NodeTable
{
  Storage storage_;
  std::pmr::vector<NodeInfo> entries_;

public:
  explicit NodeTable(Storage storage = Storage{})
      : storage_(storage), entries_(storage_.resource())
  {
  }
  explicit NodeTable(std::pmr::memory_resource* resource)
      : NodeTable(Storage(resource))
  {
  }
  NodeTable(const NodeTable&) = delete;
  NodeTable& operator=(const NodeTable&) = delete;

  std::size_t size() const noexcept { return entries_.size(); }
  bool contains(NodeId id) const noexcept { return id != 0 && id <= size(); }

  NodeId add(NodeInfo info)
  {
    if (!info.span.ordered())
      throw std::invalid_argument("dsl::live: invalid node span");
    entries_.push_back(info);
    return size();
  }

  NodeInfo at(NodeId id) const
  {
    if (!contains(id))
      throw std::out_of_range("dsl::live: unknown node ID");
    return entries_[id - 1];
  }

  template <typename T>
    requires std::is_nothrow_move_constructible_v<T>
  LocatedNode<T> attach(T value, SourceSpan span = {}, OriginId origin = 0)
  {
    const auto id = add({span, origin});
    return {std::move(value), id, span, origin};
  }
};

// Attach the supplied source envelope (including any trivia) to the root.
// This adapter uses the canonical bounded S-expression parser; it does not
// reconstruct child ranges or identify nodes across independent parses.
inline LocatedSExpr
parse_located_sexpr(const SourceManager& sources, NodeTable& nodes,
                    SourceSpan input, SExprLimits limits = {}, OriginId origin = 0)
{
  return nodes.attach(dsl::parse_bounded_sexpr(sources.slice(input), limits), input, origin);
}

// Converting a parsed root to the legacy AST keeps its logical identity.
inline LocatedAST
to_located_ast(const LocatedSExpr& expression, SExprLimits limits = {})
{
  if (!expression.span.ordered())
    throw std::invalid_argument("dsl::live: invalid node span");
  // Located values can also be constructed natively or edited. Bound
  // traversal independently of how the original value was created.
  const auto check = [&](const auto& self, const SExpr& expr, std::size_t depth) -> void
  {
    if (depth > std::min<std::size_t>(512, limits.max_depth) || limits.max_nodes == 0
        || expr.text.size() > limits.max_bytes)
      throw std::invalid_argument("dsl::live: AST conversion limit exceeded");
    --limits.max_nodes;
    limits.max_bytes -= expr.text.size();
    if (expr.is_atom && !expr.items.empty())
      throw std::invalid_argument("dsl::live: S-expression atom has items");
    for (const auto& child : expr.items)
      self(self, child, depth + 1);
  };
  check(check, expression.value, 0);
  return {dsl::sexpr_to_ast(expression.value), expression.id, expression.span,
          expression.origin};
}

// The bag allocates its entry buffer in Storage. Diagnostic values themselves
// retain ordinary owning strings/vectors so they can travel independently.
class DiagnosticBag
{
  Storage storage_;
  std::pmr::vector<Diagnostic> entries_;

public:
  explicit DiagnosticBag(Storage storage = Storage{})
      : storage_(storage), entries_(storage_.resource())
  {
  }
  explicit DiagnosticBag(std::pmr::memory_resource* resource)
      : DiagnosticBag(Storage(resource))
  {
  }
  DiagnosticBag(const DiagnosticBag&) = delete;
  DiagnosticBag& operator=(const DiagnosticBag&) = delete;

  void add(Diagnostic diagnostic) { entries_.push_back(std::move(diagnostic)); }
  void clear() noexcept { entries_.clear(); }
  std::span<const Diagnostic> entries() const noexcept { return entries_; }
  bool has_errors() const noexcept
  {
    return std::any_of(entries_.begin(), entries_.end(),
                       [](const Diagnostic& d) { return d.severity == Severity::error; });
  }
};

template <typename Signature> struct CallbackRef
{
  std::string name;  // The only part to store in a declarative specification.
  bool operator==(const CallbackRef&) const = default;
};

enum class CallbackError { missing, wrong_signature };

// Registrations are explicit and process-local. Each symbol has exactly one
// signature; bind() rejects an empty name, empty callable, or duplicate name.
// Resolved handles own their targets and survive destruction of the registry.
// There is no implicit lookup from file paths, native symbols, or scripts.
class CallbackRegistry
{
  Storage storage_;
  std::pmr::unordered_map<std::pmr::string, support_detail::CallbackEntry,
                          support_detail::TextHash, support_detail::TextEqual> callbacks_;

public:
  explicit CallbackRegistry(Storage storage = Storage{})
      : storage_(storage), callbacks_(storage_.resource())
  {
  }
  explicit CallbackRegistry(std::pmr::memory_resource* resource)
      : CallbackRegistry(Storage(resource))
  {
  }
  CallbackRegistry(const CallbackRegistry&) = delete;
  CallbackRegistry& operator=(const CallbackRegistry&) = delete;

  template <typename Signature>
  bool bind(std::string_view name, std::function<Signature> callable)
  {
    if (name.empty() || !callable || callbacks_.contains(name))
      return false;
    // This allocation intentionally does not use the registry's PMR:
    // a resolved callback can outlive that resource.
    auto target = std::make_shared<std::function<Signature>>(std::move(callable));
    return callbacks_.try_emplace(std::pmr::string(name, storage_.resource()),
                                  support_detail::CallbackEntry{
                                      &support_detail::callback_signature_tag<Signature>,
                                      std::move(target)}).second;
  }

  // Borrowed result: valid until the registry is destroyed.
  template <typename Signature>
  const std::function<Signature>* find(std::string_view name) const noexcept
  {
    const auto entry = callbacks_.find(name);
    return entry == callbacks_.end() ? nullptr : entry->second.template get<Signature>();
  }

  template <typename Signature>
  Result<std::shared_ptr<const std::function<Signature>>, CallbackError>
  resolve(const CallbackRef<Signature>& ref) const
  {
    using Return = Result<std::shared_ptr<const std::function<Signature>>, CallbackError>;
    const auto entry = callbacks_.find(ref.name);
    if (entry == callbacks_.end())
      return Return::from_err(CallbackError::missing);
    const auto* target = entry->second.template get<Signature>();
    if (!target)
      return Return::from_err(CallbackError::wrong_signature);
    return Return::from_ok({entry->second.callable, target});
  }
};
} // namespace live

// Non-effectful lexical and grammar inspection. All positions are bytes,
// except the explicitly named token indices in CursorAnalysis.
namespace live
{
enum class SyntaxStatus { complete, incomplete, invalid };

struct Token
{
  SourceSpan span{};
  std::string text, kind, channel;
  bool ignored = false, negated = false, overlay = false;
  bool error = false;
  std::vector<std::string> annotations{};
  bool partial = false;
  bool operator== (const Token &) const = default;
};

struct SyntaxLimits
{
  std::size_t bytes = 64 * 1024;
  std::size_t tokens = 4096;
  std::size_t rules = 1024;
  std::size_t states = 32768;
  std::size_t work = 1000000;
  std::size_t suggestions = 16;
  std::stop_token stop{};
};

struct SyntaxAnalysis
{
  SyntaxStatus status = SyntaxStatus::invalid;
  std::size_t position = 0;
  std::vector<std::string> expected;
  std::vector<Diagnostic> diagnostics;
  bool limited = false;
  bool cancelled = false;
};

struct LexicalAnalysis : SyntaxAnalysis
{
  std::vector<Token> tokens;
};

enum class SyntaxNodeKind { production, token, error };

struct SyntaxNode
{
  SyntaxNodeKind kind = SyntaxNodeKind::production;
  SourceSpan span{};
  std::string name, text, category, production_id, semantic_rule;
  std::vector<std::size_t> children;
  bool complete = true;
  std::string channel{};
  std::vector<std::string> annotations{};
};

struct CursorAnalysis : SyntaxAnalysis
{
  std::size_t token_index = 0;
  std::size_t cursor_token_index = 0;
  SourceSpan replacement{};
  std::vector<std::string> cursor_expected;
  bool cursor_complete = false;
  SyntaxStatus cursor_status = SyntaxStatus::invalid;
  // Node zero is the root when nonempty; child indices are into this vector.
  std::vector<SyntaxNode> tree;
};

namespace syntax_detail
{
struct Limit {};
struct Cancelled {};

struct Budget
{
  SyntaxLimits limits;
  std::size_t remaining;

  explicit Budget (SyntaxLimits supplied)
      : limits{ std::min<std::size_t> (supplied.bytes, 1024 * 1024),
                std::min<std::size_t> (supplied.tokens, 16384),
                std::min<std::size_t> (supplied.rules, 4096),
                std::min<std::size_t> (supplied.states, 131072),
                std::min<std::size_t> (supplied.work, 4000000),
                std::min<std::size_t> (supplied.suggestions, 64),
                supplied.stop },
        remaining (limits.work)
  {}

  void spend (std::size_t n = 1)
  {
    if (limits.stop.stop_requested ())
      throw Cancelled{};
    if (n > remaining)
      throw Limit{};
    remaining -= n;
  }
};

inline void
issue (SyntaxAnalysis &out, const char *code, std::string message,
       SourceSpan span)
{
  out.diagnostics.push_back ({ Severity::error, code, std::move (message), span });
}

inline void
cancelled (SyntaxAnalysis &out)
{
  out.status = SyntaxStatus::invalid;
  out.cancelled = true;
  issue (out, "syntax.cancelled", "syntax inspection cancelled",
         { out.position, out.position });
}

inline void
limited (SyntaxAnalysis &out)
{
  out.status = SyntaxStatus::invalid;
  out.limited = true;
  issue (out, "syntax.budget", "syntax inspection budget exhausted",
         { out.position, out.position });
}

inline void
canonicalize (std::vector<std::string> &names)
{
  std::sort (names.begin (), names.end ());
  names.erase (std::unique (names.begin (), names.end ()), names.end ());
}

struct PatternProbe
{
  std::optional<std::size_t> end;
  bool extendible = false;
};

inline bool
test_atom (const peg_detail::CompiledPattern::Atom &atom, char c, Budget &budget)
{
  budget.spend ();
  if (atom.is_class && !atom.any)
    budget.spend (atom.cls.size ());
  return peg_detail::CompiledPattern::atom_test (atom, c);
}

// For the deterministic greedy PEG pattern subset, compute which first
// bytes can begin an accepted suffix. A repetition can only hand off at
// EOF or at a byte its atom does not match. This avoids suggesting that
// e.g. "a*a" can be completed by appending another 'a'.
inline bool
has_suffix (const peg_detail::CompiledPattern &pattern, std::size_t first,
            bool continuing_plus, Budget &budget)
{
  std::array<bool, 256> leading{};
  // Once the pattern ends, a prefix match accepts arbitrary trailing bytes.
  leading.fill (true);
  bool nullable = true;
  for (auto i = pattern.atoms.size (); i > first;)
    {
      const auto &atom = pattern.atoms[--i];
      const char quant = i == first && continuing_plus ? '*' : atom.quant;
      std::array<bool, 256> mask{};
      bool any = false, outside = false;
      for (std::size_t b = 0; b < mask.size (); ++b)
        {
          // char is interpreted the same way as in the original matcher.
          mask[b] = test_atom (atom, static_cast<char> (b), budget);
          any |= leading[b];
          outside |= leading[b] && !mask[b];
        }
      const bool after_one = (quant == '*' || quant == '+')
                                 ? nullable || outside : nullable || any;
      for (std::size_t b = 0; b < mask.size (); ++b)
        {
          if (quant == '!')
            leading[b] = !mask[b] && leading[b];
          else if (quant == '*' || quant == '?')
            leading[b] = mask[b] ? after_one : leading[b];
          else
            leading[b] = mask[b] && after_one;
        }
      nullable = nullable && (quant == '!' || quant == '*' || quant == '?');
    }
  return std::any_of (leading.begin (), leading.end (), [] (bool x) { return x; });
}

inline PatternProbe
probe (const peg_detail::CompiledPattern &pattern, std::string_view text,
       std::size_t pos, Budget &budget, bool want_partial)
{
  auto pending = pattern.atoms.size ();
  bool continuing_plus = false;
  for (std::size_t i = 0; i < pattern.atoms.size (); ++i)
    {
      budget.spend ();
      const auto &atom = pattern.atoms[i];
      const auto begin = pos;
      if (pos == text.size () && pending == pattern.atoms.size ())
        pending = i;
      const auto matches = [&] {
        return pos < text.size () && test_atom (atom, text[pos], budget);
      };
      switch (atom.quant)
        {
        case '*': case '+':
          while (matches ())
            ++pos;
          if (pos == text.size () && pending == pattern.atoms.size ())
            {
              pending = i;
              continuing_plus = atom.quant == '+' && pos > begin;
            }
          if (atom.quant == '*' || pos > begin)
            continue;
          break;
        case '?':
          if (matches ())
            ++pos;
          continue;
        case '!':
          if (!matches ())
            continue;
          return {};
        default:
          if (matches ())
            {
              ++pos;
              continue;
            }
        }
      return { std::nullopt,
               want_partial && pending < pattern.atoms.size ()
                   && has_suffix (pattern, pending, continuing_plus, budget) };
    }
  return { pos, false };
}

inline std::vector<const PEGRule *>
lexical_rules (const PEGDefinition &definition, Budget &budget)
{
  std::vector<const PEGDefinition *> levels;
  std::unordered_set<const PEGDefinition *> visited;
  for (auto *parent = &definition; parent; parent = parent->parent)
    {
      budget.spend ();
      if (levels.size () >= budget.limits.rules)
        throw Limit{};
      if (!visited.insert (parent).second)
        throw std::invalid_argument ("cycle in PEG inheritance");
      levels.push_back (parent);
    }
  std::vector<const PEGRule *> rules;
  std::unordered_set<const PEGRule *> visible;
  for (auto i = levels.rbegin (); i != levels.rend (); ++i)
    for (const auto &rule : (*i)->local_rules ())
      {
        if (rules.size () >= budget.limits.rules)
          throw Limit{};
        budget.spend ();
        budget.spend (rule.pattern_text.size ());
        budget.spend (rule.channel.name.size ());
        budget.spend (rule.compiled.atoms.size ());
        if (!rule.compiled.valid)
          throw std::invalid_argument ("invalid compiled PEG rule");
        for (const auto &atom : rule.compiled.atoms)
          {
            budget.spend (atom.cls.size ());
            if (atom.quant != 0 && atom.quant != '*' && atom.quant != '+'
                && atom.quant != '?' && atom.quant != '!')
              throw std::invalid_argument ("invalid PEG quantifier");
          }
        rules.push_back (&rule);
        visible.insert (&rule);
      }
  for (const auto *rule : rules)
    if (rule->if_succeed && !visible.contains (rule->if_succeed))
      throw std::invalid_argument ("PEG guard must point to a visible rule");
  return rules;
}
} // namespace syntax_detail

// Inspect a prefix ending at cursor. Tokens own their text and metadata;
// a guard preview is an overlay and never advances the primary stream.
// No action is invoked or copied, including for ignored and negated rules.
inline LexicalAnalysis
analyze_peg (const PEGDefinition &definition, std::string_view source,
             std::size_t cursor = std::string_view::npos,
             SyntaxLimits limits = {})
{
  using namespace syntax_detail;
  LexicalAnalysis out;
  if (cursor == std::string_view::npos)
    cursor = source.size ();
  if (cursor > source.size ())
    {
      issue (out, "syntax.cursor", "lexical cursor lies outside the source",
             { source.size (), source.size () });
      return out;
    }
  Budget budget (limits);
  try
    {
      if (cursor > budget.limits.bytes)
        throw Limit{};
      const auto text = source.substr (0, cursor);
      const auto rules = lexical_rules (definition, budget);
      std::optional<std::size_t> first_error;
      const auto match = [&] (const PEGRule &rule, bool want_partial) {
        auto candidate = probe (rule.compiled, text, out.position, budget,
                                want_partial && !rule.must_fail);
        if (rule.must_fail)
          candidate.end = candidate.end ? std::nullopt
              : std::optional<std::size_t> (out.position + 1);
        return candidate;
      };
      const auto emit = [&] (const PEGRule &rule, std::size_t end, bool overlay) {
        if (out.tokens.size () == budget.limits.tokens)
          throw Limit{};
        budget.spend (end - out.position);
        budget.spend (rule.pattern_text.size ());
        budget.spend (rule.channel.name.size ());
        out.tokens.push_back ({ { out.position, end },
            std::string (text.substr (out.position, end - out.position)),
            rule.pattern_text, rule.channel.name, rule.channel.is_ignore (),
            rule.must_fail, overlay });
      };
      while (out.position < text.size ())
        {
          budget.spend ();
          const PEGRule *selected = nullptr;
          std::size_t end = 0;
          for (const auto *rule : rules)
            {
              budget.spend ();
              if (auto hit = match (*rule, false).end)
                {
                  selected = rule;
                  end = *hit;
                  break;
                }
            }
          if (!selected)
            {
              const PEGRule *partial = nullptr;
              for (const auto *rule : rules)
                {
                  budget.spend ();
                  if (match (*rule, true).extendible)
                    {
                      if (!partial)
                        partial = rule;
                      budget.spend (rule->pattern_text.size ());
                      out.expected.push_back (rule->pattern_text);
                    }
                }
              canonicalize (out.expected);
              if (!out.expected.empty ())
                {
                  emit (*partial, text.size (), false);
                  out.tokens.back ().annotations.push_back ("incomplete");
                  out.tokens.back ().partial = true;
                  out.status = first_error ? SyntaxStatus::invalid
                                           : SyntaxStatus::incomplete;
                  out.position = first_error.value_or (out.position);
                  return out;
                }
              if (!first_error)
                first_error = out.position;
              const auto after = out.position + 1;
              if (!out.tokens.empty () && out.tokens.back ().error
                  && out.tokens.back ().span.end == out.position)
                {
                  auto &bad = out.tokens.back ();
                  bad.span.end = after;
                  bad.text += text[out.position];
                  out.diagnostics.back ().span.end = after;
                }
              else
                {
                  if (out.tokens.size () == budget.limits.tokens)
                    throw Limit{};
                  issue (out, "peg.unmatched", "no PEG rule matches these bytes",
                         { out.position, after });
                  out.tokens.push_back ({ { out.position, after },
                      std::string (text.substr (out.position, 1)), "error", {},
                      false, false, false, true });
                }
              out.position = after;
              continue;
            }
          if (end == out.position)
            {
              issue (out, "peg.zero-width", "PEG rule matched without consuming input",
                     { end, end });
              return out;
            }
          emit (*selected, end, false);
          if (selected->if_succeed)
            if (auto hit = match (*selected->if_succeed, false).end)
              emit (*selected->if_succeed, *hit, true);
          out.position = end;
        }
      out.status = first_error ? SyntaxStatus::invalid : SyntaxStatus::complete;
      out.position = first_error.value_or (out.position);
    }
  catch (const Limit &)
    {
      limited (out);
    }
  catch (const Cancelled &)
    {
      cancelled (out);
    }
  catch (const std::invalid_argument &e)
    {
      issue (out, "peg.definition", e.what (), {});
    }
  return out;
}

enum class SyntaxSymbolKind { nonterminal, literal, category };

struct SyntaxSymbol
{
  SyntaxSymbolKind kind;
  std::string name;
};

struct SyntaxProduction
{
  std::string id, lhs;
  std::vector<SyntaxSymbol> rhs;
  std::string semantic_rule; // Metadata only. The inspector never dispatches.
};

struct SyntaxGrammar
{
  std::string start;
  std::vector<SyntaxProduction> productions;
};

namespace syntax_detail
{
inline constexpr auto none = std::numeric_limits<std::size_t>::max ();

struct GrammarIndex
{
  const SyntaxGrammar &grammar;
  std::vector<std::vector<std::size_t>> by_lhs;
  std::vector<std::size_t> lhs;
  std::vector<std::vector<std::size_t>> references;
  std::vector<bool> productive;
  std::size_t start{};

  GrammarIndex (const SyntaxGrammar &input, Budget &budget) : grammar (input)
  {
    if (input.productions.size () > budget.limits.rules)
      throw Limit{};
    std::unordered_map<std::string_view, std::size_t> symbols;
    std::unordered_set<std::string_view> ids;
    for (const auto &p : input.productions)
      {
        budget.spend ();
        budget.spend (p.id.size ());
        budget.spend (p.lhs.size ());
        budget.spend (p.semantic_rule.size ());
        budget.spend (p.rhs.size ());
        if (p.id.empty () || p.lhs.empty () || !ids.insert (p.id).second)
          throw std::invalid_argument ("empty or duplicate production identity");
        lhs.push_back (symbols.try_emplace (p.lhs, symbols.size ()).first->second);
      }
    budget.spend (input.start.size ());
    auto entry = symbols.find (input.start);
    if (entry == symbols.end ())
      throw std::invalid_argument ("undefined syntax grammar start symbol");
    start = entry->second;
    by_lhs.resize (symbols.size ());
    references.resize (lhs.size ());
    productive.resize (lhs.size ());
    std::vector<std::vector<std::size_t>> parents (symbols.size ());
    std::vector<std::size_t> missing (lhs.size ());
    std::vector<std::size_t> ready;
    std::vector<bool> reachable (symbols.size ());
    const auto mark = [&] (std::size_t p) {
      productive[p] = true;
      if (!reachable[lhs[p]])
        {
          reachable[lhs[p]] = true;
          ready.push_back (lhs[p]);
        }
    };
    for (std::size_t p = 0; p < lhs.size (); ++p)
      {
        by_lhs[lhs[p]].push_back (p);
        for (const auto &s : input.productions[p].rhs)
          {
            budget.spend (s.name.size ());
            switch (s.kind)
              {
              case SyntaxSymbolKind::nonterminal:
                {
                  auto child = symbols.find (s.name);
                  if (child == symbols.end ())
                    throw std::invalid_argument ("undefined syntax nonterminal");
                  references[p].push_back (child->second);
                  parents[child->second].push_back (p);
                  ++missing[p];
                  break;
                }
              case SyntaxSymbolKind::category:
                if (s.name.empty ())
                  throw std::invalid_argument ("empty syntax token category");
                [[fallthrough]];
              case SyntaxSymbolKind::literal:
                references[p].push_back (none);
                break;
              default:
                throw std::invalid_argument ("invalid syntax symbol kind");
              }
          }
        if (!missing[p])
          mark (p);
      }
    // Prune never-completable alternatives; a live chart then represents
    // viable prefixes, not merely prefixes of a dead recursive cycle.
    for (std::size_t i = 0; i < ready.size (); ++i)
      for (auto p : parents[ready[i]])
        {
          budget.spend ();
          if (!--missing[p])
            mark (p);
        }
  }
};

inline std::string
terminal_name (const SyntaxSymbol &symbol)
{
  return symbol.kind == SyntaxSymbolKind::category ? "<" + symbol.name + ">"
                                                   : symbol.name;
}

struct ItemKey
{
  std::size_t rule, dot, origin;
  bool operator== (const ItemKey &) const = default;
};

struct ItemHash
{
  std::size_t operator() (const ItemKey &item) const noexcept
  {
    return (item.rule * std::size_t{ 16777619 } ^ item.dot)
               * std::size_t{ 16777619 } ^ item.origin;
  }
};

struct Item
{
  ItemKey key;
  std::size_t end, previous = none, child = none, token = none;
};

struct Column
{
  std::vector<std::size_t> items;
  std::unordered_set<ItemKey, ItemHash> keys;
  std::unordered_map<std::size_t, std::vector<std::size_t>> waiting;
  std::unordered_map<std::size_t, std::size_t> nullable;
};

// The chart stores compact first-derivation backpointers, not recursive trees
// or copies of ambiguous subtrees. Predictions and completions are indexed.
class Earley
{
  const GrammarIndex &g;
  Budget &budget;
  std::vector<Column> chart;
  std::vector<Item> items;
  std::size_t placements = 0;

  std::size_t width (const ItemKey &key) const
  {
    return key.rule == none ? 1 : g.grammar.productions[key.rule].rhs.size ();
  }

  std::size_t next_nonterminal (const ItemKey &key) const
  {
    return key.rule == none ? (key.dot ? none : g.start)
        : key.dot == width (key) ? none : g.references[key.rule][key.dot];
  }

  void insert (std::size_t column, Item item, std::size_t reused = none)
  {
    budget.spend ();
    auto &dst = chart[column];
    if (dst.keys.contains (item.key))
      return;
    if (placements == budget.limits.states)
      throw Limit{};
    ++placements;
    dst.keys.insert (item.key);
    if (reused == none)
      {
        reused = items.size ();
        items.push_back (item);
      }
    dst.items.push_back (reused);
  }

  void advance (std::size_t id, std::size_t to, std::size_t child,
                std::size_t token = none)
  {
    auto key = items[id].key;
    ++key.dot;
    insert (to, { key, to, id, child, token });
  }

public:
  Earley (const GrammarIndex &index, Budget &b, std::size_t size)
    : g (index), budget (b), chart (size + 1)
  {
    insert (0, { { none, 0, 0 }, 0 });
  }

  void close (std::size_t pos)
  {
    auto &col = chart[pos];
    for (std::size_t next = 0; next < col.items.size (); ++next)
      {
        budget.spend ();
        const auto id = col.items[next];
        const auto key = items[id].key;
        if (auto nonterminal = next_nonterminal (key); nonterminal != none)
          {
            auto &waiting = col.waiting[nonterminal];
            const auto first = waiting.empty ();
            waiting.push_back (id);
            // Always handle a nullable child completed before this parent.
            if (auto done = col.nullable.find (nonterminal);
                done != col.nullable.end ())
              advance (id, pos, done->second);
            if (first)
              for (auto p : g.by_lhs[nonterminal])
                {
                  budget.spend ();
                  if (g.productive[p])
                    insert (pos, { { p, 0, pos }, pos });
                }
          }
        else if (key.rule != none && key.dot == width (key))
          {
            const auto nt = g.lhs[key.rule];
            if (key.origin == pos && !col.nullable.try_emplace (nt, id).second)
              continue;
            const auto &old = chart[key.origin].waiting;
            if (auto it = old.find (nt); it != old.end ())
              for (auto parent : it->second)
                advance (parent, pos, id);
          }
      }
  }

  bool scan (std::size_t pos, const Token &token)
  {
    if (token.error)
      return false;
    for (auto id : chart[pos].items)
      {
        budget.spend ();
        const auto key = items[id].key;
        if (key.rule == none || key.dot == width (key)
            || next_nonterminal (key) != none)
          continue;
        const auto &symbol = g.grammar.productions[key.rule].rhs[key.dot];
        auto &candidate = symbol.kind == SyntaxSymbolKind::category
                              ? token.kind : token.text;
        if (candidate.size () == symbol.name.size ())
          {
            budget.spend (candidate.size ());
            if (candidate == symbol.name)
              advance (id, pos + 1, none, pos);
          }
      }
    return !chart[pos + 1].items.empty ();
  }

  // Panic-mode deletion: keep the reachable frontier, never silently change
  // the classification. A skipped token is emitted as its own error node.
  void skip (std::size_t pos)
  {
    for (auto id : chart[pos].items)
      insert (pos + 1, items[id], id);
  }

  std::size_t accepted (std::size_t pos)
  {
    if (!chart[pos].keys.contains ({ none, 1, 0 }))
      return none;
    for (auto id : chart[pos].items)
      {
        budget.spend ();
        if (items[id].key.rule == none && items[id].key.dot == 1)
          return items[id].child;
      }
    return none;
  }

  std::vector<std::string> expected (std::size_t pos)
  {
    std::vector<std::string> result;
    std::unordered_set<std::string_view> literal, categories;
    for (auto id : chart[pos].items)
      {
        budget.spend ();
        const auto key = items[id].key;
        if (key.rule == none || key.dot == width (key)
            || next_nonterminal (key) != none)
          continue;
        const auto &symbol = g.grammar.productions[key.rule].rhs[key.dot];
        budget.spend (symbol.name.size ());
        auto &seen = symbol.kind == SyntaxSymbolKind::category ? categories : literal;
        if (seen.insert (symbol.name).second)
          result.push_back (terminal_name (symbol));
      }
    canonicalize (result);
    return result;
  }

  std::size_t partial (std::size_t pos)
  {
    auto best = none;
    for (auto id : chart[pos].items)
      {
        budget.spend ();
        const auto &candidate = items[id];
        if (candidate.key.rule == none || candidate.key.dot == 0)
          continue;
        if (best == none || candidate.end > items[best].end
            || (candidate.end == items[best].end
                && candidate.key.origin < items[best].key.origin))
          best = id;
      }
    return best;
  }

  void build (CursorAnalysis &out, const std::vector<const Token *> &tokens,
              const std::vector<bool> &skipped, std::size_t chosen,
              std::size_t eof, SourceId source, bool accepted)
  {
    auto &tree = out.tree;
    struct Job { std::size_t node, item; };
    std::vector<Job> jobs;
    std::vector<bool> covered (tokens.size ());
    const auto add = [&] (SyntaxNode node) {
      budget.spend ();
      if (tree.size () == budget.limits.states)
        throw Limit{};
      tree.push_back (std::move (node));
      return tree.size () - 1;
    };
    const auto leaf = [&] (std::size_t t) {
      const auto &token = *tokens[t];
      budget.spend (token.text.size ());
      budget.spend (token.kind.size ());
      budget.spend (token.channel.size ());
      budget.spend (token.annotations.size ());
      for (const auto &note : token.annotations)
        budget.spend (note.size ());
      covered[t] = true;
      return add ({ skipped[t] ? SyntaxNodeKind::error : SyntaxNodeKind::token,
                    token.span, "token", token.text, token.kind, {}, {}, {},
                    !skipped[t], token.channel, token.annotations });
    };
    const auto production = [&] (std::size_t id) {
      const auto &item = items[id];
      const auto &p = g.grammar.productions[item.key.rule];
      auto begin = item.key.origin < tokens.size ()
                     ? tokens[item.key.origin]->span.begin : eof;
      auto end = item.end > item.key.origin
                   ? tokens[item.end - 1]->span.end : begin;
      budget.spend (p.lhs.size ());
      budget.spend (p.id.size ());
      budget.spend (p.semantic_rule.size ());
      auto node = add ({ SyntaxNodeKind::production, { begin, end, source },
                         p.lhs, {}, {}, p.id, p.semantic_rule, {},
                         item.key.dot == width (item.key) });
      jobs.push_back ({ node, id });
      return node;
    };
    if (chosen != none && (accepted || (items[chosen].key.origin == 0
        && g.lhs[items[chosen].key.rule] == g.start)))
      production (chosen);
    else
      {
        budget.spend (g.grammar.start.size ());
        add ({ SyntaxNodeKind::production, { 0, eof, source },
               g.grammar.start, {}, {}, {}, {}, {}, false });
        if (chosen != none)
          {
            auto child = production (chosen);
            tree[0].children.push_back (child);
          }
      }
    while (!jobs.empty ())
      {
        auto job = jobs.back ();
        jobs.pop_back ();
        for (auto id = job.item; items[id].previous != none;
             id = items[id].previous)
          {
            budget.spend ();
            const auto &item = items[id];
            auto child = item.token != none ? leaf (item.token)
                                            : production (item.child);
            tree[job.node].children.push_back (child);
          }
        std::reverse (tree[job.node].children.begin (), tree[job.node].children.end ());
      }
    for (std::size_t t = 0; t < tokens.size (); ++t)
      if (!covered[t])
        {
          auto child = leaf (t);
          tree[0].children.push_back (child);
        }
    tree[0].span = { tokens.empty () ? eof : tokens.front ()->span.begin,
                     eof, source };
    tree[0].complete = out.status == SyntaxStatus::complete;
    // A nullable child can be anchored on the far side of trivia. Retain
    // a containing production span without moving any actual token range.
    for (auto i = tree.size (); i > 0;)
      {
        auto &node = tree[--i];
        for (auto child : node.children)
          {
            budget.spend ();
            node.span.begin = std::min (node.span.begin, tree[child].span.begin);
            node.span.end = std::max (node.span.end, tree[child].span.end);
          }
      }
    std::stable_sort (tree[0].children.begin (), tree[0].children.end (),
        [&] (auto a, auto b) { return tree[a].span.begin < tree[b].span.begin; });
  }
};

inline SyntaxGrammar
adapt_grammar (const CFG &cfg, Budget &budget)
{
  if (cfg.productions.size () > budget.limits.rules)
    throw Limit{};
  budget.spend (cfg.start.size ());
  SyntaxGrammar out{ cfg.start, {} };
  std::unordered_map<std::string, std::size_t> duplicates;
  for (const auto &rule : cfg.productions)
    {
      budget.spend ();
      budget.spend (rule.lhs.size ());
      budget.spend (rule.rhs.size ());
      std::string identity;
      const auto field = [&] (std::string_view text) {
        identity += std::to_string (text.size ());
        identity += ':';
        identity += text;
      };
      field (rule.lhs);
      std::vector<SyntaxSymbol> rhs;
      for (const auto &symbol : rule.rhs)
        {
          budget.spend (symbol.name.size ());
          identity += symbol.terminal ? 't' : 'n';
          field (symbol.name);
          rhs.push_back ({ symbol.terminal ? SyntaxSymbolKind::literal
                                            : SyntaxSymbolKind::nonterminal,
                           symbol.name });
        }
      budget.spend (identity.size ());
      const auto ordinal = duplicates[identity]++;
      identity += '#';
      identity += std::to_string (ordinal);
      out.productions.push_back ({ std::move (identity), rule.lhs,
                                  std::move (rhs), {} });
    }
  return out;
}

inline void
analyze (CursorAnalysis &out, const SyntaxGrammar &grammar,
         const std::vector<Token> &input, std::size_t cursor,
         std::size_t eof, Budget &budget,
         std::optional<std::size_t> token_cursor = {})
{
  budget.spend (0);
  if (input.size () > budget.limits.tokens || eof > budget.limits.bytes)
    throw Limit{};
  if (cursor > eof)
    throw std::invalid_argument ("cursor lies outside the syntax input");
  std::vector<const Token *> tokens;
  const Token *partial = nullptr;
  std::optional<SourceId> source;
  std::size_t previous = 0;
  for (const auto &token : input)
    {
      budget.spend ();
      if (!token.span.ordered () || token.span.end > eof
          || token.text.size () != token.span.end - token.span.begin)
        throw std::invalid_argument ("invalid token byte range");
      if (source && *source != token.span.source)
        throw std::invalid_argument ("mixed source IDs in syntax input");
      source = token.span.source;
      if (token.overlay)
        continue;
      if (token.span.begin < previous)
        throw std::invalid_argument ("overlapping primary tokens");
      previous = token.span.end;
      if (partial)
        throw std::invalid_argument ("unfinished token must be last");
      if (token.partial)
        {
          if (token.error || token.span.empty ())
            throw std::invalid_argument ("invalid unfinished token");
          partial = &token;
          continue;
        }
      if (!token.ignored || token.error)
        tokens.push_back (&token);
    }
  auto sid = source.value_or (SourceId{});
  out.replacement = { cursor, cursor, sid };
  if (token_cursor)
    out.cursor_token_index = *token_cursor;
  else
    while (out.cursor_token_index < tokens.size ())
      {
        const auto span = tokens[out.cursor_token_index]->span;
        if (cursor < span.end)
          {
            if (cursor >= span.begin)
              out.replacement = span;
            break;
          }
        ++out.cursor_token_index;
      }
  if (partial && !partial->ignored && cursor >= partial->span.begin
      && cursor <= partial->span.end)
    out.replacement = partial->span;

  GrammarIndex index (grammar, budget);
  Earley chart (index, budget, tokens.size ());
  std::vector<bool> skipped (tokens.size ());
  bool recovered = false;
  for (std::size_t pos = 0; pos <= tokens.size (); ++pos)
    {
      if (!recovered)
        {
          out.position = pos < tokens.size () ? tokens[pos]->span.begin : eof;
          out.token_index = pos;
        }
      chart.close (pos);
      if (pos == out.cursor_token_index)
        {
          out.cursor_expected = chart.expected (pos);
          if (!recovered)
            {
              out.cursor_complete = chart.accepted (pos) != none;
              out.cursor_status = out.cursor_complete ? SyntaxStatus::complete
                  : out.cursor_expected.empty () ? SyntaxStatus::invalid
                                                  : SyntaxStatus::incomplete;
              if (partial && cursor > partial->span.begin
                  && out.cursor_status == SyntaxStatus::complete)
                {
                  out.cursor_complete = false;
                  out.cursor_status = SyntaxStatus::incomplete;
                }
            }
        }
      if (pos == tokens.size () || chart.scan (pos, *tokens[pos]))
        continue;
      skipped[pos] = true;
      if (!recovered || out.diagnostics.size () < budget.limits.suggestions)
        {
          auto expected = chart.expected (pos);
          if (chart.accepted (pos) != none)
            expected.push_back ("<end-of-input>");
          if (!recovered)
            out.expected = expected;
          issue (out, tokens[pos]->error ? "cfg.lexical" : "cfg.unexpected",
                 tokens[pos]->error ? "malformed lexical token" : "unexpected token",
                 tokens[pos]->span);
          auto &notes = out.diagnostics.back ().notes;
          for (const auto &name : expected)
            {
              if (notes.size () == budget.limits.suggestions)
                break;
              budget.spend (name.size ());
              notes.push_back ({ "expected " + name, tokens[pos]->span });
            }
        }
      recovered = true;
      chart.skip (pos);
    }
  const auto accepted = chart.accepted (tokens.size ());
  if (!recovered)
    {
      if (accepted != none && !partial)
        out.status = SyntaxStatus::complete;
      else
        {
          out.expected = chart.expected (tokens.size ());
          if (!out.expected.empty () || accepted != none)
            {
              out.status = SyntaxStatus::incomplete;
              if (partial)
                out.position = partial->span.begin;
            }
          else
            issue (out, "cfg.unproductive", "grammar cannot derive a complete input",
                   { eof, eof, sid });
        }
    }
  chart.build (out, tokens, skipped,
               accepted != none ? accepted : chart.partial (tokens.size ()),
               eof, sid, accepted != none);
  if (partial && !partial->ignored)
    {
      if (out.tree.size () == budget.limits.states)
        throw Limit{};
      budget.spend (partial->text.size ());
      budget.spend (partial->kind.size ());
      budget.spend (partial->channel.size ());
      budget.spend (partial->annotations.size ());
      for (const auto &note : partial->annotations)
        budget.spend (note.size ());
      const auto node = out.tree.size ();
      out.tree.push_back ({ SyntaxNodeKind::token, partial->span, "token",
                            partial->text, partial->kind, {}, {}, {}, false,
                            partial->channel, partial->annotations });
      out.tree[0].children.push_back (node);
      if (tokens.empty ())
        out.tree[0].span.begin = partial->span.begin;
    }
}

template <class F>
inline CursorAnalysis
guard_analysis (SyntaxLimits limits, F &&run)
{
  CursorAnalysis out;
  Budget budget (limits);
  try
    {
      budget.spend (0);
      run (out, budget);
    }
  catch (const Limit &)
    {
      out.tree.clear ();
      limited (out);
    }
  catch (const Cancelled &)
    {
      out.tree.clear ();
      cancelled (out);
    }
  catch (const std::invalid_argument &e)
    {
      issue (out, "cfg.definition", e.what (), {});
    }
  return out;
}

inline std::size_t
extent (const std::vector<Token> &tokens, std::size_t cursor, Budget &budget)
{
  if (tokens.size () > budget.limits.tokens)
    throw Limit{};
  std::size_t end = 0;
  for (const auto &t : tokens)
    {
      budget.spend ();
      end = std::max (end, t.span.end);
    }
  return std::max (end, cursor);
}
} // namespace syntax_detail

// `source_size` (when supplied) makes the cursor and all source ranges
// checkable against the original source, including gaps between tokens.
template <class Grammar>
  requires (std::is_same_v<Grammar, SyntaxGrammar> || std::is_same_v<Grammar, CFG>)
inline CursorAnalysis
analyze_cfg (const Grammar &grammar, const std::vector<Token> &tokens,
             std::size_t byte_cursor, SyntaxLimits limits = {},
             std::optional<std::size_t> source_size = {})
{
  return syntax_detail::guard_analysis (limits, [&] (auto &out, auto &budget) {
    const auto eof = source_size ? *source_size
                      : syntax_detail::extent (tokens, byte_cursor, budget);
    if constexpr (std::is_same_v<Grammar, CFG>)
      syntax_detail::analyze (out, syntax_detail::adapt_grammar (grammar, budget),
                              tokens, byte_cursor, eof, budget);
    else
      syntax_detail::analyze (out, grammar, tokens, byte_cursor, eof, budget);
  });
}

// Legacy token strings get synthetic byte ranges separated by one byte.
// `token_cursor` is an index between tokens, not a source byte offset.
template <class Grammar>
  requires (std::is_same_v<Grammar, SyntaxGrammar> || std::is_same_v<Grammar, CFG>)
inline CursorAnalysis
analyze_cfg (const Grammar &grammar, const std::vector<std::string> &tokens,
             std::size_t token_cursor = std::string_view::npos,
             SyntaxLimits limits = {})
{
  return syntax_detail::guard_analysis (limits, [&] (auto &out, auto &budget) {
    if (token_cursor == std::string_view::npos)
      token_cursor = tokens.size ();
    if (token_cursor > tokens.size ())
      throw std::invalid_argument ("token cursor lies outside the syntax input");
    if (tokens.size () > budget.limits.tokens)
      throw syntax_detail::Limit{};
    std::vector<Token> rich;
    std::size_t end = 0, cursor = 0;
    for (std::size_t i = 0; i < tokens.size (); ++i)
      {
        const auto &token = tokens[i];
        budget.spend ();
        budget.spend (token.size ());
        if (i && end == budget.limits.bytes)
          throw syntax_detail::Limit{};
        auto begin = end + (i ? 1 : 0);
        if (token.size () > budget.limits.bytes - begin)
          throw syntax_detail::Limit{};
        end = begin + token.size ();
        if (i == token_cursor)
          cursor = begin;
        rich.push_back ({ { begin, end }, token, {}, {} });
      }
    if (token_cursor == tokens.size ())
      cursor = end;
    if constexpr (std::is_same_v<Grammar, CFG>)
      syntax_detail::analyze (out, syntax_detail::adapt_grammar (grammar, budget),
                              rich, cursor, end, budget, token_cursor);
    else
      syntax_detail::analyze (out, grammar, rich, cursor, end, budget, token_cursor);
  });
}

// (grammar (start Expr)
//   (production add Expr (Expr "+" Expr) (semantic add-expressions))
//   (production value Expr ((category Number))))
inline SyntaxGrammar
syntax_grammar (const SExpr &spec)
{
  syntax_detail::Budget budget ({});
  const auto atom = [] (const SExpr &s) { return s.is_atom && s.items.empty (); };
  const auto head = [&] (const SExpr &s, std::string_view name) {
    return !s.is_atom && s.text.empty () && !s.items.empty ()
        && atom (s.items[0]) && !s.items[0].quoted && s.items[0].text == name;
  };
  const auto copy = [&] (const SExpr &s) {
    if (!atom (s))
      throw std::invalid_argument ("expected syntax grammar atom");
    budget.spend (s.text.size ());
    return s.text;
  };
  SyntaxGrammar grammar;
  bool started = false;
  try
    {
      if (!head (spec, "grammar"))
        throw std::invalid_argument ("expected (grammar ...)");
      for (std::size_t i = 1; i < spec.items.size (); ++i)
        {
          budget.spend ();
          const auto &form = spec.items[i];
          const auto &fields = form.items;
          if (head (form, "start") && fields.size () == 2 && !started)
            {
              grammar.start = copy (fields[1]);
              started = true;
            }
          else if (head (form, "production") && fields.size () >= 4
                   && fields.size () <= 5 && !fields[3].is_atom)
            {
              if (grammar.productions.size () == budget.limits.rules)
                throw syntax_detail::Limit{};
              SyntaxProduction p{ copy (fields[1]), copy (fields[2]), {}, {} };
              if (fields.size () == 5)
                {
                  const auto &tag = fields[4];
                  if (!head (tag, "semantic") || tag.items.size () != 2)
                    throw std::invalid_argument ("invalid production semantic association");
                  p.semantic_rule = copy (tag.items[1]);
                  if (p.semantic_rule.empty ())
                    throw std::invalid_argument ("empty production semantic association");
                }
              for (const auto &symbol : fields[3].items)
                {
                  budget.spend ();
                  if (atom (symbol))
                    p.rhs.push_back ({ symbol.quoted ? SyntaxSymbolKind::literal
                                                      : SyntaxSymbolKind::nonterminal,
                                       copy (symbol) });
                  else if (head (symbol, "category") && symbol.items.size () == 2)
                    p.rhs.push_back ({ SyntaxSymbolKind::category, copy (symbol.items[1]) });
                  else
                    throw std::invalid_argument ("invalid syntax grammar symbol");
                }
              grammar.productions.push_back (std::move (p));
            }
          else
            throw std::invalid_argument ("invalid syntax grammar declaration");
        }
      if (!started)
        throw std::invalid_argument ("missing syntax grammar start symbol");
      (void) syntax_detail::GrammarIndex (grammar, budget);
      return grammar;
    }
  catch (const syntax_detail::Limit &)
    {
      throw std::length_error ("syntax grammar construction budget exhausted");
    }
}

inline SyntaxGrammar
syntax_grammar (std::string_view text)
{
  return syntax_grammar (parse_bounded_sexpr (text, { 64 * 1024, 16384, 8 }));
}

// Optional, single-prefix memo owned by a caller (not shared between
// threads). Edits strictly after the cursor preserve a cached result.
// Each miss uses a fresh bounded chart; no reuse is assumed after a
// change to the selected token prefix. There are no global cache entries.
class SyntaxPrefixCache
{
  SyntaxGrammar grammar_;
  struct Entry
  {
    std::size_t cursor;
    std::array<std::size_t, 6> limits;
    std::vector<Token> tokens;
    CursorAnalysis result;
  };
  std::optional<Entry> entry_;

public:
  explicit SyntaxPrefixCache (SyntaxGrammar grammar) : grammar_ (std::move (grammar))
  {}

  void clear () noexcept { entry_.reset (); }

  CursorAnalysis inspect (const std::vector<Token> &tokens, std::size_t cursor,
                          std::size_t source_size, SyntaxLimits limits = {})
  {
    return syntax_detail::guard_analysis (limits, [&] (auto &out, auto &budget) {
      if (cursor > source_size)
        throw std::invalid_argument ("cursor lies outside the syntax input");
      if (cursor > budget.limits.bytes)
        throw syntax_detail::Limit{};
      const auto &b = budget.limits;
      const std::array bound{ b.bytes, b.tokens, b.rules, b.states, b.work,
                              b.suggestions };
      std::vector<Token> prefix;
      for (const auto &token : tokens)
        {
          budget.spend ();
          if (!token.span.ordered () || token.span.end > source_size)
            throw std::invalid_argument ("invalid token byte range");
          if (token.span.end > cursor)
            {
              if (!token.overlay)
                break;
              continue;
            }
          if (prefix.size () == b.tokens)
            throw syntax_detail::Limit{};
          budget.spend (token.text.size ());
          budget.spend (token.kind.size ());
          budget.spend (token.channel.size ());
          budget.spend (token.annotations.size ());
          for (const auto &note : token.annotations)
            budget.spend (note.size ());
          prefix.push_back (token);
        }
      if (entry_ && entry_->cursor == cursor && entry_->limits == bound
          && entry_->tokens == prefix)
        {
          out = entry_->result;
          budget.spend (0);
          return;
        }
      syntax_detail::analyze (out, grammar_, prefix, cursor, cursor, budget);
      budget.spend (0);
      entry_ = Entry{ cursor, bound, std::move (prefix), out };
    });
  }
};

enum class PatternMatchStatus { matched, no_match, invalid, limited, cancelled };

namespace syntax_detail
{
inline void
validate_tree (const std::vector<SyntaxNode> &nodes, std::size_t root, Budget &budget)
{
  if (nodes.size () > budget.limits.states)
    throw Limit{};
  if (root >= nodes.size ())
    throw std::invalid_argument ("invalid recovered syntax root");
  std::vector<bool> visited (nodes.size ());
  for (std::size_t i = 0; i < nodes.size (); ++i)
    {
      budget.spend ();
      const auto &node = nodes[i];
      if (!node.span.ordered () || (node.kind != SyntaxNodeKind::production
          && node.kind != SyntaxNodeKind::token && node.kind != SyntaxNodeKind::error)
          || (node.kind != SyntaxNodeKind::production && !node.children.empty ()))
        throw std::invalid_argument ("invalid recovered syntax node");
      for (auto child : node.children)
        {
          budget.spend ();
          if (child <= i || child >= nodes.size () || visited[child])
            throw std::invalid_argument ("invalid recovered syntax child");
          const auto where = nodes[child].span;
          if (where.source != node.span.source || where.begin < node.span.begin
              || where.end > node.span.end)
            throw std::invalid_argument ("recovered syntax child lies outside its parent");
          visited[child] = true;
        }
    }
  for (std::size_t i = 1; i < visited.size (); ++i)
    if (!visited[i])
      throw std::invalid_argument ("unreachable recovered syntax node");
}
} // namespace syntax_detail

struct SyntaxTraversal
{
  // Empty on cancellation, budget exhaustion, or invalid nodes.
  std::vector<std::size_t> indices;
  std::vector<Diagnostic> diagnostics;
};

// A callback-free companion to ASTVisitor: return bounded, validated
// traversal indices so clients can choose what action, if any, to take.
inline SyntaxTraversal
syntax_visit_order (const CursorAnalysis &parsed, VisitOrder order = VisitOrder::Preorder,
                    std::size_t root = 0, SyntaxLimits limits = {})
{
  std::vector<std::size_t> traversal;
  auto outcome = syntax_detail::guard_analysis (limits, [&] (auto &, auto &budget) {
    syntax_detail::validate_tree (parsed.tree, root, budget);
    if (order != VisitOrder::Preorder && order != VisitOrder::Postorder)
      throw std::invalid_argument ("invalid syntax traversal order");
    struct Frame { std::size_t node, next; };
    std::vector<Frame> stack{ { root, 0 } };
    if (order == VisitOrder::Preorder)
      traversal.push_back (root);
    while (!stack.empty ())
      {
        budget.spend ();
        auto &frame = stack.back ();
        const auto &children = parsed.tree[frame.node].children;
        if (frame.next == children.size ())
          {
            if (order == VisitOrder::Postorder)
              traversal.push_back (frame.node);
            stack.pop_back ();
          }
        else
          {
            auto child = children[frame.next++];
            if (order == VisitOrder::Preorder)
              traversal.push_back (child);
            stack.push_back ({ child, 0 });
          }
      }
  });
  if (!outcome.diagnostics.empty ())
    traversal.clear ();
  return { std::move (traversal), std::move (outcome.diagnostics) };
}

// Immutable, bounded predicate over ordered child trees. Bare unquoted '_'
// is a wildcard; use (atom "_") to match that atom literally.
class StructuralPattern
{
  enum class Op
  {
    any, atom, list, node, text, category, channel, annotation, production,
    semantic, error, incomplete, all, either, invert
  };
  struct Clause
  {
    Op op;
    std::string value{};
    std::vector<std::size_t> children{};
  };
  std::vector<Clause> clauses_;

  std::size_t compile (const SExpr &form, syntax_detail::Budget &budget,
                       std::size_t depth)
  {
    budget.spend ();
    if (depth == 64 || clauses_.size () == budget.limits.states)
      throw syntax_detail::Limit{};
    const auto is_atom = [] (const SExpr &s) { return s.is_atom && s.items.empty (); };
    const auto &args = form.items;
    const auto id = clauses_.size ();
    Clause op{ Op::any };
    if (is_atom (form) && !form.quoted && form.text == "_")
      {
        clauses_.push_back (std::move (op));
        return id;
      }
    if (form.is_atom || !form.text.empty () || args.empty ()
        || !is_atom (args[0]) || args[0].quoted)
      throw std::invalid_argument ("expected structural pattern operator");
    const auto name = std::string_view (args[0].text);
    constexpr std::pair<std::string_view, Op> operations[]{
      { "any", Op::any }, { "atom", Op::atom }, { "list", Op::list },
      { "node", Op::node }, { "text", Op::text }, { "category", Op::category },
      { "channel", Op::channel }, { "annotation", Op::annotation },
      { "production", Op::production }, { "semantic", Op::semantic },
      { "error", Op::error }, { "incomplete", Op::incomplete },
      { "and", Op::all }, { "or", Op::either }, { "not", Op::invert }
    };
    const auto found = std::find_if (std::begin (operations), std::end (operations),
        [&] (const auto &candidate) { return candidate.first == name; });
    if (found == std::end (operations))
      throw std::invalid_argument ("unknown structural pattern operator");
    op.op = found->second;
    std::size_t first_child = args.size ();
    switch (op.op)
      {
      case Op::list: case Op::all: case Op::either:
        first_child = 1;
        break;
      case Op::invert:
        if (args.size () != 2)
          throw std::invalid_argument ("not pattern takes one subpattern");
        first_child = 1;
        break;
      case Op::any: case Op::error: case Op::incomplete:
        if (args.size () != 1)
          throw std::invalid_argument ("unexpected structural pattern argument");
        break;
      default:
        if (args.size () < 2 || !is_atom (args[1])
            || (op.op != Op::node && args.size () != 2))
          throw std::invalid_argument ("expected one structural pattern value");
        budget.spend (args[1].text.size ());
        op.value = args[1].text;
        if (op.op == Op::node)
          first_child = 2;
        break;
      }
    clauses_.push_back (std::move (op));
    for (std::size_t i = first_child; i < args.size (); ++i)
      {
        const auto child = compile (args[i], budget, depth + 1);
        clauses_[id].children.push_back (child);
      }
    return id;
  }

  struct View
  {
    std::variant<const ASTNode *, const SExpr *, const Token *,
                 const SyntaxNode *> node;
    const std::vector<SyntaxNode> *tree = nullptr;

    std::size_t size () const
    {
      if (auto p = std::get_if<const ASTNode *> (&node))
        return (*p)->children ().size ();
      if (auto p = std::get_if<const SExpr *> (&node))
        return (*p)->items.size ();
      if (auto p = std::get_if<const SyntaxNode *> (&node))
        return (*p)->children.size ();
      return 0;
    }

    View child (std::size_t i) const
    {
      if (auto p = std::get_if<const ASTNode *> (&node))
        return { &(*p)->children ()[i] };
      if (auto p = std::get_if<const SExpr *> (&node))
        return { &(*p)->items[i] };
      return { &(*tree)[std::get<const SyntaxNode *> (node)->children[i]], tree };
    }
  };

  bool test (std::size_t index, View view, syntax_detail::Budget &budget) const
  {
    budget.spend ();
    const auto &op = clauses_[index];
    const auto eq = [&] (std::string_view found) {
      if (found.size () != op.value.size ())
        return false;
      budget.spend (found.size ());
      return found == op.value;
    };
    const auto children = [&] {
      if (view.size () != op.children.size ())
        return false;
      for (std::size_t i = 0; i < op.children.size (); ++i)
        if (!test (op.children[i], view.child (i), budget))
          return false;
      return true;
    };
    const auto annotation = [&] (const auto &values) {
      for (const auto &value : values)
        {
          budget.spend ();
          if (eq (value))
            return true;
        }
      return false;
    };
    const auto ast = std::get_if<const ASTNode *> (&view.node);
    const auto sexpr = std::get_if<const SExpr *> (&view.node);
    const auto token = std::get_if<const Token *> (&view.node);
    const auto syntax = std::get_if<const SyntaxNode *> (&view.node);
    if ((sexpr && ((*sexpr)->is_atom ? !(*sexpr)->items.empty ()
                                    : !(*sexpr)->text.empty ()))
        || (ast && (*ast)->is_leaf () && (*ast)->size () != 0))
      throw std::invalid_argument ("invalid structural pattern input node");
    switch (op.op)
      {
      case Op::any: return true;
      case Op::atom: return sexpr && (*sexpr)->is_atom && eq ((*sexpr)->text);
      case Op::list: return sexpr && !(*sexpr)->is_atom && children ();
      case Op::node: return ((ast && eq ((*ast)->tag ()))
                           || (syntax && eq ((*syntax)->name))) && children ();
      case Op::text: return (ast && (*ast)->is_leaf () && eq ((*ast)->value ()))
                          || (sexpr && (*sexpr)->is_atom && eq ((*sexpr)->text))
                          || (token && eq ((*token)->text))
                          || (syntax && (*syntax)->kind != SyntaxNodeKind::production
                              && eq ((*syntax)->text));
      case Op::category: return (token && eq ((*token)->kind))
                             || (syntax && (*syntax)->kind != SyntaxNodeKind::production
                                 && eq ((*syntax)->category));
      case Op::channel: return (token && eq ((*token)->channel))
                            || (syntax && (*syntax)->kind != SyntaxNodeKind::production
                                && eq ((*syntax)->channel));
      case Op::annotation: return (token && annotation ((*token)->annotations))
                               || (syntax && annotation ((*syntax)->annotations));
      case Op::production: return syntax && !(*syntax)->production_id.empty ()
                                && eq ((*syntax)->production_id);
      case Op::semantic: return syntax && !(*syntax)->semantic_rule.empty ()
                              && eq ((*syntax)->semantic_rule);
      case Op::error: return (token && (*token)->error)
                          || (syntax && (*syntax)->kind == SyntaxNodeKind::error);
      case Op::incomplete: return (syntax && !(*syntax)->complete)
                               || (token && (*token)->partial);
      case Op::all:
        for (auto child : op.children)
          if (!test (child, view, budget))
            return false;
        return true;
      case Op::either:
        for (auto child : op.children)
          if (test (child, view, budget))
            return true;
        return false;
      case Op::invert: return !test (op.children[0], view, budget);
      }
    return false;
  }

  template <class F>
  PatternMatchStatus match_impl (SyntaxLimits limits, F &&view) const
  {
    syntax_detail::Budget budget (limits);
    try
      {
        budget.spend (0);
        if (clauses_.empty ())
          return PatternMatchStatus::invalid;
        return test (0, view (budget), budget)
                   ? PatternMatchStatus::matched : PatternMatchStatus::no_match;
      }
    catch (const syntax_detail::Limit &) { return PatternMatchStatus::limited; }
    catch (const syntax_detail::Cancelled &) { return PatternMatchStatus::cancelled; }
    catch (const std::invalid_argument &) { return PatternMatchStatus::invalid; }
  }

public:
  explicit StructuralPattern (const SExpr &form)
  {
    syntax_detail::Budget budget ({});
    try { compile (form, budget, 0); }
    catch (const syntax_detail::Limit &)
      { throw std::length_error ("structural pattern construction budget exhausted"); }
  }

  explicit StructuralPattern (std::string_view form)
      : StructuralPattern (parse_bounded_sexpr (form, { 64 * 1024, 16384, 64 }))
  {}

  template <class Node>
    requires (std::is_same_v<Node, Token> || std::is_same_v<Node, ASTNode>
              || std::is_same_v<Node, SExpr>)
  PatternMatchStatus match (const Node &node, SyntaxLimits limits = {}) const
  {
    return match_impl (limits, [&] (auto &) {
      if constexpr (std::is_same_v<Node, Token>)
        if (!node.span.ordered () || node.text.size () != node.span.end - node.span.begin)
          throw std::invalid_argument ("invalid token range");
      return View{ &node };
    });
  }

  PatternMatchStatus match (const CursorAnalysis &parsed, std::size_t root = 0,
                            SyntaxLimits limits = {}) const
  {
    return match_impl (limits, [&] (auto &budget) {
      const auto &nodes = parsed.tree;
      syntax_detail::validate_tree (nodes, root, budget);
      return View{ &nodes[root], &nodes };
    });
  }
};
} // namespace live

// Derived from TTyPack (MIT): TTyTk-TermInfo.hpp (slot tables, disk layout,
// parameter operations), TTyTk-InputProto.hpp (wire key forms),
// TTyTk-TermMode.hpp (DEC modes), and TTyTk-Winsize.hpp (window dimensions).
// See the embedded license notice.
namespace terminal
{
namespace terminfo
{
namespace detail
{
inline constexpr std::string_view flag_names =
    "bw am xsb xhp xenl eo gn hc km hs in da db mir msgr os eslok xt hz ul xon nxon mc5i chts nrrmc "
    "npc ndscr ccc bce hls xhpa crxm daisy xvpa sam cpix lpix OTbs OTns OTnc OTMT OTNL OTpt OTxr";
inline constexpr std::string_view number_names =
    "cols it lines lm xmc pb vt wsl nlab lh lw ma wnum colors pairs ncv bufsz spinv spinh maddr mjump "
    "mcs mls npins orc orl orhi orvi cps widcs btns bitwin bitype OTug OTdC OTdN OTdB OTdT OTkn";
inline constexpr std::string_view string_names =
    "cbt bel cr csr tbc clear el ed hpa cmdch cup cud1 home civis cub1 mrcup cnorm cuf1 ll cuu1 cvvis "
    "dch1 dl1 dsl hd smacs blink bold smcup smdc dim smir invis prot rev smso smul ech rmacs sgr0 "
    "rmcup rmdc rmir rmso rmul flash ff fsl is1 is2 is3 if ich1 il1 ip kbs ktbc kclr kctab kdch1 kdl1 "
    "kcud1 krmir kel ked kf0 kf1 kf10 kf2 kf3 kf4 kf5 kf6 kf7 kf8 kf9 khome kich1 kil1 kcub1 kll knp "
    "kpp kcuf1 kind kri khts kcuu1 rmkx smkx lf0 lf1 lf10 lf2 lf3 lf4 lf5 lf6 lf7 lf8 lf9 rmm smm nel "
    "pad dch dl cud ich indn il cub cuf rin cuu pfkey pfloc pfx mc0 mc4 mc5 rep rs1 rs2 rs3 rf rc vpa "
    "sc ind ri sgr hts wind ht tsl uc hu iprog ka1 ka3 kb2 kc1 kc3 mc5p rmp acsc pln kcbt smxon rmxon "
    "smam rmam xonc xoffc enacs smln rmln kbeg kcan kclo kcmd kcpy kcrt kend kent kext kfnd khlp kmrk "
    "kmsg kmov knxt kopn kopt kprv kprt krdo kref krfr krpl krst kres ksav kspd kund kBEG kCAN kCMD "
    "kCPY kCRT kDC kDL kslt kEND kEOL kEXT kFND kHLP kHOM kIC kLFT kMSG kMOV kNXT kOPT kPRV kPRT kRDO "
    "kRPL kRIT kRES kSAV kSPD kUND rfi kf11 kf12 kf13 kf14 kf15 kf16 kf17 kf18 kf19 kf20 kf21 kf22 "
    "kf23 kf24 kf25 kf26 kf27 kf28 kf29 kf30 kf31 kf32 kf33 kf34 kf35 kf36 kf37 kf38 kf39 kf40 kf41 "
    "kf42 kf43 kf44 kf45 kf46 kf47 kf48 kf49 kf50 kf51 kf52 kf53 kf54 kf55 kf56 kf57 kf58 kf59 kf60 "
    "kf61 kf62 kf63 el1 mgc smgl smgr fln sclk dclk rmclk cwin wingo hup dial qdial tone pulse hook "
    "pause wait u0 u1 u2 u3 u4 u5 u6 u7 u8 u9 op oc initc initp scp setf setb cpi lpi chr cvr defc "
    "swidm sdrfq sitm slm smicm snlq snrmq sshm ssubm ssupm sum rwidm ritm rlm rmicm rshm rsubm rsupm "
    "rum mhpa mcud1 mcub1 mcuf1 mvpa mcuu1 porder mcud mcub mcuf mcuu scs smgb smgbp smglp smgrp smgt "
    "smgtp sbim scsd rbim rcsd subcs supcs docr zerom csnm kmous minfo reqmp getm setaf setab pfxl "
    "devt csin s0ds s1ds s2ds s3ds smglr smgtb birep binel bicr colornm defbi endbi setcolor slines "
    "dispc smpch rmpch smsc rmsc pctrm scesc scesa ehhlm elhlm elohlm erhlm ethlm evhlm sgr1 slength "
    "OTi2 OTrs OTnl OTbc OTko OTma OTG2 OTG3 OTG1 OTG4 OTGR OTGL OTGU OTGD OTGH OTGV OTGC meml memu box1";

inline std::size_t index_of (std::string_view names, std::string_view name)
{
  for (std::size_t i = 0; !names.empty (); ++i)
    {
      auto end = names.find (' ');
      if (names.substr (0, end) == name) return i;
      if (end == names.npos) break;
      names.remove_prefix (end + 1);
    }
  return names.npos;
}

struct Reader
{
  std::string_view bytes;
  std::size_t pos = 0;
  bool valid = true;
  std::string_view take (std::size_t n)
  {
    if (!valid || n > bytes.size () - pos)
      { valid = false; return {}; }
    auto s = bytes.substr (pos, n);
    pos += n;
    return s;
  }
  void align ()
  {
    if (pos % 2 != 0 && take (1) != std::string_view ("\0", 1))
      valid = false;
  }
  std::int32_t word (unsigned width = 2)
  {
    auto s = take (width);
    if (!valid) return 0;
    std::uint32_t bits = 0;
    for (unsigned i = 0; i < width; ++i)
      bits |= std::uint32_t (static_cast<unsigned char> (s[i])) << (8 * i);
    if (width == 2 && (bits & 0x8000u)) bits |= 0xffff0000u;
    return std::bit_cast<std::int32_t> (bits);
  }
  std::size_t count ()
  {
    auto n = word ();
    if (n < 0) valid = false;
    return valid ? static_cast<std::size_t> (n) : 0;
  }
  std::vector<std::int32_t> words (std::size_t n, unsigned width = 2)
  {
    if (!valid || n > (bytes.size () - pos) / width)
      { valid = false; return {}; }
    std::vector<std::int32_t> result;
    result.reserve (n);
    while (n-- > 0) result.push_back (word (width));
    return result;
  }
};

struct StringTable
{
  std::string_view bytes;
  std::vector<std::size_t> ends;
  explicit StringTable (std::string_view table) : bytes (table), ends (table.size ())
  {
    auto end = bytes.npos;
    for (std::size_t i = bytes.size (); i > 0;)
      {
        --i;
        if (bytes[i] == '\0') end = i;
        ends[i] = end;
      }
  }
  std::optional<std::string_view> get (std::int32_t offset, std::size_t base = 0) const
  {
    if (base > bytes.size () || offset < 0 || static_cast<std::size_t> (offset) >= bytes.size () - base)
      return std::nullopt;
    auto start = base + static_cast<std::size_t> (offset);
    if (ends[start] == bytes.npos) return std::nullopt;
    return bytes.substr (start, ends[start] - start);
  }
};
} // namespace detail

enum class SlotState { absent, cancelled, present };

// Negative numeric/string disk slots denote missing or cancelled capabilities.
struct Entry
{
  std::string names;
  std::vector<bool> flags;
  std::vector<std::int32_t> numbers;
  std::vector<std::optional<std::string>> strings;
  std::unordered_map<std::string, bool> extended_flags;
  std::unordered_map<std::string, std::int32_t> extended_numbers;
  std::unordered_map<std::string, std::optional<std::string>> extended_strings;
  std::vector<bool> cancelled_flags, cancelled_strings;
  std::unordered_set<std::string> cancelled_extended_flags, cancelled_extended_strings;

  bool flag (std::size_t i) const { return i < flags.size () && flags[i]; }
  std::optional<std::int32_t> num (std::size_t i) const
  {
    if (i < numbers.size () && numbers[i] >= 0) return numbers[i];
    return std::nullopt;
  }
  std::optional<std::string_view> str (std::size_t i) const
  {
    if (i < strings.size () && strings[i]) return *strings[i];
    return std::nullopt;
  }
  bool flag (std::string_view name) const
  {
    auto it = extended_flags.find (std::string (name));
    return it == extended_flags.end () ? flag (detail::index_of (detail::flag_names, name)) : it->second;
  }
  std::optional<std::int32_t> num (std::string_view name) const
  {
    auto it = extended_numbers.find (std::string (name));
    if (it == extended_numbers.end ()) return num (detail::index_of (detail::number_names, name));
    if (it->second >= 0) return it->second;
    return std::nullopt;
  }
  std::optional<std::string_view> str (std::string_view name) const
  {
    auto it = extended_strings.find (std::string (name));
    if (it == extended_strings.end ()) return str (detail::index_of (detail::string_names, name));
    if (it->second) return *it->second;
    return std::nullopt;
  }
  SlotState flag_state (std::size_t i) const
  {
    if (i < cancelled_flags.size () && cancelled_flags[i]) return SlotState::cancelled;
    return flag (i) ? SlotState::present : SlotState::absent;
  }
  SlotState num_state (std::size_t i) const
  {
    return i < numbers.size () ? numeric_state (numbers[i]) : SlotState::absent;
  }
  SlotState str_state (std::size_t i) const
  {
    if (i < cancelled_strings.size () && cancelled_strings[i]) return SlotState::cancelled;
    return str (i) ? SlotState::present : SlotState::absent;
  }
  SlotState flag_state (std::string_view name) const
  {
    std::string key (name);
    if (cancelled_extended_flags.contains (key)) return SlotState::cancelled;
    if (auto it = extended_flags.find (key); it != extended_flags.end ())
      return it->second ? SlotState::present : SlotState::absent;
    return flag_state (detail::index_of (detail::flag_names, name));
  }
  SlotState num_state (std::string_view name) const
  {
    if (auto it = extended_numbers.find (std::string (name)); it != extended_numbers.end ())
      return numeric_state (it->second);
    return num_state (detail::index_of (detail::number_names, name));
  }
  SlotState str_state (std::string_view name) const
  {
    std::string key (name);
    if (cancelled_extended_strings.contains (key)) return SlotState::cancelled;
    if (auto it = extended_strings.find (key); it != extended_strings.end ())
      return it->second ? SlotState::present : SlotState::absent;
    return str_state (detail::index_of (detail::string_names, name));
  }
private:
  static SlotState numeric_state (std::int32_t n)
  {
    return n >= 0 ? SlotState::present : n == -2 ? SlotState::cancelled : SlotState::absent;
  }
};

inline constexpr std::size_t max_entry_bytes = 128 * 1024;

[[nodiscard]] inline Result<Entry, std::string> load_from (std::string_view bytes)
{
  if (bytes.size () > max_entry_bytes) return std::string ("terminfo entry exceeds size limit");
  detail::Reader r{ bytes };
  auto magic = r.word ();
  auto name_count = r.count (), bool_count = r.count (), num_count = r.count ();
  auto str_count = r.count (), table_size = r.count ();
  if (!r.valid || (magic != 0432 && magic != 01036))
    return std::string ("invalid terminfo header");
  const unsigned width = magic == 01036 ? 4 : 2;
  auto names = r.take (name_count);
  if (names.size () < 2 || names.find ('\0') != names.size () - 1)
    return std::string ("invalid terminfo names");
  auto flags = r.take (bool_count);
  if (r.pos != bytes.size () || num_count || str_count || table_size) r.align ();
  auto numbers = r.words (num_count, width);
  auto offsets = r.words (str_count);
  auto table = r.take (table_size);
  if (!r.valid) return std::string ("truncated or misaligned terminfo entry");
  Entry result;
  result.names = names.substr (0, names.size () - 1);
  const auto valid_flag = [] (unsigned char c) { return c <= 1 || c >= 254; };
  for (const unsigned char c : flags)
    {
      if (!valid_flag (c)) return std::string ("invalid terminfo boolean");
      result.flags.push_back (c == 1);
      result.cancelled_flags.push_back (c == 254);
    }
  for (auto n : numbers)
    if (n < -2) return std::string ("invalid terminfo numeric slot");
  result.numbers = std::move (numbers);
  std::size_t capacity = 2 * 1024 * 1024;
  const auto charge = [&] (std::string_view s) {
    if (s.size () > capacity) return false;
    capacity -= s.size ();
    return true;
  };
  detail::StringTable legacy (table);
  for (auto offset : offsets)
    {
      auto s = legacy.get (offset);
      if (s && charge (*s)) result.strings.emplace_back (std::string (*s));
      else if (offset == -1 || offset == -2) result.strings.emplace_back ();
      else return std::string ("invalid or oversized terminfo string");
      result.cancelled_strings.push_back (offset == -2);
    }
  if (r.pos == bytes.size ()) return result;
  r.align ();
  if (r.valid && r.pos == bytes.size ()) return result;
  auto eb = r.count (), en = r.count (), es = r.count ();
  auto items = r.count (), size = r.count ();
  auto flags_ext = r.take (eb);
  r.align ();
  auto numbers_ext = r.words (en, width);
  const auto name_slots = eb + en + es;
  auto offsets_ext = r.words (es + name_slots);
  auto table_ext = r.take (size);
  if (!r.valid || r.pos != bytes.size ())
    return std::string ("invalid extended terminfo section");
  detail::StringTable extended (table_ext);
  std::vector<std::optional<std::string_view>> strings_ext;
  std::size_t base = 0, present = 0;
  for (std::size_t i = 0; i < es; ++i)
    {
      const auto offset = offsets_ext[i];
      auto s = extended.get (offset);
      if (s)
        {
          ++present;
          const auto end = static_cast<std::size_t> (offset) + s->size () + 1;
          if (end > base) base = end;
        }
      else if (offset != -1 && offset != -2)
        return std::string ("invalid extended terminfo string");
      strings_ext.push_back (s);
    }
  if (items != name_slots + present)
    return std::string ("invalid extended terminfo item count");
  for (std::size_t i = 0; i < name_slots; ++i)
    {
      auto name = extended.get (offsets_ext[es + i], base);
      if (!name || name->empty () || !charge (*name))
        return std::string ("invalid extended terminfo capability name");
      auto key = std::string (*name);
      bool inserted;
      if (i < eb)
        {
          if (!valid_flag (static_cast<unsigned char> (flags_ext[i])))
            return std::string ("invalid extended terminfo boolean");
          if (static_cast<unsigned char> (flags_ext[i]) == 254)
            result.cancelled_extended_flags.insert (key);
          inserted = result.extended_flags.emplace (std::move (key), flags_ext[i] == '\1').second;
        }
      else if (i < eb + en)
        {
          if (numbers_ext[i - eb] < -2) return std::string ("invalid extended terminfo numeric slot");
          inserted = result.extended_numbers.emplace (std::move (key), numbers_ext[i - eb]).second;
        }
      else
        {
          auto s = strings_ext[i - eb - en];
          if (s && !charge (*s)) return std::string ("oversized extended terminfo string");
          if (offsets_ext[i - eb - en] == -2) result.cancelled_extended_strings.insert (key);
          inserted = result.extended_strings.emplace (std::move (key),
              s ? std::optional<std::string> (std::string (*s)) : std::nullopt).second;
        }
      if (!inserted) return std::string ("duplicate extended terminfo capability");
    }
  return result;
}

[[nodiscard]] inline std::vector<std::string> search_paths ()
{
  constexpr std::string_view system = "/usr/share/terminfo";
  std::vector<std::string> paths;
  if (const auto* p = std::getenv ("TERMINFO"); p && *p) paths.emplace_back (p);
  if (const auto* p = std::getenv ("HOME"); p && *p) paths.push_back (std::string (p) + "/.terminfo");
  if (const auto* p = std::getenv ("TERMINFO_DIRS"))
    {
      std::string_view rest (p);
      for (;;)
        {
          auto end = rest.find (':');
          auto path = rest.substr (0, end);
          paths.emplace_back (path.empty () ? system : path);
          if (end == rest.npos) break;
          rest.remove_prefix (end + 1);
        }
    }
  paths.emplace_back ("/etc/terminfo");
  paths.emplace_back ("/lib/terminfo");
  paths.emplace_back (system);
  return paths;
}

// Directory trees, not ncurses hashed databases or inline environment blobs.
[[nodiscard]] inline Result<Entry, std::string>
load (std::string_view name, const std::vector<std::string>& paths)
{
  if (name.empty () || name.size () > 255 || name == "." || name == "..")
    return std::string ("invalid terminal name");
  for (unsigned char c : name)
    if (c < 33 || c >= 127 || c == '/' || c == '\\' || c == ':')
      return std::string ("invalid terminal name");
  const unsigned char first = static_cast<unsigned char> (name.front ());
  constexpr std::string_view hex = "0123456789abcdef";
  const std::string dirs[] = { std::string (1, name.front ()), { hex[first >> 4], hex[first & 15] } };
  std::string error = "terminfo entry not found";
  for (const auto& path : paths)
    for (const auto& dir : dirs)
      {
        // Do not interpret a first-character directory of "." as the search root itself.
        if (dir == ".") continue;
        const auto file = std::filesystem::path (path) / dir / std::string (name);
        std::error_code ec;
        if (!std::filesystem::is_regular_file (file, ec)) continue;
        std::ifstream input (file, std::ios::binary);
        if (!input) { error = "cannot open terminfo entry"; continue; }
        std::string bytes (max_entry_bytes + 1, '\0');
        input.read (bytes.data (), static_cast<std::streamsize> (bytes.size ()));
        if (input.bad () || (input.fail () && !input.eof ()))
          { error = "cannot read terminfo entry"; continue; }
        bytes.resize (static_cast<std::size_t> (input.gcount ()));
        auto parsed = load_from (bytes);
        if (parsed.is_ok ()) return parsed;
        error = parsed.unwrap_err ();
      }
  return error;
}

[[nodiscard]] inline Result<Entry, std::string> load (std::string_view name)
{ return load (name, search_paths ()); }

[[nodiscard]] inline Result<Entry, std::string> load ()
{
  if (const auto* name = std::getenv ("TERM")) return load (name);
  return std::string ("TERM is not set");
}

using Parameter = std::variant<std::int64_t, std::string_view>;
// Uppercase variables persist only when an explicit state is supplied. Lowercase
// variables and the operand stack start at zero/empty on every call.
struct ParameterState
{
  std::array<std::variant<std::int64_t, std::string>, 26> variables{};
};

namespace detail
{
inline std::int64_t arithmetic (char op, std::int64_t a, std::int64_t b)
{
  const auto x = static_cast<std::uint64_t> (a), y = static_cast<std::uint64_t> (b);
  const bool overflow_div = a == std::numeric_limits<std::int64_t>::min () && b == -1;
  switch (op)
    {
    case '+': return std::bit_cast<std::int64_t> (x + y);
    case '-': return std::bit_cast<std::int64_t> (x - y);
    case '*': return std::bit_cast<std::int64_t> (x * y);
    case '/': return !b ? 0 : overflow_div ? a : a / b;
    case 'm': return !b || overflow_div ? 0 : a % b;
    case '&': return a & b;
    case '|': return a | b;
    case '^': return a ^ b;
    case '=': return a == b;
    case '>': return a > b;
    case '<': return a < b;
    case 'A': return a != 0 && b != 0;
    case 'O': return a != 0 || b != 0;
    default: return 0;
    }
}
} // namespace detail

// Capability programs and text parameters produce control bytes as well as text;
// use only a trusted database/program with appropriately validated arguments.
[[nodiscard]] inline Result<std::string, std::string_view>
expand (std::string_view pattern, std::array<Parameter, 9> parameters = {},
        ParameterState* state = nullptr)
{
  constexpr std::size_t limit = 65536;
  if (pattern.size () > limit) return std::string_view ("terminfo parameter program exceeds size limit");
  for (const auto& p : parameters)
    if (auto* s = std::get_if<std::string_view> (&p); s && s->size () > limit)
      return std::string_view ("terminfo parameter exceeds size limit");
  std::string out;
  std::vector<Parameter> stack;
  std::array<Parameter, 26> locals{}, globals{};
  if (state)
    for (std::size_t i = 0; i < globals.size (); ++i)
      {
        if (const auto* n = std::get_if<std::int64_t> (&state->variables[i])) globals[i] = *n;
        else if (const auto* s = std::get_if<std::string> (&state->variables[i]))
          {
            if (s->size () > limit) return std::string_view ("terminfo variable exceeds size limit");
            globals[i] = std::string_view (*s);
          }
        else return std::string_view ("invalid terminfo variable");
      }
  struct Condition { bool enclosing, selected, tested, in_else; };
  std::vector<Condition> cond;
  bool active = true;
  constexpr std::string_view stack_error = "terminfo parameter stack underflow or wrong operand type";
  constexpr std::string_view syntax_error = "invalid terminfo parameter program";
  constexpr std::string_view output_error = "terminfo output exceeds size limit";
  const auto append = [&] (std::string_view text) {
    if (text.size () > limit - out.size ()) return false;
    out.append (text);
    return true;
  };
  const auto pop = [&] {
    auto v = stack.back ();
    stack.pop_back ();
    return v;
  };
  const auto number = [&] () -> std::optional<std::int64_t> {
    if (stack.empty ()) return std::nullopt;
    auto v = pop ();
    if (auto* n = std::get_if<std::int64_t> (&v)) return *n;
    return std::nullopt;
  };
  const auto digit = [] (char c) { return c >= '0' && c <= '9'; };
  for (std::size_t i = 0; i < pattern.size ();)
    {
      auto op = pattern[i++];
      if (op != '%')
        {
          if (active && !append (std::string_view (&op, 1))) return output_error;
          continue;
        }
      if (i == pattern.size ()) return syntax_error;
      op = pattern[i++];
      if (op == '?')
        {
          if (cond.size () >= 128) return std::string_view ("terminfo conditional nesting exceeds limit");
          cond.push_back ({ active, false, false, false });
        }
      else if (op == 't')
        {
          if (cond.empty () || (cond.back ().tested && !cond.back ().in_else)) return syntax_error;
          auto& c = cond.back ();
          if (active)
            {
              auto n = number ();
              if (!n) return stack_error;
              active = *n != 0;
            }
          c.selected = c.selected || active;
          c.tested = true;
          c.in_else = false;
        }
      else if (op == 'e')
        {
          if (cond.empty () || !cond.back ().tested || cond.back ().in_else) return syntax_error;
          auto& c = cond.back ();
          c.in_else = true;
          active = c.enclosing && !c.selected;
        }
      else if (op == ';')
        {
          if (cond.empty () || !cond.back ().tested) return syntax_error;
          active = cond.back ().enclosing;
          cond.pop_back ();
        }
      else if (op == 'p' || op == 'g' || op == 'P')
        {
          if (i == pattern.size ()) return syntax_error;
          auto ch = pattern[i++];
          if (op == 'p')
            {
              if (ch < '1' || ch > '9') return syntax_error;
              if (active) stack.push_back (parameters[ch - '1']);
            }
          else
            {
              Parameter* slot = ch >= 'a' && ch <= 'z' ? &locals[ch - 'a']
                                : ch >= 'A' && ch <= 'Z' ? &globals[ch - 'A'] : nullptr;
              if (!slot) return syntax_error;
              if (active)
                {
                  if (op == 'g') stack.push_back (*slot);
                  else
                    {
                      if (stack.empty ()) return stack_error;
                      *slot = pop ();
                    }
                }
            }
        }
      else if (op == '\'')
        {
          if (pattern.size () - i < 2 || pattern[i + 1] != '\'') return syntax_error;
          if (active) stack.emplace_back (static_cast<std::int64_t> (static_cast<unsigned char> (pattern[i])));
          i += 2;
        }
      else if (op == '{')
        {
          const auto end = pattern.find ('}', i);
          if (end == pattern.npos) return syntax_error;
          std::int64_t n = 0;
          auto parsed = std::from_chars (pattern.data () + i, pattern.data () + end, n);
          if (parsed.ec != std::errc{} || parsed.ptr != pattern.data () + end) return syntax_error;
          if (active) stack.emplace_back (n);
          i = end + 1;
        }
      else if (op == '%' || op == 'i' || op == 'l' || op == 'c' || op == '!' || op == '~' ||
               std::string_view ("+-*/m&|^=<>AO").find (op) != pattern.npos)
        {
          if (!active) continue;
          if (op == '%')
            { if (!append ("%")) return output_error; }
          else if (op == 'i')
            {
              for (unsigned k = 0; k < 2; ++k)
                {
                  auto* n = std::get_if<std::int64_t> (&parameters[k]);
                  if (!n) return stack_error;
                  *n = detail::arithmetic ('+', *n, 1);
                }
            }
          else if (op == 'l')
            {
              if (stack.empty ()) return stack_error;
              auto v = pop ();
              auto* s = std::get_if<std::string_view> (&v);
              if (!s) return stack_error;
              stack.emplace_back (static_cast<std::int64_t> (s->size ()));
            }
          else
            {
              auto b = number ();
              if (!b) return stack_error;
              if (op == 'c')
                {
                  unsigned char byte = static_cast<unsigned char> (*b);
                  const char ch = static_cast<char> (byte != 0 ? byte : 128);
                  if (!append (std::string_view (&ch, 1))) return output_error;
                }
              else if (op == '!' || op == '~') stack.emplace_back (op == '!' ? !*b : ~*b);
              else
                {
                  auto a = number ();
                  if (!a) return stack_error;
                  stack.emplace_back (detail::arithmetic (op, *a, *b));
                }
            }
        }
      else
        {
          // Parse a bounded printf subset; the optional colon disambiguates '-' and '+'.
          const auto start = op == ':' ? i : i - 1;
          i = start;
          while (i < pattern.size () && std::string_view ("-+# 0").find (pattern[i]) != pattern.npos) ++i;
          auto flags = pattern.substr (start, i - start);
          const auto get_size = [&] (std::size_t& size) {
            while (i < pattern.size () && digit (pattern[i]))
              {
                size = size * 10 + static_cast<unsigned> (pattern[i++] - '0');
                if (size > 4096) return false;
              }
            return true;
          };
          std::size_t width = 0, precision = 0;
          if (flags.size () > 5 || !get_size (width)) return syntax_error;
          const bool has_precision = i < pattern.size () && pattern[i] == '.';
          if (has_precision)
            {
              ++i;
              if (!get_size (precision)) return syntax_error;
            }
          if (i == pattern.size ()) return syntax_error;
          const auto conversion = pattern[i++];
          if (std::string_view ("doxXs").find (conversion) == pattern.npos ||
              (conversion == 's' && flags.find_first_of ("+# 0") != pattern.npos) ||
              (conversion == 'd' && flags.find ('#') != pattern.npos) ||
              (conversion != 'd' && flags.find_first_of ("+ ") != pattern.npos)) return syntax_error;
          if (!active) continue;
          if (conversion == 's')
            {
              if (stack.empty ()) return stack_error;
              auto v = pop ();
              auto* text = std::get_if<std::string_view> (&v);
              if (!text) return stack_error;
              auto view = has_precision ? text->substr (0, precision) : *text;
              const std::string padding (width > view.size () ? width - view.size () : 0, ' ');
              if (flags.find ('-') == pattern.npos && !append (padding)) return output_error;
              if (!append (view)) return output_error;
              if (flags.find ('-') != pattern.npos && !append (padding)) return output_error;
            }
          else
            {
              auto n = number ();
              if (!n) return stack_error;
              std::string format ("%");
              format += pattern.substr (start, i - 1 - start);
              format += "ll";
              format += conversion;
              std::array<char, 4128> buf;
              auto count = conversion == 'd'
                  ? std::snprintf (buf.data (), buf.size (), format.c_str (), static_cast<long long> (*n))
                  : std::snprintf (buf.data (), buf.size (), format.c_str (), static_cast<unsigned long long> (*n));
              if (count < 0 || static_cast<std::size_t> (count) >= buf.size ()) return syntax_error;
              if (!append (std::string_view (buf.data (), static_cast<std::size_t> (count)))) return output_error;
            }
        }
    }
  if (!cond.empty ()) return syntax_error;
  if (state)
    {
      ParameterState updated;
      for (std::size_t i = 0; i < globals.size (); ++i)
        if (auto* n = std::get_if<std::int64_t> (&globals[i])) updated.variables[i] = *n;
        else updated.variables[i] = std::string (std::get<std::string_view> (globals[i]));
      *state = std::move (updated);
    }
  return out;
}

template <typename... Args>
  requires (sizeof...(Args) <= 9 &&
            ((std::is_integral_v<std::remove_cvref_t<Args>> ||
              std::is_convertible_v<Args, std::string_view>) && ...))
[[nodiscard]] inline Result<std::string, std::string_view> tparm (std::string_view pattern, Args&&... args)
{
  [[maybe_unused]] const auto is_null = [] (const auto& value) {
    using T = std::remove_cvref_t<decltype (value)>;
    if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) return value == nullptr;
    return false;
  };
  if ((is_null (args) || ...)) return std::string_view ("null terminfo string parameter");
  [[maybe_unused]] const auto convert = [] (auto&& value) -> Parameter {
    using T = std::remove_cvref_t<decltype (value)>;
    if constexpr (std::is_integral_v<T>) return std::int64_t (value);
    else if constexpr (std::is_null_pointer_v<T>) return std::string_view{};
    else return std::string_view (std::forward<decltype (value)> (value));
  };
  return expand (pattern, { convert (std::forward<Args> (args))... });
}

[[nodiscard]] inline Result<std::string, std::string_view> tparm (std::string_view pattern,
                                                                const std::vector<int>& values)
{
  if (values.size () > 9) return std::string_view ("too many terminfo parameters");
  std::array<Parameter, 9> p{};
  for (std::size_t i = 0; i < values.size (); ++i) p[i] = std::int64_t (values[i]);
  return expand (pattern, p);
}

struct Padding
{
  std::chrono::microseconds delay{};
  bool proportional = false;
  bool mandatory = false;
};
using OutputChunk = std::variant<std::string, Padding>;

// Parse padding after parameter expansion. The caller can skip nonmandatory
// padding according to flow control, baud rate and its transport policy.
// No expansion or discovery API performs sleeps or writes.
[[nodiscard]] inline Result<std::vector<OutputChunk>, std::string_view>
padding (std::string_view output, unsigned affected_lines = 1)
{
  constexpr std::int64_t limit = 10'000'000; // individual and total microseconds
  constexpr std::string_view invalid = "invalid terminfo padding";
  if (output.size () > 65536) return std::string_view ("terminfo output exceeds size limit");
  std::vector<OutputChunk> result;
  std::int64_t elapsed = 0;
  const auto digit = [] (char c) { return c >= '0' && c <= '9'; };
  while (!output.empty ())
    {
      auto p = output.find ("$<");
      if (p != 0) result.emplace_back (std::string (output.substr (0, p)));
      if (p == output.npos) break;
      output.remove_prefix (p + 2);
      std::size_t i = 0;
      std::int64_t us = 0;
      while (i < output.size () && digit (output[i]))
        {
          us = us * 10 + 1000 * (output[i++] - '0');
          if (us > limit) return invalid;
        }
      auto integral = i;
      if (i < output.size () && output[i] == '.')
        {
          auto fractional = ++i;
          std::int64_t place = 100;
          while (i < output.size () && digit (output[i]))
            {
              us += place * (output[i++] - '0');
              place /= 10;
            }
          if (i == fractional) return invalid;
        }
      if (!i || (!integral && output[0] != '.')) return invalid;
      Padding delay;
      while (i < output.size () && output[i] != '>')
        {
          if (output[i] == '*' && !delay.proportional) delay.proportional = true;
          else if (output[i] == '/' && !delay.mandatory) delay.mandatory = true;
          else return invalid;
          ++i;
        }
      if (i == output.size ()) return invalid;
      const std::int64_t factor = delay.proportional ? affected_lines : 1;
      if (factor && us > (limit - elapsed) / factor)
        return std::string_view ("terminfo padding exceeds delay budget");
      us *= factor;
      elapsed += us;
      delay.delay = std::chrono::microseconds (us);
      result.emplace_back (delay);
      output.remove_prefix (i + 1);
    }
  return result;
}
} // namespace terminfo

enum class Key
{
  text, escape, enter, backspace, tab, up, down, left, right, home, end,
  insert, delete_key, page_up, page_down, function, paste, focus_in, focus_out,
  control, unknown, mouse
};

enum Modifier : unsigned { shift = 1, alt = 2, ctrl = 4 };

struct InputEvent
{
  Key key = Key::unknown;
  // Text holds one UTF-8 scalar or replacement character. Paste holds opaque
  // input bytes, never interpreted as commands or written directly to a terminal.
  std::string text{};
  unsigned modifiers = 0;
  unsigned code = 0;
  unsigned column = 0, row = 0; // one-based SGR mouse coordinates
  bool pressed = false, motion = false;
};

class InputDecoder
{
public:
  explicit InputDecoder (std::size_t max_paste_bytes = 64 * 1024)
      : max_paste_ (std::min<std::size_t> (max_paste_bytes, 1024 * 1024)) {}

  [[nodiscard]] std::vector<InputEvent> feed (std::string_view bytes)
  {
    std::vector<InputEvent> events;
    for (unsigned char c : bytes) consume (c, events);
    return events;
  }

  [[nodiscard]] bool escape_pending () const noexcept { return mode_ == Mode::escape; }

  // Resolve an isolated Escape after a caller-chosen inter-byte timeout. A
  // partial paste/control sequence is rejected, not replayed as printable text.
  [[nodiscard]] std::vector<InputEvent> flush ()
  {
    std::vector<InputEvent> events;
    if (mode_ == Mode::escape) events.push_back ({ Key::escape });
    else if (mode_ == Mode::utf8) events.push_back ({ Key::text, "\xef\xbf\xbd", utf8_mods_ });
    else if (mode_ != Mode::ground) events.push_back ({ Key::unknown });
    cancel ();
    return events;
  }

  void cancel () noexcept
  {
    mode_ = Mode::ground;
    sequence_.clear ();
    paste_.clear ();
    paste_match_ = 0;
    paste_overflow_ = false;
    string_esc_ = false;
  }

private:
  enum class Mode { ground, escape, utf8, csi, ss3, csi_discard, osc, dcs, paste };
  Mode mode_ = Mode::ground;
  std::string sequence_, paste_;
  std::size_t max_paste_, paste_match_ = 0;
  bool paste_overflow_ = false, string_esc_ = false;
  unsigned utf8_mods_ = 0, utf8_remaining_ = 0, utf8_scalar_ = 0, utf8_min_ = 0;

  static bool parse_unsigned (std::string_view text, unsigned& value)
  {
    if (text.empty ()) return false;
    auto result = std::from_chars (text.data (), text.data () + text.size (), value);
    return result.ec == std::errc{} && result.ptr == text.data () + text.size ();
  }

  static std::string_view take_field (std::string_view& s)
  {
    auto p = s.find (';');
    auto field = s.substr (0, p);
    s = p == s.npos ? std::string_view{} : s.substr (p + 1);
    return field;
  }

  static InputEvent control_sequence (std::string_view params, unsigned char final)
  {
    InputEvent event;
    if (params.starts_with ('<') && (final == 'M' || final == 'm'))
      {
        params.remove_prefix (1);
        unsigned button = 0;
        if (!parse_unsigned (take_field (params), button) || button > 255 ||
            !parse_unsigned (take_field (params), event.column) ||
            !parse_unsigned (params, event.row) || !event.column || !event.row) return {};
        event.key = Key::mouse;
        event.code = button & (3u | 64u | 128u);
        event.modifiers = (button >> 2) & 7u;
        event.motion = (button & 32u) != 0;
        event.pressed = final == 'M' && !(event.motion && event.code == 3);
        return event;
      }
    if (params.empty ())
      {
        if (final == 'I') return { Key::focus_in };
        if (final == 'O') return { Key::focus_out };
        if (final == 'Z') return { Key::tab, {}, shift };
      }
    if (params.empty () && final == '~') return {};
    unsigned n = 1;
    if (!params.empty ())
      {
        const bool has_mods = params.find (';') != params.npos;
        if (!parse_unsigned (take_field (params), n)) return {};
        if (has_mods)
          {
            unsigned encoded = 0;
            if (!parse_unsigned (params, encoded) || !encoded || encoded > 8) return {};
            event.modifiers = encoded - 1;
          }
      }
    if (final == '~')
      {
        switch (n)
          {
          case 1: case 7: event.key = Key::home; break;
          case 2: event.key = Key::insert; break;
          case 3: event.key = Key::delete_key; break;
          case 4: case 8: event.key = Key::end; break;
          case 5: event.key = Key::page_up; break;
          case 6: event.key = Key::page_down; break;
          default:
            {
              constexpr unsigned names[] = { 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24 };
              for (unsigned i = 0; i < 12; ++i)
                if (n == names[i]) { event.key = Key::function; event.code = i + 1; break; }
            }
            break;
          }
      }
    else if (n == 1)
      {
        switch (final)
          {
          case 'A': event.key = Key::up; break;
          case 'B': event.key = Key::down; break;
          case 'C': event.key = Key::right; break;
          case 'D': event.key = Key::left; break;
          case 'H': event.key = Key::home; break;
          case 'F': event.key = Key::end; break;
          case 'P': case 'Q': case 'R': case 'S':
            event.key = Key::function; event.code = final - 'P' + 1; break;
          default: break;
          }
      }
    return event;
  }

  void plain (unsigned char byte, unsigned mods, std::vector<InputEvent>& out)
  {
    if (byte == '\r' || byte == '\n') out.push_back ({ Key::enter, {}, mods });
    else if (byte == '\t') out.push_back ({ Key::tab, {}, mods });
    else if (byte == 127 || byte == 8) out.push_back ({ Key::backspace, {}, mods });
    else if (byte < 32) out.push_back ({ Key::control, {}, mods | ctrl, byte });
    else if (byte < 127) out.push_back ({ Key::text, std::string (1, static_cast<char> (byte)), mods });
    else if (byte < 194 || byte > 244)
      out.push_back ({ Key::text, "\xef\xbf\xbd", mods });
    else
      {
        mode_ = Mode::utf8;
        sequence_.assign (1, static_cast<char> (byte));
        utf8_mods_ = mods;
        utf8_remaining_ = byte < 224 ? 1 : byte < 240 ? 2 : 3;
        utf8_scalar_ = byte & (utf8_remaining_ == 1 ? 31 : utf8_remaining_ == 2 ? 15 : 7);
        utf8_min_ = utf8_remaining_ == 1 ? 128 : utf8_remaining_ == 2 ? 2048 : 65536;
      }
  }

  void paste_append (std::string_view bytes)
  {
    if (paste_overflow_) return;
    if (bytes.size () > max_paste_ - paste_.size ())
      { paste_overflow_ = true; paste_.clear (); }
    else paste_.append (bytes);
  }

  void consume (unsigned char byte, std::vector<InputEvent>& out)
  {
    constexpr std::string_view paste_end = "\x1b[201~";
    if (mode_ == Mode::paste)
      {
        if (byte == static_cast<unsigned char> (paste_end[paste_match_]))
          {
            if (++paste_match_ == paste_end.size ())
              {
                if (paste_overflow_) out.push_back ({ Key::unknown });
                else out.push_back ({ Key::paste, std::move (paste_) });
                cancel ();
              }
            return;
          }
        paste_append (paste_end.substr (0, paste_match_));
        paste_match_ = 0;
        if (byte == 27) paste_match_ = 1;
        else
          {
            const char c = static_cast<char> (byte);
            paste_append (std::string_view (&c, 1));
          }
        return;
      }
    if (mode_ == Mode::utf8)
      {
        if ((byte & 192) == 128)
          {
            sequence_ += static_cast<char> (byte);
            utf8_scalar_ = (utf8_scalar_ << 6) | (byte & 63);
            if (--utf8_remaining_ == 0)
              {
                if (utf8_scalar_ < utf8_min_ || utf8_scalar_ > 0x10ffff ||
                    (utf8_scalar_ >= 0xd800 && utf8_scalar_ <= 0xdfff))
                  sequence_ = "\xef\xbf\xbd";
                if (utf8_scalar_ >= 128 && utf8_scalar_ <= 159)
                  out.push_back ({ Key::unknown }); // Unicode C1 controls
                else out.push_back ({ Key::text, std::move (sequence_), utf8_mods_ });
                mode_ = Mode::ground;
              }
            return;
          }
        out.push_back ({ Key::text, "\xef\xbf\xbd", utf8_mods_ });
        mode_ = Mode::ground;
      }
    if (mode_ == Mode::osc || mode_ == Mode::dcs)
      {
        if ((mode_ == Mode::osc && byte == 7) || (string_esc_ && byte == '\\') ||
            byte == 24 || byte == 26)
          {
            out.push_back ({ Key::unknown });
            cancel ();
          }
        else string_esc_ = byte == 27;
        return;
      }
    if (mode_ == Mode::csi || mode_ == Mode::ss3 || mode_ == Mode::csi_discard)
      {
        if (byte >= 0x40 && byte <= 0x7e)
          {
            if (mode_ == Mode::csi && byte == '~' && sequence_ == "200")
              {
                mode_ = Mode::paste;
                return;
              }
            // CSI row;column R is indistinguishable from some modified F3
            // encodings; avoid treating terminal replies as key presses.
            if (mode_ == Mode::csi_discard || (mode_ == Mode::csi && byte == 'R' && !sequence_.empty ()))
              out.push_back ({ Key::unknown });
            else out.push_back (control_sequence (sequence_, byte));
            cancel ();
          }
        else if (byte >= 0x20 && byte <= 0x3f)
          {
            if (sequence_.size () >= 64) mode_ = Mode::csi_discard;
            if (mode_ != Mode::csi_discard) sequence_ += static_cast<char> (byte);
          }
        else if (byte == 27 || byte == 24 || byte == 26)
          {
            out.push_back ({ Key::unknown });
            cancel ();
            if (byte == 27) mode_ = Mode::escape;
          }
        return;
      }
    if (mode_ == Mode::escape)
      {
        sequence_.clear ();
        switch (byte)
          {
          case '[': mode_ = Mode::csi; return;
          case 'O': mode_ = Mode::ss3; return;
          case ']': mode_ = Mode::osc; return;
          case 'P': case '^': case '_': case 'X': mode_ = Mode::dcs; return;
          case 27: out.push_back ({ Key::escape }); return;
          default:
            mode_ = Mode::ground;
            plain (byte, alt, out);
            return;
          }
      }
    if (byte == 27) mode_ = Mode::escape;
    else plain (byte, 0, out);
  }
};

struct WindowSize
{
  unsigned rows = 0, columns = 0, pixel_width = 0, pixel_height = 0;
  bool operator== (const WindowSize&) const = default;
};

[[nodiscard]] inline std::optional<WindowSize> window_size (int fd)
{
#if LIBDSL_LIVE_POSIX
  struct ::winsize size{};
  if (::ioctl (fd, TIOCGWINSZ, &size) == 0)
    return WindowSize{ size.ws_row, size.ws_col, size.ws_xpixel, size.ws_ypixel };
#else
  (void) fd;
#endif
  return std::nullopt;
}

[[nodiscard]] inline bool is_terminal (int fd) noexcept
{
#if LIBDSL_LIVE_POSIX
  return ::isatty (fd) == 1;
#else
  (void) fd;
  return false;
#endif
}

// The descriptor remains caller-owned and must stay open until restoration.
// Restore explicitly to observe errors; destruction performs a best-effort retry.
class RawMode
{
public:
  explicit RawMode (int fd) noexcept
  {
#if LIBDSL_LIVE_POSIX
    if (::tcgetattr (fd, &saved_) != 0) { error_ = errno; return; }
    auto raw = saved_;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    while (::tcsetattr (fd, TCSANOW, &raw) != 0)
      if (errno != EINTR) { error_ = errno; return; }
    fd_ = fd;
#else
    (void) fd;
    error_ = ENOSYS;
#endif
  }

  RawMode (const RawMode&) = delete;
  RawMode& operator= (const RawMode&) = delete;
  ~RawMode () { (void) restore (); }

  [[nodiscard]] bool active () const noexcept { return fd_ >= 0; }
  [[nodiscard]] int error () const noexcept { return error_; }

  [[nodiscard]] bool restore () noexcept
  {
#if LIBDSL_LIVE_POSIX
    if (active ())
      {
        while (::tcsetattr (fd_, TCSANOW, &saved_) != 0)
          if (errno != EINTR) { error_ = errno; return false; }
        fd_ = -1;
        error_ = 0;
      }
#endif
    return error_ == 0;
  }

private:
  int fd_ = -1, error_ = 0;
#if LIBDSL_LIVE_POSIX
  ::termios saved_{};
#endif
};

enum class PrivateMode : unsigned
{
  cursor_keys = 1, cursor_visible = 25, mouse = 1000, focus = 1004,
  sgr_mouse = 1006, alternate_screen = 1049, bracketed_paste = 2004
};

[[nodiscard]] inline std::string mode_sequence (PrivateMode mode, bool enabled)
{
  switch (mode)
    {
    case PrivateMode::cursor_keys: case PrivateMode::cursor_visible:
    case PrivateMode::mouse: case PrivateMode::focus: case PrivateMode::sgr_mouse:
    case PrivateMode::alternate_screen: case PrivateMode::bracketed_paste:
      return "\x1b[?" + std::to_string (static_cast<unsigned> (mode)) + (enabled ? 'h' : 'l');
    }
  return {};
}

enum class IOStatus { data, timeout, eof, interrupted, unsupported, failed };
struct IOResult
{
  IOStatus status = IOStatus::unsupported;
  std::size_t bytes = 0;
  int error = 0;
};

// A caller can supply an in-memory, GUI, or stream transport without termios.
// Missing callbacks mean unsupported/noninteractive; writes may be partial.
struct Backend
{
  std::function<IOResult (std::span<char>, std::chrono::milliseconds)> read;
  std::function<IOResult (std::string_view)> write;
  std::function<std::optional<WindowSize> ()> size;
  std::function<bool ()> interactive;
};

// Borrowed descriptors; the caller owns descriptor lifetime and SIGPIPE policy
// for nonterminal output. A negative timeout waits indefinitely. If another
// thread reads the same fd concurrently, the fd should be nonblocking.
[[nodiscard]] inline Backend descriptor_backend (int input_fd, int output_fd)
{
#if LIBDSL_LIVE_POSIX
  return {
    [input_fd] (std::span<char> buffer, std::chrono::milliseconds timeout) -> IOResult {
      if (buffer.empty ()) return { IOStatus::data };
      if (input_fd < 0) return { IOStatus::failed, 0, EBADF };
      const auto ms = timeout.count ();
      const auto wait = ms < 0 ? -1 : ms > std::numeric_limits<int>::max ()
                   ? std::numeric_limits<int>::max () : static_cast<int> (ms);
      ::pollfd fd{ input_fd, POLLIN, 0 };
      auto ready = ::poll (&fd, 1, wait);
      if (ready < 0) return { errno == EINTR ? IOStatus::interrupted : IOStatus::failed, 0, errno };
      if (!ready) return { IOStatus::timeout };
      if (fd.revents & POLLNVAL) return { IOStatus::failed, 0, EBADF };
      auto n = ::read (input_fd, buffer.data (), std::min<std::size_t> (buffer.size (), 65536));
      if (n > 0) return { IOStatus::data, static_cast<std::size_t> (n) };
      if (!n) return { IOStatus::eof };
      const int e = errno;
      if (e == EAGAIN || e == EWOULDBLOCK) return { IOStatus::timeout };
      return { e == EINTR ? IOStatus::interrupted : IOStatus::failed, 0, e };
    },
    [output_fd] (std::string_view bytes) -> IOResult {
      if (bytes.empty ()) return { IOStatus::data };
      auto n = ::write (output_fd, bytes.data (), std::min<std::size_t> (bytes.size (), 65536));
      if (n >= 0) return { IOStatus::data, static_cast<std::size_t> (n) };
      const int e = errno;
      if (e == EAGAIN || e == EWOULDBLOCK) return { IOStatus::timeout };
      return { e == EINTR ? IOStatus::interrupted : IOStatus::failed, 0, e };
    },
    [input_fd, output_fd] {
      auto dimensions = window_size (output_fd);
      if (!dimensions && input_fd != output_fd) dimensions = window_size (input_fd);
      return dimensions;
    },
    [input_fd, output_fd] { return is_terminal (input_fd) && is_terminal (output_fd); }
  };
#else
  (void) input_fd;
  (void) output_fd;
  return {};
#endif
}

class ResizeMonitor
{
public:
  // Poll from an event loop (or after its SIGWINCH notification); never install
  // a process-wide handler. Zero/unknown dimensions do not overwrite a known size.
  [[nodiscard]] std::optional<WindowSize> observe (const Backend& backend)
  {
    auto now = backend.size ? backend.size () : std::optional<WindowSize>{};
    if (!now || !now->rows || !now->columns || now == last_) return std::nullopt;
    last_ = now;
    return now;
  }
  void reset () noexcept { last_.reset (); }

private:
  std::optional<WindowSize> last_;
};

struct Capabilities
{
  bool interactive = false;
  std::optional<terminfo::Entry> entry;
  std::string diagnostic;

  [[nodiscard]] std::int32_t colors () const
  { return interactive && entry ? entry->num ("colors").value_or (0) : 0; }

  [[nodiscard]] bool true_color () const
  {
    return interactive && entry &&
        (colors () >= 16777216 || entry->flag ("RGB") || entry->num ("RGB") ||
         entry->str ("RGB") || entry->flag ("Tc"));
  }

  [[nodiscard]] bool bracketed_paste () const
  {
    return interactive && entry && entry->str ("BE") && entry->str ("BD");
  }
};

// Passive capability discovery: never send probes or change terminal state.
// The loader can be injected for tests or non-filesystem databases.
using CapabilityLoader = std::function<Result<terminfo::Entry, std::string> ()>;
[[nodiscard]] inline Capabilities discover_capabilities (const Backend& backend,
                                                         const CapabilityLoader& loader = {})
{
  Capabilities result;
  if (!backend.interactive || !backend.interactive ()) return result;
  result.interactive = true;
  auto loaded = loader ? loader () : terminfo::load ();
  if (loaded.is_ok ()) result.entry = loaded.unwrap ();
  else result.diagnostic = loaded.unwrap_err ();
  return result;
}
} // namespace terminal

struct Terminfo
{
  template <class Derived> struct Mixin
  {
    using TerminfoEntry = terminal::terminfo::Entry;
    using InputDecoder = terminal::InputDecoder;

    [[nodiscard]] static Result<TerminfoEntry, std::string>
    load_terminfo (std::string_view bytes)
    {
      return terminal::terminfo::load_from (bytes);
    }
  };
};

// Live terminal styling and Unicode text. Adapted from TTyPack (MIT):
// third_party/ttypack/include/TTyTk/{TTyTk-Core,TTyTk-Grampheme,
// TTyTk-CharWidth,TTyTk-EscapeCodes,TTyTk-ColorTools,TTyTk-OSCTools,
// TTyTk-CapTools,TTyTk-TermMode}.hpp.
namespace terminal
{
namespace styled_detail
{
struct Utf8Scalar {
    char32_t value;
    std::size_t length;
    bool valid;
};

// Consume the longest prefix that can begin a well-formed UTF-8 scalar
// (Unicode maximal subpart). All callers share the same recovery boundaries.
[[nodiscard]] constexpr auto decode_utf8_scalar(const std::string_view input,
                                               const std::size_t offset) noexcept -> Utf8Scalar {
    if (offset >= input.size()) return {U'\uFFFD', 0, false};
    const auto lead = static_cast<unsigned char>(input[offset]);
    if (lead < 0x80) return {lead, 1, true};

    unsigned length = 0;
    char32_t value = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        value = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        value = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        value = lead & 0x07;
    } else {
        return {U'\uFFFD', 1, false};
    }

    for (std::size_t index = 1; index < length; ++index) {
        if (index >= input.size() - offset) return {U'\uFFFD', index, false};
        const auto byte = static_cast<unsigned char>(input[offset + index]);
        if (byte < 0x80 || byte > 0xBF ||
            (index == 1 && ((lead == 0xE0 && byte < 0xA0) || (lead == 0xED && byte > 0x9F) ||
                            (lead == 0xF0 && byte < 0x90) || (lead == 0xF4 && byte > 0x8F)))) {
            return {U'\uFFFD', index, false};
        }
        value = (value << 6U) | (byte & 0x3FU);
    }
    return {value, length, true};
}

enum class Property { other, cr, lf, control, extend, zwj, regional_indicator, prepend,
                      spacing_mark, l, v, t, lv, lvt };
enum class Indic { none, consonant, linker, extend };

template <typename Value>
struct Range {
    char32_t first;
    char32_t last;
    Value value;
};

template <typename Value, std::size_t N>
[[nodiscard]] constexpr auto lookup(char32_t cp, const std::array<Range<Value>, N>& ranges)
    noexcept -> Value {
    std::size_t first = 0;
    std::size_t last = N;
    while (first < last) {
        const auto mid = first + (last - first) / 2;
        if (cp < ranges[mid].first) last = mid;
        else if (cp > ranges[mid].last) first = mid + 1;
        else return ranges[mid].value;
    }
    return Value{};
}

// Generated from Unicode 16.0.0 GraphemeBreakProperty.txt,
// DerivedCoreProperties.txt (InCB) and emoji-data.txt (Extended_Pictographic).
// See the Unicode data license notice at the end of this file.
inline constexpr std::array<Range<Property>, 578> break_ranges = {{
  {0x0000, 0x0009, Property::control}, {0x000A, 0x000A, Property::lf}, {0x000B, 0x000C,
  Property::control}, {0x000D, 0x000D, Property::cr}, {0x000E, 0x001F, Property::control}, {0x007F,
  0x009F, Property::control}, {0x00AD, 0x00AD, Property::control}, {0x0300, 0x036F,
  Property::extend}, {0x0483, 0x0489, Property::extend}, {0x0591, 0x05BD, Property::extend},
  {0x05BF, 0x05BF, Property::extend}, {0x05C1, 0x05C2, Property::extend}, {0x05C4, 0x05C5,
  Property::extend}, {0x05C7, 0x05C7, Property::extend}, {0x0600, 0x0605, Property::prepend},
  {0x0610, 0x061A, Property::extend}, {0x061C, 0x061C, Property::control}, {0x064B, 0x065F,
  Property::extend}, {0x0670, 0x0670, Property::extend}, {0x06D6, 0x06DC, Property::extend},
  {0x06DD, 0x06DD, Property::prepend}, {0x06DF, 0x06E4, Property::extend}, {0x06E7, 0x06E8,
  Property::extend}, {0x06EA, 0x06ED, Property::extend}, {0x070F, 0x070F, Property::prepend},
  {0x0711, 0x0711, Property::extend}, {0x0730, 0x074A, Property::extend}, {0x07A6, 0x07B0,
  Property::extend}, {0x07EB, 0x07F3, Property::extend}, {0x07FD, 0x07FD, Property::extend},
  {0x0816, 0x0819, Property::extend}, {0x081B, 0x0823, Property::extend}, {0x0825, 0x0827,
  Property::extend}, {0x0829, 0x082D, Property::extend}, {0x0859, 0x085B, Property::extend},
  {0x0890, 0x0891, Property::prepend}, {0x0897, 0x089F, Property::extend}, {0x08CA, 0x08E1,
  Property::extend}, {0x08E2, 0x08E2, Property::prepend}, {0x08E3, 0x0902, Property::extend},
  {0x0903, 0x0903, Property::spacing_mark}, {0x093A, 0x093A, Property::extend}, {0x093B, 0x093B,
  Property::spacing_mark}, {0x093C, 0x093C, Property::extend}, {0x093E, 0x0940,
  Property::spacing_mark}, {0x0941, 0x0948, Property::extend}, {0x0949, 0x094C,
  Property::spacing_mark}, {0x094D, 0x094D, Property::extend}, {0x094E, 0x094F,
  Property::spacing_mark}, {0x0951, 0x0957, Property::extend}, {0x0962, 0x0963, Property::extend},
  {0x0981, 0x0981, Property::extend}, {0x0982, 0x0983, Property::spacing_mark}, {0x09BC, 0x09BC,
  Property::extend}, {0x09BE, 0x09BE, Property::extend}, {0x09BF, 0x09C0, Property::spacing_mark},
  {0x09C1, 0x09C4, Property::extend}, {0x09C7, 0x09C8, Property::spacing_mark}, {0x09CB, 0x09CC,
  Property::spacing_mark}, {0x09CD, 0x09CD, Property::extend}, {0x09D7, 0x09D7, Property::extend},
  {0x09E2, 0x09E3, Property::extend}, {0x09FE, 0x09FE, Property::extend}, {0x0A01, 0x0A02,
  Property::extend}, {0x0A03, 0x0A03, Property::spacing_mark}, {0x0A3C, 0x0A3C, Property::extend},
  {0x0A3E, 0x0A40, Property::spacing_mark}, {0x0A41, 0x0A42, Property::extend}, {0x0A47, 0x0A48,
  Property::extend}, {0x0A4B, 0x0A4D, Property::extend}, {0x0A51, 0x0A51, Property::extend},
  {0x0A70, 0x0A71, Property::extend}, {0x0A75, 0x0A75, Property::extend}, {0x0A81, 0x0A82,
  Property::extend}, {0x0A83, 0x0A83, Property::spacing_mark}, {0x0ABC, 0x0ABC, Property::extend},
  {0x0ABE, 0x0AC0, Property::spacing_mark}, {0x0AC1, 0x0AC5, Property::extend}, {0x0AC7, 0x0AC8,
  Property::extend}, {0x0AC9, 0x0AC9, Property::spacing_mark}, {0x0ACB, 0x0ACC,
  Property::spacing_mark}, {0x0ACD, 0x0ACD, Property::extend}, {0x0AE2, 0x0AE3, Property::extend},
  {0x0AFA, 0x0AFF, Property::extend}, {0x0B01, 0x0B01, Property::extend}, {0x0B02, 0x0B03,
  Property::spacing_mark}, {0x0B3C, 0x0B3C, Property::extend}, {0x0B3E, 0x0B3F, Property::extend},
  {0x0B40, 0x0B40, Property::spacing_mark}, {0x0B41, 0x0B44, Property::extend}, {0x0B47, 0x0B48,
  Property::spacing_mark}, {0x0B4B, 0x0B4C, Property::spacing_mark}, {0x0B4D, 0x0B4D,
  Property::extend}, {0x0B55, 0x0B57, Property::extend}, {0x0B62, 0x0B63, Property::extend},
  {0x0B82, 0x0B82, Property::extend}, {0x0BBE, 0x0BBE, Property::extend}, {0x0BBF, 0x0BBF,
  Property::spacing_mark}, {0x0BC0, 0x0BC0, Property::extend}, {0x0BC1, 0x0BC2,
  Property::spacing_mark}, {0x0BC6, 0x0BC8, Property::spacing_mark}, {0x0BCA, 0x0BCC,
  Property::spacing_mark}, {0x0BCD, 0x0BCD, Property::extend}, {0x0BD7, 0x0BD7, Property::extend},
  {0x0C00, 0x0C00, Property::extend}, {0x0C01, 0x0C03, Property::spacing_mark}, {0x0C04, 0x0C04,
  Property::extend}, {0x0C3C, 0x0C3C, Property::extend}, {0x0C3E, 0x0C40, Property::extend},
  {0x0C41, 0x0C44, Property::spacing_mark}, {0x0C46, 0x0C48, Property::extend}, {0x0C4A, 0x0C4D,
  Property::extend}, {0x0C55, 0x0C56, Property::extend}, {0x0C62, 0x0C63, Property::extend},
  {0x0C81, 0x0C81, Property::extend}, {0x0C82, 0x0C83, Property::spacing_mark}, {0x0CBC, 0x0CBC,
  Property::extend}, {0x0CBE, 0x0CBE, Property::spacing_mark}, {0x0CBF, 0x0CC0, Property::extend},
  {0x0CC1, 0x0CC1, Property::spacing_mark}, {0x0CC2, 0x0CC2, Property::extend}, {0x0CC3, 0x0CC4,
  Property::spacing_mark}, {0x0CC6, 0x0CC8, Property::extend}, {0x0CCA, 0x0CCD, Property::extend},
  {0x0CD5, 0x0CD6, Property::extend}, {0x0CE2, 0x0CE3, Property::extend}, {0x0CF3, 0x0CF3,
  Property::spacing_mark}, {0x0D00, 0x0D01, Property::extend}, {0x0D02, 0x0D03,
  Property::spacing_mark}, {0x0D3B, 0x0D3C, Property::extend}, {0x0D3E, 0x0D3E, Property::extend},
  {0x0D3F, 0x0D40, Property::spacing_mark}, {0x0D41, 0x0D44, Property::extend}, {0x0D46, 0x0D48,
  Property::spacing_mark}, {0x0D4A, 0x0D4C, Property::spacing_mark}, {0x0D4D, 0x0D4D,
  Property::extend}, {0x0D4E, 0x0D4E, Property::prepend}, {0x0D57, 0x0D57, Property::extend},
  {0x0D62, 0x0D63, Property::extend}, {0x0D81, 0x0D81, Property::extend}, {0x0D82, 0x0D83,
  Property::spacing_mark}, {0x0DCA, 0x0DCA, Property::extend}, {0x0DCF, 0x0DCF, Property::extend},
  {0x0DD0, 0x0DD1, Property::spacing_mark}, {0x0DD2, 0x0DD4, Property::extend}, {0x0DD6, 0x0DD6,
  Property::extend}, {0x0DD8, 0x0DDE, Property::spacing_mark}, {0x0DDF, 0x0DDF, Property::extend},
  {0x0DF2, 0x0DF3, Property::spacing_mark}, {0x0E31, 0x0E31, Property::extend}, {0x0E33, 0x0E33,
  Property::spacing_mark}, {0x0E34, 0x0E3A, Property::extend}, {0x0E47, 0x0E4E, Property::extend},
  {0x0EB1, 0x0EB1, Property::extend}, {0x0EB3, 0x0EB3, Property::spacing_mark}, {0x0EB4, 0x0EBC,
  Property::extend}, {0x0EC8, 0x0ECE, Property::extend}, {0x0F18, 0x0F19, Property::extend},
  {0x0F35, 0x0F35, Property::extend}, {0x0F37, 0x0F37, Property::extend}, {0x0F39, 0x0F39,
  Property::extend}, {0x0F3E, 0x0F3F, Property::spacing_mark}, {0x0F71, 0x0F7E, Property::extend},
  {0x0F7F, 0x0F7F, Property::spacing_mark}, {0x0F80, 0x0F84, Property::extend}, {0x0F86, 0x0F87,
  Property::extend}, {0x0F8D, 0x0F97, Property::extend}, {0x0F99, 0x0FBC, Property::extend},
  {0x0FC6, 0x0FC6, Property::extend}, {0x102D, 0x1030, Property::extend}, {0x1031, 0x1031,
  Property::spacing_mark}, {0x1032, 0x1037, Property::extend}, {0x1039, 0x103A, Property::extend},
  {0x103B, 0x103C, Property::spacing_mark}, {0x103D, 0x103E, Property::extend}, {0x1056, 0x1057,
  Property::spacing_mark}, {0x1058, 0x1059, Property::extend}, {0x105E, 0x1060, Property::extend},
  {0x1071, 0x1074, Property::extend}, {0x1082, 0x1082, Property::extend}, {0x1084, 0x1084,
  Property::spacing_mark}, {0x1085, 0x1086, Property::extend}, {0x108D, 0x108D, Property::extend},
  {0x109D, 0x109D, Property::extend}, {0x1100, 0x115F, Property::l}, {0x1160, 0x11A7, Property::v},
  {0x11A8, 0x11FF, Property::t}, {0x135D, 0x135F, Property::extend}, {0x1712, 0x1715,
  Property::extend}, {0x1732, 0x1734, Property::extend}, {0x1752, 0x1753, Property::extend},
  {0x1772, 0x1773, Property::extend}, {0x17B4, 0x17B5, Property::extend}, {0x17B6, 0x17B6,
  Property::spacing_mark}, {0x17B7, 0x17BD, Property::extend}, {0x17BE, 0x17C5,
  Property::spacing_mark}, {0x17C6, 0x17C6, Property::extend}, {0x17C7, 0x17C8,
  Property::spacing_mark}, {0x17C9, 0x17D3, Property::extend}, {0x17DD, 0x17DD, Property::extend},
  {0x180B, 0x180D, Property::extend}, {0x180E, 0x180E, Property::control}, {0x180F, 0x180F,
  Property::extend}, {0x1885, 0x1886, Property::extend}, {0x18A9, 0x18A9, Property::extend},
  {0x1920, 0x1922, Property::extend}, {0x1923, 0x1926, Property::spacing_mark}, {0x1927, 0x1928,
  Property::extend}, {0x1929, 0x192B, Property::spacing_mark}, {0x1930, 0x1931,
  Property::spacing_mark}, {0x1932, 0x1932, Property::extend}, {0x1933, 0x1938,
  Property::spacing_mark}, {0x1939, 0x193B, Property::extend}, {0x1A17, 0x1A18, Property::extend},
  {0x1A19, 0x1A1A, Property::spacing_mark}, {0x1A1B, 0x1A1B, Property::extend}, {0x1A55, 0x1A55,
  Property::spacing_mark}, {0x1A56, 0x1A56, Property::extend}, {0x1A57, 0x1A57,
  Property::spacing_mark}, {0x1A58, 0x1A5E, Property::extend}, {0x1A60, 0x1A60, Property::extend},
  {0x1A62, 0x1A62, Property::extend}, {0x1A65, 0x1A6C, Property::extend}, {0x1A6D, 0x1A72,
  Property::spacing_mark}, {0x1A73, 0x1A7C, Property::extend}, {0x1A7F, 0x1A7F, Property::extend},
  {0x1AB0, 0x1ACE, Property::extend}, {0x1B00, 0x1B03, Property::extend}, {0x1B04, 0x1B04,
  Property::spacing_mark}, {0x1B34, 0x1B3D, Property::extend}, {0x1B3E, 0x1B41,
  Property::spacing_mark}, {0x1B42, 0x1B44, Property::extend}, {0x1B6B, 0x1B73, Property::extend},
  {0x1B80, 0x1B81, Property::extend}, {0x1B82, 0x1B82, Property::spacing_mark}, {0x1BA1, 0x1BA1,
  Property::spacing_mark}, {0x1BA2, 0x1BA5, Property::extend}, {0x1BA6, 0x1BA7,
  Property::spacing_mark}, {0x1BA8, 0x1BAD, Property::extend}, {0x1BE6, 0x1BE6, Property::extend},
  {0x1BE7, 0x1BE7, Property::spacing_mark}, {0x1BE8, 0x1BE9, Property::extend}, {0x1BEA, 0x1BEC,
  Property::spacing_mark}, {0x1BED, 0x1BED, Property::extend}, {0x1BEE, 0x1BEE,
  Property::spacing_mark}, {0x1BEF, 0x1BF3, Property::extend}, {0x1C24, 0x1C2B,
  Property::spacing_mark}, {0x1C2C, 0x1C33, Property::extend}, {0x1C34, 0x1C35,
  Property::spacing_mark}, {0x1C36, 0x1C37, Property::extend}, {0x1CD0, 0x1CD2, Property::extend},
  {0x1CD4, 0x1CE0, Property::extend}, {0x1CE1, 0x1CE1, Property::spacing_mark}, {0x1CE2, 0x1CE8,
  Property::extend}, {0x1CED, 0x1CED, Property::extend}, {0x1CF4, 0x1CF4, Property::extend},
  {0x1CF7, 0x1CF7, Property::spacing_mark}, {0x1CF8, 0x1CF9, Property::extend}, {0x1DC0, 0x1DFF,
  Property::extend}, {0x200B, 0x200B, Property::control}, {0x200C, 0x200C, Property::extend},
  {0x200D, 0x200D, Property::zwj}, {0x200E, 0x200F, Property::control}, {0x2028, 0x202E,
  Property::control}, {0x2060, 0x206F, Property::control}, {0x20D0, 0x20F0, Property::extend},
  {0x2CEF, 0x2CF1, Property::extend}, {0x2D7F, 0x2D7F, Property::extend}, {0x2DE0, 0x2DFF,
  Property::extend}, {0x302A, 0x302F, Property::extend}, {0x3099, 0x309A, Property::extend},
  {0xA66F, 0xA672, Property::extend}, {0xA674, 0xA67D, Property::extend}, {0xA69E, 0xA69F,
  Property::extend}, {0xA6F0, 0xA6F1, Property::extend}, {0xA802, 0xA802, Property::extend},
  {0xA806, 0xA806, Property::extend}, {0xA80B, 0xA80B, Property::extend}, {0xA823, 0xA824,
  Property::spacing_mark}, {0xA825, 0xA826, Property::extend}, {0xA827, 0xA827,
  Property::spacing_mark}, {0xA82C, 0xA82C, Property::extend}, {0xA880, 0xA881,
  Property::spacing_mark}, {0xA8B4, 0xA8C3, Property::spacing_mark}, {0xA8C4, 0xA8C5,
  Property::extend}, {0xA8E0, 0xA8F1, Property::extend}, {0xA8FF, 0xA8FF, Property::extend},
  {0xA926, 0xA92D, Property::extend}, {0xA947, 0xA951, Property::extend}, {0xA952, 0xA952,
  Property::spacing_mark}, {0xA953, 0xA953, Property::extend}, {0xA960, 0xA97C, Property::l},
  {0xA980, 0xA982, Property::extend}, {0xA983, 0xA983, Property::spacing_mark}, {0xA9B3, 0xA9B3,
  Property::extend}, {0xA9B4, 0xA9B5, Property::spacing_mark}, {0xA9B6, 0xA9B9, Property::extend},
  {0xA9BA, 0xA9BB, Property::spacing_mark}, {0xA9BC, 0xA9BD, Property::extend}, {0xA9BE, 0xA9BF,
  Property::spacing_mark}, {0xA9C0, 0xA9C0, Property::extend}, {0xA9E5, 0xA9E5, Property::extend},
  {0xAA29, 0xAA2E, Property::extend}, {0xAA2F, 0xAA30, Property::spacing_mark}, {0xAA31, 0xAA32,
  Property::extend}, {0xAA33, 0xAA34, Property::spacing_mark}, {0xAA35, 0xAA36, Property::extend},
  {0xAA43, 0xAA43, Property::extend}, {0xAA4C, 0xAA4C, Property::extend}, {0xAA4D, 0xAA4D,
  Property::spacing_mark}, {0xAA7C, 0xAA7C, Property::extend}, {0xAAB0, 0xAAB0, Property::extend},
  {0xAAB2, 0xAAB4, Property::extend}, {0xAAB7, 0xAAB8, Property::extend}, {0xAABE, 0xAABF,
  Property::extend}, {0xAAC1, 0xAAC1, Property::extend}, {0xAAEB, 0xAAEB, Property::spacing_mark},
  {0xAAEC, 0xAAED, Property::extend}, {0xAAEE, 0xAAEF, Property::spacing_mark}, {0xAAF5, 0xAAF5,
  Property::spacing_mark}, {0xAAF6, 0xAAF6, Property::extend}, {0xABE3, 0xABE4,
  Property::spacing_mark}, {0xABE5, 0xABE5, Property::extend}, {0xABE6, 0xABE7,
  Property::spacing_mark}, {0xABE8, 0xABE8, Property::extend}, {0xABE9, 0xABEA,
  Property::spacing_mark}, {0xABEC, 0xABEC, Property::spacing_mark}, {0xABED, 0xABED,
  Property::extend}, {0xD7B0, 0xD7C6, Property::v}, {0xD7CB, 0xD7FB, Property::t}, {0xFB1E, 0xFB1E,
  Property::extend}, {0xFE00, 0xFE0F, Property::extend}, {0xFE20, 0xFE2F, Property::extend},
  {0xFEFF, 0xFEFF, Property::control}, {0xFF9E, 0xFF9F, Property::extend}, {0xFFF0, 0xFFFB,
  Property::control}, {0x101FD, 0x101FD, Property::extend}, {0x102E0, 0x102E0, Property::extend},
  {0x10376, 0x1037A, Property::extend}, {0x10A01, 0x10A03, Property::extend}, {0x10A05, 0x10A06,
  Property::extend}, {0x10A0C, 0x10A0F, Property::extend}, {0x10A38, 0x10A3A, Property::extend},
  {0x10A3F, 0x10A3F, Property::extend}, {0x10AE5, 0x10AE6, Property::extend}, {0x10D24, 0x10D27,
  Property::extend}, {0x10D69, 0x10D6D, Property::extend}, {0x10EAB, 0x10EAC, Property::extend},
  {0x10EFC, 0x10EFF, Property::extend}, {0x10F46, 0x10F50, Property::extend}, {0x10F82, 0x10F85,
  Property::extend}, {0x11000, 0x11000, Property::spacing_mark}, {0x11001, 0x11001,
  Property::extend}, {0x11002, 0x11002, Property::spacing_mark}, {0x11038, 0x11046,
  Property::extend}, {0x11070, 0x11070, Property::extend}, {0x11073, 0x11074, Property::extend},
  {0x1107F, 0x11081, Property::extend}, {0x11082, 0x11082, Property::spacing_mark}, {0x110B0,
  0x110B2, Property::spacing_mark}, {0x110B3, 0x110B6, Property::extend}, {0x110B7, 0x110B8,
  Property::spacing_mark}, {0x110B9, 0x110BA, Property::extend}, {0x110BD, 0x110BD,
  Property::prepend}, {0x110C2, 0x110C2, Property::extend}, {0x110CD, 0x110CD, Property::prepend},
  {0x11100, 0x11102, Property::extend}, {0x11127, 0x1112B, Property::extend}, {0x1112C, 0x1112C,
  Property::spacing_mark}, {0x1112D, 0x11134, Property::extend}, {0x11145, 0x11146,
  Property::spacing_mark}, {0x11173, 0x11173, Property::extend}, {0x11180, 0x11181,
  Property::extend}, {0x11182, 0x11182, Property::spacing_mark}, {0x111B3, 0x111B5,
  Property::spacing_mark}, {0x111B6, 0x111BE, Property::extend}, {0x111BF, 0x111BF,
  Property::spacing_mark}, {0x111C0, 0x111C0, Property::extend}, {0x111C2, 0x111C3,
  Property::prepend}, {0x111C9, 0x111CC, Property::extend}, {0x111CE, 0x111CE,
  Property::spacing_mark}, {0x111CF, 0x111CF, Property::extend}, {0x1122C, 0x1122E,
  Property::spacing_mark}, {0x1122F, 0x11231, Property::extend}, {0x11232, 0x11233,
  Property::spacing_mark}, {0x11234, 0x11237, Property::extend}, {0x1123E, 0x1123E,
  Property::extend}, {0x11241, 0x11241, Property::extend}, {0x112DF, 0x112DF, Property::extend},
  {0x112E0, 0x112E2, Property::spacing_mark}, {0x112E3, 0x112EA, Property::extend}, {0x11300,
  0x11301, Property::extend}, {0x11302, 0x11303, Property::spacing_mark}, {0x1133B, 0x1133C,
  Property::extend}, {0x1133E, 0x1133E, Property::extend}, {0x1133F, 0x1133F,
  Property::spacing_mark}, {0x11340, 0x11340, Property::extend}, {0x11341, 0x11344,
  Property::spacing_mark}, {0x11347, 0x11348, Property::spacing_mark}, {0x1134B, 0x1134C,
  Property::spacing_mark}, {0x1134D, 0x1134D, Property::extend}, {0x11357, 0x11357,
  Property::extend}, {0x11362, 0x11363, Property::spacing_mark}, {0x11366, 0x1136C,
  Property::extend}, {0x11370, 0x11374, Property::extend}, {0x113B8, 0x113B8, Property::extend},
  {0x113B9, 0x113BA, Property::spacing_mark}, {0x113BB, 0x113C0, Property::extend}, {0x113C2,
  0x113C2, Property::extend}, {0x113C5, 0x113C5, Property::extend}, {0x113C7, 0x113C9,
  Property::extend}, {0x113CA, 0x113CA, Property::spacing_mark}, {0x113CC, 0x113CD,
  Property::spacing_mark}, {0x113CE, 0x113D0, Property::extend}, {0x113D1, 0x113D1,
  Property::prepend}, {0x113D2, 0x113D2, Property::extend}, {0x113E1, 0x113E2, Property::extend},
  {0x11435, 0x11437, Property::spacing_mark}, {0x11438, 0x1143F, Property::extend}, {0x11440,
  0x11441, Property::spacing_mark}, {0x11442, 0x11444, Property::extend}, {0x11445, 0x11445,
  Property::spacing_mark}, {0x11446, 0x11446, Property::extend}, {0x1145E, 0x1145E,
  Property::extend}, {0x114B0, 0x114B0, Property::extend}, {0x114B1, 0x114B2,
  Property::spacing_mark}, {0x114B3, 0x114B8, Property::extend}, {0x114B9, 0x114B9,
  Property::spacing_mark}, {0x114BA, 0x114BA, Property::extend}, {0x114BB, 0x114BC,
  Property::spacing_mark}, {0x114BD, 0x114BD, Property::extend}, {0x114BE, 0x114BE,
  Property::spacing_mark}, {0x114BF, 0x114C0, Property::extend}, {0x114C1, 0x114C1,
  Property::spacing_mark}, {0x114C2, 0x114C3, Property::extend}, {0x115AF, 0x115AF,
  Property::extend}, {0x115B0, 0x115B1, Property::spacing_mark}, {0x115B2, 0x115B5,
  Property::extend}, {0x115B8, 0x115BB, Property::spacing_mark}, {0x115BC, 0x115BD,
  Property::extend}, {0x115BE, 0x115BE, Property::spacing_mark}, {0x115BF, 0x115C0,
  Property::extend}, {0x115DC, 0x115DD, Property::extend}, {0x11630, 0x11632,
  Property::spacing_mark}, {0x11633, 0x1163A, Property::extend}, {0x1163B, 0x1163C,
  Property::spacing_mark}, {0x1163D, 0x1163D, Property::extend}, {0x1163E, 0x1163E,
  Property::spacing_mark}, {0x1163F, 0x11640, Property::extend}, {0x116AB, 0x116AB,
  Property::extend}, {0x116AC, 0x116AC, Property::spacing_mark}, {0x116AD, 0x116AD,
  Property::extend}, {0x116AE, 0x116AF, Property::spacing_mark}, {0x116B0, 0x116B7,
  Property::extend}, {0x1171D, 0x1171D, Property::extend}, {0x1171E, 0x1171E,
  Property::spacing_mark}, {0x1171F, 0x1171F, Property::extend}, {0x11722, 0x11725,
  Property::extend}, {0x11726, 0x11726, Property::spacing_mark}, {0x11727, 0x1172B,
  Property::extend}, {0x1182C, 0x1182E, Property::spacing_mark}, {0x1182F, 0x11837,
  Property::extend}, {0x11838, 0x11838, Property::spacing_mark}, {0x11839, 0x1183A,
  Property::extend}, {0x11930, 0x11930, Property::extend}, {0x11931, 0x11935,
  Property::spacing_mark}, {0x11937, 0x11938, Property::spacing_mark}, {0x1193B, 0x1193E,
  Property::extend}, {0x1193F, 0x1193F, Property::prepend}, {0x11940, 0x11940,
  Property::spacing_mark}, {0x11941, 0x11941, Property::prepend}, {0x11942, 0x11942,
  Property::spacing_mark}, {0x11943, 0x11943, Property::extend}, {0x119D1, 0x119D3,
  Property::spacing_mark}, {0x119D4, 0x119D7, Property::extend}, {0x119DA, 0x119DB,
  Property::extend}, {0x119DC, 0x119DF, Property::spacing_mark}, {0x119E0, 0x119E0,
  Property::extend}, {0x119E4, 0x119E4, Property::spacing_mark}, {0x11A01, 0x11A0A,
  Property::extend}, {0x11A33, 0x11A38, Property::extend}, {0x11A39, 0x11A39,
  Property::spacing_mark}, {0x11A3A, 0x11A3A, Property::prepend}, {0x11A3B, 0x11A3E,
  Property::extend}, {0x11A47, 0x11A47, Property::extend}, {0x11A51, 0x11A56, Property::extend},
  {0x11A57, 0x11A58, Property::spacing_mark}, {0x11A59, 0x11A5B, Property::extend}, {0x11A84,
  0x11A89, Property::prepend}, {0x11A8A, 0x11A96, Property::extend}, {0x11A97, 0x11A97,
  Property::spacing_mark}, {0x11A98, 0x11A99, Property::extend}, {0x11C2F, 0x11C2F,
  Property::spacing_mark}, {0x11C30, 0x11C36, Property::extend}, {0x11C38, 0x11C3D,
  Property::extend}, {0x11C3E, 0x11C3E, Property::spacing_mark}, {0x11C3F, 0x11C3F,
  Property::extend}, {0x11C92, 0x11CA7, Property::extend}, {0x11CA9, 0x11CA9,
  Property::spacing_mark}, {0x11CAA, 0x11CB0, Property::extend}, {0x11CB1, 0x11CB1,
  Property::spacing_mark}, {0x11CB2, 0x11CB3, Property::extend}, {0x11CB4, 0x11CB4,
  Property::spacing_mark}, {0x11CB5, 0x11CB6, Property::extend}, {0x11D31, 0x11D36,
  Property::extend}, {0x11D3A, 0x11D3A, Property::extend}, {0x11D3C, 0x11D3D, Property::extend},
  {0x11D3F, 0x11D45, Property::extend}, {0x11D46, 0x11D46, Property::prepend}, {0x11D47, 0x11D47,
  Property::extend}, {0x11D8A, 0x11D8E, Property::spacing_mark}, {0x11D90, 0x11D91,
  Property::extend}, {0x11D93, 0x11D94, Property::spacing_mark}, {0x11D95, 0x11D95,
  Property::extend}, {0x11D96, 0x11D96, Property::spacing_mark}, {0x11D97, 0x11D97,
  Property::extend}, {0x11EF3, 0x11EF4, Property::extend}, {0x11EF5, 0x11EF6,
  Property::spacing_mark}, {0x11F00, 0x11F01, Property::extend}, {0x11F02, 0x11F02,
  Property::prepend}, {0x11F03, 0x11F03, Property::spacing_mark}, {0x11F34, 0x11F35,
  Property::spacing_mark}, {0x11F36, 0x11F3A, Property::extend}, {0x11F3E, 0x11F3F,
  Property::spacing_mark}, {0x11F40, 0x11F42, Property::extend}, {0x11F5A, 0x11F5A,
  Property::extend}, {0x13430, 0x1343F, Property::control}, {0x13440, 0x13440, Property::extend},
  {0x13447, 0x13455, Property::extend}, {0x1611E, 0x16129, Property::extend}, {0x1612A, 0x1612C,
  Property::spacing_mark}, {0x1612D, 0x1612F, Property::extend}, {0x16AF0, 0x16AF4,
  Property::extend}, {0x16B30, 0x16B36, Property::extend}, {0x16D63, 0x16D63, Property::v},
  {0x16D67, 0x16D6A, Property::v}, {0x16F4F, 0x16F4F, Property::extend}, {0x16F51, 0x16F87,
  Property::spacing_mark}, {0x16F8F, 0x16F92, Property::extend}, {0x16FE4, 0x16FE4,
  Property::extend}, {0x16FF0, 0x16FF1, Property::extend}, {0x1BC9D, 0x1BC9E, Property::extend},
  {0x1BCA0, 0x1BCA3, Property::control}, {0x1CF00, 0x1CF2D, Property::extend}, {0x1CF30, 0x1CF46,
  Property::extend}, {0x1D165, 0x1D169, Property::extend}, {0x1D16D, 0x1D172, Property::extend},
  {0x1D173, 0x1D17A, Property::control}, {0x1D17B, 0x1D182, Property::extend}, {0x1D185, 0x1D18B,
  Property::extend}, {0x1D1AA, 0x1D1AD, Property::extend}, {0x1D242, 0x1D244, Property::extend},
  {0x1DA00, 0x1DA36, Property::extend}, {0x1DA3B, 0x1DA6C, Property::extend}, {0x1DA75, 0x1DA75,
  Property::extend}, {0x1DA84, 0x1DA84, Property::extend}, {0x1DA9B, 0x1DA9F, Property::extend},
  {0x1DAA1, 0x1DAAF, Property::extend}, {0x1E000, 0x1E006, Property::extend}, {0x1E008, 0x1E018,
  Property::extend}, {0x1E01B, 0x1E021, Property::extend}, {0x1E023, 0x1E024, Property::extend},
  {0x1E026, 0x1E02A, Property::extend}, {0x1E08F, 0x1E08F, Property::extend}, {0x1E130, 0x1E136,
  Property::extend}, {0x1E2AE, 0x1E2AE, Property::extend}, {0x1E2EC, 0x1E2EF, Property::extend},
  {0x1E4EC, 0x1E4EF, Property::extend}, {0x1E5EE, 0x1E5EF, Property::extend}, {0x1E8D0, 0x1E8D6,
  Property::extend}, {0x1E944, 0x1E94A, Property::extend}, {0x1F1E6, 0x1F1FF,
  Property::regional_indicator}, {0x1F3FB, 0x1F3FF, Property::extend}, {0xE0000, 0xE001F,
  Property::control}, {0xE0020, 0xE007F, Property::extend}, {0xE0080, 0xE00FF, Property::control},
  {0xE0100, 0xE01EF, Property::extend}, {0xE01F0, 0xE0FFF, Property::control},
}};

inline constexpr std::array<Range<Indic>, 403> indic_ranges = {{
  {0x0300, 0x036F, Indic::extend}, {0x0483, 0x0489, Indic::extend}, {0x0591, 0x05BD, Indic::extend},
  {0x05BF, 0x05BF, Indic::extend}, {0x05C1, 0x05C2, Indic::extend}, {0x05C4, 0x05C5, Indic::extend},
  {0x05C7, 0x05C7, Indic::extend}, {0x0610, 0x061A, Indic::extend}, {0x064B, 0x065F, Indic::extend},
  {0x0670, 0x0670, Indic::extend}, {0x06D6, 0x06DC, Indic::extend}, {0x06DF, 0x06E4, Indic::extend},
  {0x06E7, 0x06E8, Indic::extend}, {0x06EA, 0x06ED, Indic::extend}, {0x0711, 0x0711, Indic::extend},
  {0x0730, 0x074A, Indic::extend}, {0x07A6, 0x07B0, Indic::extend}, {0x07EB, 0x07F3, Indic::extend},
  {0x07FD, 0x07FD, Indic::extend}, {0x0816, 0x0819, Indic::extend}, {0x081B, 0x0823, Indic::extend},
  {0x0825, 0x0827, Indic::extend}, {0x0829, 0x082D, Indic::extend}, {0x0859, 0x085B, Indic::extend},
  {0x0897, 0x089F, Indic::extend}, {0x08CA, 0x08E1, Indic::extend}, {0x08E3, 0x0902, Indic::extend},
  {0x0915, 0x0939, Indic::consonant}, {0x093A, 0x093A, Indic::extend}, {0x093C, 0x093C,
  Indic::extend}, {0x0941, 0x0948, Indic::extend}, {0x094D, 0x094D, Indic::linker}, {0x0951, 0x0957,
  Indic::extend}, {0x0958, 0x095F, Indic::consonant}, {0x0962, 0x0963, Indic::extend}, {0x0978,
  0x097F, Indic::consonant}, {0x0981, 0x0981, Indic::extend}, {0x0995, 0x09A8, Indic::consonant},
  {0x09AA, 0x09B0, Indic::consonant}, {0x09B2, 0x09B2, Indic::consonant}, {0x09B6, 0x09B9,
  Indic::consonant}, {0x09BC, 0x09BC, Indic::extend}, {0x09BE, 0x09BE, Indic::extend}, {0x09C1,
  0x09C4, Indic::extend}, {0x09CD, 0x09CD, Indic::linker}, {0x09D7, 0x09D7, Indic::extend}, {0x09DC,
  0x09DD, Indic::consonant}, {0x09DF, 0x09DF, Indic::consonant}, {0x09E2, 0x09E3, Indic::extend},
  {0x09F0, 0x09F1, Indic::consonant}, {0x09FE, 0x09FE, Indic::extend}, {0x0A01, 0x0A02,
  Indic::extend}, {0x0A3C, 0x0A3C, Indic::extend}, {0x0A41, 0x0A42, Indic::extend}, {0x0A47, 0x0A48,
  Indic::extend}, {0x0A4B, 0x0A4D, Indic::extend}, {0x0A51, 0x0A51, Indic::extend}, {0x0A70, 0x0A71,
  Indic::extend}, {0x0A75, 0x0A75, Indic::extend}, {0x0A81, 0x0A82, Indic::extend}, {0x0A95, 0x0AA8,
  Indic::consonant}, {0x0AAA, 0x0AB0, Indic::consonant}, {0x0AB2, 0x0AB3, Indic::consonant},
  {0x0AB5, 0x0AB9, Indic::consonant}, {0x0ABC, 0x0ABC, Indic::extend}, {0x0AC1, 0x0AC5,
  Indic::extend}, {0x0AC7, 0x0AC8, Indic::extend}, {0x0ACD, 0x0ACD, Indic::linker}, {0x0AE2, 0x0AE3,
  Indic::extend}, {0x0AF9, 0x0AF9, Indic::consonant}, {0x0AFA, 0x0AFF, Indic::extend}, {0x0B01,
  0x0B01, Indic::extend}, {0x0B15, 0x0B28, Indic::consonant}, {0x0B2A, 0x0B30, Indic::consonant},
  {0x0B32, 0x0B33, Indic::consonant}, {0x0B35, 0x0B39, Indic::consonant}, {0x0B3C, 0x0B3C,
  Indic::extend}, {0x0B3E, 0x0B3F, Indic::extend}, {0x0B41, 0x0B44, Indic::extend}, {0x0B4D, 0x0B4D,
  Indic::linker}, {0x0B55, 0x0B57, Indic::extend}, {0x0B5C, 0x0B5D, Indic::consonant}, {0x0B5F,
  0x0B5F, Indic::consonant}, {0x0B62, 0x0B63, Indic::extend}, {0x0B71, 0x0B71, Indic::consonant},
  {0x0B82, 0x0B82, Indic::extend}, {0x0BBE, 0x0BBE, Indic::extend}, {0x0BC0, 0x0BC0, Indic::extend},
  {0x0BCD, 0x0BCD, Indic::extend}, {0x0BD7, 0x0BD7, Indic::extend}, {0x0C00, 0x0C00, Indic::extend},
  {0x0C04, 0x0C04, Indic::extend}, {0x0C15, 0x0C28, Indic::consonant}, {0x0C2A, 0x0C39,
  Indic::consonant}, {0x0C3C, 0x0C3C, Indic::extend}, {0x0C3E, 0x0C40, Indic::extend}, {0x0C46,
  0x0C48, Indic::extend}, {0x0C4A, 0x0C4C, Indic::extend}, {0x0C4D, 0x0C4D, Indic::linker}, {0x0C55,
  0x0C56, Indic::extend}, {0x0C58, 0x0C5A, Indic::consonant}, {0x0C62, 0x0C63, Indic::extend},
  {0x0C81, 0x0C81, Indic::extend}, {0x0CBC, 0x0CBC, Indic::extend}, {0x0CBF, 0x0CC0, Indic::extend},
  {0x0CC2, 0x0CC2, Indic::extend}, {0x0CC6, 0x0CC8, Indic::extend}, {0x0CCA, 0x0CCD, Indic::extend},
  {0x0CD5, 0x0CD6, Indic::extend}, {0x0CE2, 0x0CE3, Indic::extend}, {0x0D00, 0x0D01, Indic::extend},
  {0x0D15, 0x0D3A, Indic::consonant}, {0x0D3B, 0x0D3C, Indic::extend}, {0x0D3E, 0x0D3E,
  Indic::extend}, {0x0D41, 0x0D44, Indic::extend}, {0x0D4D, 0x0D4D, Indic::linker}, {0x0D57, 0x0D57,
  Indic::extend}, {0x0D62, 0x0D63, Indic::extend}, {0x0D81, 0x0D81, Indic::extend}, {0x0DCA, 0x0DCA,
  Indic::extend}, {0x0DCF, 0x0DCF, Indic::extend}, {0x0DD2, 0x0DD4, Indic::extend}, {0x0DD6, 0x0DD6,
  Indic::extend}, {0x0DDF, 0x0DDF, Indic::extend}, {0x0E31, 0x0E31, Indic::extend}, {0x0E34, 0x0E3A,
  Indic::extend}, {0x0E47, 0x0E4E, Indic::extend}, {0x0EB1, 0x0EB1, Indic::extend}, {0x0EB4, 0x0EBC,
  Indic::extend}, {0x0EC8, 0x0ECE, Indic::extend}, {0x0F18, 0x0F19, Indic::extend}, {0x0F35, 0x0F35,
  Indic::extend}, {0x0F37, 0x0F37, Indic::extend}, {0x0F39, 0x0F39, Indic::extend}, {0x0F71, 0x0F7E,
  Indic::extend}, {0x0F80, 0x0F84, Indic::extend}, {0x0F86, 0x0F87, Indic::extend}, {0x0F8D, 0x0F97,
  Indic::extend}, {0x0F99, 0x0FBC, Indic::extend}, {0x0FC6, 0x0FC6, Indic::extend}, {0x102D, 0x1030,
  Indic::extend}, {0x1032, 0x1037, Indic::extend}, {0x1039, 0x103A, Indic::extend}, {0x103D, 0x103E,
  Indic::extend}, {0x1058, 0x1059, Indic::extend}, {0x105E, 0x1060, Indic::extend}, {0x1071, 0x1074,
  Indic::extend}, {0x1082, 0x1082, Indic::extend}, {0x1085, 0x1086, Indic::extend}, {0x108D, 0x108D,
  Indic::extend}, {0x109D, 0x109D, Indic::extend}, {0x135D, 0x135F, Indic::extend}, {0x1712, 0x1715,
  Indic::extend}, {0x1732, 0x1734, Indic::extend}, {0x1752, 0x1753, Indic::extend}, {0x1772, 0x1773,
  Indic::extend}, {0x17B4, 0x17B5, Indic::extend}, {0x17B7, 0x17BD, Indic::extend}, {0x17C6, 0x17C6,
  Indic::extend}, {0x17C9, 0x17D3, Indic::extend}, {0x17DD, 0x17DD, Indic::extend}, {0x180B, 0x180D,
  Indic::extend}, {0x180F, 0x180F, Indic::extend}, {0x1885, 0x1886, Indic::extend}, {0x18A9, 0x18A9,
  Indic::extend}, {0x1920, 0x1922, Indic::extend}, {0x1927, 0x1928, Indic::extend}, {0x1932, 0x1932,
  Indic::extend}, {0x1939, 0x193B, Indic::extend}, {0x1A17, 0x1A18, Indic::extend}, {0x1A1B, 0x1A1B,
  Indic::extend}, {0x1A56, 0x1A56, Indic::extend}, {0x1A58, 0x1A5E, Indic::extend}, {0x1A60, 0x1A60,
  Indic::extend}, {0x1A62, 0x1A62, Indic::extend}, {0x1A65, 0x1A6C, Indic::extend}, {0x1A73, 0x1A7C,
  Indic::extend}, {0x1A7F, 0x1A7F, Indic::extend}, {0x1AB0, 0x1ACE, Indic::extend}, {0x1B00, 0x1B03,
  Indic::extend}, {0x1B34, 0x1B3D, Indic::extend}, {0x1B42, 0x1B44, Indic::extend}, {0x1B6B, 0x1B73,
  Indic::extend}, {0x1B80, 0x1B81, Indic::extend}, {0x1BA2, 0x1BA5, Indic::extend}, {0x1BA8, 0x1BAD,
  Indic::extend}, {0x1BE6, 0x1BE6, Indic::extend}, {0x1BE8, 0x1BE9, Indic::extend}, {0x1BED, 0x1BED,
  Indic::extend}, {0x1BEF, 0x1BF3, Indic::extend}, {0x1C2C, 0x1C33, Indic::extend}, {0x1C36, 0x1C37,
  Indic::extend}, {0x1CD0, 0x1CD2, Indic::extend}, {0x1CD4, 0x1CE0, Indic::extend}, {0x1CE2, 0x1CE8,
  Indic::extend}, {0x1CED, 0x1CED, Indic::extend}, {0x1CF4, 0x1CF4, Indic::extend}, {0x1CF8, 0x1CF9,
  Indic::extend}, {0x1DC0, 0x1DFF, Indic::extend}, {0x200D, 0x200D, Indic::extend}, {0x20D0, 0x20F0,
  Indic::extend}, {0x2CEF, 0x2CF1, Indic::extend}, {0x2D7F, 0x2D7F, Indic::extend}, {0x2DE0, 0x2DFF,
  Indic::extend}, {0x302A, 0x302F, Indic::extend}, {0x3099, 0x309A, Indic::extend}, {0xA66F, 0xA672,
  Indic::extend}, {0xA674, 0xA67D, Indic::extend}, {0xA69E, 0xA69F, Indic::extend}, {0xA6F0, 0xA6F1,
  Indic::extend}, {0xA802, 0xA802, Indic::extend}, {0xA806, 0xA806, Indic::extend}, {0xA80B, 0xA80B,
  Indic::extend}, {0xA825, 0xA826, Indic::extend}, {0xA82C, 0xA82C, Indic::extend}, {0xA8C4, 0xA8C5,
  Indic::extend}, {0xA8E0, 0xA8F1, Indic::extend}, {0xA8FF, 0xA8FF, Indic::extend}, {0xA926, 0xA92D,
  Indic::extend}, {0xA947, 0xA951, Indic::extend}, {0xA953, 0xA953, Indic::extend}, {0xA980, 0xA982,
  Indic::extend}, {0xA9B3, 0xA9B3, Indic::extend}, {0xA9B6, 0xA9B9, Indic::extend}, {0xA9BC, 0xA9BD,
  Indic::extend}, {0xA9C0, 0xA9C0, Indic::extend}, {0xA9E5, 0xA9E5, Indic::extend}, {0xAA29, 0xAA2E,
  Indic::extend}, {0xAA31, 0xAA32, Indic::extend}, {0xAA35, 0xAA36, Indic::extend}, {0xAA43, 0xAA43,
  Indic::extend}, {0xAA4C, 0xAA4C, Indic::extend}, {0xAA7C, 0xAA7C, Indic::extend}, {0xAAB0, 0xAAB0,
  Indic::extend}, {0xAAB2, 0xAAB4, Indic::extend}, {0xAAB7, 0xAAB8, Indic::extend}, {0xAABE, 0xAABF,
  Indic::extend}, {0xAAC1, 0xAAC1, Indic::extend}, {0xAAEC, 0xAAED, Indic::extend}, {0xAAF6, 0xAAF6,
  Indic::extend}, {0xABE5, 0xABE5, Indic::extend}, {0xABE8, 0xABE8, Indic::extend}, {0xABED, 0xABED,
  Indic::extend}, {0xFB1E, 0xFB1E, Indic::extend}, {0xFE00, 0xFE0F, Indic::extend}, {0xFE20, 0xFE2F,
  Indic::extend}, {0xFF9E, 0xFF9F, Indic::extend}, {0x101FD, 0x101FD, Indic::extend}, {0x102E0,
  0x102E0, Indic::extend}, {0x10376, 0x1037A, Indic::extend}, {0x10A01, 0x10A03, Indic::extend},
  {0x10A05, 0x10A06, Indic::extend}, {0x10A0C, 0x10A0F, Indic::extend}, {0x10A38, 0x10A3A,
  Indic::extend}, {0x10A3F, 0x10A3F, Indic::extend}, {0x10AE5, 0x10AE6, Indic::extend}, {0x10D24,
  0x10D27, Indic::extend}, {0x10D69, 0x10D6D, Indic::extend}, {0x10EAB, 0x10EAC, Indic::extend},
  {0x10EFC, 0x10EFF, Indic::extend}, {0x10F46, 0x10F50, Indic::extend}, {0x10F82, 0x10F85,
  Indic::extend}, {0x11001, 0x11001, Indic::extend}, {0x11038, 0x11046, Indic::extend}, {0x11070,
  0x11070, Indic::extend}, {0x11073, 0x11074, Indic::extend}, {0x1107F, 0x11081, Indic::extend},
  {0x110B3, 0x110B6, Indic::extend}, {0x110B9, 0x110BA, Indic::extend}, {0x110C2, 0x110C2,
  Indic::extend}, {0x11100, 0x11102, Indic::extend}, {0x11127, 0x1112B, Indic::extend}, {0x1112D,
  0x11134, Indic::extend}, {0x11173, 0x11173, Indic::extend}, {0x11180, 0x11181, Indic::extend},
  {0x111B6, 0x111BE, Indic::extend}, {0x111C0, 0x111C0, Indic::extend}, {0x111C9, 0x111CC,
  Indic::extend}, {0x111CF, 0x111CF, Indic::extend}, {0x1122F, 0x11231, Indic::extend}, {0x11234,
  0x11237, Indic::extend}, {0x1123E, 0x1123E, Indic::extend}, {0x11241, 0x11241, Indic::extend},
  {0x112DF, 0x112DF, Indic::extend}, {0x112E3, 0x112EA, Indic::extend}, {0x11300, 0x11301,
  Indic::extend}, {0x1133B, 0x1133C, Indic::extend}, {0x1133E, 0x1133E, Indic::extend}, {0x11340,
  0x11340, Indic::extend}, {0x1134D, 0x1134D, Indic::extend}, {0x11357, 0x11357, Indic::extend},
  {0x11366, 0x1136C, Indic::extend}, {0x11370, 0x11374, Indic::extend}, {0x113B8, 0x113B8,
  Indic::extend}, {0x113BB, 0x113C0, Indic::extend}, {0x113C2, 0x113C2, Indic::extend}, {0x113C5,
  0x113C5, Indic::extend}, {0x113C7, 0x113C9, Indic::extend}, {0x113CE, 0x113D0, Indic::extend},
  {0x113D2, 0x113D2, Indic::extend}, {0x113E1, 0x113E2, Indic::extend}, {0x11438, 0x1143F,
  Indic::extend}, {0x11442, 0x11444, Indic::extend}, {0x11446, 0x11446, Indic::extend}, {0x1145E,
  0x1145E, Indic::extend}, {0x114B0, 0x114B0, Indic::extend}, {0x114B3, 0x114B8, Indic::extend},
  {0x114BA, 0x114BA, Indic::extend}, {0x114BD, 0x114BD, Indic::extend}, {0x114BF, 0x114C0,
  Indic::extend}, {0x114C2, 0x114C3, Indic::extend}, {0x115AF, 0x115AF, Indic::extend}, {0x115B2,
  0x115B5, Indic::extend}, {0x115BC, 0x115BD, Indic::extend}, {0x115BF, 0x115C0, Indic::extend},
  {0x115DC, 0x115DD, Indic::extend}, {0x11633, 0x1163A, Indic::extend}, {0x1163D, 0x1163D,
  Indic::extend}, {0x1163F, 0x11640, Indic::extend}, {0x116AB, 0x116AB, Indic::extend}, {0x116AD,
  0x116AD, Indic::extend}, {0x116B0, 0x116B7, Indic::extend}, {0x1171D, 0x1171D, Indic::extend},
  {0x1171F, 0x1171F, Indic::extend}, {0x11722, 0x11725, Indic::extend}, {0x11727, 0x1172B,
  Indic::extend}, {0x1182F, 0x11837, Indic::extend}, {0x11839, 0x1183A, Indic::extend}, {0x11930,
  0x11930, Indic::extend}, {0x1193B, 0x1193E, Indic::extend}, {0x11943, 0x11943, Indic::extend},
  {0x119D4, 0x119D7, Indic::extend}, {0x119DA, 0x119DB, Indic::extend}, {0x119E0, 0x119E0,
  Indic::extend}, {0x11A01, 0x11A0A, Indic::extend}, {0x11A33, 0x11A38, Indic::extend}, {0x11A3B,
  0x11A3E, Indic::extend}, {0x11A47, 0x11A47, Indic::extend}, {0x11A51, 0x11A56, Indic::extend},
  {0x11A59, 0x11A5B, Indic::extend}, {0x11A8A, 0x11A96, Indic::extend}, {0x11A98, 0x11A99,
  Indic::extend}, {0x11C30, 0x11C36, Indic::extend}, {0x11C38, 0x11C3D, Indic::extend}, {0x11C3F,
  0x11C3F, Indic::extend}, {0x11C92, 0x11CA7, Indic::extend}, {0x11CAA, 0x11CB0, Indic::extend},
  {0x11CB2, 0x11CB3, Indic::extend}, {0x11CB5, 0x11CB6, Indic::extend}, {0x11D31, 0x11D36,
  Indic::extend}, {0x11D3A, 0x11D3A, Indic::extend}, {0x11D3C, 0x11D3D, Indic::extend}, {0x11D3F,
  0x11D45, Indic::extend}, {0x11D47, 0x11D47, Indic::extend}, {0x11D90, 0x11D91, Indic::extend},
  {0x11D95, 0x11D95, Indic::extend}, {0x11D97, 0x11D97, Indic::extend}, {0x11EF3, 0x11EF4,
  Indic::extend}, {0x11F00, 0x11F01, Indic::extend}, {0x11F36, 0x11F3A, Indic::extend}, {0x11F40,
  0x11F42, Indic::extend}, {0x11F5A, 0x11F5A, Indic::extend}, {0x13440, 0x13440, Indic::extend},
  {0x13447, 0x13455, Indic::extend}, {0x1611E, 0x16129, Indic::extend}, {0x1612D, 0x1612F,
  Indic::extend}, {0x16AF0, 0x16AF4, Indic::extend}, {0x16B30, 0x16B36, Indic::extend}, {0x16F4F,
  0x16F4F, Indic::extend}, {0x16F8F, 0x16F92, Indic::extend}, {0x16FE4, 0x16FE4, Indic::extend},
  {0x16FF0, 0x16FF1, Indic::extend}, {0x1BC9D, 0x1BC9E, Indic::extend}, {0x1CF00, 0x1CF2D,
  Indic::extend}, {0x1CF30, 0x1CF46, Indic::extend}, {0x1D165, 0x1D169, Indic::extend}, {0x1D16D,
  0x1D172, Indic::extend}, {0x1D17B, 0x1D182, Indic::extend}, {0x1D185, 0x1D18B, Indic::extend},
  {0x1D1AA, 0x1D1AD, Indic::extend}, {0x1D242, 0x1D244, Indic::extend}, {0x1DA00, 0x1DA36,
  Indic::extend}, {0x1DA3B, 0x1DA6C, Indic::extend}, {0x1DA75, 0x1DA75, Indic::extend}, {0x1DA84,
  0x1DA84, Indic::extend}, {0x1DA9B, 0x1DA9F, Indic::extend}, {0x1DAA1, 0x1DAAF, Indic::extend},
  {0x1E000, 0x1E006, Indic::extend}, {0x1E008, 0x1E018, Indic::extend}, {0x1E01B, 0x1E021,
  Indic::extend}, {0x1E023, 0x1E024, Indic::extend}, {0x1E026, 0x1E02A, Indic::extend}, {0x1E08F,
  0x1E08F, Indic::extend}, {0x1E130, 0x1E136, Indic::extend}, {0x1E2AE, 0x1E2AE, Indic::extend},
  {0x1E2EC, 0x1E2EF, Indic::extend}, {0x1E4EC, 0x1E4EF, Indic::extend}, {0x1E5EE, 0x1E5EF,
  Indic::extend}, {0x1E8D0, 0x1E8D6, Indic::extend}, {0x1E944, 0x1E94A, Indic::extend}, {0x1F3FB,
  0x1F3FF, Indic::extend}, {0xE0020, 0xE007F, Indic::extend}, {0xE0100, 0xE01EF, Indic::extend},
}};

inline constexpr std::array<Range<bool>, 78> pictographic_ranges = {{
  {0x00A9, 0x00A9, true}, {0x00AE, 0x00AE, true}, {0x203C, 0x203C, true}, {0x2049, 0x2049, true},
  {0x2122, 0x2122, true}, {0x2139, 0x2139, true}, {0x2194, 0x2199, true}, {0x21A9, 0x21AA, true},
  {0x231A, 0x231B, true}, {0x2328, 0x2328, true}, {0x2388, 0x2388, true}, {0x23CF, 0x23CF, true},
  {0x23E9, 0x23F3, true}, {0x23F8, 0x23FA, true}, {0x24C2, 0x24C2, true}, {0x25AA, 0x25AB, true},
  {0x25B6, 0x25B6, true}, {0x25C0, 0x25C0, true}, {0x25FB, 0x25FE, true}, {0x2600, 0x2605, true},
  {0x2607, 0x2612, true}, {0x2614, 0x2685, true}, {0x2690, 0x2705, true}, {0x2708, 0x2712, true},
  {0x2714, 0x2714, true}, {0x2716, 0x2716, true}, {0x271D, 0x271D, true}, {0x2721, 0x2721, true},
  {0x2728, 0x2728, true}, {0x2733, 0x2734, true}, {0x2744, 0x2744, true}, {0x2747, 0x2747, true},
  {0x274C, 0x274C, true}, {0x274E, 0x274E, true}, {0x2753, 0x2755, true}, {0x2757, 0x2757, true},
  {0x2763, 0x2767, true}, {0x2795, 0x2797, true}, {0x27A1, 0x27A1, true}, {0x27B0, 0x27B0, true},
  {0x27BF, 0x27BF, true}, {0x2934, 0x2935, true}, {0x2B05, 0x2B07, true}, {0x2B1B, 0x2B1C, true},
  {0x2B50, 0x2B50, true}, {0x2B55, 0x2B55, true}, {0x3030, 0x3030, true}, {0x303D, 0x303D, true},
  {0x3297, 0x3297, true}, {0x3299, 0x3299, true}, {0x1F000, 0x1F0FF, true}, {0x1F10D, 0x1F10F,
  true}, {0x1F12F, 0x1F12F, true}, {0x1F16C, 0x1F171, true}, {0x1F17E, 0x1F17F, true}, {0x1F18E,
  0x1F18E, true}, {0x1F191, 0x1F19A, true}, {0x1F1AD, 0x1F1E5, true}, {0x1F201, 0x1F20F, true},
  {0x1F21A, 0x1F21A, true}, {0x1F22F, 0x1F22F, true}, {0x1F232, 0x1F23A, true}, {0x1F23C, 0x1F23F,
  true}, {0x1F249, 0x1F3FA, true}, {0x1F400, 0x1F53D, true}, {0x1F546, 0x1F64F, true}, {0x1F680,
  0x1F6FF, true}, {0x1F774, 0x1F77F, true}, {0x1F7D5, 0x1F7FF, true}, {0x1F80C, 0x1F80F, true},
  {0x1F848, 0x1F84F, true}, {0x1F85A, 0x1F85F, true}, {0x1F888, 0x1F88F, true}, {0x1F8AE, 0x1F8FF,
  true}, {0x1F90C, 0x1F93A, true}, {0x1F93C, 0x1F945, true}, {0x1F947, 0x1FAFF, true}, {0x1FC00,
  0x1FFFD, true},
}};



[[nodiscard]] constexpr auto property(char32_t cp) noexcept -> Property {
    if (cp >= 0xAC00 && cp <= 0xD7A3)
        return (cp - 0xAC00) % 28 == 0 ? Property::lv : Property::lvt;
    return lookup(cp, break_ranges);
}

struct State {
    Property previous{};
    bool pictographic_run{};
    bool zwj_after_pictographic{};
    std::size_t regional_run{};
    Indic indic_run{};

    constexpr void consume(char32_t cp, Property current, Indic indic) noexcept {
        regional_run = current == Property::regional_indicator ? regional_run + 1 : 0;
        zwj_after_pictographic = current == Property::zwj && pictographic_run;
        pictographic_run = lookup(cp, pictographic_ranges) ||
                           (current == Property::extend && pictographic_run);
        if (indic == Indic::consonant) indic_run = Indic::consonant;
        else if (indic == Indic::linker && indic_run != Indic::none) indic_run = Indic::linker;
        else if (indic != Indic::extend) indic_run = Indic::none;
        previous = current;
    }
};

[[nodiscard]] constexpr auto is_control(Property p) noexcept -> bool {
    return p == Property::cr || p == Property::lf || p == Property::control;
}

[[nodiscard]] constexpr auto no_break(const State& state, char32_t cp, Property current,
                                      Indic indic) noexcept -> bool {
    const auto previous = state.previous;
    // UAX #29 extended grapheme clusters, rules in precedence order.
    if (previous == Property::cr && current == Property::lf) return true;                 // GB3
    if (is_control(previous) || is_control(current)) return false;                         // GB4–5
    if (previous == Property::l &&
        (current == Property::l || current == Property::v || current == Property::lv ||
         current == Property::lvt)) return true;                                           // GB6
    if ((previous == Property::lv || previous == Property::v) &&
        (current == Property::v || current == Property::t)) return true;                    // GB7
    if ((previous == Property::lvt || previous == Property::t) && current == Property::t)
        return true;                                                                        // GB8
    if (current == Property::extend || current == Property::zwj) return true;               // GB9
    if (current == Property::spacing_mark) return true;                                     // GB9a
    if (previous == Property::prepend) return true;                                         // GB9b
    if (state.indic_run == Indic::linker && indic == Indic::consonant) return true;           // GB9c
    if (state.zwj_after_pictographic && lookup(cp, pictographic_ranges)) return true;       // GB11
    return current == Property::regional_indicator && state.regional_run % 2U == 1U;        // GB12–13
}

[[nodiscard]] constexpr auto next_break(std::string_view text, std::size_t start = 0)
    noexcept -> std::size_t {
    if (start >= text.size()) return text.size();
    const auto first = decode_utf8_scalar(text, start);
    State state;
    state.consume(first.value, property(first.value),
                  lookup(first.value, indic_ranges));
    auto offset = start + first.length;
    while (offset < text.size()) {
        const auto scalar = decode_utf8_scalar(text, offset);
        const auto current = property(scalar.value);
        const auto indic = lookup(scalar.value, indic_ranges);
        if (!no_break(state, scalar.value, current, indic)) return offset;
        state.consume(scalar.value, current, indic);
        offset += scalar.length;
    }
    return text.size();
}

struct Interval {
    char32_t first;
    char32_t last;
};

template <std::size_t N>
[[nodiscard]] constexpr auto in_intervals(const char32_t codepoint,
                                          const std::array<Interval, N>& intervals) noexcept
    -> bool {
    std::size_t first = 0;
    std::size_t last = intervals.size();
    while (first < last) {
        const auto mid = first + (last - first) / 2;
        if (codepoint < intervals[mid].first) last = mid;
        else if (codepoint > intervals[mid].last) first = mid + 1;
        else return true;
    }
    return false;
}

// Generated from Unicode 16.0.0 EastAsianWidth.txt (W/F/A),
// DerivedGeneralCategory.txt (Mn/Me/Cf), and emoji-variation-sequences.txt.
// See the Unicode data license notice at the end of this file.
inline constexpr std::array<Interval, 368> zero_width = {{
  {0x00AD, 0x00AD}, {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1,
  0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0600, 0x0605}, {0x0610, 0x061A}, {0x061C, 0x061C},
  {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DD}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA,
  0x06ED}, {0x070F, 0x070F}, {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0}, {0x07EB, 0x07F3},
  {0x07FD, 0x07FD}, {0x0816, 0x0819}, {0x081B, 0x0823}, {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859,
  0x085B}, {0x0890, 0x0891}, {0x0897, 0x089F}, {0x08CA, 0x0902}, {0x093A, 0x093A}, {0x093C, 0x093C},
  {0x0941, 0x0948}, {0x094D, 0x094D}, {0x0951, 0x0957}, {0x0962, 0x0963}, {0x0981, 0x0981}, {0x09BC,
  0x09BC}, {0x09C1, 0x09C4}, {0x09CD, 0x09CD}, {0x09E2, 0x09E3}, {0x09FE, 0x09FE}, {0x0A01, 0x0A02},
  {0x0A3C, 0x0A3C}, {0x0A41, 0x0A42}, {0x0A47, 0x0A48}, {0x0A4B, 0x0A4D}, {0x0A51, 0x0A51}, {0x0A70,
  0x0A71}, {0x0A75, 0x0A75}, {0x0A81, 0x0A82}, {0x0ABC, 0x0ABC}, {0x0AC1, 0x0AC5}, {0x0AC7, 0x0AC8},
  {0x0ACD, 0x0ACD}, {0x0AE2, 0x0AE3}, {0x0AFA, 0x0AFF}, {0x0B01, 0x0B01}, {0x0B3C, 0x0B3C}, {0x0B3F,
  0x0B3F}, {0x0B41, 0x0B44}, {0x0B4D, 0x0B4D}, {0x0B55, 0x0B56}, {0x0B62, 0x0B63}, {0x0B82, 0x0B82},
  {0x0BC0, 0x0BC0}, {0x0BCD, 0x0BCD}, {0x0C00, 0x0C00}, {0x0C04, 0x0C04}, {0x0C3C, 0x0C3C}, {0x0C3E,
  0x0C40}, {0x0C46, 0x0C48}, {0x0C4A, 0x0C4D}, {0x0C55, 0x0C56}, {0x0C62, 0x0C63}, {0x0C81, 0x0C81},
  {0x0CBC, 0x0CBC}, {0x0CBF, 0x0CBF}, {0x0CC6, 0x0CC6}, {0x0CCC, 0x0CCD}, {0x0CE2, 0x0CE3}, {0x0D00,
  0x0D01}, {0x0D3B, 0x0D3C}, {0x0D41, 0x0D44}, {0x0D4D, 0x0D4D}, {0x0D62, 0x0D63}, {0x0D81, 0x0D81},
  {0x0DCA, 0x0DCA}, {0x0DD2, 0x0DD4}, {0x0DD6, 0x0DD6}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47,
  0x0E4E}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC}, {0x0EC8, 0x0ECE}, {0x0F18, 0x0F19}, {0x0F35, 0x0F35},
  {0x0F37, 0x0F37}, {0x0F39, 0x0F39}, {0x0F71, 0x0F7E}, {0x0F80, 0x0F84}, {0x0F86, 0x0F87}, {0x0F8D,
  0x0F97}, {0x0F99, 0x0FBC}, {0x0FC6, 0x0FC6}, {0x102D, 0x1030}, {0x1032, 0x1037}, {0x1039, 0x103A},
  {0x103D, 0x103E}, {0x1058, 0x1059}, {0x105E, 0x1060}, {0x1071, 0x1074}, {0x1082, 0x1082}, {0x1085,
  0x1086}, {0x108D, 0x108D}, {0x109D, 0x109D}, {0x135D, 0x135F}, {0x1712, 0x1714}, {0x1732, 0x1733},
  {0x1752, 0x1753}, {0x1772, 0x1773}, {0x17B4, 0x17B5}, {0x17B7, 0x17BD}, {0x17C6, 0x17C6}, {0x17C9,
  0x17D3}, {0x17DD, 0x17DD}, {0x180B, 0x180F}, {0x1885, 0x1886}, {0x18A9, 0x18A9}, {0x1920, 0x1922},
  {0x1927, 0x1928}, {0x1932, 0x1932}, {0x1939, 0x193B}, {0x1A17, 0x1A18}, {0x1A1B, 0x1A1B}, {0x1A56,
  0x1A56}, {0x1A58, 0x1A5E}, {0x1A60, 0x1A60}, {0x1A62, 0x1A62}, {0x1A65, 0x1A6C}, {0x1A73, 0x1A7C},
  {0x1A7F, 0x1A7F}, {0x1AB0, 0x1ACE}, {0x1B00, 0x1B03}, {0x1B34, 0x1B34}, {0x1B36, 0x1B3A}, {0x1B3C,
  0x1B3C}, {0x1B42, 0x1B42}, {0x1B6B, 0x1B73}, {0x1B80, 0x1B81}, {0x1BA2, 0x1BA5}, {0x1BA8, 0x1BA9},
  {0x1BAB, 0x1BAD}, {0x1BE6, 0x1BE6}, {0x1BE8, 0x1BE9}, {0x1BED, 0x1BED}, {0x1BEF, 0x1BF1}, {0x1C2C,
  0x1C33}, {0x1C36, 0x1C37}, {0x1CD0, 0x1CD2}, {0x1CD4, 0x1CE0}, {0x1CE2, 0x1CE8}, {0x1CED, 0x1CED},
  {0x1CF4, 0x1CF4}, {0x1CF8, 0x1CF9}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060,
  0x2064}, {0x2066, 0x206F}, {0x20D0, 0x20F0}, {0x2CEF, 0x2CF1}, {0x2D7F, 0x2D7F}, {0x2DE0, 0x2DFF},
  {0x302A, 0x302D}, {0x3099, 0x309A}, {0xA66F, 0xA672}, {0xA674, 0xA67D}, {0xA69E, 0xA69F}, {0xA6F0,
  0xA6F1}, {0xA802, 0xA802}, {0xA806, 0xA806}, {0xA80B, 0xA80B}, {0xA825, 0xA826}, {0xA82C, 0xA82C},
  {0xA8C4, 0xA8C5}, {0xA8E0, 0xA8F1}, {0xA8FF, 0xA8FF}, {0xA926, 0xA92D}, {0xA947, 0xA951}, {0xA980,
  0xA982}, {0xA9B3, 0xA9B3}, {0xA9B6, 0xA9B9}, {0xA9BC, 0xA9BD}, {0xA9E5, 0xA9E5}, {0xAA29, 0xAA2E},
  {0xAA31, 0xAA32}, {0xAA35, 0xAA36}, {0xAA43, 0xAA43}, {0xAA4C, 0xAA4C}, {0xAA7C, 0xAA7C}, {0xAAB0,
  0xAAB0}, {0xAAB2, 0xAAB4}, {0xAAB7, 0xAAB8}, {0xAABE, 0xAABF}, {0xAAC1, 0xAAC1}, {0xAAEC, 0xAAED},
  {0xAAF6, 0xAAF6}, {0xABE5, 0xABE5}, {0xABE8, 0xABE8}, {0xABED, 0xABED}, {0xFB1E, 0xFB1E}, {0xFE00,
  0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xFFF9, 0xFFFB}, {0x101FD, 0x101FD}, {0x102E0,
  0x102E0}, {0x10376, 0x1037A}, {0x10A01, 0x10A03}, {0x10A05, 0x10A06}, {0x10A0C, 0x10A0F},
  {0x10A38, 0x10A3A}, {0x10A3F, 0x10A3F}, {0x10AE5, 0x10AE6}, {0x10D24, 0x10D27}, {0x10D69,
  0x10D6D}, {0x10EAB, 0x10EAC}, {0x10EFC, 0x10EFF}, {0x10F46, 0x10F50}, {0x10F82, 0x10F85},
  {0x11001, 0x11001}, {0x11038, 0x11046}, {0x11070, 0x11070}, {0x11073, 0x11074}, {0x1107F,
  0x11081}, {0x110B3, 0x110B6}, {0x110B9, 0x110BA}, {0x110BD, 0x110BD}, {0x110C2, 0x110C2},
  {0x110CD, 0x110CD}, {0x11100, 0x11102}, {0x11127, 0x1112B}, {0x1112D, 0x11134}, {0x11173,
  0x11173}, {0x11180, 0x11181}, {0x111B6, 0x111BE}, {0x111C9, 0x111CC}, {0x111CF, 0x111CF},
  {0x1122F, 0x11231}, {0x11234, 0x11234}, {0x11236, 0x11237}, {0x1123E, 0x1123E}, {0x11241,
  0x11241}, {0x112DF, 0x112DF}, {0x112E3, 0x112EA}, {0x11300, 0x11301}, {0x1133B, 0x1133C},
  {0x11340, 0x11340}, {0x11366, 0x1136C}, {0x11370, 0x11374}, {0x113BB, 0x113C0}, {0x113CE,
  0x113CE}, {0x113D0, 0x113D0}, {0x113D2, 0x113D2}, {0x113E1, 0x113E2}, {0x11438, 0x1143F},
  {0x11442, 0x11444}, {0x11446, 0x11446}, {0x1145E, 0x1145E}, {0x114B3, 0x114B8}, {0x114BA,
  0x114BA}, {0x114BF, 0x114C0}, {0x114C2, 0x114C3}, {0x115B2, 0x115B5}, {0x115BC, 0x115BD},
  {0x115BF, 0x115C0}, {0x115DC, 0x115DD}, {0x11633, 0x1163A}, {0x1163D, 0x1163D}, {0x1163F,
  0x11640}, {0x116AB, 0x116AB}, {0x116AD, 0x116AD}, {0x116B0, 0x116B5}, {0x116B7, 0x116B7},
  {0x1171D, 0x1171D}, {0x1171F, 0x1171F}, {0x11722, 0x11725}, {0x11727, 0x1172B}, {0x1182F,
  0x11837}, {0x11839, 0x1183A}, {0x1193B, 0x1193C}, {0x1193E, 0x1193E}, {0x11943, 0x11943},
  {0x119D4, 0x119D7}, {0x119DA, 0x119DB}, {0x119E0, 0x119E0}, {0x11A01, 0x11A0A}, {0x11A33,
  0x11A38}, {0x11A3B, 0x11A3E}, {0x11A47, 0x11A47}, {0x11A51, 0x11A56}, {0x11A59, 0x11A5B},
  {0x11A8A, 0x11A96}, {0x11A98, 0x11A99}, {0x11C30, 0x11C36}, {0x11C38, 0x11C3D}, {0x11C3F,
  0x11C3F}, {0x11C92, 0x11CA7}, {0x11CAA, 0x11CB0}, {0x11CB2, 0x11CB3}, {0x11CB5, 0x11CB6},
  {0x11D31, 0x11D36}, {0x11D3A, 0x11D3A}, {0x11D3C, 0x11D3D}, {0x11D3F, 0x11D45}, {0x11D47,
  0x11D47}, {0x11D90, 0x11D91}, {0x11D95, 0x11D95}, {0x11D97, 0x11D97}, {0x11EF3, 0x11EF4},
  {0x11F00, 0x11F01}, {0x11F36, 0x11F3A}, {0x11F40, 0x11F40}, {0x11F42, 0x11F42}, {0x11F5A,
  0x11F5A}, {0x13430, 0x13440}, {0x13447, 0x13455}, {0x1611E, 0x16129}, {0x1612D, 0x1612F},
  {0x16AF0, 0x16AF4}, {0x16B30, 0x16B36}, {0x16F4F, 0x16F4F}, {0x16F8F, 0x16F92}, {0x16FE4,
  0x16FE4}, {0x1BC9D, 0x1BC9E}, {0x1BCA0, 0x1BCA3}, {0x1CF00, 0x1CF2D}, {0x1CF30, 0x1CF46},
  {0x1D167, 0x1D169}, {0x1D173, 0x1D182}, {0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD}, {0x1D242,
  0x1D244}, {0x1DA00, 0x1DA36}, {0x1DA3B, 0x1DA6C}, {0x1DA75, 0x1DA75}, {0x1DA84, 0x1DA84},
  {0x1DA9B, 0x1DA9F}, {0x1DAA1, 0x1DAAF}, {0x1E000, 0x1E006}, {0x1E008, 0x1E018}, {0x1E01B,
  0x1E021}, {0x1E023, 0x1E024}, {0x1E026, 0x1E02A}, {0x1E08F, 0x1E08F}, {0x1E130, 0x1E136},
  {0x1E2AE, 0x1E2AE}, {0x1E2EC, 0x1E2EF}, {0x1E4EC, 0x1E4EF}, {0x1E5EE, 0x1E5EF}, {0x1E8D0,
  0x1E8D6}, {0x1E944, 0x1E94A}, {0xE0001, 0xE0001}, {0xE0020, 0xE007F}, {0xE0100, 0xE01EF},
}};

inline constexpr std::array<Interval, 122> wide = {{
  {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3,
  0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2630, 0x2637}, {0x2648, 0x2653}, {0x267F, 0x267F},
  {0x268A, 0x268F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4,
  0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
  {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C,
  0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0},
  {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x2E99}, {0x2E9B,
  0x2EF3}, {0x2F00, 0x2FD5}, {0x2FF0, 0x303E}, {0x3041, 0x3096}, {0x3099, 0x30FF}, {0x3105, 0x312F},
  {0x3131, 0x318E}, {0x3190, 0x31E5}, {0x31EF, 0x321E}, {0x3220, 0x3247}, {0x3250, 0xA48C}, {0xA490,
  0xA4C6}, {0xA960, 0xA97C}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE52},
  {0xFE54, 0xFE66}, {0xFE68, 0xFE6B}, {0xFF01, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4},
  {0x16FF0, 0x16FF1}, {0x17000, 0x187F7}, {0x18800, 0x18CD5}, {0x18CFF, 0x18D08}, {0x1AFF0,
  0x1AFF3}, {0x1AFF5, 0x1AFFB}, {0x1AFFD, 0x1AFFE}, {0x1B000, 0x1B122}, {0x1B132, 0x1B132},
  {0x1B150, 0x1B152}, {0x1B155, 0x1B155}, {0x1B164, 0x1B167}, {0x1B170, 0x1B2FB}, {0x1D300,
  0x1D356}, {0x1D360, 0x1D376}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
  {0x1F191, 0x1F19A}, {0x1F200, 0x1F202}, {0x1F210, 0x1F23B}, {0x1F240, 0x1F248}, {0x1F250,
  0x1F251}, {0x1F260, 0x1F265}, {0x1F300, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C},
  {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4,
  0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D},
  {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4,
  0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2},
  {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0,
  0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF},
  {0x1FA70, 0x1FA7C}, {0x1FA80, 0x1FA89}, {0x1FA8F, 0x1FAC6}, {0x1FACE, 0x1FADC}, {0x1FADF,
  0x1FAE9}, {0x1FAF0, 0x1FAF8}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
}};

inline constexpr std::array<Interval, 183> emoji_variation_base = {{
  {0x0023, 0x0023}, {0x002A, 0x002A}, {0x0030, 0x0039}, {0x00A9, 0x00A9}, {0x00AE, 0x00AE}, {0x203C,
  0x203C}, {0x2049, 0x2049}, {0x2122, 0x2122}, {0x2139, 0x2139}, {0x2194, 0x2199}, {0x21A9, 0x21AA},
  {0x231A, 0x231B}, {0x2328, 0x2328}, {0x23CF, 0x23CF}, {0x23E9, 0x23F3}, {0x23F8, 0x23FA}, {0x24C2,
  0x24C2}, {0x25AA, 0x25AB}, {0x25B6, 0x25B6}, {0x25C0, 0x25C0}, {0x25FB, 0x25FE}, {0x2600, 0x2604},
  {0x260E, 0x260E}, {0x2611, 0x2611}, {0x2614, 0x2615}, {0x2618, 0x2618}, {0x261D, 0x261D}, {0x2620,
  0x2620}, {0x2622, 0x2623}, {0x2626, 0x2626}, {0x262A, 0x262A}, {0x262E, 0x262F}, {0x2638, 0x263A},
  {0x2640, 0x2640}, {0x2642, 0x2642}, {0x2648, 0x2653}, {0x265F, 0x2660}, {0x2663, 0x2663}, {0x2665,
  0x2666}, {0x2668, 0x2668}, {0x267B, 0x267B}, {0x267E, 0x267F}, {0x2692, 0x2697}, {0x2699, 0x2699},
  {0x269B, 0x269C}, {0x26A0, 0x26A1}, {0x26A7, 0x26A7}, {0x26AA, 0x26AB}, {0x26B0, 0x26B1}, {0x26BD,
  0x26BE}, {0x26C4, 0x26C5}, {0x26C8, 0x26C8}, {0x26CE, 0x26CF}, {0x26D1, 0x26D1}, {0x26D3, 0x26D4},
  {0x26E9, 0x26EA}, {0x26F0, 0x26F5}, {0x26F7, 0x26FA}, {0x26FD, 0x26FD}, {0x2702, 0x2702}, {0x2705,
  0x2705}, {0x2708, 0x270D}, {0x270F, 0x270F}, {0x2712, 0x2712}, {0x2714, 0x2714}, {0x2716, 0x2716},
  {0x271D, 0x271D}, {0x2721, 0x2721}, {0x2728, 0x2728}, {0x2733, 0x2734}, {0x2744, 0x2744}, {0x2747,
  0x2747}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2763, 0x2764},
  {0x2795, 0x2797}, {0x27A1, 0x27A1}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2934, 0x2935}, {0x2B05,
  0x2B07}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x3030, 0x3030}, {0x303D, 0x303D},
  {0x3297, 0x3297}, {0x3299, 0x3299}, {0x1F004, 0x1F004}, {0x1F170, 0x1F171}, {0x1F17E, 0x1F17F},
  {0x1F202, 0x1F202}, {0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F}, {0x1F237, 0x1F237}, {0x1F30D,
  0x1F30F}, {0x1F315, 0x1F315}, {0x1F31C, 0x1F31C}, {0x1F321, 0x1F321}, {0x1F324, 0x1F32C},
  {0x1F336, 0x1F336}, {0x1F378, 0x1F378}, {0x1F37D, 0x1F37D}, {0x1F393, 0x1F393}, {0x1F396,
  0x1F397}, {0x1F399, 0x1F39B}, {0x1F39E, 0x1F39F}, {0x1F3A7, 0x1F3A7}, {0x1F3AC, 0x1F3AE},
  {0x1F3C2, 0x1F3C2}, {0x1F3C4, 0x1F3C4}, {0x1F3C6, 0x1F3C6}, {0x1F3CA, 0x1F3CE}, {0x1F3D4,
  0x1F3E0}, {0x1F3ED, 0x1F3ED}, {0x1F3F3, 0x1F3F3}, {0x1F3F5, 0x1F3F5}, {0x1F3F7, 0x1F3F7},
  {0x1F408, 0x1F408}, {0x1F415, 0x1F415}, {0x1F41F, 0x1F41F}, {0x1F426, 0x1F426}, {0x1F43F,
  0x1F43F}, {0x1F441, 0x1F442}, {0x1F446, 0x1F449}, {0x1F44D, 0x1F44E}, {0x1F453, 0x1F453},
  {0x1F46A, 0x1F46A}, {0x1F47D, 0x1F47D}, {0x1F4A3, 0x1F4A3}, {0x1F4B0, 0x1F4B0}, {0x1F4B3,
  0x1F4B3}, {0x1F4BB, 0x1F4BB}, {0x1F4BF, 0x1F4BF}, {0x1F4CB, 0x1F4CB}, {0x1F4DA, 0x1F4DA},
  {0x1F4DF, 0x1F4DF}, {0x1F4E4, 0x1F4E6}, {0x1F4EA, 0x1F4ED}, {0x1F4F7, 0x1F4F7}, {0x1F4F9,
  0x1F4FB}, {0x1F4FD, 0x1F4FD}, {0x1F508, 0x1F508}, {0x1F50D, 0x1F50D}, {0x1F512, 0x1F513},
  {0x1F549, 0x1F54A}, {0x1F550, 0x1F567}, {0x1F56F, 0x1F570}, {0x1F573, 0x1F579}, {0x1F587,
  0x1F587}, {0x1F58A, 0x1F58D}, {0x1F590, 0x1F590}, {0x1F5A5, 0x1F5A5}, {0x1F5A8, 0x1F5A8},
  {0x1F5B1, 0x1F5B2}, {0x1F5BC, 0x1F5BC}, {0x1F5C2, 0x1F5C4}, {0x1F5D1, 0x1F5D3}, {0x1F5DC,
  0x1F5DE}, {0x1F5E1, 0x1F5E1}, {0x1F5E3, 0x1F5E3}, {0x1F5E8, 0x1F5E8}, {0x1F5EF, 0x1F5EF},
  {0x1F5F3, 0x1F5F3}, {0x1F5FA, 0x1F5FA}, {0x1F610, 0x1F610}, {0x1F687, 0x1F687}, {0x1F68D,
  0x1F68D}, {0x1F691, 0x1F691}, {0x1F694, 0x1F694}, {0x1F698, 0x1F698}, {0x1F6AD, 0x1F6AD},
  {0x1F6B2, 0x1F6B2}, {0x1F6B9, 0x1F6BA}, {0x1F6BC, 0x1F6BC}, {0x1F6CB, 0x1F6CB}, {0x1F6CD,
  0x1F6CF}, {0x1F6E0, 0x1F6E5}, {0x1F6E9, 0x1F6E9}, {0x1F6F0, 0x1F6F0}, {0x1F6F3, 0x1F6F3},
}};

[[nodiscard]] constexpr auto of(const char32_t codepoint) noexcept -> int {
    if (codepoint < 0x20 || (codepoint >= 0x7f && codepoint <= 0x9f) ||
        in_intervals(codepoint, zero_width)) {
        return 0;
    }
    if (in_intervals(codepoint, wide)) {
        return 2;
    }
    return 1;
}

[[nodiscard]] constexpr auto of_grapheme(const std::string_view cluster) noexcept -> int {
    int width = 0;
    char32_t previous = 0;
    std::size_t regional_indicators = 0;
    bool keycap_base = false;
    for (std::size_t offset = 0; offset < cluster.size();) {
        const auto scalar = decode_utf8_scalar(cluster, offset);
        const auto cp = scalar.value;
        if ((cp == 0xFE0F && in_intervals(previous, emoji_variation_base)) ||
            (cp == 0x20E3 && keycap_base) ||
            ((cp >= 0x1F1E6 && cp <= 0x1F1FF) && ++regional_indicators == 2)) return 2;
        if (cp != 0xFE0F)
            keycap_base = cp == U'#' || cp == U'*' || (cp >= U'0' && cp <= U'9');
        const auto w = of(cp);
        if (w > width) width = w;
        previous = cp;
        offset += scalar.length;
    }
    return width;
}

/*
UNICODE LICENSE V3

COPYRIGHT AND PERMISSION NOTICE

Copyright © 1991-2026 Unicode, Inc.

NOTICE TO USER: Carefully read the following legal agreement. BY
DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR
SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE
TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.

Permission is hereby granted, free of charge, to any person obtaining a
copy of data files and any associated documentation (the "Data Files") or
software and any associated documentation (the "Software") to deal in the
Data Files or Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, and/or sell
copies of the Data Files or Software, and to permit persons to whom the
Data Files or Software are furnished to do so, provided that either (a)
this copyright and permission notice appear with all copies of the Data
Files or Software, or (b) this copyright and permission notice appear in
associated Documentation.

THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
THIRD PARTY RIGHTS.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,
OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA
FILES OR SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall
not be used in advertising or otherwise to promote the sale, use or other
dealings in these Data Files or Software without prior written
authorization of the copyright holder.
*/

constexpr bool unsafe_scalar(char32_t cp) noexcept
{
  // C0/C1 and DEL, plus explicit bidi formatting and Unicode line/paragraph
  // separators. Keep marks, ZWJ and variation selectors for ordinary text.
  return cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp == 0x061c
    || cp == 0x200e || cp == 0x200f || (cp >= 0x2028 && cp <= 0x202e)
    || (cp >= 0x2066 && cp <= 0x206f);
}
} // namespace styled_detail

struct Glyph
{
  std::size_t begin{}, end{}, width{}; // Half-open UTF-8 byte span, then cells.
};

// Start must be a known cluster boundary to segment the original text.
// At/beyond the end, return {size, size, 0} without reading input.
[[nodiscard]] inline Glyph next_glyph(std::string_view text, std::size_t start = 0) noexcept
{
  if (start >= text.size())
    return {text.size(), text.size(), 0};
  const auto end = styled_detail::next_break(text, start);
  return {start, end, static_cast<std::size_t>(
    styled_detail::of_grapheme(text.substr(start, end - start)))};
}

[[nodiscard]] inline std::vector<Glyph> glyphs(std::string_view text)
{
  std::vector<Glyph> result;
  for (std::size_t offset = 0; offset < text.size();)
    {
      const auto glyph = next_glyph(text, offset);
      result.push_back(glyph);
      offset = glyph.end;
    }
  return result;
}

// Width in terminal cells, summed over clusters; East Asian Ambiguous is
// narrow. This measures text, not ANSI streams or column positioning by tabs.
[[nodiscard]] inline std::size_t display_width(std::string_view text) noexcept
{
  std::size_t width = 0;
  for (std::size_t offset = 0; offset < text.size();)
    {
      const auto glyph = next_glyph(text, offset);
      if (glyph.width > std::numeric_limits<std::size_t>::max() - width)
        return std::numeric_limits<std::size_t>::max();
      width += glyph.width;
      offset = glyph.end;
    }
  return width;
}

// Text is always data: strip controls, and replace each malformed UTF-8
// maximal subpart with U+FFFD. Explicit layout is a separate builder operation.
[[nodiscard]] inline std::string sanitize_text(std::string_view text)
{
  std::string result;
  result.reserve(text.size());
  for (std::size_t offset = 0; offset < text.size();)
    {
      const auto scalar = styled_detail::decode_utf8_scalar(text, offset);
      if (!scalar.valid)
        result += "\xEF\xBF\xBD";
      else if (!styled_detail::unsafe_scalar(scalar.value))
        result += text.substr(offset, scalar.length);
      offset += scalar.length;
    }
  return result;
}

enum class RenderMode { true_color, color256, ansi, grayscale, monochrome, plain, debug };

struct RenderCapabilities
{
  bool true_color = true;
  bool color256 = true;
  bool ansi = true;
  bool attributes = true;
  bool cursor = true;
  bool erase = true;
  bool hyperlinks = true;
  bool title = true;
  bool raw = false;
};

struct Color
{
  // Keep the full native value until the validating builder sees it.
  std::int64_t red{}, green{}, blue{};
  bool operator==(const Color &) const = default;
};

struct Style
{
  std::optional<Color> foreground;
  std::optional<Color> background;
  bool bold{}, faint{}, italic{}, underline{}, inverse{}, strike{};
  bool operator==(const Style &) const = default;
};

namespace styled_detail
{
inline constexpr std::array<Color, 16> ansi_palette = {{
  {0, 0, 0},       {205, 49, 49},   {13, 188, 121}, {229, 229, 16},
  {36, 114, 200},  {188, 63, 188},  {17, 168, 205}, {229, 229, 229},
  {102, 102, 102}, {241, 76, 76},   {35, 209, 139}, {245, 245, 67},
  {59, 142, 234},  {214, 112, 214}, {41, 184, 219}, {255, 255, 255}
}};

inline constexpr std::array<std::string_view, 16> color_names = {{
  "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
  "bright-black", "bright-red", "bright-green", "bright-yellow",
  "bright-blue", "bright-magenta", "bright-cyan", "bright-white"
}};

constexpr Color palette_color(unsigned index) noexcept
{
  if (index < 16)
    return ansi_palette[index];
  if (index < 232)
    {
      const auto n = index - 16;
      const auto channel = [](unsigned v) { return v == 0 ? 0 : 55 + 40 * v; };
      return {channel(n / 36), channel(n / 6 % 6), channel(n % 6)};
    }
  const auto shade = 8 + (index - 232) * 10;
  return {shade, shade, shade};
}

inline void validate_color(const Color &color)
{
  if (color.red < 0 || color.red > 255 || color.green < 0 || color.green > 255
      || color.blue < 0 || color.blue > 255)
    throw std::invalid_argument("styled text RGB channel outside [0, 255]");
}

inline unsigned nearest_color(Color c, unsigned first, unsigned end) noexcept
{
  auto best_distance = std::numeric_limits<std::int64_t>::max();
  unsigned best = first;
  for (unsigned i = first; i < end; ++i)
    {
      const auto p = palette_color(i);
      const auto r = c.red - p.red, g = c.green - p.green, b = c.blue - p.blue;
      const auto distance = 30 * r * r + 59 * g * g + 11 * b * b;
      if (distance < best_distance)
        {
          best = i;
          best_distance = distance;
        }
    }
  return best;
}

inline std::string hex_color(Color c)
{
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result{"#"};
  for (const auto v : std::array{c.red, c.green, c.blue})
    {
      result += digits[static_cast<std::size_t>(v / 16)];
      result += digits[static_cast<std::size_t>(v % 16)];
    }
  return result;
}

struct Attribute
{
  std::string_view name;
  bool Style::* field;
  unsigned sgr;
};

inline constexpr std::array attributes = {
  Attribute{"bold", &Style::bold, 1}, Attribute{"faint", &Style::faint, 2},
  Attribute{"italic", &Style::italic, 3}, Attribute{"underline", &Style::underline, 4},
  Attribute{"inverse", &Style::inverse, 7}, Attribute{"strike", &Style::strike, 9}
};

inline void add_parameter(std::string &parameters, std::int64_t value)
{
  if (!parameters.empty())
    parameters += ';';
  parameters += std::to_string(value);
}

inline void add_color(std::string &parameters, Color c, bool bg, RenderMode mode)
{
  if (mode == RenderMode::ansi)
    {
      const auto index = nearest_color(c, 0, 16);
      add_parameter(parameters, (index >= 8 ? 90 : 30) + index % 8 + (bg ? 10 : 0));
      return;
    }
  add_parameter(parameters, bg ? 48 : 38);
  if (mode == RenderMode::true_color)
    {
      add_parameter(parameters, 2);
      add_parameter(parameters, c.red);
      add_parameter(parameters, c.green);
      add_parameter(parameters, c.blue);
      return;
    }
  if (mode == RenderMode::grayscale)
    {
      const auto shade = (30 * c.red + 59 * c.green + 11 * c.blue + 50) / 100;
      c = {shade, shade, shade};
    }
  add_parameter(parameters, 5);
  add_parameter(parameters, mode == RenderMode::grayscale
    ? nearest_color(c, 232, 256) : nearest_color(c, 0, 256));
}

inline std::string style_prefix(const Style &style, RenderMode mode)
{
  if (mode == RenderMode::plain)
    return {};
  std::string parameters;
  if (mode == RenderMode::debug)
    {
      if (style.foreground)
        parameters += " fg=" + hex_color(*style.foreground);
      if (style.background)
        parameters += " bg=" + hex_color(*style.background);
      for (const auto attr : attributes)
        if (style.*(attr.field))
          {
            parameters += ' ';
            parameters += attr.name;
          }
      return parameters.empty() ? std::string{} : "[style" + parameters + ']';
    }
  for (const auto attr : attributes)
    if (style.*(attr.field))
      add_parameter(parameters, attr.sgr);
  if (mode != RenderMode::monochrome)
    {
      if (style.foreground)
        add_color(parameters, *style.foreground, false, mode);
      if (style.background)
        add_color(parameters, *style.background, true, mode);
    }
  return parameters.empty() ? std::string{} : "\x1b[" + parameters + 'm';
}

inline RenderMode supported_mode(RenderMode mode, const RenderCapabilities &caps)
{
  switch (mode)
    {
    case RenderMode::true_color:
      if (caps.true_color) return mode;
      [[fallthrough]];
    case RenderMode::color256:
      if (caps.color256) return RenderMode::color256;
      [[fallthrough]];
    case RenderMode::ansi:
      if (caps.ansi) return RenderMode::ansi;
      [[fallthrough]];
    case RenderMode::monochrome:
      return RenderMode::monochrome;
    case RenderMode::grayscale:
      return caps.color256 ? mode : RenderMode::monochrome;
    case RenderMode::plain: case RenderMode::debug:
      return mode;
    }
  throw std::invalid_argument("unknown styled text render mode");
}

inline Style supported_style(Style style, RenderMode mode, const RenderCapabilities &caps)
{
  if (!caps.attributes && mode != RenderMode::debug)
    for (const auto attr : attributes)
      style.*(attr.field) = false;
  return style;
}

inline void validate_style(const Style &style)
{
  if (style.foreground) validate_color(*style.foreground);
  if (style.background) validate_color(*style.background);
}

inline Style inherit_style(Style outer, const Style &inner)
{
  if (inner.foreground) outer.foreground = inner.foreground;
  if (inner.background) outer.background = inner.background;
  for (const auto attr : attributes)
    outer.*(attr.field) = outer.*(attr.field) || inner.*(attr.field);
  return outer;
}

inline bool list_form(const SExpr &value) noexcept
{
  return !value.is_atom && !value.quoted && value.text.empty() && !value.items.empty()
    && value.items.front().is_atom && !value.items.front().quoted
    && value.items.front().items.empty();
}

inline std::string_view data_atom(const SExpr &value, bool quoted)
{
  if (!value.is_atom || value.quoted != quoted || !value.items.empty())
    throw std::invalid_argument("invalid styled text atom");
  return value.text;
}

inline std::int64_t decimal(const SExpr &value)
{
  const auto source = data_atom(value, false);
  std::int64_t result{};
  const auto parsed = std::from_chars(source.data(), source.data() + source.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != source.data() + source.size())
    throw std::invalid_argument("invalid styled text integer");
  return result;
}

inline Color parse_color(const SExpr &form)
{
  if (form.items.size() == 4)
    return {decimal(form.items[1]), decimal(form.items[2]), decimal(form.items[3])};
  if (form.items.size() != 2)
    throw std::invalid_argument("styled text color needs a name, #RRGGBB or three channels");
  const auto value = data_atom(form.items[1], false);
  if (value.size() == 7 && value.front() == '#')
    {
      std::array<unsigned, 3> channels{};
      for (std::size_t i = 0; i < channels.size(); ++i)
        {
          const auto first = value.data() + 1 + 2 * i;
          const auto parsed = std::from_chars(first, first + 2, channels[i], 16);
          if (parsed.ec != std::errc{} || parsed.ptr != first + 2)
            throw std::invalid_argument("invalid styled text hexadecimal color");
        }
      return {channels[0], channels[1], channels[2]};
    }
  for (std::size_t i = 0; i < color_names.size(); ++i)
    if (value == color_names[i])
      return ansi_palette[i];
  throw std::invalid_argument("unknown styled text color");
}

inline Style parse_style(const SExpr &form, std::size_t end)
{
  if (end - 1 > 2 + attributes.size())
    throw std::invalid_argument("too many styled text options");
  Style style;
  for (std::size_t j = 1; j < end; ++j)
    {
      const auto &option = form.items[j];
      if (!list_form(option))
        throw std::invalid_argument("invalid styled text option");
      const auto &key = option.items.front().text;
      if (key == "fg" || key == "bg")
        {
          auto &color = key == "fg" ? style.foreground : style.background;
          if (color)
            throw std::invalid_argument("duplicate styled text color");
          color = parse_color(option);
          continue;
        }
      bool found = false;
      for (const auto attr : attributes)
        if (key == attr.name && option.items.size() == 1 && !(style.*(attr.field)))
          {
            style.*(attr.field) = true;
            found = true;
            break;
          }
      if (!found)
        throw std::invalid_argument("unknown, duplicate or malformed styled text option");
    }
  return style;
}

// A preflight for native SExprs, which can bypass the reader's own limits.
inline void validate_expression(const SExpr &form, std::size_t depth,
                                std::size_t &nodes, std::size_t &bytes)
{
  if (depth == 0 || nodes == 0 || form.text.size() > bytes)
    throw std::invalid_argument("terminal expression limits exceeded");
  --nodes;
  bytes -= form.text.size();
  if (form.is_atom ? !form.items.empty() : (form.quoted || !form.text.empty()))
    throw std::invalid_argument("inconsistent terminal S-expression");
  if (form.items.size() > nodes)
    throw std::invalid_argument("terminal expression node limit exceeded");
  for (const auto &child : form.items)
    validate_expression(child, depth - 1, nodes, bytes);
}

inline void validate_expression(const SExpr &form)
{
  SExprLimits limits;
  validate_expression(form, limits.max_depth, limits.max_nodes, limits.max_bytes);
}

inline void debug_text(std::string &out, std::string_view value)
{
  constexpr std::string_view hex = "0123456789ABCDEF";
  for (const unsigned char byte : value)
    {
      if (byte == '\\' || byte == '"' || byte == '[' || byte == ']')
        {
          out += '\\';
          out += static_cast<char>(byte);
        }
      else if (byte < 0x20 || byte == 0x7f)
        {
          out += "\\x";
          out += hex[byte >> 4];
          out += hex[byte & 15];
        }
      else
        out += static_cast<char>(byte);
    }
}
} // namespace styled_detail

class StyledText
{
public:
  StyledText &text(std::string_view value) { return span(value, {}); }

  StyledText &span(std::string_view value, Style style)
  {
    styled_detail::validate_style(style);
    auto clean = sanitize_text(value);
    if (!clean.empty())
      spans_.push_back({std::move(clean), std::move(style)});
    return *this;
  }

  // Layout commands are separate from accepting text.
  StyledText &line_break()
  {
    spans_.push_back({"\n", {}});
    return *this;
  }

  StyledText &styled(Style outer, StyledText child)
  {
    styled_detail::validate_style(outer);
    for (auto &part : child.spans_)
      {
        if (part.text != "\n")
          part.style = styled_detail::inherit_style(outer, part.style);
        spans_.push_back(std::move(part));
      }
    return *this;
  }

  [[nodiscard]] std::string render(RenderMode requested = RenderMode::plain,
                                   const RenderCapabilities &caps = {}) const
  {
    const auto mode = styled_detail::supported_mode(requested, caps);
    std::string result;
    for (const auto &span : spans_)
      {
        const auto prefix = styled_detail::style_prefix(
          styled_detail::supported_style(span.style, mode, caps), mode);
        result += prefix;
        if (mode != RenderMode::debug) result += span.text;
        else if (span.text == "\n") result += "\\n";
        else styled_detail::debug_text(result, span.text);
        if (!prefix.empty())
          result += mode == RenderMode::debug ? "[/style]" : "\x1b[0m";
      }
    return result;
  }

  // Maximum line width. SGR is never included in the measurement.
  [[nodiscard]] std::size_t width() const
  {
    const auto value = render();
    std::size_t width = 0;
    for (std::size_t start = 0; start < value.size();)
      {
        const auto end = value.find('\n', start);
        width = std::max(width, display_width(std::string_view(value).substr(start, end - start)));
        if (end == std::string::npos)
          break;
        start = end + 1;
      }
    return width;
  }

  // Hard-wrap at grapheme boundaries and preserve existing explicit breaks.
  // A two-cell glyph may overrun a one-cell line instead of being split.
  [[nodiscard]] StyledText wrap(std::size_t columns) const
  {
    if (columns == 0)
      throw std::invalid_argument("styled wrapping requires positive columns");
    const auto logical = render();
    std::size_t column = 0, part = 0, used = 0;
    StyledText result;
    for (std::size_t offset = 0; offset < logical.size();)
      {
        const auto glyph = next_glyph(logical, offset);
        if (logical[offset] == '\n')
          {
            result.line_break();
            column = 0;
          }
        else
          {
            if (glyph.width != 0 && column != 0
                && (column >= columns || glyph.width > columns - column))
              {
                result.line_break();
                column = 0;
              }
            column += glyph.width;
          }
        auto remaining = glyph.end - offset;
        while (remaining != 0)
          {
            const auto &source = spans_[part];
            const auto take = std::min(remaining, source.text.size() - used);
            if (logical[offset] != '\n')
              {
                auto data = std::string_view(source.text).substr(used, take);
                if (!result.spans_.empty() && result.spans_.back().text != "\n"
                    && result.spans_.back().style == source.style)
                  result.spans_.back().text += data;
                else
                  result.spans_.push_back({std::string(data), source.style});
              }
            remaining -= take;
            used += take;
            if (used == source.text.size())
              {
                ++part;
                used = 0;
              }
          }
        offset = glyph.end;
      }
    return result;
  }

  // (styled (text "hello ") (span (fg #ff0080) (bold) "world") (line)).
  [[nodiscard]] static StyledText from_sexpr(const SExpr &form)
  {
    using namespace styled_detail;
    validate_expression(form);
    if (!list_form(form) || form.items.front().text != "styled")
      throw std::invalid_argument("expected (styled ...)");
    StyledText result;
    for (std::size_t i = 1; i < form.items.size(); ++i)
      {
        const auto &item = form.items[i];
        if (!list_form(item) || item.items.size() > 2 + 2 + attributes.size())
          throw std::invalid_argument("invalid styled text form");
        const auto &name = item.items.front().text;
        if (name == "line" && item.items.size() == 1)
          {
            result.line_break();
            continue;
          }
        Style style;
        if (name == "span" && item.items.size() >= 2)
          style = parse_style(item, item.items.size() - 1);
        else if (name != "text" || item.items.size() != 2)
          throw std::invalid_argument("unknown or malformed styled text form");
        const auto value = data_atom(item.items.back(), true);
        result.span(value, std::move(style));
      }
    return result;
  }

  [[nodiscard]] static StyledText parse(std::string_view source)
  {
    return from_sexpr(parse_bounded_sexpr(source, SExprLimits{}));
  }

private:
  struct Span { std::string text; Style style; };
  std::vector<Span> spans_;
};

namespace escape
{
namespace control
{
inline constexpr std::string_view esc = "\x1b";
inline constexpr std::string_view st = "\x1b\\";
struct LineBreak {};
}
namespace csi
{
inline constexpr std::string_view introducer = "\x1b[";
struct CursorTo { std::int64_t row = 1, column = 1; }; // One-based.
enum class Direction { up, down, right, left };
struct CursorMove { Direction direction; std::int64_t count = 1; };
enum class EraseMode { to_end, to_start, all };
enum class EraseTarget { display, line };
struct Erase { EraseTarget target; EraseMode mode; };
}
namespace sgr
{
inline constexpr std::string_view reset = "\x1b[0m";
inline constexpr unsigned foreground = 38, background = 48;
inline constexpr unsigned rgb = 2, indexed = 5;
struct Push { Style style; };
struct Pop {};
}
namespace osc
{
inline constexpr std::string_view introducer = "\x1b]";
inline constexpr unsigned window_title = 2, hyperlink = 8;
struct Title { std::string text; };
struct OpenLink { std::string uri; };
struct CloseLink {};
}
namespace dcs
{
inline constexpr std::string_view introducer = "\x1bP";
inline constexpr std::string_view terminator = control::st;
// Arbitrary DCS payloads are not supported by the textual DSL.
}
namespace extensions
{
inline constexpr unsigned bracketed_paste_mode = 2004;
inline constexpr unsigned focus_reporting_mode = 1004;
inline constexpr unsigned sgr_mouse_mode = 1006;
inline constexpr unsigned osc8_hyperlink = osc::hyperlink;
}

struct Text { std::string value; };

// Raw output is an explicit native-only opt-in, separately gated at render.
// It deliberately bypasses safe normalization and terminal-state guarantees.
enum class RawPermission { trusted_terminal_bytes };
class Raw
{
public:
  [[nodiscard]] std::string_view bytes() const noexcept { return bytes_; }
private:
  explicit Raw(std::string_view bytes) : bytes_(bytes) {}
  std::string bytes_;
  friend class Sequence;
};

using Operation = std::variant<Text, control::LineBreak, sgr::Push, sgr::Pop,
  csi::CursorTo, csi::CursorMove, csi::Erase, osc::Title, osc::OpenLink, osc::CloseLink, Raw>;

class Sequence
{
  // Bounded before allocation, recursion, or indexing; independent of host
  // terminal parser's parameter limits.
  static constexpr std::int64_t max_parameter = 32767;

  static std::int64_t parameter(std::int64_t n)
  {
    if (n < 1 || n > max_parameter)
      throw std::invalid_argument("terminal cursor parameter out of range");
    return n;
  }
  static char movement(csi::Direction value)
  {
    switch (value)
      {
      case csi::Direction::up: return 'A';
      case csi::Direction::down: return 'B';
      case csi::Direction::right: return 'C';
      case csi::Direction::left: return 'D';
      }
    throw std::invalid_argument("unknown terminal cursor direction");
  }
  static char erase_target(csi::EraseTarget value)
  {
    switch (value)
      {
      case csi::EraseTarget::display: return 'J';
      case csi::EraseTarget::line: return 'K';
      }
    throw std::invalid_argument("unknown terminal erase target");
  }
  static char erase_mode(csi::EraseMode value)
  {
    switch (value)
      {
      case csi::EraseMode::to_end: return '0';
      case csi::EraseMode::to_start: return '1';
      case csi::EraseMode::all: return '2';
      }
    throw std::invalid_argument("unknown terminal erase mode");
  }
  static void validate_uri(std::string_view uri)
  {
    // Accept only web destinations, not application/file/shell handlers.
    const std::size_t scheme = uri.starts_with("https://") ? 8
      : uri.starts_with("http://") ? 7 : 0;
    if (scheme == 0 || uri.size() > 4096 || uri.size() <= scheme
        || uri.find_first_of(" /?#\\", scheme) == scheme
        || uri.find_first_of(" \\") != std::string_view::npos || sanitize_text(uri) != uri)
      throw std::invalid_argument("invalid terminal hyperlink URI");
  }

  template <class T> Sequence &add(T value)
  {
    ops_.emplace_back(std::move(value));
    return *this;
  }

public:
  Sequence &text(std::string_view value)
  {
    auto clean = sanitize_text(value);
    if (!clean.empty()) add(Text{std::move(clean)});
    return *this;
  }
  Sequence &line_break() { return add(control::LineBreak{}); }
  Sequence &cursor_to(std::int64_t row, std::int64_t column)
  {
    return add(csi::CursorTo{parameter(row), parameter(column)});
  }
  Sequence &cursor_move(csi::Direction direction, std::int64_t count = 1)
  {
    (void)movement(direction);
    return add(csi::CursorMove{direction, parameter(count)});
  }
  Sequence &erase(csi::EraseTarget target, csi::EraseMode mode)
  {
    (void)erase_target(target);
    (void)erase_mode(mode);
    return add(csi::Erase{target, mode});
  }
  Sequence &title(std::string_view value)
  {
    if (value.size() > 4096)
      throw std::invalid_argument("terminal title too long");
    return add(osc::Title{sanitize_text(value)});
  }
  Sequence &raw(std::string_view value, RawPermission permission)
  {
    if (permission != RawPermission::trusted_terminal_bytes)
      throw std::invalid_argument("raw escape emission requires explicit permission");
    if (value.size() > 65536)
      throw std::invalid_argument("raw terminal escape too long");
    return add(Raw(value));
  }
  Sequence &then(Sequence other)
  {
    prepare(other.ops_.size());
    for (auto &op : other.ops_)
      ops_.push_back(std::move(op));
    return *this;
  }
  Sequence &styled(Style style, Sequence body)
  {
    styled_detail::validate_style(style);
    return scope(sgr::Push{std::move(style)}, std::move(body), sgr::Pop{});
  }
  Sequence &hyperlink(std::string_view uri, Sequence body)
  {
    validate_uri(uri);
    return scope(osc::OpenLink{std::string(uri)}, std::move(body), osc::CloseLink{});
  }

  [[nodiscard]] const std::vector<Operation> &operations() const noexcept { return ops_; }

  template <class Visitor> void visit(Visitor &&visitor) const
  {
    for (const auto &op : ops_)
      std::visit(visitor, op);
  }

  // Merge adjacent text only; controls and scope boundaries remain ordered.
  [[nodiscard]] Sequence normalized() const
  {
    Sequence result;
    for (const auto &op : ops_)
      {
        if (const auto *text = std::get_if<Text>(&op); text && !result.ops_.empty())
          if (auto *previous = std::get_if<Text>(&result.ops_.back()))
            {
              previous->value += text->value;
              continue;
            }
        result.ops_.push_back(op);
      }
    return result;
  }

  // Maximum logical line width. Typed terminal actions occupy no text cells;
  // cursor operations and raw output are not simulated.
  [[nodiscard]] std::size_t width() const
  {
    std::size_t width = 0;
    std::string line;
    for (const auto &op : ops_)
      {
        if (const auto *text = std::get_if<Text>(&op))
          line += text->value;
        else if (std::holds_alternative<control::LineBreak>(op))
          {
            width = std::max(width, display_width(line));
            line.clear();
          }
      }
    return std::max(width, display_width(line));
  }

  [[nodiscard]] std::string render(RenderMode requested = RenderMode::plain,
                                   const RenderCapabilities &caps = {}) const
  {
    using namespace styled_detail;
    const auto mode = supported_mode(requested, caps);
    const bool debug = mode == RenderMode::debug;
    const bool terminal_output = !debug && mode != RenderMode::plain;
    std::vector<Style> styles;
    std::vector<std::string_view> links;
    std::string out;
    const auto osc_command = [&](unsigned command, std::string_view payload) {
      out += osc::introducer;
      out += std::to_string(command);
      out += ';';
      out += payload;
      out += control::st;
    };
    const auto write_link = [&](std::string_view uri) {
      osc_command(osc::hyperlink, ";" + std::string(uri));
    };
    const auto show = [&](std::string_view name, std::string_view text) {
      out += '[';
      out += name;
      out += " \"";
      debug_text(out, text);
      out += "\"]";
    };
    visit([&]<class T>(const T &op) {
      if constexpr (std::is_same_v<T, Text>)
        {
          if (debug) debug_text(out, op.value);
          else out += op.value;
        }
      else if constexpr (std::is_same_v<T, control::LineBreak>)
        out += debug ? "\\n" : "\n";
      else if constexpr (std::is_same_v<T, sgr::Push>)
        {
          const Style previous = styles.empty() ? Style{} : styles.back();
          const auto current = inherit_style(previous, op.style);
          styles.push_back(current);
          const auto prefix = style_prefix(supported_style(current, mode, caps), mode);
          if (debug) out += prefix.empty() ? "[style]" : prefix;
          else if (terminal_output && current != previous) out += prefix;
        }
      else if constexpr (std::is_same_v<T, sgr::Pop>)
        {
          const auto current = styles.back();
          styles.pop_back();
          const Style previous = styles.empty() ? Style{} : styles.back();
          if (debug) out += "[/style]";
          else if (terminal_output && current != previous
                   && !style_prefix(supported_style(current, mode, caps), mode).empty())
            {
              out += sgr::reset;
              out += style_prefix(supported_style(previous, mode, caps), mode);
            }
        }
      else if constexpr (std::is_same_v<T, osc::OpenLink>)
        {
          links.push_back(op.uri);
          if (debug) show("link", op.uri);
          else if (terminal_output && caps.hyperlinks) write_link(op.uri);
        }
      else if constexpr (std::is_same_v<T, osc::CloseLink>)
        {
          links.pop_back();
          if (debug) out += "[/link]";
          else if (terminal_output && caps.hyperlinks)
            write_link(links.empty() ? std::string_view{} : links.back());
        }
      else if constexpr (std::is_same_v<T, osc::Title>)
        {
          if (debug) show("title", op.text);
          else if (terminal_output && caps.title) osc_command(osc::window_title, op.text);
        }
      else if constexpr (std::is_same_v<T, csi::CursorTo>)
        {
          const auto coordinates = std::to_string(op.row) + ';' + std::to_string(op.column);
          if (debug) show("cursor", coordinates);
          else if (terminal_output && caps.cursor)
            { out += csi::introducer; out += coordinates; out += 'H'; }
        }
      else if constexpr (std::is_same_v<T, csi::CursorMove>)
        {
          const auto code = std::to_string(op.count) + movement(op.direction);
          if (debug) show("move", code);
          else if (terminal_output && caps.cursor) { out += csi::introducer; out += code; }
        }
      else if constexpr (std::is_same_v<T, csi::Erase>)
        {
          const std::string code{erase_mode(op.mode), erase_target(op.target)};
          if (debug) show("erase", code);
          else if (terminal_output && caps.erase) { out += csi::introducer; out += code; }
        }
      else if constexpr (std::is_same_v<T, Raw>)
        {
          if (debug)
            {
              // Raw may be arbitrary invalid UTF-8 or contain high C1 bytes.
              constexpr std::string_view digits = "0123456789ABCDEF";
              out += "[raw";
              for (const unsigned char byte : op.bytes())
                {
                  out += "\\x";
                  out += digits[byte >> 4];
                  out += digits[byte & 15];
                }
              out += ']';
            }
          else if (terminal_output)
            {
              if (!caps.raw)
                throw std::invalid_argument("raw terminal output not enabled");
              out += op.bytes();
            }
        }
    });
    return out;
  }

  // (seq (style (bold) (fg #ff0000) (seq (text "hi") (line)))
  //      (cursor 1 2) (move up 2) (erase line all)
  //      (link "https://example.com" (text "site")) (title "window"))
  [[nodiscard]] static Sequence from_sexpr(const SExpr &form)
  {
    styled_detail::validate_expression(form);
    Sequence result;
    parse_form(form, result);
    return result;
  }

  [[nodiscard]] static Sequence parse(std::string_view source)
  {
    return from_sexpr(parse_bounded_sexpr(source, SExprLimits{}));
  }

private:
  void prepare(std::size_t extra)
  {
    const auto maximum = ops_.max_size();
    if (extra > maximum - ops_.size())
      throw std::length_error("terminal sequence is too large");
    const auto required = ops_.size() + extra;
    const auto capacity = ops_.capacity();
    if (required > capacity)
      ops_.reserve(std::max(required, capacity > maximum / 2 ? maximum : capacity * 2));
  }

  template <class First, class Last> Sequence &scope(First first, Sequence body, Last last)
  {
    if (ops_.max_size() < 2 || body.ops_.size() > ops_.max_size() - 2)
      throw std::length_error("terminal sequence is too large");
    prepare(body.ops_.size() + 2);
    add(std::move(first));
    then(std::move(body));
    return add(std::move(last));
  }

  static void parse_form(const SExpr &form, Sequence &out)
  {
    using namespace styled_detail;
    if (!list_form(form))
      throw std::invalid_argument("expected terminal operation form");
    const auto &name = form.items.front().text;
    const auto size = form.items.size();
    if (name == "seq")
      {
        for (std::size_t i = 1; i < size; ++i)
          parse_form(form.items[i], out);
      }
    else if (name == "text" && size == 2) out.text(data_atom(form.items[1], true));
    else if (name == "line" && size == 1) out.line_break();
    else if (name == "cursor" && size == 3)
      out.cursor_to(decimal(form.items[1]), decimal(form.items[2]));
    else if (name == "move" && size == 3)
      {
        constexpr std::array<std::string_view, 4> names = {"up", "down", "right", "left"};
        const auto key = data_atom(form.items[1], false);
        const auto found = std::find(names.begin(), names.end(), key);
        if (found == names.end())
          throw std::invalid_argument("unknown terminal cursor direction");
        out.cursor_move(static_cast<csi::Direction>(found - names.begin()), decimal(form.items[2]));
      }
    else if (name == "erase" && size == 3)
      {
        const auto target = data_atom(form.items[1], false);
        const auto mode = data_atom(form.items[2], false);
        if ((target != "display" && target != "line")
            || (mode != "all" && mode != "to-start" && mode != "to-end"))
          throw std::invalid_argument("unknown terminal erase target or mode");
        out.erase(target == "display" ? csi::EraseTarget::display : csi::EraseTarget::line,
          mode == "all" ? csi::EraseMode::all : (mode == "to-start"
            ? csi::EraseMode::to_start : csi::EraseMode::to_end));
      }
    else if (name == "title" && size == 2) out.title(data_atom(form.items[1], true));
    else if (name == "style" && size >= 2)
      {
        const auto style = parse_style(form, size - 1);
        Sequence body;
        parse_form(form.items.back(), body);
        out.styled(style, std::move(body));
      }
    else if (name == "link" && size == 3)
      {
        auto uri = data_atom(form.items[1], true);
        Sequence body;
        parse_form(form.items[2], body);
        out.hyperlink(uri, std::move(body));
      }
    else
      throw std::invalid_argument("unknown or malformed terminal operation");
  }

  std::vector<Operation> ops_;
};
} // namespace escape
} // namespace terminal

struct GlobalEscapeCodes
{
  template <class Derived> struct Mixin
  {
    using StyledText = terminal::StyledText;
    using RenderMode = terminal::RenderMode;
    using EscapeSequence = terminal::escape::Sequence;
    using TextColor = terminal::Color;
    using TextStyle = terminal::Style;

    [[nodiscard]] static StyledText styled_text() { return {}; }
    [[nodiscard]] static StyledText styled_text(std::string_view source)
    {
      return StyledText::parse(source);
    }
    [[nodiscard]] static std::string render_styled_text(
      const StyledText &value, RenderMode mode = RenderMode::plain)
    {
      return value.render(mode);
    }
    [[nodiscard]] static EscapeSequence escape_sequence() { return {}; }
    [[nodiscard]] static EscapeSequence escape_sequence(std::string_view source)
    {
      return EscapeSequence::parse(source);
    }
  };
};

// ============================================================
//  Hierarchical option parsing
// ============================================================

namespace cli
{
enum class ValueKind { string, integer, real, boolean };
using Value = std::variant<std::string, std::int64_t, double, bool>;
using ValuePredicate = std::function<bool (const Value &)>;
using Environment = std::function<std::optional<std::string> (std::string_view)>;
struct ParseResult;
using Handler = std::function<void (const ParseResult &)>;
using Handlers = std::unordered_map<std::string, Handler>;

struct Cardinality
{
  std::size_t minimum = 0;
  std::optional<std::size_t> maximum;
};

// Bounds use the same decimal representation as the corresponding CLI type.
struct NumericRange
{
  std::optional<std::string> lower;
  std::optional<std::string> upper;
};

struct Dialect
{
  bool cluster_short = true;
  bool attached_short = true;
  bool long_equals = true;
  bool options_after_positionals = true;
  bool negative_numbers = true;
};

struct Option
{
  std::string name;
  char short_name = 0;
  std::string description;
  bool takes_value = false;
  bool required = false;
  bool repeatable = false;
  std::optional<std::string> default_value;
  std::vector<std::string> aliases;
  std::vector<char> short_aliases;
  bool negatable = false;
  ValueKind kind = ValueKind::string;
  std::vector<std::string> choices;
  ValuePredicate predicate;
  Cardinality count;
  std::vector<std::string> dependencies;
  std::optional<std::string> environment;
  std::optional<NumericRange> range;
};

struct Positional
{
  std::string name;
  std::string description;
  bool required = true;
  bool repeatable = false;
  ValueKind kind = ValueKind::string;
  std::vector<std::string> choices;
  ValuePredicate predicate;
  Cardinality count;
  std::optional<NumericRange> range;
};

struct OptionGroup
{
  std::string name;
  std::vector<std::string> members;
  Cardinality count;
};

struct Command
{
  std::string name;
  std::string description;
  std::vector<Option> options;
  std::vector<Command> subcommands;
  std::vector<Positional> positionals;
  bool require_subcommand = false;
  std::vector<std::string> aliases;
  std::vector<OptionGroup> groups;
  std::optional<Dialect> dialect;
  std::string handler_name;
  Handler handler;
};

enum class ErrorCode
{
  unknown_option, unknown_command, unexpected_value, missing_value,
  duplicate_option, invalid_value, missing_option, missing_command,
  missing_argument, unexpected_argument, cardinality, dependency, group,
  dialect, handler
};

struct ParseError
{
  ErrorCode code;
  // Absent for constraints involving the completed invocation rather than a
  // single token. Index zero denotes the first argument, not argv[0] in main.
  std::optional<std::size_t> argv_index;
  std::string message;
};

struct ParseResult
{
  std::vector<std::string> command_path;
  std::vector<std::string> positionals;
  std::vector<std::string> errors;
  std::unordered_map<std::string, std::vector<std::string>> options;
  bool help_requested = false;
  std::unordered_map<std::string, std::vector<std::string>> arguments;
  std::unordered_map<std::string, std::vector<Value>> typed_options;
  std::unordered_map<std::string, std::vector<Value>> typed_arguments;
  std::unordered_map<std::string, std::size_t> occurrences;
  std::vector<ParseError> diagnostics;
  // Retained for completion; set only when the final token lacks its operand.
  std::optional<std::string> pending_option;
  bool options_enabled = true;

  explicit operator bool () const noexcept { return errors.empty (); }

  bool has (std::string_view name) const
  {
    return options.contains (std::string (name));
  }

  // A flag is present with no values; for repeatable options return the last
  // value. The view remains valid only while its result/string is unchanged.
  std::optional<std::string_view> value (std::string_view name) const &
  {
    auto it = options.find (std::string (name));
    if (it == options.end () || it->second.empty ())
      return std::nullopt;
    return it->second.back ();
  }
  std::optional<std::string_view> value (std::string_view) const && = delete;

  template <class T> const T *get (std::string_view name) const &
  {
    const auto option = typed_options.find (std::string (name));
    if (option == typed_options.end () || option->second.empty ())
      return nullptr;
    return std::get_if<T> (&option->second.back ());
  }
  template <class T> const T *get (std::string_view) const && = delete;

  template <class T> const T *get_argument (std::string_view name, std::size_t index = 0) const &
  {
    const auto argument = typed_arguments.find (std::string (name));
    return argument != typed_arguments.end () && index < argument->second.size ()
               ? std::get_if<T> (&argument->second[index]) : nullptr;
  }
  template <class T>
  const T *get_argument (std::string_view, std::size_t = 0) const && = delete;
};

class OptParser
{
public:
  explicit OptParser (const Command &root) : root_ (checked (root)) {}
  explicit OptParser (Command &&root) : root_ (std::move (checked (root))) {}

  static OptParser from_sexpr (const SExpr &expression)
  {
    const SExprLimits limits;
    auto nodes = limits.max_nodes;
    auto bytes = limits.max_bytes;
    validate_form (expression, 0, limits.max_depth, nodes, bytes);
    std::size_t entries = 0;
    return OptParser (read_command (expression, 0, entries));
  }

  static OptParser from_text (std::string_view source)
  {
    return from_sexpr (parse_bounded_sexpr (source, SExprLimits{}));
  }

  const Command &root () const & noexcept { return root_; }
  const Command &root () const && = delete;

  // argv excludes the program name. Ancestor options are inherited. Before any
  // positional a known child wins over a positional of the same spelling;
  // use `--` to disable options and subcommand lookup. Use attached syntax for
  // option operands resembling options. Environment defaults precede literals.
  // `required` checks effective presence; count constraints, groups, and
  // dependencies count explicit spellings, including negated switches.
  ParseResult parse (const std::vector<std::string> &argv,
                     const Environment &environment = {}) const
  {
    return parse_impl (argv, environment);
  }

private:
  ParseResult parse_impl (const std::vector<std::string> &argv,
                          const Environment &environment,
                          bool prefix = false) const
  {
    ParseResult out;
    std::vector<const Command *> scope{ &root_ };
    bool options_enabled = true;
    bool have_positional = false;
    const Dialect dialect = root_.dialect.value_or (Dialect{});
    std::vector<std::size_t> positional_indices;
    for (std::size_t i = 0; i < argv.size (); ++i)
      {
        const std::string_view arg = argv[i];
        if (options_enabled && arg == "--")
          options_enabled = false;
        else if (options_enabled && arg.starts_with ("--"))
          {
            const auto equal = arg.find ('=');
            const auto name = arg.substr (2, equal == arg.npos ? arg.npos
                                                              : equal - 2);
            const auto attached = equal == arg.npos
                                      ? std::nullopt
                                      : std::optional{ arg.substr (equal + 1) };
            if (attached && !dialect.long_equals)
              error (out, ErrorCode::dialect, "attached long option operands are disabled", i);
            else if (name == "help" && !attached)
              out.help_requested = true;
            else if (name == "help")
              error (out, ErrorCode::unexpected_value, "option '--help' takes no value", i);
            else
              {
                bool negated = false;
                auto *option = find_option (scope, name, 0);
                if (!option && name.starts_with ("no-"))
                  {
                    option = find_option (scope, name.substr (3), 0);
                    if (option && option->negatable)
                      negated = true;
                    else
                      option = nullptr;
                  }
                if (option)
                  consume (*option, attached, argv, i, dialect, out, negated, prefix);
                else
                  error (out, ErrorCode::unknown_option,
                         "unknown option '--" + safe (name) + "'", i);
              }
          }
        else if (options_enabled && resembles_option (arg, dialect))
          {
            for (std::size_t j = 1; j < arg.size (); ++j)
              {
                char name = arg[j];
                if (name == 'h')
                  {
                    if (j + 1 < arg.size () && arg[j + 1] == '=')
                      {
                        error (out, ErrorCode::unexpected_value,
                               "option '-h' takes no value", i);
                        break;
                      }
                    if (!dialect.cluster_short && arg.size () > 2)
                      {
                        error (out, ErrorCode::dialect, "short option clusters are disabled", i);
                        break;
                      }
                    out.help_requested = true;
                  }
                else if (auto *option = find_option (scope, {}, name))
                  {
                    auto rest = arg.substr (j + 1);
                    if (option->takes_value || rest.starts_with ('='))
                      {
                        std::optional<std::string_view> attached;
                        if (!rest.empty ())
                          attached = rest.starts_with ('=') ? rest.substr (1) : rest;
                        if (attached && !dialect.attached_short && option->takes_value)
                          error (out, ErrorCode::dialect,
                                 "attached short option operands are disabled", i);
                        else
                          consume (*option, attached, argv, i, dialect, out, false, prefix);
                        break;
                      }
                    if (!dialect.cluster_short && arg.size () > 2)
                      {
                        error (out, ErrorCode::dialect, "short option clusters are disabled", i);
                        break;
                      }
                    consume (*option, std::nullopt, argv, i, dialect, out, false, prefix);
                  }
                else
                  {
                    error (out, ErrorCode::unknown_option,
                           "unknown short option '-" + safe (arg.substr (j, 1)) + "'", i);
                    break;
                  }
              }
          }
        else if (options_enabled && !have_positional)
          {
            if (auto *child = find_command (*scope.back (), arg))
              {
                scope.push_back (child);
                out.command_path.push_back (child->name);
              }
            else if (!scope.back ()->subcommands.empty ()
                     && scope.back ()->positionals.empty ())
              {
                error (out, ErrorCode::unknown_command,
                       "unknown subcommand '" + safe (arg) + "'", i);
                break;
              }
            else
              {
                have_positional = true;
                out.positionals.emplace_back (arg);
                positional_indices.push_back (i);
                if (!dialect.options_after_positionals)
                  options_enabled = false;
              }
          }
        else
          {
            out.positionals.emplace_back (arg);
            positional_indices.push_back (i);
          }
      }

    out.options_enabled = options_enabled;
    if (prefix)
      return out;
    const auto &command = *scope.back ();
    if (!out.help_requested && command.require_subcommand)
      error (out, ErrorCode::missing_command,
             "missing subcommand for '" + command.name + "'");
    for (const auto *level : scope)
      for (const auto &option : level->options)
        if (occurrences (out, option.name) == 0)
          {
            std::optional<std::string> value;
            if (option.environment && !out.help_requested)
              value = environment ? environment (*option.environment)
                                  : read_environment (*option.environment);
            if (!value)
              value = option.default_value;
            if (value)
              store (option, *value, out, std::nullopt);
            if (option.required && !out.has (option.name) && !out.help_requested)
              error (out, ErrorCode::missing_option,
                     "missing required option '--" + option.name + "'");
          }

    std::size_t next = 0;
    for (const auto &pos : command.positionals)
      {
        std::size_t value_count = 0;
        auto limits = cardinality (pos);
        while (next < out.positionals.size ()
               && (!limits.maximum || value_count < *limits.maximum))
          {
            const auto &raw = out.positionals[next];
            out.arguments[pos.name].push_back (raw);
            auto value = converted (pos.kind, pos.choices, pos.predicate, raw, pos.range);
            if (value)
              out.typed_arguments[pos.name].push_back (std::move (*value));
            else
              error (out, ErrorCode::invalid_value,
                     "invalid value for positional argument '" + pos.name + "'",
                     positional_indices[next]);
            ++next;
            ++value_count;
          }
        if (value_count < limits.minimum && !out.help_requested)
          error (out, ErrorCode::missing_argument,
                 "too few values for positional argument '" + pos.name + "'");
      }
    if (next < out.positionals.size ())
      error (out, ErrorCode::unexpected_argument,
             "unexpected positional argument '" + safe (out.positionals[next]) + "'",
             positional_indices[next]);
    if (!out.help_requested)
      check_constraints (scope, out);
    return out;
  }

public:
  // Analyze complete tokens and return candidates for the next, partial token.
  // No environment reads, predicates, or handlers execute during completion.
  std::vector<std::string> complete (const std::vector<std::string> &argv,
                                     std::string_view partial = {}) const
  {
    const auto parsed = parse_impl (argv, {}, true);
    if (!parsed)
      return {};
    auto scope = resolve (parsed.command_path);
    const auto &command = *scope.back ();
    std::vector<std::string> results;
    const auto add = [&] (std::string suggestion)
    {
      if (std::string_view (suggestion).starts_with (partial))
        results.push_back (std::move (suggestion));
    };
    if (parsed.pending_option)
      {
        const auto *option = find_option (scope, *parsed.pending_option, 0);
        for (const auto &choice : option->choices)
          add (choice);
      }
    else if (parsed.options_enabled && partial.starts_with ('-'))
      {
        const auto equals = partial.find ('=');
        if (partial.starts_with ("--") && equals != partial.npos
            && root_.dialect.value_or (Dialect{}).long_equals)
          {
            auto *option = find_option (scope, partial.substr (2, equals - 2), 0);
            if (option && option->takes_value)
              for (const auto &choice : option->choices)
                add (std::string (partial.substr (0, equals + 1)) + choice);
          }
        else
          {
            add ("--help");
            add ("-h");
            for (const auto *level : scope)
              for (const auto &option : level->options)
                {
                  const auto count = occurrences (parsed, option.name);
                  if ((count && !option.repeatable)
                      || (option.count.maximum && count >= *option.count.maximum))
                    continue;
                  const auto long_name = [&] (const std::string &name)
                  {
                    add ("--" + name);
                    if (option.negatable)
                      add ("--no-" + name);
                  };
                  long_name (option.name);
                  for (const auto &alias : option.aliases)
                    long_name (alias);
                  if (option.short_name)
                    add (std::string ("-") + option.short_name);
                  for (char alias : option.short_aliases)
                    add (std::string ("-") + alias);
                }
          }
      }
    else
      {
        if (parsed.options_enabled && parsed.positionals.empty ())
          for (const auto &child : command.subcommands)
            {
              add (child.name);
              for (const auto &alias : child.aliases)
                add (alias);
            }
        auto remaining = parsed.positionals.size ();
        for (const auto &pos : command.positionals)
          {
            auto limits = cardinality (pos);
            if (!limits.maximum || remaining < *limits.maximum)
              {
                for (const auto &choice : pos.choices)
                  add (choice);
                break;
              }
            remaining -= *limits.maximum;
          }
      }
    std::sort (results.begin (), results.end ());
    results.erase (std::unique (results.begin (), results.end ()), results.end ());
    return results;
  }

  // Text specifications name, but cannot supply or execute, a native handler.
  // Only the selected command's explicitly installed handler is called.
  ParseResult dispatch (const std::vector<std::string> &argv,
                        const Handlers &handlers = {},
                        const Environment &environment = {}) const
  {
    auto result = parse (argv, environment);
    if (!result || result.help_requested)
      return result;
    const auto &command = *resolve (result.command_path).back ();
    if (command.handler)
      command.handler (result);
    else
      {
        auto found = handlers.find (command.handler_name);
        if (command.handler_name.empty () || found == handlers.end () || !found->second)
          error (result, ErrorCode::handler, "no handler installed for '" + command.name + "'");
        else
          found->second (result);
      }
    return result;
  }

  // Help is formatted from the same command tree used for parsing. Text from
  // the specification is rendered as data; only structural newlines are raw.
  std::string help (const std::vector<std::string> &command_path = {},
                    terminal::RenderMode mode = terminal::RenderMode::plain,
                    std::size_t columns = 80) const
  {
    const auto scope = resolve (command_path);
    columns = std::max<std::size_t> (24, columns);
    const auto &command = *scope.back ();
    std::string usage = "Usage:";
    for (const auto *level : scope)
      usage += " " + level->name;
    usage += " [options]";
    if (!command.subcommands.empty ())
      usage += command.require_subcommand ? " <command>" : " [command]";
    for (const auto &pos : command.positionals)
      {
        usage += pos.required ? " <" : " [";
        usage += pos.name;
        if (pos.repeatable)
          usage += "...";
        usage += pos.required ? '>' : ']';
      }
    terminal::StyledText out;
    terminal::Style emphasis;
    emphasis.bold = true;
    emphasis.foreground = terminal::Color{ 63, 181, 219 };
    wrap (out, usage, columns, {}, emphasis);
    if (!command.description.empty ())
      {
        out.line_break ();
        wrap (out, command.description, columns);
      }

    using Row = std::pair<std::string, std::string>;
    const auto section = [&] (std::string_view heading, const std::vector<Row> &rows)
    {
      if (rows.empty ())
        return;
      out.line_break ().span (heading, emphasis).line_break ();
      std::size_t width = 0;
      for (const auto &[label, description] : rows)
        width = std::max (width, terminal::display_width (label));
      for (const auto &[label, description] : rows)
        {
          if (description.empty () || width > columns / 2)
            {
              out.text ("  ");
              wrap (out, label, columns - 2, "  ");
              if (!description.empty ())
                {
                  out.text ("    ");
                  wrap (out, description, columns - 4, "    ");
                }
            }
          else
            {
              out.text ("  ").text (label)
                 .text (std::string (width - terminal::display_width (label), ' ')).text ("  ");
              wrap (out, description, columns - width - 4, std::string (width + 4, ' '));
            }
        }
    };

    std::vector<Row> options{ { "-h, --help", "Show this help" } };
    for (const auto *level : scope)
      for (const auto &option : level->options)
        {
          std::string label;
          if (option.short_name != 0)
            label = std::string ("-") + option.short_name + ", ";
          for (char alias : option.short_aliases)
            label += std::string ("-") + alias + ", ";
          label += "--" + option.name;
          if (option.negatable)
            label += " / --no-" + option.name;
          for (const auto &alias : option.aliases)
            label += ", --" + alias;
          if (option.takes_value)
            label += " <" + std::string (kind_name (option.kind)) + ">";
          auto description = option.description;
          if (option.required)
            description += " (required)";
          if (option.repeatable)
            description += " (repeatable)";
          if (option.default_value)
            description += " (default: " + *option.default_value + ")";
          if (option.environment)
            description += " (environment: " + *option.environment + ")";
          if (!option.dependencies.empty ())
            {
              description += " (requires:";
              for (const auto &name : option.dependencies)
                description += " --" + name;
              description += ")";
            }
          options.emplace_back (std::move (label), std::move (description));
        }
    section ("Options:", options);
    std::vector<Row> groups;
    for (const auto *level : scope)
      for (const auto &group : level->groups)
        {
          std::string description = std::to_string (group.count.minimum) + ".."
                                    + (group.count.maximum
                                         ? std::to_string (*group.count.maximum) : "*")
                                    + " of:";
          for (const auto &name : group.members)
            description += " --" + name;
          groups.emplace_back (group.name, std::move (description));
        }
    section ("Option groups:", groups);
    std::vector<Row> commands;
    for (const auto &child : command.subcommands)
      {
        auto label = child.name;
        for (const auto &alias : child.aliases)
          label += ", " + alias;
        commands.emplace_back (std::move (label), child.description);
      }
    section ("Commands:", commands);
    std::vector<Row> positionals;
    for (const auto &pos : command.positionals)
      positionals.emplace_back (pos.name, pos.description);
    section ("Arguments:", positionals);
    return out.render (mode);
  }

  std::string help_tree (terminal::RenderMode mode = terminal::RenderMode::plain,
                         std::size_t columns = 80) const
  {
    std::string out;
    std::vector<std::string> path;
    const auto visit = [&] (auto &self, const Command &node) -> void
    {
      if (!out.empty ())
        out += '\n';
      out += help (path, mode, columns);
      for (const auto &child : node.subcommands)
        {
          path.push_back (child.name);
          self (self, child);
          path.pop_back ();
        }
    };
    visit (visit, root_);
    return out;
  }

private:
  static constexpr std::size_t max_depth = 64;
  static constexpr std::size_t max_entries = 65536;
  Command root_;

  std::vector<const Command *> resolve (const std::vector<std::string> &path) const
  {
    std::vector<const Command *> scope{ &root_ };
    for (const auto &part : path)
      {
        auto *child = find_command (*scope.back (), part);
        if (!child)
          fail ("unknown subcommand path component '" + safe (part) + "'");
        scope.push_back (child);
      }
    return scope;
  }

  static void wrap (terminal::StyledText &out, std::string_view raw, std::size_t width,
                    std::string_view indent = {}, terminal::Style style = {})
  {
    const auto clean = safe (raw);
    std::string_view text (clean);
    while (!text.empty ())
      {
        std::size_t offset = 0;
        std::size_t used = 0;
        std::size_t space = text.npos;
        while (offset < text.size ())
          {
            auto glyph = terminal::next_glyph (text, offset);
            if (glyph.width > width - used)
              break;
            if (text[offset] == ' ')
              space = offset;
            used += glyph.width;
            offset = glyph.end;
          }
        if (!offset)
          offset = terminal::next_glyph (text).end;
        else if (offset < text.size () && space != text.npos && space != 0
                 && text[offset] != ' ')
          offset = space;
        out.span (text.substr (0, offset), style).line_break ();
        text.remove_prefix (offset);
        while (!text.empty () && text.front () == ' ')
          text.remove_prefix (1);
        if (!text.empty ())
          out.text (indent);
      }
    if (clean.empty ())
      out.line_break ();
  }

  [[noreturn]] static void fail (std::string message)
  {
    throw std::invalid_argument ("OptParser: " + std::move (message));
  }

  static bool alpha (unsigned char c)
  {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  }

  static bool digit (unsigned char c) { return c >= '0' && c <= '9'; }

  static bool identifier (std::string_view name)
  {
    return !name.empty () && (alpha (name.front ()) || digit (name.front ())
                             || name.front () == '_')
           && std::all_of (name.begin (), name.end (), [] (unsigned char c)
                           { return alpha (c) || digit (c) || c == '-' || c == '_'; });
  }

  static std::string safe (std::string_view text)
  {
    return terminal::sanitize_text (text);
  }

  static void error (ParseResult &out, ErrorCode code, std::string message,
                     std::optional<std::size_t> index = std::nullopt)
  {
    out.errors.push_back (message);
    out.diagnostics.push_back ({ code, index, std::move (message) });
  }

  static std::optional<double> real (std::string_view raw)
  {
    if (raw.empty ())
      return std::nullopt;
    double value{};
    auto [end, status] = std::from_chars (raw.data (), raw.data () + raw.size (), value);
    if (status != std::errc{} || end != raw.data () + raw.size () || !std::isfinite (value))
      return std::nullopt;
    return value;
  }

  static bool resembles_option (std::string_view arg, const Dialect &dialect)
  {
    return arg.size () > 1 && arg[0] == '-'
           && !(dialect.negative_numbers
                && (digit (arg[1]) || arg[1] == '.') && real (arg));
  }

  static std::optional<Value> convert (ValueKind kind, std::string_view raw)
  {
    switch (kind)
      {
      case ValueKind::string:
        return std::string (raw);
      case ValueKind::integer:
        if (!raw.empty ())
          {
            std::int64_t value{};
            auto [end, status] = std::from_chars (raw.data (), raw.data () + raw.size (), value);
            if (status == std::errc{} && end == raw.data () + raw.size ())
              return value;
          }
        break;
      case ValueKind::real:
        if (auto value = real (raw))
          return *value;
        break;
      case ValueKind::boolean:
        if (raw == "true" || raw == "1")
          return true;
        if (raw == "false" || raw == "0")
          return false;
        break;
      }
    return std::nullopt;
  }

  static std::string_view kind_name (ValueKind kind)
  {
    switch (kind)
      {
      case ValueKind::string: return "value";
      case ValueKind::integer: return "integer";
      case ValueKind::real: return "real";
      case ValueKind::boolean: return "boolean";
      }
    return {};
  }

  static std::optional<Value> converted (ValueKind kind,
                                         const std::vector<std::string> &choices,
                                         const ValuePredicate &predicate,
                                         std::string_view raw,
                                         const std::optional<NumericRange> &range = {})
  {
    if (!choices.empty () && std::find (choices.begin (), choices.end (), raw) == choices.end ())
      return std::nullopt;
    auto value = convert (kind, raw);
    if (value && range)
      {
        if (range->lower)
          if (auto lower = convert (kind, *range->lower); !lower || *value < *lower)
            return std::nullopt;
        if (range->upper)
          if (auto upper = convert (kind, *range->upper); !upper || *value > *upper)
            return std::nullopt;
      }
    if (value && predicate && !predicate (*value))
      return std::nullopt;
    return value;
  }

  static std::optional<std::string> read_environment (const std::string &name)
  {
    if (const auto *value = std::getenv (name.c_str ()))
      return std::string (value);
    return std::nullopt;
  }

  static Cardinality cardinality (const Positional &pos)
  {
    return { std::max (pos.required ? std::size_t{1} : std::size_t{0}, pos.count.minimum),
             pos.repeatable ? pos.count.maximum
                            : std::optional<std::size_t>{ pos.count.maximum.value_or (1) } };
  }

  static std::size_t occurrences (const ParseResult &out, const std::string &name)
  {
    const auto it = out.occurrences.find (name);
    return it == out.occurrences.end () ? 0 : it->second;
  }

  static void check_constraints (const std::vector<const Command *> &scope,
                                 ParseResult &out)
  {
    for (const auto *level : scope)
      {
        for (const auto &option : level->options)
          {
            const auto count = occurrences (out, option.name);
            if (count < option.count.minimum)
              error (out, ErrorCode::cardinality,
                     "too few occurrences of '--" + option.name + "'");
            if (count != 0)
              for (const auto &dependency : option.dependencies)
                if (occurrences (out, dependency) == 0)
                  error (out, ErrorCode::dependency,
                         "'--" + option.name + "' requires '--" + dependency + "'");
          }
        for (const auto &group : level->groups)
          {
            const auto count = static_cast<std::size_t> (
                std::count_if (group.members.begin (), group.members.end (),
                               [&] (const std::string &member)
                               { return occurrences (out, member) != 0; }));
            if (count < group.count.minimum
                || (group.count.maximum && count > *group.count.maximum))
              error (out, ErrorCode::group,
                     "option group '" + group.name + "' violates its cardinality");
          }
      }
  }

  static const Command *find_command (const Command &command, std::string_view name)
  {
    for (const auto &child : command.subcommands)
      if (child.name == name
          || std::find (child.aliases.begin (), child.aliases.end (), name) != child.aliases.end ())
        return &child;
    return nullptr;
  }

  static const Option *find_option (const std::vector<const Command *> &scope,
                                    std::string_view name, char short_name)
  {
    if (name.empty () && short_name == 0)
      return nullptr;
    for (const auto *level : scope)
      for (const auto &option : level->options)
        if (!name.empty ()
                ? option.name == name
                      || std::find (option.aliases.begin (), option.aliases.end (), name)
                             != option.aliases.end ()
                : option.short_name == short_name
                      || std::find (option.short_aliases.begin (), option.short_aliases.end (),
                                    short_name) != option.short_aliases.end ())
          return &option;
    return nullptr;
  }

  static void store (const Option &option, std::string_view raw, ParseResult &out,
                     std::optional<std::size_t> index, bool skip_predicate = false)
  {
    const ValuePredicate none;
    auto value = converted (option.negatable ? ValueKind::boolean : option.kind,
                            option.choices, skip_predicate ? none : option.predicate,
                            raw, option.range);
    if (!value)
      {
        error (out, ErrorCode::invalid_value,
               "invalid value for '--" + option.name + "'", index);
        return;
      }
    out.options[option.name].emplace_back (raw);
    out.typed_options[option.name].push_back (std::move (*value));
  }

  static void consume (const Option &option, std::optional<std::string_view> attached,
                       const std::vector<std::string> &argv, std::size_t &i,
                       const Dialect &dialect, ParseResult &out,
                       bool negated, bool prefix)
  {
    const std::size_t option_index = i;
    if (!option.takes_value && attached)
      {
        error (out, ErrorCode::unexpected_value,
               "option '--" + option.name + "' takes no value", option_index);
        return;
      }
    if (option.takes_value && !attached)
      {
        if (i + 1 < argv.size () && !resembles_option (argv[i + 1], dialect))
          attached = argv[++i];
        else
          {
            if (prefix && i + 1 == argv.size ())
              out.pending_option = option.name;
            else
              error (out, ErrorCode::missing_value,
                     "missing value for '--" + option.name + "'", option_index);
            return;
          }
      }
    auto &count = out.occurrences[option.name];
    if (count != 0 && !option.repeatable)
      {
        error (out, ErrorCode::duplicate_option,
               "duplicate option '--" + option.name + "'", option_index);
        return;
      }
    if (option.count.maximum && count >= *option.count.maximum)
      {
        error (out, ErrorCode::cardinality,
               "too many occurrences of '--" + option.name + "'", option_index);
        return;
      }
    ++count;
    if (attached)
      store (option, *attached, out, i, prefix);
    else if (option.negatable)
      store (option, negated ? "false" : "true", out, option_index, prefix);
    else
      {
        out.options.try_emplace (option.name);
        out.typed_options[option.name].emplace_back (true);
      }
  }

  static void count (std::size_t &entries, std::size_t amount)
  {
    if (amount > max_entries - entries)
      fail ("specification entry limit exceeded");
    entries += amount;
  }

  static void validate (const Command &command, std::size_t depth,
                        std::size_t &entries, std::unordered_set<std::string> &longs,
                        std::unordered_set<char> &shorts,
                        std::unordered_set<std::string> &canonical)
  {
    if (depth >= max_depth)
      fail ("command nesting limit exceeded");
    count (entries, 1);
    count (entries, command.options.size ());
    count (entries, command.positionals.size ());
    count (entries, command.aliases.size ());
    count (entries, command.groups.size ());
    if (!identifier (command.name))
      fail ("invalid command name '" + safe (command.name) + "'");
    if (depth == 0 && !command.aliases.empty ())
      fail ("program name cannot have aliases");
    if (depth != 0 && command.dialect)
      fail ("parsing dialect may only be specified at the program root");
    if ((!command.handler_name.empty () && !identifier (command.handler_name))
        || (command.handler && !command.handler_name.empty ()))
      fail ("expected one valid handler name or one native handler");
    if (command.require_subcommand
        && (command.subcommands.empty () || !command.positionals.empty ()))
      fail ("required subcommand needs children and no positional arguments");

    const auto add_long = [&] (const std::string &name, bool negatable)
    {
      if (!identifier (name) || !longs.insert (name).second
          || (negatable && !longs.insert ("no-" + name).second))
        fail ("invalid, reserved, or duplicate option '--" + safe (name) + "'");
    };
    const auto add_short = [&] (char name)
    {
      if (!alpha (name) || !shorts.insert (name).second)
        fail ("invalid, reserved, or duplicate short option");
    };
    for (const auto &option : command.options)
      {
        count (entries, option.aliases.size ());
        count (entries, option.short_aliases.size ());
        count (entries, option.choices.size ());
        count (entries, option.dependencies.size ());
        add_long (option.name, option.negatable);
        canonical.insert (option.name);
        for (const auto &alias : option.aliases)
          add_long (alias, option.negatable);
        if (option.short_name)
          add_short (option.short_name);
        for (char alias : option.short_aliases)
          add_short (alias);
        validate_count (option.count, option.repeatable);
        if (option.count.maximum == 0)
          fail ("option must allow at least one occurrence");
        if (option.negatable && (option.takes_value || option.kind != ValueKind::string))
          fail ("negatable switch cannot take a value or specify a type");
        if (!option.takes_value && !option.negatable
            && (option.default_value || option.environment || option.kind != ValueKind::string
                || !option.choices.empty () || option.predicate || option.range))
          fail ("plain flag does not support value properties");
        auto kind = option.negatable ? ValueKind::boolean : option.kind;
        validate_choices (kind, option.choices);
        validate_range (kind, option.choices, option.range);
        if (option.default_value
            && !converted (kind, option.choices, {}, *option.default_value, option.range))
          fail ("invalid default for '--" + option.name + "'");
        if (option.environment
            && (!identifier (*option.environment) || option.environment->find ('-') != std::string::npos
                || digit (option.environment->front ())))
          fail ("invalid environment variable name for '--" + option.name + "'");
      }

    std::unordered_set<std::string> names;
    for (const auto &option : command.options)
      {
        names.clear ();
        for (const auto &name : option.dependencies)
          if (name == option.name || !canonical.contains (name) || !names.insert (name).second)
            fail ("invalid dependency for '--" + option.name + "'");
      }
    names.clear ();
    for (const auto &group : command.groups)
      {
        count (entries, group.members.size ());
        if (!identifier (group.name) || !names.insert (group.name).second
            || group.members.empty ())
          fail ("invalid or duplicate option group");
        validate_count (group.count, true);
        if (group.count.minimum > group.members.size ()
            || (group.count.maximum && *group.count.maximum > group.members.size ()))
          fail ("option group count exceeds its membership");
        std::unordered_set<std::string> members;
        for (const auto &name : group.members)
          if (!canonical.contains (name) || !members.insert (name).second)
            fail ("invalid member of option group '" + group.name + "'");
      }
    names.clear ();
    bool optional = false;
    for (std::size_t i = 0; i < command.positionals.size (); ++i)
      {
        const auto &pos = command.positionals[i];
        count (entries, pos.choices.size ());
        if (!identifier (pos.name) || !names.insert (pos.name).second)
          fail ("invalid or duplicate positional argument '" + safe (pos.name) + "'");
        validate_count (pos.count, pos.repeatable);
        validate_count (cardinality (pos), pos.repeatable);
        if (pos.count.maximum == 0)
          fail ("positional argument must allow at least one value");
        validate_choices (pos.kind, pos.choices);
        validate_range (pos.kind, pos.choices, pos.range);
        const auto limits = cardinality (pos);
        if ((optional && limits.minimum != 0)
            || (pos.repeatable && i + 1 != command.positionals.size ()))
          fail ("required positionals precede optional ones; only the last may repeat");
        optional |= limits.minimum == 0;
      }
    names.clear ();
    for (const auto &child : command.subcommands)
      {
        if (!names.insert (child.name).second)
          fail ("duplicate subcommand '" + safe (child.name) + "'");
        for (const auto &alias : child.aliases)
          if (!identifier (alias) || !names.insert (alias).second)
            fail ("invalid or duplicate subcommand alias '" + safe (alias) + "'");
        validate (child, depth + 1, entries, longs, shorts, canonical);
      }
    for (const auto &option : command.options)
      {
        longs.erase (option.name);
        if (option.negatable)
          longs.erase ("no-" + option.name);
        canonical.erase (option.name);
        for (const auto &alias : option.aliases)
          {
            longs.erase (alias);
            if (option.negatable)
              longs.erase ("no-" + alias);
          }
        if (option.short_name)
          shorts.erase (option.short_name);
        for (char alias : option.short_aliases)
          shorts.erase (alias);
      }
  }

  static void validate_count (const Cardinality &value, bool repeatable)
  {
    if ((value.maximum && value.minimum > *value.maximum)
        || (!repeatable && (value.minimum > 1 || (value.maximum && *value.maximum > 1))))
      fail ("invalid cardinality");
  }

  static void validate_choices (ValueKind kind, const std::vector<std::string> &choices)
  {
    if (kind_name (kind).empty ())
      fail ("unknown value type");
    std::unordered_set<std::string> seen;
    for (const auto &choice : choices)
      if (!convert (kind, choice) || !seen.insert (choice).second)
        fail ("invalid or repeated value choice");
  }

  static void validate_range (ValueKind kind, const std::vector<std::string> &choices,
                              const std::optional<NumericRange> &range)
  {
    if (!range)
      return;
    if ((kind != ValueKind::real && kind != ValueKind::integer)
        || (!range->lower && !range->upper))
      fail ("range requires a numeric type and at least one bound");
    const auto lower = range->lower ? convert (kind, *range->lower) : std::nullopt;
    const auto upper = range->upper ? convert (kind, *range->upper) : std::nullopt;
    if ((range->lower && !lower) || (range->upper && !upper)
        || (lower && upper && *lower > *upper))
      fail ("invalid numeric range bounds");
    for (const auto &choice : choices)
      if (!converted (kind, {}, {}, choice, range))
        fail ("choice outside numeric range");
  }

  template <class T> static T &checked (T &root)
  {
    std::unordered_set<std::string> longs{ "help" };
    std::unordered_set<char> shorts{ 'h' };
    std::unordered_set<std::string> canonical;
    std::size_t entries = 0;
    validate (root, 0, entries, longs, shorts, canonical);
    return root;
  }

  static const std::string &atom (const SExpr &e)
  {
    if (!e.is_atom || !e.items.empty ())
      fail ("expected an atom");
    return e.text;
  }

  static void validate_form (const SExpr &e, std::size_t depth, std::size_t depth_limit,
                             std::size_t &nodes, std::size_t &bytes)
  {
    if (depth > depth_limit || nodes == 0 || e.text.size () > bytes)
      fail ("S-expression budget exceeded");
    --nodes;
    bytes -= e.text.size ();
    if ((e.is_atom && !e.items.empty ()) || (!e.is_atom && (e.quoted || !e.text.empty ())))
      fail ("malformed S-expression node");
    if (e.items.size () > nodes)
      fail ("S-expression node budget exceeded");
    for (const auto &child : e.items)
      validate_form (child, depth + 1, depth_limit, nodes, bytes);
  }

  static const std::string &head (const SExpr &e)
  {
    if (e.is_atom || e.items.empty () || e.quoted || !e.text.empty ()
        || e.items.front ().quoted)
      fail ("expected a list with an unquoted keyword");
    return atom (e.items.front ());
  }

  static const std::string &property (const SExpr &e)
  {
    if (e.items.size () != 2)
      fail ("expected precisely one value for '" + safe (head (e)) + "'");
    return atom (e.items[1]);
  }

  static bool boolean (const SExpr &e)
  {
    const auto &v = property (e);
    if (!e.items[1].quoted && v == "true")
      return true;
    if (!e.items[1].quoted && v == "false")
      return false;
    fail ("expected 'true' or 'false' for '" + safe (head (e)) + "'");
  }

  static void unique_property (std::unordered_set<std::string> &seen,
                               const std::string &name)
  {
    if (!seen.insert (name).second)
      fail ("duplicate property '" + safe (name) + "'");
  }

  static std::vector<std::string> atoms (const SExpr &e)
  {
    if (e.items.size () < 2 || e.items.size () - 1 > max_entries)
      fail ("expected a bounded nonempty list of values for '" + safe (head (e)) + "'");
    std::vector<std::string> out;
    out.reserve (e.items.size () - 1);
    for (std::size_t i = 1; i < e.items.size (); ++i)
      out.push_back (atom (e.items[i]));
    return out;
  }

  static char short_name (const std::string &name)
  {
    if (name.size () != 1 || !alpha (name.front ()))
      fail ("short option requires a single letter");
    return name.front ();
  }

  static std::size_t natural (const SExpr &e)
  {
    const auto &text = atom (e);
    std::size_t n{};
    auto [end, error] = std::from_chars (text.data (), text.data () + text.size (), n);
    if (e.quoted || error != std::errc{} || end != text.data () + text.size ())
      fail ("expected a nonnegative size");
    return n;
  }

  static Cardinality read_count (const SExpr &e)
  {
    if (e.items.size () != 3)
      fail ("cardinality requires minimum and maximum (or '*')");
    const auto minimum = natural (e.items[1]);
    return { minimum, atom (e.items[2]) == "*" && !e.items[2].quoted
                          ? std::nullopt : std::optional{ natural (e.items[2]) } };
  }

  static ValueKind value_kind (const SExpr &e)
  {
    const auto &name = property (e);
    if (!e.items[1].quoted)
      {
        if (name == "string")
          return ValueKind::string;
        if (name == "integer")
          return ValueKind::integer;
        if (name == "real")
          return ValueKind::real;
        if (name == "boolean")
          return ValueKind::boolean;
      }
    fail ("unknown value type '" + safe (name) + "'");
  }

  static NumericRange read_range (const SExpr &e)
  {
    if (e.items.size () != 3)
      fail ("range requires lower and upper bounds (or '*')");
    const auto bound = [] (const SExpr &item) -> std::optional<std::string>
    {
      const auto &raw = atom (item);
      return raw == "*" && !item.quoted ? std::nullopt : std::optional{ raw };
    };
    return { bound (e.items[1]), bound (e.items[2]) };
  }

  static Dialect read_dialect (const SExpr &e)
  {
    Dialect dialect;
    std::unordered_set<std::string> seen;
    for (std::size_t i = 1; i < e.items.size (); ++i)
      {
        const auto &field = e.items[i];
        const auto &key = head (field);
        unique_property (seen, key);
        if (key == "cluster-short")
          dialect.cluster_short = boolean (field);
        else if (key == "attached-short")
          dialect.attached_short = boolean (field);
        else if (key == "long-equals")
          dialect.long_equals = boolean (field);
        else if (key == "options-after-positionals")
          dialect.options_after_positionals = boolean (field);
        else if (key == "negative-numbers")
          dialect.negative_numbers = boolean (field);
        else
          fail ("unknown dialect property '" + safe (key) + "'");
      }
    return dialect;
  }

  static OptionGroup read_group (const SExpr &e)
  {
    if (e.items.size () < 2)
      fail ("option group requires a name");
    OptionGroup out;
    out.name = atom (e.items[1]);
    std::unordered_set<std::string> seen;
    for (std::size_t i = 2; i < e.items.size (); ++i)
      {
        const auto &field = e.items[i];
        const auto &key = head (field);
        unique_property (seen, key);
        if (key == "members")
          out.members = atoms (field);
        else if (key == "count")
          out.count = read_count (field);
        else
          fail ("unknown option group property '" + safe (key) + "'");
      }
    if (!seen.contains ("count"))
      fail ("option group requires cardinality");
    return out;
  }

  static Option read_option (const SExpr &e)
  {
    if (e.items.size () < 2)
      fail ("option requires a name");
    Option out;
    out.name = atom (e.items[1]);
    const auto &form = head (e);
    out.negatable = form == "switch";
    std::unordered_set<std::string> seen;
    for (std::size_t i = 2; i < e.items.size (); ++i)
      {
        const auto &field = e.items[i];
        const auto &key = head (field);
        unique_property (seen, key);
        if (key == "description")
          out.description = property (field);
        else if (key == "short")
          out.short_name = short_name (property (field));
        else if (key == "value" && form == "option")
          out.takes_value = boolean (field);
        else if (key == "required")
          out.required = boolean (field);
        else if (key == "repeatable")
          out.repeatable = boolean (field);
        else if (key == "default")
          out.default_value = property (field);
        else if (key == "environment")
          out.environment = property (field);
        else if (key == "aliases")
          out.aliases = atoms (field);
        else if (key == "short-aliases")
          for (const auto &alias : atoms (field))
            out.short_aliases.push_back (short_name (alias));
        else if (key == "negatable" && form == "option")
          out.negatable = boolean (field);
        else if (key == "type")
          out.kind = value_kind (field);
        else if (key == "choices")
          out.choices = atoms (field);
        else if (key == "count")
          out.count = read_count (field);
        else if (key == "requires")
          out.dependencies = atoms (field);
        else if (key == "range")
          out.range = read_range (field);
        else
          fail ("unknown option property '" + safe (key) + "'");
      }
    return out;
  }

  static Positional read_positional (const SExpr &e)
  {
    if (e.items.size () < 2)
      fail ("positional requires a name");
    Positional out;
    out.name = atom (e.items[1]);
    std::unordered_set<std::string> seen;
    for (std::size_t i = 2; i < e.items.size (); ++i)
      {
        const auto &field = e.items[i];
        const auto &key = head (field);
        unique_property (seen, key);
        if (key == "description")
          out.description = property (field);
        else if (key == "required")
          out.required = boolean (field);
        else if (key == "repeatable")
          out.repeatable = boolean (field);
        else if (key == "type")
          out.kind = value_kind (field);
        else if (key == "choices")
          out.choices = atoms (field);
        else if (key == "count")
          out.count = read_count (field);
        else if (key == "range")
          out.range = read_range (field);
        else
          fail ("unknown positional property '" + safe (key) + "'");
      }
    return out;
  }

  // (command program (description "...") (require-subcommand true)
  //   (option verbose (short v) (repeatable true))
  //   (command run (option output (value true) (default "stdout"))
  //                (positional input (required true))))
  static Command read_command (const SExpr &e, std::size_t depth, std::size_t &entries)
  {
    if (depth >= max_depth)
      fail ("command nesting limit exceeded");
    count (entries, 1);
    const auto &form = head (e);
    if ((form != "command" && !(depth == 0 && form == "program")) || e.items.size () < 2)
      fail ("expected '(command name ...)'");
    Command out;
    out.name = atom (e.items[1]);
    std::unordered_set<std::string> seen;
    for (std::size_t i = 2; i < e.items.size (); ++i)
      {
        const auto &field = e.items[i];
        const auto &key = head (field);
        if (key == "command")
          out.subcommands.push_back (read_command (field, depth + 1, entries));
        else if (key == "option" || key == "flag" || key == "switch")
          {
            count (entries, 1);
            out.options.push_back (read_option (field));
          }
        else if (key == "positional")
          {
            count (entries, 1);
            out.positionals.push_back (read_positional (field));
          }
        else if (key == "group")
          {
            count (entries, 1);
            out.groups.push_back (read_group (field));
          }
        else
          {
            unique_property (seen, key);
            if (key == "description")
              out.description = property (field);
            else if (key == "require-subcommand")
              out.require_subcommand = boolean (field);
            else if (key == "aliases")
              out.aliases = atoms (field);
            else if (key == "dialect")
              out.dialect = read_dialect (field);
            else if (key == "handler")
              {
                out.handler_name = property (field);
                if (!identifier (out.handler_name))
                  fail ("invalid handler name");
              }
            else
              fail ("unknown command property '" + safe (key) + "'");
          }
      }
    return out;
  }
};
} // namespace cli

struct OptParser
{
  template <class Derived> struct Mixin
  {
    using OptionParser = cli::OptParser;
    using CLICommand = cli::Command;
    using CLIOption = cli::Option;
    using CLIPositional = cli::Positional;
    using CLIParseResult = cli::ParseResult;
  };
};

// ============================================================
//  Portable line editor and REPL session
// ============================================================

namespace repl
{
struct EditorLimits
{
  std::size_t input_bytes = 64 * 1024;
  std::size_t history_entries = 128;
  std::size_t undo_entries = 64;
  std::size_t kill_entries = 16;
};

class Editor
{
  struct Draft
  {
    std::pmr::string text;
    std::size_t cursor;
    std::optional<std::size_t> mark;

    Draft (std::string_view source, std::size_t at,
           std::optional<std::size_t> selected, std::pmr::memory_resource *resource)
      : text (source, resource), cursor (at), mark (selected) {}
  };

  // Declared first so owned resources outlive all the buffers.
  live::Storage storage_;
  EditorLimits limits_;
  std::pmr::string text_;
  std::size_t cursor_ = 0;
  std::optional<std::size_t> mark_;
  std::deque<std::string> history_;
  std::size_t history_pos_ = 0;
  std::optional<Draft> draft_;
  std::pmr::deque<Draft> undo_, redo_;
  std::pmr::deque<std::pmr::string> kills_;
  std::size_t kill_pos_ = 0;
  std::optional<std::pair<std::size_t, std::size_t>> yanked_;

  void
  remember_edit (std::pmr::deque<Draft> &stack)
  {
    if (!limits_.undo_entries)
      return;
    stack.emplace_back (text_, cursor_, mark_, storage_.resource ());
    if (stack.size () > limits_.undo_entries)
      stack.pop_front ();
  }

  bool
  restore_edit (std::pmr::deque<Draft> &from, std::pmr::deque<Draft> &to)
  {
    if (from.empty ())
      return false;
    remember_edit (to);
    auto &value = from.back ();
    text_ = std::move (value.text);
    cursor_ = value.cursor;
    mark_ = value.mark;
    from.pop_back ();
    yanked_.reset ();
    leave_history ();
    return true;
  }

  void
  leave_history ()
  {
    history_pos_ = history_.size ();
    draft_.reset ();
  }

  static std::size_t
  next_boundary (std::string_view source, std::size_t at)
  {
    auto end = terminal::next_glyph (source, at).end;
    return end > at && end <= source.size () ? end : at + 1;
  }

  static std::size_t
  boundary_at_or_after (std::string_view source, std::size_t target)
  {
    if (target >= source.size ())
      return source.size ();
    std::size_t at = 0;
    while (at < target)
      at = next_boundary (source, at);
    return at;
  }

  std::size_t
  previous_boundary (std::size_t target) const
  {
    std::size_t at = 0;
    while (at < target)
      {
        auto next = next_boundary (text_, at);
        if (next >= target)
          return at;
        at = next;
      }
    return at;
  }

  void
  swap (Editor &other) noexcept
  {
    // Only called for editors using the very same resource.
    using std::swap;
    swap (limits_, other.limits_);
    swap (text_, other.text_);
    swap (cursor_, other.cursor_);
    swap (mark_, other.mark_);
    swap (history_, other.history_);
    swap (history_pos_, other.history_pos_);
    swap (draft_, other.draft_);
    swap (undo_, other.undo_);
    swap (redo_, other.redo_);
    swap (kills_, other.kills_);
    swap (kill_pos_, other.kill_pos_);
    swap (yanked_, other.yanked_);
  }

  struct CopyIntoStorage {};

  Editor (CopyIntoStorage, const Editor &other, live::Storage storage)
    : storage_ (std::move (storage)), limits_ (other.limits_),
      text_ (other.text_, storage_.resource ()), cursor_ (other.cursor_),
      mark_ (other.mark_), history_ (other.history_), history_pos_ (other.history_pos_),
      undo_ (storage_.resource ()), redo_ (storage_.resource ()),
      kills_ (other.kills_, storage_.resource ()), kill_pos_ (other.kill_pos_),
      yanked_ (other.yanked_)
  {
    auto copy_draft = [&] (auto &into, const Draft &value)
      { into.emplace_back (value.text, value.cursor, value.mark, storage_.resource ()); };
    if (other.draft_)
      draft_.emplace (other.draft_->text, other.draft_->cursor, other.draft_->mark,
                      storage_.resource ());
    for (const auto &value : other.undo_)
      copy_draft (undo_, value);
    for (const auto &value : other.redo_)
      copy_draft (redo_, value);
  }

public:
  explicit Editor (EditorLimits limits = {}, live::Storage storage = live::Storage{})
    : storage_ (std::move (storage)), limits_ (limits), text_ (storage_.resource ()),
      undo_ (storage_.resource ()), redo_ (storage_.resource ()),
      kills_ (storage_.resource ()) {}
  Editor (const Editor &other) : Editor (CopyIntoStorage{}, other, other.storage_) {}
  Editor (Editor &&other) : Editor (EditorLimits{}, other.storage_) { swap (other); }
  Editor &operator= (const Editor &other)
  {
    if (this != &other)
      {
        Editor copy (CopyIntoStorage{}, other, storage_);
        swap (copy);
      }
    return *this;
  }
  Editor &operator= (Editor &&other)
  {
    if (this == &other)
      return *this;
    if (storage_.resource () != other.storage_.resource ())
      return *this = static_cast<const Editor &> (other);
    Editor moved (std::move (other));
    swap (moved);
    return *this;
  }

  live::Storage storage () const noexcept { return storage_; }
  std::string_view text () const { return text_; }
  std::size_t cursor () const { return cursor_; } // Byte offset.
  EditorLimits limits () const { return limits_; }
  const std::deque<std::string> &history () const { return history_; }
  std::optional<std::size_t> mark () const { return mark_; }
  void set_mark () { mark_ = cursor_; yanked_.reset (); }
  void clear_mark () { mark_.reset (); }

  std::optional<std::pair<std::size_t, std::size_t>>
  selection () const
  {
    if (!mark_ || *mark_ == cursor_)
      return std::nullopt;
    return std::pair{ std::min (*mark_, cursor_), std::max (*mark_, cursor_) };
  }

  bool undo () { return restore_edit (undo_, redo_); }
  bool redo () { return restore_edit (redo_, undo_); }

  static bool
  is_boundary (std::string_view source, std::size_t at)
  {
    return at <= source.size () && boundary_at_or_after (source, at) == at;
  }

  bool
  move_to (std::size_t at)
  {
    if (!is_boundary (text_, at))
      return false;
    if (at != cursor_)
      yanked_.reset ();
    cursor_ = at;
    return true;
  }

  bool
  move_left ()
  {
    if (!cursor_)
      return false;
    yanked_.reset ();
    cursor_ = previous_boundary (cursor_);
    return true;
  }

  bool
  move_right ()
  {
    if (cursor_ == text_.size ())
      return false;
    yanked_.reset ();
    cursor_ = next_boundary (text_, cursor_);
    return true;
  }

  void
  move_home ()
  {
    auto newline = cursor_ ? text_.rfind ('\n', cursor_ - 1) : std::string::npos;
    // A newline can be part of a CRLF cluster.
    cursor_ = newline == std::string::npos ? 0 : newline + 1;
    yanked_.reset ();
  }

  void
  move_end ()
  {
    auto newline = text_.find ('\n', cursor_);
    if (newline == std::string::npos)
      cursor_ = text_.size ();
    else
      cursor_ = is_boundary (text_, newline) ? newline
                                             : previous_boundary (newline);
    yanked_.reset ();
  }

  bool
  move_line (bool forward)
  {
    auto current = cursor_;
    move_home ();
    const auto line = cursor_;
    cursor_ = current;
    auto column = terminal::display_width (
      std::string_view (text_).substr (line, current - line));
    std::size_t target;
    if (forward)
      {
        auto newline = text_.find ('\n', current);
        if (newline == std::string::npos)
          return false;
        target = newline + 1;
      }
    else
      {
        if (!line)
          return false;
        auto newline = line > 1 ? text_.rfind ('\n', line - 2) : std::string::npos;
        target = newline == std::string::npos ? 0 : newline + 1;
      }
    while (target < text_.size ())
      {
        const auto glyph = terminal::next_glyph (text_, target);
        if (std::string_view (text_).substr (target, glyph.end - target).find ('\n')
              != std::string_view::npos || glyph.width > column)
          break;
        target = glyph.end;
        column -= glyph.width;
      }
    cursor_ = target;
    return true;
  }

  bool
  replace (std::size_t begin, std::size_t end, std::string_view inserted)
  {
    if (begin > end || end > text_.size ())
      return false;
    auto retained = text_.size () - (end - begin);
    if (inserted.size () > limits_.input_bytes - retained
        || !is_boundary (text_, begin) || !is_boundary (text_, end))
      return false;
    if (begin == end && inserted.empty ())
      return true;

    // inserted may point into text_. Compute the new glyph boundary before
    // replacing the buffer: an insertion can join the following cluster.
    std::pmr::string next (text_, storage_.resource ());
    next.replace (begin, end - begin, inserted);
    auto cursor = boundary_at_or_after (next, begin + inserted.size ());
    remember_edit (undo_);
    text_ = std::move (next);
    cursor_ = cursor;
    yanked_.reset ();
    mark_.reset ();
    redo_.clear ();
    leave_history ();
    return true;
  }

  bool
  insert (std::string_view text)
  {
    if (auto range = selection ())
      return replace (range->first, range->second, text);
    return replace (cursor_, cursor_, text);
  }

  bool
  set_text (std::string_view source)
  {
    return replace (0, text_.size (), source);
  }

  bool
  erase_before ()
  {
    if (auto range = selection ())
      return replace (range->first, range->second, {});
    return cursor_ && replace (previous_boundary (cursor_), cursor_, {});
  }

  bool
  erase_after ()
  {
    if (auto range = selection ())
      return replace (range->first, range->second, {});
    return cursor_ < text_.size ()
           && replace (cursor_, next_boundary (text_, cursor_), {});
  }

  bool
  kill (std::size_t begin, std::size_t end, bool remove = true)
  {
    if (begin >= end || !limits_.kill_entries || !is_boundary (text_, begin)
        || !is_boundary (text_, end))
      return false;
    kills_.emplace_front (std::string_view (text_).substr (begin, end - begin));
    try
      {
        if (remove)
          replace (begin, end, {});
      }
    catch (...)
      {
        kills_.pop_front ();
        throw;
      }
    if (kills_.size () > limits_.kill_entries)
      kills_.pop_back ();
    kill_pos_ = 0;
    yanked_.reset ();
    return true;
  }

  bool
  yank (bool older = false)
  {
    if (kills_.empty () || (older && !yanked_))
      return false;
    auto at = older ? (kill_pos_ + 1) % kills_.size () : 0;
    auto range = older ? yanked_ : selection ();
    auto [begin, end] = range.value_or (std::pair{ cursor_, cursor_ });
    const auto size = kills_[at].size ();
    if (!replace (begin, end, kills_[at]))
      return false;
    kill_pos_ = at;
    // If the insertion joined a nearby glyph, yank-pop cannot replace it
    // safely without splitting that glyph.
    if (is_boundary (text_, begin) && is_boundary (text_, begin + size))
      yanked_ = std::pair{ begin, begin + size };
    return true;
  }

  void
  clear ()
  {
    text_.clear ();
    cursor_ = 0;
    mark_.reset ();
    undo_.clear ();
    redo_.clear ();
    yanked_.reset ();
    leave_history ();
  }

  bool
  remember (std::string_view entry)
  {
    if (entry.empty () || entry.size () > limits_.input_bytes
        || limits_.history_entries == 0)
      return false;
    if (history_.empty () || history_.back () != entry)
      {
        history_.emplace_back (entry);
        if (history_.size () > limits_.history_entries)
          history_.pop_front ();
      }
    leave_history ();
    return true;
  }

  bool
  history_previous ()
  {
    if (history_pos_ == 0)
      return false;
    std::pmr::string previous (history_[history_pos_ - 1], storage_.resource ());
    if (history_pos_ == history_.size ())
      draft_.emplace (text_, cursor_, mark_, storage_.resource ());
    --history_pos_;
    text_ = std::move (previous);
    cursor_ = text_.size ();
    mark_.reset ();
    undo_.clear ();
    redo_.clear ();
    yanked_.reset ();
    return true;
  }

  bool
  history_next ()
  {
    if (!draft_ || history_pos_ >= history_.size ())
      return false;
    if (history_pos_ + 1 == history_.size ())
      {
        text_ = std::move (draft_->text);
        cursor_ = draft_->cursor;
        mark_ = draft_->mark;
        leave_history ();
      }
    else
      {
        text_ = history_[history_pos_ + 1];
        ++history_pos_;
        cursor_ = text_.size ();
        mark_.reset ();
      }
    undo_.clear ();
    redo_.clear ();
    yanked_.reset ();
    return true;
  }

  // An explicit caller-owned storage backend decides paths, permissions,
  // lifetime and atomic writes. Records are byte strings, including newlines.
  template <IOBackend Backend>
  void
  save_history (IO<Backend> &out) const
  {
    const std::string_view magic = "LDSLH1\n";
    out.write_all (std::as_bytes (std::span (magic)));
    auto write_size = [&] (std::size_t number)
      {
        auto line = std::to_string (number) + '\n';
        out.write_all (std::as_bytes (std::span (line)));
      };
    write_size (history_.size ());
    for (const auto &entry : history_)
      {
        write_size (entry.size ());
        out.write_all (std::as_bytes (std::span (entry)));
      }
  }

  template <IOBackend Backend>
  void
  load_history (IO<Backend> &in)
  {
    std::array<char, 7> magic{};
    in.read_exact (std::as_writable_bytes (std::span (magic)));
    if (std::string_view (magic.data (), magic.size ()) != "LDSLH1\n")
      throw std::runtime_error ("unrecognized REPL history format");
    auto read_size = [&] (std::size_t limit)
      {
        std::size_t result = 0;
        for (std::size_t digits = 0;; ++digits)
          {
            char byte;
            in.read_exact (std::as_writable_bytes (std::span (&byte, 1)));
            if (byte == '\n' && digits)
              return result;
            if (digits > std::numeric_limits<std::size_t>::digits10
                || byte < '0' || byte > '9')
              throw std::runtime_error ("invalid REPL history length");
            auto digit = static_cast<std::size_t> (byte - '0');
            if (digit > limit || result > (limit - digit) / 10)
              throw std::runtime_error ("REPL history exceeds storage policy");
            result = result * 10 + digit;
          }
      };
    auto count = read_size (limits_.history_entries);
    std::deque<std::string> loaded;
    for (std::size_t i = 0; i < count; ++i)
      {
        auto bytes = read_size (limits_.input_bytes);
        if (!bytes)
          throw std::runtime_error ("empty REPL history record");
        auto &entry = loaded.emplace_back (bytes, '\0');
        in.read_exact (std::as_writable_bytes (std::span (entry)));
      }
    history_.swap (loaded);
    leave_history ();
  }
};

struct Completion
{
  std::size_t begin = 0;
  std::size_t end = 0;
  std::string text;
  std::string label;
};

struct AnalysisCallbacks
{
  // Analysis callbacks are expected to be pure. No evaluator is called by
  // inspection or completion, and invalid/incomplete input is not evaluated.
  std::function<live::SyntaxStatus (std::string_view)> continuation;
  std::function<std::vector<Completion> (std::string_view, std::size_t)> complete;
  std::function<std::size_t (std::string_view, std::size_t)> indentation{};
};

struct Evaluation
{
  bool success = true;
  std::string output;
};

using Evaluator = std::function<Evaluation (std::string_view)>;
using CancellableEvaluator
  = std::function<Evaluation (std::string_view, std::stop_token)>;

enum class Command
{
  none, insert, submit, newline, left, right, up, down, home, end,
  backspace, delete_glyph, history_previous, history_next, complete,
  mark, unmark, cut, copy, kill_to_end, kill_to_start, kill_line, copy_line,
  yank, yank_older, undo, redo, clear, interrupt, eof, vi_normal, vi_insert,
  vi_append, word_forward, word_backward, word_next, kill_word_forward,
  kill_word_backward, kill_word_next, buffer_home, buffer_end, first_nonblank,
  vi_insert_home, vi_append_end
};

enum class KeyStyle { emacs, vi };
enum class KeyMode { insert, normal };

struct KeyStroke
{
  terminal::Key key;
  std::string text{};
  unsigned modifiers = 0;
  unsigned code = 0;
  bool operator== (const KeyStroke &) const = default;
};

class Keymap
{
  struct Binding
  {
    KeyMode mode;
    std::vector<KeyStroke> chord;
    Command action;
  };
  std::vector<Binding> bindings_;

  static bool
  prefix (std::span<const KeyStroke> first, std::span<const KeyStroke> second)
  {
    return first.size () <= second.size ()
           && std::equal (first.begin (), first.end (), second.begin ());
  }

public:
  struct Match
  {
    std::optional<Command> command;
    bool pending = false;
  };

  // Exact chords can be replaced. Ambiguous prefix bindings are rejected
  // rather than interpreted via timing-dependent execution.
  bool
  bind (KeyMode mode, std::vector<KeyStroke> chord, Command action)
  {
    if (chord.empty () || chord.size () > 8)
      return false;
    for (auto &item : bindings_)
      {
        if (item.mode != mode)
          continue;
        if (item.chord == chord)
          {
            item.action = action;
            return true;
          }
        if (prefix (chord, item.chord) || prefix (item.chord, chord))
          return false;
      }
    bindings_.push_back ({ mode, std::move (chord), action });
    return true;
  }

  bool
  unbind (KeyMode mode, std::span<const KeyStroke> chord)
  {
    return std::erase_if (bindings_, [&] (const Binding &item)
      {
        return item.mode == mode && item.chord.size () == chord.size ()
               && prefix (chord, item.chord);
      }) != 0;
  }

  Match
  match (KeyMode mode, std::span<const KeyStroke> chord) const
  {
    for (const auto &item : bindings_)
      if (item.mode == mode && prefix (chord, item.chord))
        {
          if (item.chord.size () == chord.size ())
            return { item.action, false };
          return { std::nullopt, true };
        }
    return {};
  }

  static Keymap
  defaults (KeyStyle style)
  {
    using K = terminal::Key;
    using C = Command;
    Keymap keys;
    auto bind = [&] (KeyMode mode, KeyStroke key, C command)
      { keys.bind (mode, { std::move (key) }, command); };
    for (auto mode : { KeyMode::insert, KeyMode::normal })
      {
        bind (mode, { K::enter }, C::submit);
        bind (mode, { K::enter, {}, terminal::alt }, C::newline);
        bind (mode, { K::backspace }, C::backspace);
        bind (mode, { K::delete_key }, C::delete_glyph);
        bind (mode, { K::left }, C::left);
        bind (mode, { K::right }, C::right);
        bind (mode, { K::up }, C::up);
        bind (mode, { K::down }, C::down);
        bind (mode, { K::home }, C::home);
        bind (mode, { K::end }, C::end);
        bind (mode, { K::up, {}, terminal::alt }, C::history_previous);
        bind (mode, { K::down, {}, terminal::alt }, C::history_next);
        bind (mode, { K::tab }, C::complete);
        bind (mode, { K::control, {}, terminal::ctrl, 3 }, C::interrupt);
        bind (mode, { K::control, {}, terminal::ctrl, 4 }, C::eof);
      }
    if (style == KeyStyle::vi)
      {
        bind (KeyMode::insert, { K::escape }, C::vi_normal);
        bind (KeyMode::insert, { K::control, {}, terminal::ctrl, 23 },
              C::kill_word_backward);
        const auto normal = KeyMode::normal;
        for (const auto &[text, command] :
             { std::pair{ "h", C::left }, { "l", C::right }, { "k", C::up },
               { "j", C::down }, { "0", C::home }, { "$", C::end },
               { "i", C::vi_insert }, { "a", C::vi_append }, { "x", C::delete_glyph },
               { "u", C::undo }, { "v", C::mark }, { "p", C::yank },
                { "D", C::kill_to_end }, { "w", C::word_next },
                { "b", C::word_backward }, { "^", C::first_nonblank },
                { "I", C::vi_insert_home }, { "A", C::vi_append_end },
                { "G", C::buffer_end }, { "X", C::backspace } })
          bind (normal, { K::text, text }, command);
        bind (normal, { K::escape }, C::unmark);
        bind (normal, { K::control, {}, terminal::ctrl, 18 }, C::redo);
        keys.bind (normal, { { K::text, "d" }, { K::text, "d" } }, C::kill_line);
        keys.bind (normal, { { K::text, "y" }, { K::text, "y" } }, C::copy_line);
        keys.bind (normal, { { K::text, "d" }, { K::text, "w" } }, C::kill_word_next);
        keys.bind (normal, { { K::text, "d" }, { K::text, "b" } }, C::kill_word_backward);
        keys.bind (normal, { { K::text, "g" }, { K::text, "g" } }, C::buffer_home);
        bind (normal, { K::text, "Y" }, C::copy_line);
      }
    else
      {
        const auto insert = KeyMode::insert;
        for (const auto &[code, command] :
             { std::pair{ 0u, C::mark }, { 1u, C::home }, { 2u, C::left },
                { 5u, C::end }, { 6u, C::right }, { 7u, C::unmark }, { 11u, C::kill_to_end },
               { 14u, C::history_next }, { 16u, C::history_previous },
                { 21u, C::kill_to_start }, { 23u, C::cut }, { 25u, C::yank },
               { 31u, C::undo } })
          bind (insert, { K::control, {}, terminal::ctrl, code }, command);
        bind (insert, { K::escape }, C::unmark);
        bind (insert, { K::text, "w", terminal::alt }, C::copy);
        bind (insert, { K::text, "y", terminal::alt }, C::yank_older);
        bind (insert, { K::text, "f", terminal::alt }, C::word_forward);
        bind (insert, { K::text, "b", terminal::alt }, C::word_backward);
        bind (insert, { K::text, "d", terminal::alt }, C::kill_word_forward);
        bind (insert, { K::backspace, {}, terminal::alt }, C::kill_word_backward);
        bind (insert, { K::text, "<", terminal::alt }, C::buffer_home);
        bind (insert, { K::text, ">", terminal::alt }, C::buffer_end);
        keys.bind (insert, { { K::control, {}, terminal::ctrl, 24 },
                            { K::control, {}, terminal::ctrl, 18 } }, C::redo);
      }
    return keys;
  }
};

enum class SubmitStatus
{
  ignored, continued, rejected, evaluated, unsupported
};

struct Submission
{
  SubmitStatus status;
  Evaluation result;
};

struct RunOptions
{
  std::string prompt = "> ";
  std::string continuation_prompt = "| ";
  bool redraw = false; // Host opts in only on an ANSI-capable terminal.
  std::size_t columns = 80;
  std::size_t rows = 24;
  terminal::RenderMode color = terminal::RenderMode::plain;
  bool show_prompts = true;
  bool submit_final = false; // Opt-in for noninteractive streams without a final LF.
};

enum class RunStatus { eof, stopped, unfinished_input, rejected };

struct EditView
{
  std::string text;
  std::size_t cursor_column = 0;
  std::string_view document; // Borrowed; valid until the next edit.
  std::size_t visible_begin = 0, visible_end = 0, cursor_byte = 0;
  std::optional<std::pair<std::size_t, std::size_t>> selection;
};

using PromptProvider = std::function<std::vector<terminal::StyledText> (
  std::string_view source, std::size_t cursor, bool continuing)>;
// A renderer may style but not change the already measured viewport text.
using RenderProvider = std::function<terminal::StyledText (const EditView &)>;
using EditHandler = std::function<bool (Editor &, Command, std::string_view)>;

struct EventResult
{
  std::optional<Submission> submission;
  bool stopped = false;
  bool interrupted = false;
  std::string notice{};
};

class Session
{
  Editor editor_;
  AnalysisCallbacks analysis_;
  std::shared_ptr<CancellableEvaluator> evaluator_;
  Keymap keymap_ = Keymap::defaults (KeyStyle::emacs);
  KeyMode key_mode_ = KeyMode::insert;
  std::vector<KeyStroke> chord_;
  PromptProvider prompt_;
  RenderProvider render_;
  EditHandler edit_;
  bool continued_ = false;
  bool overflow_ = false;
  bool auto_indent_ = false;

  bool
  insert_newline (bool interactive, bool replace_selection = true)
  {
    auto count = interactive && auto_indent_ && analysis_.indentation
      ? analysis_.indentation (editor_.text (), editor_.cursor ()) : 0;
    auto retained = editor_.text ().size ();
    if (replace_selection)
      if (auto selection = editor_.selection ())
        retained -= selection->second - selection->first;
    const auto available = editor_.limits ().input_bytes - retained;
    if (count >= available)
      return false;
    const auto storage = editor_.storage ();
    std::pmr::string text (count + 1, ' ', storage.resource ());
    text.front () = '\n';
    return replace_selection ? editor_.insert (text)
                             : editor_.replace (editor_.cursor (), editor_.cursor (), text);
  }

  static bool
  blank (std::string_view source)
  {
    return source.find_first_not_of (" \t\r\n\f\v") == std::string_view::npos;
  }

  static std::string
  output_text (std::string_view output, std::string_view line_break)
  {
    std::string safe;
    while (!output.empty ())
      {
        auto end = output.find ('\n');
        safe += terminal::sanitize_text (output.substr (0, end));
        if (end == std::string_view::npos)
          break;
        safe += line_break;
        output.remove_prefix (end + 1);
      }
    return safe;
  }

public:
  explicit Session (AnalysisCallbacks analysis = {}, Evaluator evaluator = {},
                     EditorLimits limits = {}, live::Storage storage = live::Storage{})
    : editor_ (limits, std::move (storage)), analysis_ (std::move (analysis))
  {
    set_evaluator (std::move (evaluator));
  }
  Session (const Session &) = delete;
  Session &operator= (const Session &) = delete;
  Session (Session &&) = default;
  Session &operator= (Session &&) = default;

  Editor &editor () { return editor_; }
  const Editor &editor () const { return editor_; }
  bool continuing () const { return continued_; }
  void set_analysis (AnalysisCallbacks analysis) { analysis_ = std::move (analysis); }
  void
  set_evaluator (Evaluator evaluator)
  {
    if (!evaluator)
      evaluator_.reset ();
    else
      set_cancellable_evaluator (
        [fn = std::move (evaluator)] (std::string_view text, std::stop_token)
        { return fn (text); });
  }
  void
  set_cancellable_evaluator (CancellableEvaluator evaluator)
  {
    if (!evaluator)
      evaluator_.reset ();
    else
      evaluator_ = std::make_shared<CancellableEvaluator> (std::move (evaluator));
  }

  void set_prompt (PromptProvider prompt) { prompt_ = std::move (prompt); }
  void set_renderer (RenderProvider render) { render_ = std::move (render); }
  void set_editor (EditHandler edit) { edit_ = std::move (edit); }
  void set_auto_indent (bool enabled) { auto_indent_ = enabled; }
  Keymap &keymap () { return keymap_; }
  KeyMode key_mode () const { return key_mode_; }

  void
  set_key_style (KeyStyle style)
  {
    keymap_ = Keymap::defaults (style);
    key_mode_ = KeyMode::insert;
    chord_.clear ();
  }

  live::SyntaxStatus
  syntax_status () const
  {
    return analysis_.continuation ? analysis_.continuation (editor_.text ())
                                  : live::SyntaxStatus::complete;
  }

  std::vector<Completion>
  completions () const
  {
    if (!analysis_.complete)
      return {};
    auto results = analysis_.complete (editor_.text (), editor_.cursor ());
    const auto source = editor_.text ();
    const auto budget = editor_.limits ().input_bytes - source.size ();
    std::erase_if (results, [&] (const Completion &item)
      {
        return item.begin > item.end || item.end > source.size ()
               || !Editor::is_boundary (source, item.begin)
               || !Editor::is_boundary (source, item.end)
               || item.text.size () > budget + (item.end - item.begin);
      });
    return results;
  }

  bool
  apply_completion (const Completion &item)
  {
    return editor_.replace (item.begin, item.end, item.text);
  }

  void
  cancel ()
  {
    editor_.clear ();
    continued_ = false;
    overflow_ = false;
    chord_.clear ();
  }

  Submission
  submit (std::stop_token stop = {}, bool interactive = true)
  {
    if (stop.stop_requested ())
      return { SubmitStatus::rejected, { false, "evaluation cancelled" } };
    if (overflow_)
      {
        cancel ();
        return { SubmitStatus::rejected, { false, "input byte limit reached; input discarded" } };
      }
    if (blank (editor_.text ()))
      {
        cancel ();
        return { SubmitStatus::ignored, {} };
      }
    switch (syntax_status ())
      {
      case live::SyntaxStatus::incomplete:
        if (!insert_newline (interactive, false))
          return { SubmitStatus::rejected, { false, "input byte limit reached" } };
        continued_ = true;
        return { SubmitStatus::continued, {} };
      case live::SyntaxStatus::invalid:
        return { SubmitStatus::rejected, { false, "invalid input" } };
      case live::SyntaxStatus::complete:
        break;
      default:
        return { SubmitStatus::rejected, { false, "invalid analysis status" } };
      }
    if (!evaluator_)
      return { SubmitStatus::unsupported, { false, "no evaluator installed" } };
    if (stop.stop_requested ())
      return { SubmitStatus::rejected, { false, "evaluation cancelled" } };
    // Take a stable snapshot and consume the input before entering the
    // effectful callback; even a throwing evaluator cannot retry it by mistake.
    auto evaluate = evaluator_;
    const auto storage = editor_.storage ();
    std::pmr::string source (editor_.text (), storage.resource ());
    editor_.remember (source);
    cancel ();
    return { SubmitStatus::evaluated, (*evaluate) (source, stop) };
  }

  EventResult
  command (Command action, std::string_view text = {}, std::stop_token stop = {})
  {
    if (edit_ && action != Command::submit && action != Command::interrupt
        && action != Command::eof && edit_ (editor_, action, text))
      return {};

    auto edge_kill = [&] (bool to_start)
      {
        if (auto selected = editor_.selection ())
          {
            editor_.kill (selected->first, selected->second);
            return;
          }
        auto cursor = editor_.cursor ();
        if (to_start)
          editor_.move_home ();
        else
          editor_.move_end ();
        auto edge = editor_.cursor ();
        editor_.move_to (cursor);
        if (!to_start && edge == cursor && edge < editor_.text ().size ())
          edge = terminal::next_glyph (editor_.text (), edge).end;
        editor_.kill (std::min (cursor, edge), std::max (cursor, edge));
      };

    auto word_target = [&] (Command motion)
      {
        const auto source = editor_.text ();
        const auto cursor = editor_.cursor ();
        enum class Run { space, word, punctuation };
        auto kind = [&] (std::size_t at)
          {
            const auto c = static_cast<unsigned char> (source[at]);
            if (c == ' ' || (c >= '\t' && c <= '\r'))
              return Run::space;
            if (c >= 0x80 || c == '_' || (c >= '0' && c <= '9')
                || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
              return Run::word;
            return Run::punctuation;
          };
        auto next = [&] (std::size_t at)
          { return terminal::next_glyph (source, at).end; };
        if (motion == Command::word_backward)
          {
            std::size_t start = 0;
            auto previous = Run::space;
            for (std::size_t at = 0; at < cursor; at = next (at))
              {
                auto current = kind (at);
                if (current != Run::space && current != previous)
                  start = at;
                previous = current;
              }
            return start;
          }
        auto at = cursor;
        auto skip_space = [&]
          {
            while (at < source.size () && kind (at) == Run::space)
              at = next (at);
          };
        if (motion == Command::word_forward)
          skip_space ();
        if (at < source.size () && kind (at) != Run::space)
          {
            const auto run = kind (at);
            do at = next (at);
            while (at < source.size () && kind (at) == run);
          }
        if (motion == Command::word_next)
          skip_space ();
        return at;
      };

    auto first_nonblank = [&]
      {
        editor_.move_home ();
        const auto source = editor_.text ();
        auto at = editor_.cursor ();
        while (at < source.size () && (source[at] == ' ' || source[at] == '\t'))
          at = terminal::next_glyph (source, at).end;
        editor_.move_to (at);
      };

    switch (action)
      {
      case Command::none: break;
      case Command::insert:
        if (!text.empty () && !editor_.insert (text))
          overflow_ = true;
        break;
      case Command::newline:
        if (!insert_newline (true))
          overflow_ = true;
        break;
      case Command::submit: return { submit (stop) };
      case Command::left: editor_.move_left (); break;
      case Command::right: editor_.move_right (); break;
      case Command::up:
        if (!editor_.move_line (false))
          editor_.history_previous ();
        break;
      case Command::down:
        if (!editor_.move_line (true))
          editor_.history_next ();
        break;
      case Command::home: editor_.move_home (); break;
      case Command::end: editor_.move_end (); break;
      case Command::buffer_home: editor_.move_to (0); break;
      case Command::buffer_end: editor_.move_to (editor_.text ().size ()); break;
      case Command::first_nonblank: first_nonblank (); break;
      case Command::word_forward:
      case Command::word_backward:
      case Command::word_next:
        editor_.move_to (word_target (action));
        break;
      case Command::kill_word_forward:
      case Command::kill_word_backward:
      case Command::kill_word_next:
        if (auto selected = editor_.selection ())
          editor_.kill (selected->first, selected->second);
        else
          {
            auto cursor = editor_.cursor ();
            auto target = word_target (action == Command::kill_word_forward
              ? Command::word_forward : action == Command::kill_word_backward
              ? Command::word_backward : Command::word_next);
            editor_.kill (std::min (cursor, target), std::max (cursor, target));
          }
        break;
      case Command::backspace: editor_.erase_before (); break;
      case Command::delete_glyph: editor_.erase_after (); break;
      case Command::history_previous: editor_.history_previous (); break;
      case Command::history_next: editor_.history_next (); break;
      case Command::complete:
        {
          auto choices = completions ();
          if (choices.size () == 1)
            apply_completion (choices.front ());
          else if (!choices.empty ())
            {
              EventResult result;
              constexpr std::size_t max_choices = 32, max_label_bytes = 1024;
              for (std::size_t i = 0; i < std::min (max_choices, choices.size ()); ++i)
                {
                  const auto &choice = choices[i];
                  auto label = std::string_view (choice.label.empty () ? choice.text : choice.label);
                  result.notice += terminal::sanitize_text (label.substr (0, max_label_bytes));
                  result.notice += '\n';
                }
              if (choices.size () > max_choices)
                result.notice += "...\n";
              return result;
            }
          break;
        }
      case Command::mark:
        if (editor_.mark ())
          editor_.clear_mark ();
        else
          editor_.set_mark ();
        break;
      case Command::unmark: editor_.clear_mark (); break;
      case Command::cut:
      case Command::copy:
        if (auto selected = editor_.selection ())
          editor_.kill (selected->first, selected->second, action == Command::cut);
        break;
      case Command::kill_to_end: edge_kill (false); break;
      case Command::kill_to_start: edge_kill (true); break;
      case Command::kill_line:
      case Command::copy_line:
        {
          auto cursor = editor_.cursor ();
          editor_.move_home ();
          auto begin = editor_.cursor ();
          editor_.move_end ();
          auto end = editor_.cursor ();
          if (end < editor_.text ().size ())
            end = terminal::next_glyph (editor_.text (), end).end;
          editor_.move_to (cursor);
          editor_.kill (begin, end, action == Command::kill_line);
          break;
        }
      case Command::yank: editor_.yank (); break;
      case Command::yank_older: editor_.yank (true); break;
      case Command::undo: editor_.undo (); break;
      case Command::redo: editor_.redo (); break;
      case Command::clear: cancel (); break;
      case Command::interrupt:
        cancel ();
        return { {}, false, true };
      case Command::eof:
        if (overflow_)
          return { submit (stop) };
        if (editor_.text ().empty ())
          return { {}, true, false };
        editor_.erase_after ();
        break;
      case Command::vi_normal:
        key_mode_ = KeyMode::normal;
        editor_.move_left ();
        break;
      case Command::vi_append: editor_.move_right (); [[fallthrough]];
      case Command::vi_insert: key_mode_ = KeyMode::insert; break;
      case Command::vi_insert_home:
        first_nonblank ();
        key_mode_ = KeyMode::insert;
        break;
      case Command::vi_append_end:
        editor_.move_end ();
        key_mode_ = KeyMode::insert;
        break;
      }
    return {};
  }

  EventResult
  handle (const terminal::InputEvent &event, std::stop_token stop = {})
  {
    using K = terminal::Key;
    // Bracketed pastes are inserted atomically as data; their contents never
    // enter the keymap or trigger submission, even when multiline.
    if (event.key == K::paste)
      {
        chord_.clear ();
        return command (Command::insert, event.text, stop);
      }
    if (event.key == K::unknown || event.key == K::focus_in
        || event.key == K::focus_out || event.key == K::mouse)
      return {};
    if (event.key == K::control && event.code == 3)
      chord_.clear ();

    KeyStroke stroke{ event.key, event.text, event.modifiers, event.code };
    const bool in_chord = !chord_.empty ();
    chord_.push_back (stroke);
    auto match = keymap_.match (key_mode_, chord_);
    if (!match.command && !match.pending && in_chord)
      {
        chord_.assign (1, stroke);
        match = keymap_.match (key_mode_, chord_);
      }
    if (match.pending)
      return {};
    chord_.clear ();
    if (match.command)
      return command (*match.command, event.text, stop);
    if (event.key == K::text && key_mode_ == KeyMode::insert
        && !(event.modifiers & (terminal::alt | terminal::ctrl)))
      return command (Command::insert, event.text, stop);
    return {};
  }

private:
  // Draw a single physical line, reserving the last terminal column in the
  // caller to avoid autowrap. The viewport stays on whole glyphs.
  static EditView
  line_view (std::string_view source, std::size_t cursor,
             std::string_view prompt, std::size_t width)
  {
    EditView view;
    std::size_t remaining = width;
    auto add = [&] (std::string_view glyph, std::size_t &budget)
      {
        auto safe = terminal::sanitize_text (glyph);
        auto cells = terminal::display_width (safe);
        if (cells > budget)
          return false;
        view.text += safe;
        budget -= cells;
        return true;
      };
    for (std::size_t begin = 0; begin < prompt.size ();)
      {
        auto end = terminal::next_glyph (prompt, begin).end;
        if (!add (prompt.substr (begin, end - begin), remaining))
          break;
        begin = end;
      }
    // Keep only the glyphs nearest to the cursor when its prefix overflows.
    struct Cell { std::string text; std::size_t width, begin; };
    std::deque<Cell> prefix;
    std::size_t used = 0;
    for (std::size_t begin = 0; begin < cursor;)
      {
        auto end = terminal::next_glyph (source, begin).end;
        auto safe = terminal::sanitize_text (source.substr (begin, end - begin));
        auto cells = terminal::display_width (safe);
        if (cells > remaining)
          {
            prefix.clear ();
            used = 0;
          }
        else
          {
            while (cells > remaining - used)
              {
                used -= prefix.front ().width;
                prefix.pop_front ();
              }
            prefix.push_back ({ std::move (safe), cells, begin });
            used += cells;
          }
        begin = end;
      }
    view.visible_begin = prefix.empty () ? cursor : prefix.front ().begin;
    view.visible_end = cursor;
    for (const auto &cell : prefix)
      view.text += cell.text;
    remaining -= used;
    view.cursor_column = width - remaining;
    for (std::size_t begin = cursor; begin < source.size ();)
      {
        auto end = terminal::next_glyph (source, begin).end;
        if (!add (source.substr (begin, end - begin), remaining))
          break;
        begin = end;
        view.visible_end = end;
      }
    return view;
  }

  std::vector<terminal::StyledText>
  prompt_lines (const RunOptions &options) const
  {
    if (!options.show_prompts)
      return {};
    auto plain = [] (std::string_view text)
      {
        std::vector<terminal::StyledText> lines;
        for (;;)
          {
            auto newline = text.find ('\n');
            lines.emplace_back ().text (text.substr (0, newline));
            if (newline == std::string_view::npos)
              break;
            text.remove_prefix (newline + 1);
          }
        return lines;
      };
    auto lines = prompt_ ? prompt_ (editor_.text (), editor_.cursor (), continued_)
                         : plain (continued_ ? options.continuation_prompt : options.prompt);
    for (const auto &line : lines)
      if (line.render ().find ('\n') != std::string::npos)
        throw std::invalid_argument ("each REPL prompt element must be one line");
    return lines;
  }

  struct Frame
  {
    std::vector<std::string> lines;
    std::size_t cursor_row = 0, cursor_column = 0;
  };

  Frame
  frame (const RunOptions &options) const
  {
    const auto width = std::clamp<std::size_t> (options.columns, 2, 4096) - 1;
    const auto rows = std::clamp<std::size_t> (options.rows, 1, 512);
    auto prompts = prompt_lines (options);
    Frame result;
    auto format_prompt = [&] (const terminal::StyledText &value, std::size_t limit)
      {
        if (value.width () <= limit)
          return std::pair{ value.render (options.color), value.width () };
        auto clipped = line_view (value.render (), 0, {}, limit);
        return std::pair{ clipped.text, terminal::display_width (clipped.text) };
      };
    for (std::size_t i = 0; i + 1 < prompts.size () && i + 1 < rows; ++i)
      result.lines.push_back (format_prompt (prompts[i], width).first);

    const auto source = editor_.text ();
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    auto cursor = editor_.cursor ();
    std::size_t active = 0;
    for (std::size_t start = 0;;)
      {
        if (start <= cursor)
          active = lines.size ();
        auto newline = source.find ('\n', start);
        if (newline == std::string_view::npos)
          {
            lines.emplace_back (start, source.size ());
            break;
          }
        auto end = newline > start && source[newline - 1] == '\r' ? newline - 1 : newline;
        lines.emplace_back (start, end);
        start = newline + 1;
      }
    const auto available = rows - result.lines.size ();
    const auto first = active >= available ? active + 1 - available : 0;
    const auto last = std::min (lines.size (), first + available);
    for (std::size_t index = first; index < last; ++index)
      {
        std::pair<std::string, std::size_t> prompt;
        if (index == first && !prompts.empty ())
          prompt = format_prompt (prompts.back (), width - 1);
        auto [begin, end] = lines[index];
        auto position = index == active ? std::min (cursor, end) : begin;
        auto view = line_view (source.substr (begin, end - begin), position - begin,
                               {}, width - prompt.second);
        view.document = source;
        view.visible_begin += begin;
        view.visible_end += begin;
        view.cursor_byte = cursor;
        view.selection = editor_.selection ();
        std::string styled;
        if (render_)
          {
            auto output = render_ (view);
            if (output.render () != view.text)
              throw std::invalid_argument ("REPL renderer must preserve visible text");
            styled = output.render (options.color);
          }
        else if (auto picked = view.selection; picked && options.color != terminal::RenderMode::plain
                 && picked->first < view.visible_end && picked->second > view.visible_begin)
          {
            auto first_marked = std::max (picked->first, view.visible_begin);
            auto last_marked = std::min (picked->second, view.visible_end);
            styled = terminal::sanitize_text (source.substr (view.visible_begin, first_marked - view.visible_begin));
            styled += "\x1b[7m";
            styled += terminal::sanitize_text (source.substr (first_marked, last_marked - first_marked));
            styled += "\x1b[0m";
            styled += terminal::sanitize_text (source.substr (last_marked, view.visible_end - last_marked));
          }
        else
          styled = std::move (view.text);
        if (index == active)
          {
            result.cursor_row = result.lines.size ();
            result.cursor_column = prompt.second + view.cursor_column;
          }
        result.lines.push_back (std::move (prompt.first) + styled);
      }
    return result;
  }

  template <class Write, class Read, class Resize>
  RunStatus
  drive (Write write, Read read, Resize resize, RunOptions options, std::stop_token stop)
  {
    chord_.clear ();
    if (options.color == terminal::RenderMode::debug)
      options.color = terminal::RenderMode::plain;
    terminal::InputDecoder decoder (editor_.limits ().input_bytes);
    std::array<char, 1024> buffer{};
    const std::string_view line_break = options.redraw ? "\r\n" : "\n";
    std::size_t old_rows = 0, old_cursor = 0;
    auto move = [&] (std::size_t count, char direction)
      {
        if (count)
          write ("\x1b[" + std::to_string (count) + direction);
      };
    auto finish_frame = [&]
      {
        if (!old_rows)
          return;
        move (old_rows - 1 - old_cursor, 'B');
        write (line_break);
        old_rows = old_cursor = 0;
      };
    auto draw = [&] (bool new_prompt)
      {
        if (!options.redraw)
          {
            if (new_prompt)
              {
                auto prompts = prompt_lines (options);
                for (std::size_t i = 0; i < prompts.size (); ++i)
                  {
                    write (prompts[i].render ());
                    if (i + 1 < prompts.size ())
                      write (line_break);
                  }
              }
            return;
          }
        auto next = frame (options);
        move (old_cursor, 'A');
        const auto count = std::max (old_rows, next.lines.size ());
        for (std::size_t row = 0; row < count; ++row)
          {
            write ("\r\x1b[2K");
            if (row < next.lines.size ())
              write (next.lines[row]);
            if (row + 1 < count)
              write (line_break);
          }
        move (count - 1 - next.cursor_row, 'A');
        write ("\r");
        move (next.cursor_column, 'C');
        old_rows = next.lines.size ();
        old_cursor = next.cursor_row;
      };
    auto report = [&] (const EventResult &result)
      {
        if (result.interrupted)
          {
            finish_frame ();
            write ("^C");
            write (line_break);
          }
        if (result.submission && result.submission->status != SubmitStatus::continued)
          {
            finish_frame ();
            const auto &output = result.submission->result.output;
            if (!output.empty ())
              {
                write (output_text (output, line_break));
                if (output.back () != '\n')
                  write (line_break);
              }
          }
        if (!result.notice.empty ())
          {
            finish_frame ();
            write (output_text (result.notice, line_break));
          }
      };
    resize (options);
    draw (true);
    bool cr = false;
    for (;;)
      {
        if (stop.stop_requested ())
          {
            finish_frame ();
            return RunStatus::stopped;
          }
        auto count = read (std::span (buffer));
        if (stop.stop_requested ())
          {
            finish_frame ();
            return RunStatus::stopped;
          }
        bool needs_draw = resize (options);
        if (count && *count > buffer.size ())
          throw std::runtime_error ("invalid REPL read count");
        std::vector<terminal::InputEvent> events;
        if (!count)
          {
            if (decoder.escape_pending ())
              events = decoder.flush ();
          }
        else if (*count == 0)
          events = decoder.flush ();
        else
          {
            // Coalesce CR/LF key events, never bytes inside a bracketed paste.
            for (char byte : std::span (buffer).first (*count))
              {
                auto next = decoder.feed (std::string_view (&byte, 1));
                const bool enter = next.size () == 1 && next.front ().key == terminal::Key::enter;
                if (!(cr && byte == '\n' && enter))
                  for (auto &event : next)
                    events.push_back (std::move (event));
                cr = byte == '\r' && enter;
              }
          }
        for (const auto &event : events)
          {
            if (stop.stop_requested ())
              {
                finish_frame ();
                return RunStatus::stopped;
              }
            auto result = handle (event, stop);
            if (result.stopped)
              {
                finish_frame ();
                return RunStatus::stopped;
              }
            report (result);
            const bool new_prompt = result.interrupted || result.submission.has_value ()
                                    || !result.notice.empty ();
            needs_draw = true;
            if (options.redraw || new_prompt)
              {
                draw (new_prompt);
                needs_draw = false;
              }
          }
        if (count && *count == 0)
          {
            if (options.submit_final && (!editor_.text ().empty () || overflow_))
              {
                auto result = command (Command::submit, {}, stop);
                report (result);
                if (result.submission && (result.submission->status == SubmitStatus::rejected
                                          || result.submission->status == SubmitStatus::unsupported))
                  return RunStatus::rejected;
              }
            finish_frame ();
            return editor_.text ().empty () && !overflow_ ? RunStatus::eof
                                                         : RunStatus::unfinished_input;
          }
        if (needs_draw)
          draw (false);
      }
  }

public:
  EditView
  edit_view (std::string_view prompt, std::size_t width) const
  {
    auto view = line_view (editor_.text (), editor_.cursor (), prompt, width);
    view.document = editor_.text ();
    view.cursor_byte = editor_.cursor ();
    view.selection = editor_.selection ();
    return view;
  }

  template <IOBackend Backend>
  RunStatus
  run (IO<Backend> &io, RunOptions options = {}, std::stop_token stop = {})
  {
    return drive ([&] (std::string_view bytes)
                  { io.write_all (std::as_bytes (std::span (bytes))); },
                  [&] (std::span<char> bytes) -> std::optional<std::size_t>
                  { return io.read (std::as_writable_bytes (bytes)); },
                  [] (RunOptions &) { return false; }, std::move (options), stop);
  }

  // Batch input is source, not key events. Tabs and escape bytes are preserved;
  // CR immediately before LF is normalized as a line ending.
  template <IOBackend Backend>
  RunStatus
  run_lines (IO<Backend> &io, bool submit_final = true, std::stop_token stop = {})
  {
    std::array<char, 1024> buffer{};
    const auto storage = editor_.storage ();
    std::pmr::string pending (storage.resource ());
    auto append = [&] (char byte)
      {
        if (pending.size () < editor_.limits ().input_bytes - editor_.text ().size ())
          pending += byte;
        else
          overflow_ = true;
      };
    auto flush = [&]
      {
        editor_.clear_mark ();
        editor_.move_to (editor_.text ().size ());
        if (!overflow_ && !editor_.insert (pending))
          overflow_ = true;
        pending.clear ();
      };
    auto evaluate_line = [&]
      {
        flush ();
        auto output = submit (stop, false);
        auto safe = output_text (output.result.output, "\n");
        if (!safe.empty ())
          {
            if (safe.back () != '\n')
              safe += '\n';
            io.write_all (std::as_bytes (std::span (safe)));
          }
        return output.status != SubmitStatus::rejected && output.status != SubmitStatus::unsupported;
      };
    bool cr = false;
    for (;;)
      {
        if (stop.stop_requested ())
          return RunStatus::stopped;
        auto count = io.read (std::as_writable_bytes (std::span (buffer)));
        if (stop.stop_requested ())
          return RunStatus::stopped;
        if (!count)
          break;
        for (char byte : std::span (buffer).first (count))
          {
            if (stop.stop_requested ())
              return RunStatus::stopped;
            if (cr && byte != '\n')
              append ('\r');
            cr = byte == '\r';
            if (byte == '\n')
              {
                if (!evaluate_line ())
                  return RunStatus::rejected;
              }
            else if (!cr)
              append (byte);
          }
      }
    if (cr)
      append ('\r');
    if (submit_final && (!pending.empty () || overflow_))
      {
        if (!evaluate_line ())
          return RunStatus::rejected;
      }
    else
      flush ();
    return editor_.text ().empty () && !overflow_ ? RunStatus::eof
                                                 : RunStatus::unfinished_input;
  }

  RunStatus
  run_terminal (int input_fd, int output_fd, RunOptions options = {},
                std::stop_token stop = {})
  {
#if LIBDSL_LIVE_POSIX
    auto backend = terminal::descriptor_backend (input_fd, output_fd);
    if (!backend.interactive ())
      throw std::invalid_argument ("REPL terminal requires terminal descriptors");
    terminal::RawMode raw (input_fd);
    if (!raw.active ())
      throw std::system_error (raw.error (), std::generic_category (), "REPL raw mode");
    options.redraw = true;
    terminal::ResizeMonitor size;
    struct DescriptorIO
    {
      terminal::Backend &backend;
      std::size_t read (std::span<std::byte>) { throw std::logic_error ("timed reader required"); }
      std::size_t write (std::span<const std::byte> data)
      {
        auto bytes = std::string_view (reinterpret_cast<const char *> (data.data ()), data.size ());
        for (;;)
          {
            auto result = backend.write (bytes);
            if (result.status == terminal::IOStatus::interrupted)
              continue;
            if (result.status != terminal::IOStatus::data)
              throw std::system_error (result.error, std::generic_category (), "REPL terminal write");
            return result.bytes;
          }
      }
    } descriptor{ backend };
    dsl::IO io (descriptor);
    auto write = [&] (std::string_view data)
      { io.write_all (std::as_bytes (std::span (data))); };
    auto paste_mode = [&] (bool enabled)
      { write (terminal::mode_sequence (terminal::PrivateMode::bracketed_paste, enabled)); };
    RunStatus status;
    try
      {
        paste_mode (true);
        status = drive (write, [&] (std::span<char> bytes) -> std::optional<std::size_t>
          {
            for (;;)
              {
                auto read = backend.read (bytes, std::chrono::milliseconds (45));
                switch (read.status)
                  {
                  case terminal::IOStatus::data: return read.bytes;
                  case terminal::IOStatus::eof: return 0;
                  case terminal::IOStatus::timeout:
                  case terminal::IOStatus::interrupted: return std::nullopt;
                  default: throw std::system_error (read.error, std::generic_category (),
                                                     "REPL terminal read");
                  }
              }
          }, [&] (RunOptions &run_options)
          {
            if (auto dimensions = size.observe (backend))
              {
                run_options.rows = dimensions->rows;
                run_options.columns = dimensions->columns;
                return true;
              }
            return false;
          }, std::move (options), stop);
        paste_mode (false);
      }
    catch (...)
      {
        try { paste_mode (false); } catch (...) {}
        throw;
      }
    if (!raw.restore ())
      throw std::system_error (raw.error (), std::generic_category (), "REPL restore terminal");
    return status;
#else
    (void) input_fd;
    (void) output_fd;
    (void) options;
    (void) stop;
    throw std::system_error (std::make_error_code (std::errc::operation_not_supported),
                             "native REPL terminal");
#endif
  }
};
} // namespace repl

struct REPL
{
  template <class Derived> struct Mixin
  {
    using ReplEditor = repl::Editor;
    using ReplSession = repl::Session;

    static repl::Session
    repl_session (repl::AnalysisCallbacks analysis = {},
                  repl::Evaluator evaluator = {}, repl::EditorLimits limits = {})
    {
      return repl::Session (std::move (analysis), std::move (evaluator), limits);
    }
  };
};

// Pure, cursor-aware analysis for interactive editors. Byte offsets refer to
// the original source, never to terminal escape sequences or display cells.
namespace repl
{
enum class HighlightKind
{
  symbol, keyword, number, string, comment, directive, delimiter, error, match
};

struct HighlightSpan
{
  live::SourceSpan span;
  HighlightKind kind;
  std::size_t depth = 0;
};

struct Delimiter
{
  live::SourceSpan span;
  char character;
  bool opening;
  std::optional<live::SourceSpan> mate;
  std::size_t depth = 0;
};

enum class CompletionKind { symbol, directive, argument, suppressed };
enum class CompletionSource { lexical, grammatical, semantic, filesystem, directive, option, script };

struct CompletionContext
{
  CompletionKind kind = CompletionKind::symbol;
  live::SourceSpan replacement;
  std::string_view prefix;
  std::string_view directive;
  std::size_t argument = 0; // Zero-based directive argument index.
  std::vector<std::string> preceding_arguments;
};

struct CompletionResult
{
  // Views in context borrow the input source.
  CompletionContext context;
  std::vector<std::string> candidates;
  struct Detail
  {
    std::string text, description;
    CompletionSource source = CompletionSource::lexical;
    int priority = 0;
  };
  std::vector<Detail> details;
};

struct AnalysisResult
{
  live::SyntaxStatus syntax = live::SyntaxStatus::complete;
  std::vector<HighlightSpan> highlights;
  std::vector<Delimiter> delimiters;
  std::optional<std::size_t> selected_delimiter;
  std::vector<live::Diagnostic> diagnostics;
  std::size_t indentation = 0; // Desired indentation of the cursor's line.
  std::size_t next_indentation = 0; // Desired indentation after a newline at cursor.
  bool cancelled = false;

  std::optional<live::SourceSpan> match() const noexcept
  {
    if (!selected_delimiter || *selected_delimiter >= delimiters.size())
      return std::nullopt;
    return delimiters[*selected_delimiter].mate;
  }

  // Previous/next structural token, strictly before/after the cursor.
  std::optional<live::SourceSpan> navigate(std::size_t cursor, bool forward) const noexcept
  {
    std::optional<live::SourceSpan> nearest;
    for (const auto& item : delimiters)
      if ((forward && item.span.begin > cursor && (!nearest || item.span.begin < nearest->begin))
          || (!forward && item.span.begin < cursor && (!nearest || item.span.begin > nearest->begin)))
        nearest = item.span;
    return nearest;
  }
};

// The default lexical profile mirrors S-expression syntax. Delimiter pairs
// and quoting can be adjusted for other languages without requiring a parser.
struct AnalysisConfig
{
  std::string delimiters = "()";
  std::vector<std::pair<std::string, std::string>> blocks;
  std::string quotes = "\"";
  std::string line_comment = ";";
  char directive_prefix = ':';
  std::size_t indent_width = 2;
  std::size_t max_source_bytes = 1024 * 1024;
  std::size_t max_tokens = 65536;
  std::size_t max_depth = 128;
  std::size_t max_candidates = 128;
  std::size_t max_candidate_bytes = 4096;
  bool align_arguments = false;
  std::vector<std::string> keywords;
  std::vector<std::string> symbols;
  std::vector<std::string> directives;
  std::map<std::string, HighlightKind, std::less<>> channels, patterns, annotations;

  static AnalysisConfig from_sexpr(const SExpr& form);
  static AnalysisConfig parse(std::string_view text);
};

// A monotonically replaced editor snapshot. Publish only a current ticket;
// on replacement, work using the previous ticket is also cancelled.
class AnalysisRevision
{
  std::atomic<std::shared_ptr<const std::stop_source>> current_;

public:
  std::stop_token replace()
  {
    auto next = std::make_shared<std::stop_source>();
    auto old = current_.exchange(next);
    if (old)
      old->request_stop();
    return next->get_token();
  }

  bool current(std::stop_token ticket) const noexcept
  {
    auto current = current_.load();
    return current && ticket == current->get_token() && !ticket.stop_requested();
  }
};

class Analysis
{
public:
  // Providers are explicit native callbacks. The output collector enforces
  // prefix filtering, uniqueness, text validity and result budgets.
  using CompletionSink = std::function<bool(std::string_view)>;
  using CompletionProvider =
      std::function<void(const CompletionContext&, const CompletionSink&)>;
  using DetailSink = std::function<bool(std::string_view, std::string_view, int)>;
  using DetailProvider = std::function<void(const CompletionContext&, const DetailSink&)>;
  using HighlightProvider =
      std::function<std::optional<HighlightKind>(std::string_view, live::SourceSpan)>;
  using SyntaxProvider = std::function<live::SyntaxStatus(std::string_view)>;
  using GrammarProvider = std::function<live::CursorAnalysis(std::string_view, std::size_t)>;
  using CancellableGrammarProvider =
      std::function<live::CursorAnalysis(std::string_view, std::size_t, std::stop_token)>;
  using IndentProvider =
      std::function<std::optional<std::size_t>(std::string_view, std::size_t, std::size_t)>;

private:
  struct Token
  {
    live::SourceSpan span;
    HighlightKind kind;
    bool unfinished = false;
    int structural = 0;
    bool command = false;
  };

  AnalysisConfig config_;
  std::array<int, 128> pairs_{};
  std::unordered_map<std::string_view, int> blocks_; // Points into config_.
  std::unordered_set<std::string_view> keyword_views_;
  std::vector<std::pair<CompletionSource, DetailProvider>> completion_;
  HighlightProvider highlight_;
  SyntaxProvider syntax_;
  CancellableGrammarProvider grammar_;
  IndentProvider indent_;
  std::shared_ptr<const PEGDefinition> peg_;
  live::SyntaxLimits peg_limits_;
  std::map<std::string, HighlightKind, std::less<>> peg_channels_, peg_patterns_, annotations_;

  static bool space(unsigned char c) noexcept { return c <= ' '; }
  static bool newline(char c) noexcept { return c == '\n' || c == '\r'; }
  static bool boundary(std::string_view source, std::size_t byte) noexcept
  {
    return byte == source.size() || (static_cast<unsigned char>(source[byte]) & 0xc0) != 0x80;
  }
  static bool punctuation(unsigned char c) noexcept
  {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@')
           || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
  }
  static bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
  static bool numeric(std::string_view word) noexcept
  {
    std::size_t i = 0;
    if (!word.empty() && (word[0] == '+' || word[0] == '-'))
      ++i;
    if (word.size() - i > 2 && word[i] == '0'
        && (word[i + 1] == 'x' || word[i + 1] == 'X'))
      {
        for (i += 2; i < word.size(); ++i)
          if (!digit(word[i]) && !(word[i] >= 'a' && word[i] <= 'f')
              && !(word[i] >= 'A' && word[i] <= 'F'))
            return false;
        return true;
      }
    auto digits = i;
    while (i < word.size() && digit(word[i]))
      ++i;
    bool any = i != digits;
    if (i < word.size() && word[i] == '.')
      {
        digits = ++i;
        while (i < word.size() && digit(word[i]))
          ++i;
        any = any || i != digits;
      }
    if (any && i < word.size() && (word[i] == 'e' || word[i] == 'E'))
      {
        ++i;
        if (i < word.size() && (word[i] == '+' || word[i] == '-'))
          ++i;
        digits = i;
        while (i < word.size() && digit(word[i]))
          ++i;
        any = i != digits;
      }
    return any && i == word.size();
  }

  // Returns the signed pair identifier, positive for opening characters.
  int pair(unsigned char c) const noexcept { return c < pairs_.size() ? pairs_[c] : 0; }
  int structural(std::string_view word) const
  {
    if (word.size() == 1)
      if (const auto id = pair(word.front()))
        return id;
    const auto item = blocks_.find(word);
    return item != blocks_.end() ? item->second : 0;
  }
  bool quote(char c) const noexcept { return config_.quotes.find(c) != std::string::npos; }
  bool comment(std::string_view source, std::size_t i) const noexcept
  {
    return !config_.line_comment.empty() && source.substr(i).starts_with(config_.line_comment);
  }
  static std::string_view slice(std::string_view source, live::SourceSpan span)
  {
    return source.substr(span.begin, span.end - span.begin);
  }

  static std::size_t line_start(std::string_view source, std::size_t cursor) noexcept
  {
    while (cursor && !newline(source[cursor - 1]))
      --cursor;
    return cursor;
  }

  void check_input(std::string_view source, std::size_t cursor) const
  {
    if (cursor > source.size())
      throw std::out_of_range("REPL analysis cursor outside source");
    if (source.size() > config_.max_source_bytes)
      throw std::length_error("REPL analysis source budget exceeded");
    if (!boundary(source, cursor))
      throw std::invalid_argument("REPL analysis cursor splits a UTF-8 sequence");
  }

  // In directive arguments, quoted strings still shield whitespace and
  // comments, but delimiters are not attributed to the language grammar.
  std::vector<Token> tokens(std::string_view source, std::stop_token stop) const
  {
    std::vector<Token> result;
    bool first = true; // Directives are recognized only at the start of a submission.
    bool directive = false;
    for (std::size_t i = 0; i < source.size();)
      {
        if (stop.stop_requested())
          return {};
        auto c = source[i];
        if (space(c))
          {
            ++i;
            continue;
          }
        if (result.size() >= config_.max_tokens)
          throw std::length_error("REPL analysis token budget exceeded");
        const auto start = i++;
        auto kind = HighlightKind::symbol;
        bool unfinished = false;
        int structure = 0;
        if (comment(source, start))
          {
            while (i < source.size() && !newline(source[i]) && !stop.stop_requested())
              ++i;
            result.push_back({{start, i}, HighlightKind::comment});
            continue;
          }
        if (quote(c))
          {
            kind = HighlightKind::string;
            unfinished = true;
            while (i < source.size() && !stop.stop_requested())
              {
                const auto ch = source[i++];
                if (ch == c)
                  {
                    unfinished = false;
                    break;
                  }
                if (ch == '\\' && i < source.size())
                  ++i;
              }
          }
        else if (!directive && pair(c))
          {
            kind = HighlightKind::delimiter;
            structure = pair(c);
          }
        else
          {
            if (first && config_.directive_prefix && c == config_.directive_prefix)
              {
                directive = true;
                kind = HighlightKind::directive;
              }
            while (i < source.size() && !space(source[i])
                   && (directive || !pair(source[i])) && !comment(source, i)
                   && !stop.stop_requested())
              ++i;
            if (!directive)
              {
                const auto word = source.substr(start, i - start);
                if ((structure = structural(word)))
                  kind = HighlightKind::delimiter;
                else if (keyword_views_.contains(word))
                  kind = HighlightKind::keyword;
                else if (numeric(word))
                  kind = HighlightKind::number;
              }
          }
        first = false;
        result.push_back({{start, i}, kind, unfinished, structure, kind == HighlightKind::directive});
      }
    return result;
  }

  std::vector<Token> lex(std::string_view source, std::stop_token stop,
                         std::optional<live::LexicalAnalysis>* details = nullptr) const
  {
    auto base = tokens(source, stop);
    if (!peg_ || stop.stop_requested() || std::any_of(base.begin(), base.end(),
          [](const Token& token) { return token.command; }))
      return base;
    auto limits = peg_limits_;
    limits.stop = stop.stop_possible() ? stop : limits.stop;
    limits.tokens = std::min(limits.tokens, config_.max_tokens);
    auto lexical = live::analyze_peg(*peg_, source, source.size(), limits);
    std::vector<Token> result;
    result.reserve(lexical.tokens.size());
    std::size_t consumed = 0;
    for (const auto& token : lexical.tokens)
      {
        if (stop.stop_requested())
          return {};
        if (token.overlay || token.negated || token.span.empty())
          continue;
        if (!boundary(source, token.span.begin) || !boundary(source, token.span.end)
            || token.span.begin < consumed)
          return base;
        const auto word = slice(source, token.span);
        consumed = token.span.end;
        auto kind = HighlightKind::symbol;
        bool styled = false;
        for (const auto& name : token.annotations)
          if (const auto hint = annotations_.find(name); hint != annotations_.end())
            {
              kind = hint->second;
              styled = true;
              break;
            }
        if (!styled)
          {
            if (const auto pattern = peg_patterns_.find(token.kind); pattern != peg_patterns_.end())
              kind = pattern->second;
            else if (const auto channel = peg_channels_.find(token.channel); channel != peg_channels_.end())
              kind = channel->second;
            else if (token.ignored && std::all_of(word.begin(), word.end(), space))
              continue;
            else if (token.ignored)
              kind = HighlightKind::comment;
            else if (keyword_views_.contains(word))
              kind = HighlightKind::keyword;
            else if (numeric(word))
              kind = HighlightKind::number;
            else if (quote(word.front()))
              kind = HighlightKind::string;
          }
        int id = 0;
        if (token.error)
          kind = HighlightKind::error;
        else if (kind != HighlightKind::comment && kind != HighlightKind::string
                 && (id = structural(word)))
          kind = HighlightKind::delimiter;
        else if (kind == HighlightKind::delimiter)
          kind = HighlightKind::symbol;
        result.push_back({token.span, kind, token.partial, id});
      }
    if (lexical.status != live::SyntaxStatus::complete)
      for (const auto& token : base)
        if (token.span.begin >= consumed && result.size() < config_.max_tokens)
          result.push_back(token);
    if (details)
      *details = std::move(lexical);
    return result;
  }

  CompletionContext context(std::string_view source, std::size_t cursor,
                            const std::vector<Token>& lexed) const
  {
    const auto start = line_start(source, cursor);
    CompletionContext result{CompletionKind::symbol, {cursor, cursor}, {}, {}, 0, {}};
    for (const auto& token : lexed)
      {
        const auto begin = token.span.begin;
        const auto end = token.span.end;
        if (begin > cursor)
          break;
        const auto kind = token.kind;
        const auto touches = cursor <= end;
        if (kind == HighlightKind::comment && begin <= cursor && touches)
          {
            result.kind = CompletionKind::suppressed;
            break;
          }
        if (kind == HighlightKind::string && begin < cursor
            && (cursor < end || (touches && token.unfinished)))
          {
            result.kind = CompletionKind::suppressed;
            break;
          }
        if (end <= start || begin < start || (kind == HighlightKind::delimiter && end - begin == 1))
          continue;
        if (token.command)
          {
            if (touches)
              {
                result.kind = CompletionKind::directive;
                result.replacement = cursor == begin ? live::SourceSpan{cursor, cursor}
                                                     : live::SourceSpan{begin + 1, end};
                result.prefix = source.substr(begin + 1, cursor > begin ? cursor - begin - 1 : 0);
                if (cursor == begin)
                  result.kind = CompletionKind::suppressed;
                break;
              }
            result.kind = CompletionKind::argument;
            result.directive = source.substr(begin + 1, end - begin - 1);
            continue;
          }
        if (kind == HighlightKind::string)
          {
            if (result.kind == CompletionKind::argument && end < cursor)
              {
                ++result.argument;
                std::string decoded;
                for (std::size_t i = begin + 1; i + 1 < end; ++i)
                  {
                    if (source[i] == '\\' && i + 2 < end)
                      ++i;
                    decoded += source[i];
                  }
                result.preceding_arguments.push_back(std::move(decoded));
              }
            else if (result.kind == CompletionKind::argument && touches)
              result.kind = CompletionKind::suppressed;
            continue;
          }
        if (touches)
          {
            result.replacement = token.span;
            result.prefix = source.substr(begin, cursor - begin);
            break;
          }
        if (result.kind == CompletionKind::argument)
          {
            ++result.argument;
            result.preceding_arguments.emplace_back(slice(source, token.span));
          }
      }
    return result;
  }

  void validate_config()
  {
    if (config_.delimiters.size() % 2 || config_.delimiters.size() >= pairs_.size())
      throw std::invalid_argument("REPL delimiter configuration requires distinct ASCII pairs");
    if (config_.indent_width && config_.max_depth > (std::numeric_limits<std::size_t>::max)() / config_.indent_width)
      throw std::invalid_argument("REPL indentation budget exceeded");
    if (config_.max_candidates > 1024 || config_.max_candidate_bytes > 65536)
      throw std::invalid_argument("REPL completion budget exceeded");
    if (config_.max_source_bytes > 4 * 1024 * 1024 || config_.max_tokens > 65536
        || config_.max_depth > 65536 || config_.line_comment.size() > 64
        || config_.keywords.size() > 4096 || config_.symbols.size() > 4096
        || config_.directives.size() > 4096)
      throw std::invalid_argument("REPL analysis budget exceeded");
    for (std::size_t i = 0; i < config_.delimiters.size(); ++i)
      {
        const auto c = static_cast<unsigned char>(config_.delimiters[i]);
        if (!punctuation(c) || pairs_[c] || quote(c) || c == config_.directive_prefix)
          throw std::invalid_argument("REPL delimiter configuration requires distinct ASCII pairs");
        const auto id = static_cast<int>(i / 2 + 1);
        pairs_[c] = i % 2 ? -id : id;
      }
    for (std::size_t i = 0; i < config_.quotes.size(); ++i)
      if (!punctuation(config_.quotes[i]) || config_.quotes[i] == '\\'
          || config_.quotes[i] == config_.directive_prefix || config_.quotes.find(config_.quotes[i]) != i)
        throw std::invalid_argument("invalid REPL quote configuration");
    if (config_.directive_prefix && !punctuation(config_.directive_prefix))
      throw std::invalid_argument("invalid REPL directive prefix");
    for (unsigned char c : config_.line_comment)
      if (!punctuation(c) || pair(c) || quote(c) || c == config_.directive_prefix)
        throw std::invalid_argument("invalid REPL line comment");
    for (const auto& word : config_.keywords)
      keyword_views_.insert(word);
    if (config_.blocks.size() > 4096)
      throw std::invalid_argument("too many REPL block pairs");
    int id = static_cast<int>(pairs_.size());
    for (const auto& [open, close] : config_.blocks)
      {
        ++id;
        for (const auto& [word, key] : { std::pair{std::string_view(open), id},
                                         std::pair{std::string_view(close), -id} })
          {
            if (word.empty() || word.size() > config_.max_candidate_bytes || quote(word.front())
                || (config_.directive_prefix && word.front() == config_.directive_prefix))
              throw std::invalid_argument("invalid REPL block word");
            for (std::size_t i = 0; i < word.size(); ++i)
              if (space(word[i]) || pair(word[i]) || comment(word, i))
                throw std::invalid_argument("invalid REPL block word");
            if (!blocks_.emplace(word, key).second)
              throw std::invalid_argument("duplicate REPL block word");
          }
      }
  }

  static bool candidate_text(std::string_view text)
  {
    if (text.empty())
      return false;
    for (unsigned char c : text)
      if (c < ' ' || c == 0x7f)
        return false;
    return terminal::sanitize_text(text) == text;
  }

public:
  explicit Analysis(AnalysisConfig config = {}) : config_(std::move(config))
  {
    validate_config();
    peg_channels_ = config_.channels;
    peg_patterns_ = config_.patterns;
    annotations_ = config_.annotations;
  }

  // Rebuild views so copies never borrow another analysis object's keywords.
  Analysis(const Analysis& other)
      : Analysis(other.config_)
  {
    completion_ = other.completion_;
    highlight_ = other.highlight_;
    syntax_ = other.syntax_;
    grammar_ = other.grammar_;
    indent_ = other.indent_;
    peg_ = other.peg_;
    peg_limits_ = other.peg_limits_;
    peg_channels_ = other.peg_channels_;
    peg_patterns_ = other.peg_patterns_;
    annotations_ = other.annotations_;
  }
  Analysis(Analysis&&) = default;
  Analysis& operator=(Analysis&&) = default;
  Analysis& operator=(const Analysis& other)
  {
    if (this != &other)
      *this = Analysis(other);
    return *this;
  }

  const AnalysisConfig& config() const noexcept { return config_; }

  void add_completion_provider(CompletionProvider provider)
  {
    if (!provider)
      throw std::invalid_argument("empty REPL completion provider");
    add_completion_provider(CompletionSource::semantic,
        [provider = std::move(provider)](const auto& ctx, const auto& add)
        {
          provider(ctx, [&](std::string_view value) { return add(value, {}, 0); });
        });
  }

  void add_completion_provider(CompletionSource source, DetailProvider provider)
  {
    if (!provider || completion_.size() == 128)
      throw std::invalid_argument("invalid REPL completion provider");
    completion_.emplace_back(source, std::move(provider));
  }

  void add_option_completion(std::string directive, std::shared_ptr<const cli::OptParser> parser)
  {
    if (!parser || directive.empty())
      throw std::invalid_argument("REPL option completion requires a directive and parser");
    add_completion_provider(CompletionSource::option,
        [directive = std::move(directive), parser = std::move(parser)](const auto& context, const auto& add)
        {
          if (context.kind != CompletionKind::argument || context.directive != directive)
            return;
          for (const auto& choice : parser->complete(context.preceding_arguments, context.prefix))
            if (!add(choice, {}, 30))
              break;
        });
  }

  // Explicit opt-in: list direct children of a host-selected directory only.
  // No textual configuration can acquire this filesystem capability.
  void add_path_completion(std::string directive, std::filesystem::path root,
                           std::size_t max_entries = 256)
  {
    if (root.empty() || directive.empty() || max_entries > 16384)
      throw std::invalid_argument("invalid REPL path completion capability");
    add_completion_provider(CompletionSource::filesystem,
        [directive = std::move(directive), root = std::move(root), max_entries](const auto& context, const auto& add)
        {
          if (context.kind != CompletionKind::argument || context.directive != directive)
            return;
          std::error_code error;
          std::filesystem::directory_iterator next(root, error), end;
          for (std::size_t i = 0; i < max_entries && !error && next != end; ++i, next.increment(error))
            {
              auto name = next->path().filename().string();
              if (name.find_first_of(" \t\r\n;\"\\") == std::string::npos
                  && name.starts_with(context.prefix) && !add(name, "File or directory", 5))
                break;
            }
        });
  }

  void set_highlight_provider(HighlightProvider provider) { highlight_ = std::move(provider); }
  void set_syntax_provider(SyntaxProvider provider) { syntax_ = std::move(provider); }
  void set_grammar_provider(CancellableGrammarProvider provider) { grammar_ = std::move(provider); }
  void set_grammar_provider(GrammarProvider provider)
  {
    if (!provider)
      grammar_ = {};
    else
      grammar_ = [provider = std::move(provider)](auto text, auto cursor, auto)
      {
        return provider(text, cursor);
      };
  }
  void set_indent_provider(IndentProvider provider) { indent_ = std::move(provider); }

  // Both PEG channel and pattern (including regex spelling) can annotate
  // matches. Pattern annotations take precedence over channel annotations.
  void set_peg(std::shared_ptr<const PEGDefinition> peg, live::SyntaxLimits limits = {})
  {
    peg_ = std::move(peg);
    peg_limits_ = limits;
  }

  template <typename Grammar>
    requires (std::is_same_v<Grammar, CFG> || std::is_same_v<Grammar, live::SyntaxGrammar>)
  void set_cfg(std::shared_ptr<const Grammar> grammar)
  {
    if (!grammar || !peg_)
      throw std::invalid_argument("REPL CFG analysis requires a grammar and PEG lexer");
    const auto lexer = peg_;
    const auto configured = peg_limits_;
    set_grammar_provider([grammar = std::move(grammar), lexer, configured](
        std::string_view text, std::size_t cursor, std::stop_token stop)
    {
      auto limits = configured;
      if (stop.stop_possible())
        limits.stop = stop;
      auto lexical = live::analyze_peg(*lexer, text, text.size(), limits);
      if (lexical.status != live::SyntaxStatus::complete)
        {
          live::CursorAnalysis result;
          static_cast<live::SyntaxAnalysis&>(result) = std::move(lexical);
          return result;
        }
      return live::analyze_cfg(*grammar, lexical.tokens, cursor, limits, text.size());
    });
  }

  void style_channel(std::string name, HighlightKind kind) { peg_channels_[std::move(name)] = kind; }
  void style_pattern(std::string name, HighlightKind kind) { peg_patterns_[std::move(name)] = kind; }
  void style_annotation(std::string name, HighlightKind kind) { annotations_[std::move(name)] = kind; }

  AnalysisResult inspect(std::string_view source, std::size_t cursor, std::stop_token stop = {}) const
  {
    check_input(source, cursor);
    std::optional<live::LexicalAnalysis> lexical;
    auto lexed = lex(source, stop, &lexical);
    AnalysisResult result;
    std::vector<std::size_t> stack;
    std::vector<std::size_t> styles;
    std::vector<int> ids;
    const auto start = line_start(source, cursor);
    auto content = start;
    while (content < source.size() && (source[content] == ' ' || source[content] == '\t'))
      ++content;
    auto leading_closes = content;
    const auto report = [&](live::SourceSpan span, std::string code, std::string message)
    {
      result.diagnostics.push_back({live::Severity::error, std::move(code), std::move(message), span});
    };
    bool has_directive = false;
    for (const auto& token : lexed)
      {
        if (stop.stop_requested())
          break;
        const auto begin = token.span.begin;
        auto kind = token.kind;
        auto depth = stack.size();
        has_directive |= token.command;
        if (token.unfinished)
          {
            if (result.syntax == live::SyntaxStatus::complete)
              result.syntax = live::SyntaxStatus::incomplete;
          }
        if (kind == HighlightKind::delimiter)
          {
            const auto id = token.structural;
            const auto index = result.delimiters.size();
            result.delimiters.push_back({token.span, source[begin], id > 0, std::nullopt});
            styles.push_back(result.highlights.size());
            ids.push_back(id);
            if (id > 0)
              {
                if (stack.size() == config_.max_depth)
                  throw std::length_error("REPL analysis nesting budget exceeded");
                stack.push_back(index);
              }
            else if (!stack.empty() && ids[stack.back()] == -id)
              {
                const auto opener = stack.back();
                stack.pop_back();
                result.delimiters[index].mate = result.delimiters[opener].span;
                result.delimiters[opener].mate = token.span;
                depth = stack.size();
              }
            else
              {
                result.syntax = live::SyntaxStatus::invalid;
                kind = HighlightKind::error;
                report(token.span, "repl.delimiter", "unmatched closing delimiter");
                for (auto opener : stack)
                  {
                    result.highlights[styles[opener]].kind = HighlightKind::error;
                    report(result.delimiters[opener].span, "repl.delimiter", "unmatched opening delimiter");
                  }
                stack.clear(); // Do not pair through a mismatched close.
              }
            result.delimiters.back().depth = depth;
            if (begin <= cursor && cursor <= token.span.end)
              result.selected_delimiter = index;
            if (begin == leading_closes && id < 0)
              {
                leading_closes = token.span.end;
                while (leading_closes < source.size()
                       && (source[leading_closes] == ' ' || source[leading_closes] == '\t'))
                  ++leading_closes;
              }
            if (begin < content || (begin < leading_closes && id < 0))
              result.indentation = stack.size() * config_.indent_width;
            if (token.span.end <= cursor)
              result.next_indentation = stack.size() * config_.indent_width;
          }
        else if (highlight_ && (kind == HighlightKind::symbol || kind == HighlightKind::number
                                 || kind == HighlightKind::keyword))
          {
            if (const auto style = highlight_(source, token.span))
              kind = *style;
          }
        result.highlights.push_back({token.span, kind, depth});
      }
    const auto cancelled = [] { AnalysisResult out; out.cancelled = true; return out; };
    if (stop.stop_requested() || (lexical && lexical->cancelled))
      return cancelled();
    if (!stack.empty() && result.syntax == live::SyntaxStatus::complete)
      result.syntax = live::SyntaxStatus::incomplete;
    if (syntax_ && !has_directive && result.syntax != live::SyntaxStatus::invalid)
      {
        const auto status = syntax_(source);
        if (status == live::SyntaxStatus::invalid || result.syntax == live::SyntaxStatus::complete)
          result.syntax = status;
      }
    if (grammar_ && !has_directive && result.syntax != live::SyntaxStatus::invalid)
      {
        const auto grammar = grammar_(source, cursor, stop);
        if (grammar.cancelled)
          return cancelled();
        if (grammar.status == live::SyntaxStatus::invalid || result.syntax == live::SyntaxStatus::complete)
          result.syntax = grammar.status;
        result.diagnostics.insert(result.diagnostics.end(), grammar.diagnostics.begin(),
                                  grammar.diagnostics.end());
        for (const auto& node : grammar.tree)
          {
            const auto hint = annotations_.find(node.category);
            if (hint == annotations_.end())
              continue;
            auto part = std::lower_bound(result.highlights.begin(), result.highlights.end(), node.span.begin,
                [](const HighlightSpan& span, std::size_t offset) { return span.span.begin < offset; });
            if (part != result.highlights.end() && part->span == node.span
                && part->kind != HighlightKind::delimiter && part->kind != HighlightKind::error)
              part->kind = hint->second;
          }
      }
    if (indent_ || config_.align_arguments)
      {
        if (config_.align_arguments)
          {
            const auto aligned = [&](std::size_t boundary, bool existing) -> std::optional<std::size_t>
            {
              for (auto it = result.delimiters.rbegin(); it != result.delimiters.rend(); ++it)
                {
                  if (!it->opening || it->span.end > boundary
                      || (it->mate && (it->mate->end <= boundary
                                       || (existing && it->mate->begin == content))))
                    continue;
                  auto first = it->span.end;
                  while (first < boundary && (source[first] == ' ' || source[first] == '\t'))
                    ++first;
                  if (first < boundary && !newline(source[first]) && !comment(source, first)
                      && pair(source[first]) >= 0)
                    return terminal::display_width(source.substr(line_start(source, first),
                                                                 first - line_start(source, first)));
                  break;
                }
              return std::nullopt;
            };
            result.indentation = aligned(start, true).value_or(result.indentation);
            result.next_indentation = aligned(cursor, false).value_or(result.next_indentation);
          }
        if (indent_)
          {
            result.indentation = indent_(source, start, result.indentation).value_or(result.indentation);
            result.next_indentation = indent_(source, cursor, result.next_indentation).value_or(result.next_indentation);
            if (result.indentation > config_.max_source_bytes || result.next_indentation > config_.max_source_bytes)
              throw std::length_error("REPL indentation budget exceeded");
          }
      }
    if (lexical)
      {
        result.diagnostics.insert(result.diagnostics.end(), lexical->diagnostics.begin(), lexical->diagnostics.end());
        if (lexical->status == live::SyntaxStatus::invalid || result.syntax == live::SyntaxStatus::complete)
          result.syntax = lexical->status;
      }
    if (stop.stop_requested())
      return cancelled();
    if (result.selected_delimiter)
      {
        const auto index = *result.selected_delimiter;
        if (const auto& mate = result.delimiters[index].mate)
          {
            for (auto& span : result.highlights)
              if (span.span == result.delimiters[index].span || span.span == *mate)
                span.kind = HighlightKind::match;
          }
      }
    return result;
  }

  CompletionResult complete(std::string_view source, std::size_t cursor,
                            std::stop_token stop = {}) const
  {
    check_input(source, cursor);
    const auto lexed = lex(source, stop);
    CompletionResult result{context(source, cursor, lexed), {}, {}};
    if (stop.stop_requested() || result.context.kind == CompletionKind::suppressed || !config_.max_candidates)
      return result;
    auto& accepted = result.details;
    using Detail = CompletionResult::Detail;
    const auto better = [](const Detail& a, const Detail& b)
    {
      return a.priority != b.priority ? a.priority > b.priority : a.text < b.text;
    };
    const auto add = [&](std::string_view value, std::string_view description, int priority,
                         CompletionSource origin)
    {
      if (stop.stop_requested())
        return;
      if (value.size() <= config_.max_candidate_bytes && candidate_text(value)
          && value.starts_with(result.context.prefix))
        {
          if (description.size() > config_.max_candidate_bytes
              || (!description.empty() && !candidate_text(description)))
            description = {};
          auto existing = std::find_if(accepted.begin(), accepted.end(),
              [&](const Detail& item) { return item.text == value; });
          if (existing != accepted.end())
            {
              if (existing->priority < priority)
                *existing = {std::string(value), std::string(description), origin, priority};
            }
          else if (accepted.size() < config_.max_candidates)
            accepted.push_back({std::string(value), std::string(description), origin, priority});
          else
            {
              auto worst = std::max_element(accepted.begin(), accepted.end(), better);
              if (priority > worst->priority || (priority == worst->priority && value < worst->text))
                *worst = {std::string(value), std::string(description), origin, priority};
            }
        }
    };
    const auto add_all = [&](const auto& words, CompletionSource origin, int priority)
    {
      for (const auto& word : words)
      {
        if (stop.stop_requested())
          break;
        add(word, {}, priority, origin);
      }
    };
    if (result.context.kind == CompletionKind::directive)
      add_all(config_.directives, CompletionSource::directive, 10);
    else if (result.context.kind == CompletionKind::symbol)
      {
        if (grammar_ && !stop.stop_requested())
          {
            const auto grammar = grammar_(source, cursor, stop);
            if (grammar.replacement.begin <= cursor && cursor <= grammar.replacement.end
                && grammar.replacement.end <= source.size()
                && (grammar.replacement.begin != 0 || grammar.replacement.end != 0))
              {
                check_input(source, grammar.replacement.begin);
                check_input(source, grammar.replacement.end);
                result.context.replacement = grammar.replacement;
                result.context.prefix = source.substr(grammar.replacement.begin,
                                                      cursor - grammar.replacement.begin);
              }
            add_all(grammar.cursor_expected, CompletionSource::grammatical, 20);
          }
        add_all(config_.keywords, CompletionSource::lexical, 10);
        add_all(config_.symbols, CompletionSource::lexical, 10);
        for (const auto& [opening, closing] : config_.blocks)
          {
            add(opening, "Begin block", 10, CompletionSource::lexical);
            add(closing, "End block", 10, CompletionSource::lexical);
          }
        for (const auto& token : lexed)
          {
            if (stop.stop_requested())
              break;
            if (token.kind == HighlightKind::symbol && token.span != result.context.replacement)
              add(slice(source, token.span), {}, 0, CompletionSource::lexical);
          }
      }
    for (const auto& [origin, provider] : completion_)
      {
        if (stop.stop_requested())
          break;
        std::size_t left = config_.max_candidates * 8;
        provider(result.context, [&](std::string_view word, std::string_view description, int priority)
        {
          if (!left || stop.stop_requested())
            return false;
          --left;
          add(word, description, priority, origin);
          return left && !stop.stop_requested();
        });
      }
    if (stop.stop_requested())
      accepted.clear();
    std::sort(accepted.begin(), accepted.end(), better);
    for (const auto& entry : accepted)
      result.candidates.push_back(entry.text);
    return result;
  }

  static Analysis from_sexpr(const SExpr& form) { return Analysis(AnalysisConfig::from_sexpr(form)); }
  static Analysis parse(std::string_view source) { return Analysis(AnalysisConfig::parse(source)); }
};

inline AnalysisConfig AnalysisConfig::from_sexpr(const SExpr& form)
{
  if (form.is_atom || form.items.empty() || form.items.size() > 4096
      || !form.items.front().is_atom || form.items.front().text != "analysis")
    throw std::invalid_argument("expected (analysis ...)");
  AnalysisConfig config;
  const auto atom = [](const SExpr& node) -> const std::string&
  {
    if (!node.is_atom || !node.items.empty() || node.text.size() > 4096)
      throw std::invalid_argument("expected REPL analysis configuration atom");
    return node.text;
  };
  for (std::size_t i = 1; i < form.items.size(); ++i)
    {
      const auto& option = form.items[i];
      if (option.is_atom || option.items.size() < 2 || option.items.size() > 4096)
        throw std::invalid_argument("expected a REPL analysis option");
      const auto& name = atom(option.items[0]);
      if (name == "keywords" || name == "symbols" || name == "directives")
        {
          auto& words = name == "keywords" ? config.keywords
                        : name == "symbols" ? config.symbols : config.directives;
          for (std::size_t j = 1; j < option.items.size(); ++j)
            {
              if (words.size() == 4096)
                throw std::length_error("REPL word budget exceeded");
              words.push_back(atom(option.items[j]));
            }
          continue;
        }
      if (name == "block" && option.items.size() == 3)
        {
          config.blocks.emplace_back(atom(option.items[1]), atom(option.items[2]));
          continue;
        }
      if ((name == "channel" || name == "pattern" || name == "annotation") && option.items.size() == 3)
        {
          auto& rules = name == "channel" ? config.channels
                        : name == "pattern" ? config.patterns : config.annotations;
          constexpr std::array<std::string_view, 9> styles =
              {"symbol", "keyword", "number", "string", "comment", "directive", "delimiter", "error", "match"};
          const auto kind = std::find(styles.begin(), styles.end(), atom(option.items[2]));
          if (kind == styles.end() || !rules.emplace(atom(option.items[1]),
                static_cast<HighlightKind>(kind - styles.begin())).second)
            throw std::invalid_argument("unknown highlight style or duplicate REPL annotation");
          continue;
        }
      if (option.items.size() != 2)
        throw std::invalid_argument("invalid REPL analysis option arity");
      const auto& value = atom(option.items[1]);
      if (name == "delimiters") config.delimiters = value;
      else if (name == "quotes") config.quotes = value;
      else if (name == "line-comment") config.line_comment = value;
      else if (name == "directive-prefix" && value.size() <= 1)
        config.directive_prefix = value.empty() ? '\0' : value.front();
      else if (name == "indent-width")
        {
          auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), config.indent_width);
          if (ec != std::errc{} || end != value.data() + value.size())
            throw std::invalid_argument("invalid REPL indentation width");
        }
      else if (name == "align-arguments" && (value == "true" || value == "false"))
        config.align_arguments = value == "true";
      else throw std::invalid_argument("unknown or invalid REPL analysis option");
    }
  return config;
}

inline AnalysisConfig AnalysisConfig::parse(std::string_view source)
{
  return from_sexpr(parse_bounded_sexpr(source, {65536, 4096, 8}));
}

struct AnalysisTheme
{
  using Color = terminal::Color;
  std::array<terminal::Style, 9> styles = []
  {
    std::array<terminal::Style, 9> result{};
    result[static_cast<std::size_t>(HighlightKind::keyword)].foreground = Color{225, 146, 255};
    result[static_cast<std::size_t>(HighlightKind::number)].foreground = Color{255, 194, 125};
    result[static_cast<std::size_t>(HighlightKind::string)].foreground = Color{144, 211, 133};
    result[static_cast<std::size_t>(HighlightKind::comment)].faint = true;
    result[static_cast<std::size_t>(HighlightKind::directive)].bold = true;
    result[static_cast<std::size_t>(HighlightKind::error)].foreground = Color{255, 90, 90};
    result[static_cast<std::size_t>(HighlightKind::error)].underline = true;
    result[static_cast<std::size_t>(HighlightKind::match)].bold = true;
    result[static_cast<std::size_t>(HighlightKind::match)].inverse = true;
    return result;
  }();
  std::array<Color, 4> depth_colors{{{70, 81, 155}, {108, 124, 163}, {143, 169, 113}, {255, 211, 133}}};

  terminal::Style style(const HighlightSpan& part) const
  {
    const auto index = static_cast<std::size_t>(part.kind);
    if (index >= styles.size())
      throw std::invalid_argument("invalid REPL highlight style");
    auto result = styles[index];
    if ((part.kind == HighlightKind::delimiter || part.kind == HighlightKind::match)
        && !result.foreground)
      result.foreground = depth_colors[part.depth % depth_colors.size()];
    return result;
  }
};

inline terminal::StyledText highlighted(std::string_view source, const AnalysisResult& analysis,
                                        const AnalysisTheme& theme = {})
{
  terminal::StyledText result;
  const auto boundaries = [&](live::SourceSpan span)
  {
    const auto utf8_boundary = [&](std::size_t byte)
    {
      return byte == source.size() || (static_cast<unsigned char>(source[byte]) & 0xc0) != 0x80;
    };
    return span.begin < span.end && span.end <= source.size()
           && utf8_boundary(span.begin) && utf8_boundary(span.end);
  };
  const auto append = [&](std::string_view text, terminal::Style style)
  {
    for (std::size_t start = 0; start < text.size();)
      {
        auto end = text.find_first_of("\r\n", start);
        if (end == std::string_view::npos)
          end = text.size();
        result.span(text.substr(start, end - start), style);
        if (end == text.size())
          break;
        result.line_break();
        start = end + 1;
        if (text[end] == '\r' && start < text.size() && text[start] == '\n')
          ++start;
      }
  };
  std::size_t cursor = 0;
  for (const auto& part : analysis.highlights)
    {
      if (part.span.begin < cursor || !boundaries(part.span))
        throw std::invalid_argument("invalid REPL highlight span or kind");
      append(source.substr(cursor, part.span.begin - cursor), {});
      append(source.substr(part.span.begin, part.span.end - part.span.begin), theme.style(part));
      cursor = part.span.end;
    }
  append(source.substr(cursor), {});
  return result;
}

inline AnalysisCallbacks analysis_callbacks(std::shared_ptr<const Analysis> analysis)
{
  if (!analysis)
    throw std::invalid_argument("missing REPL analysis");
  AnalysisCallbacks callbacks;
  callbacks.continuation = [analysis](std::string_view source)
  {
    return analysis->inspect(source, source.size()).syntax;
  };
  callbacks.complete = [analysis](std::string_view source, std::size_t cursor)
  {
    auto results = analysis->complete(source, cursor);
    std::vector<Completion> output;
    for (auto& entry : results.details)
      {
        auto label = entry.description.empty() ? std::string{} : entry.text + " — " + entry.description;
        output.push_back({results.context.replacement.begin, results.context.replacement.end,
                          std::move(entry.text), std::move(label)});
      }
    return output;
  };
  return callbacks;
}

inline RenderProvider analysis_renderer(std::shared_ptr<const Analysis> analysis, AnalysisTheme theme = {})
{
  if (!analysis)
    throw std::invalid_argument("missing REPL analysis");
  // An editor can render several rows from the same document snapshot.
  return [analysis = std::move(analysis), theme = std::move(theme), text = std::string{},
          cursor = std::string_view::npos, result = AnalysisResult{}](const EditView& view) mutable
  {
    if (view.visible_begin > view.visible_end || view.visible_end > view.document.size()
        || view.cursor_byte > view.document.size())
      throw std::invalid_argument("invalid REPL viewport");
    if (cursor != view.cursor_byte || text != view.document)
      {
        result = analysis->inspect(view.document, view.cursor_byte);
        cursor = view.cursor_byte;
        text = view.document;
      }
    terminal::StyledText output;
    auto part = std::lower_bound(result.highlights.begin(), result.highlights.end(), view.visible_begin,
        [](const HighlightSpan& span, std::size_t begin) { return span.span.end <= begin; });
    for (auto begin = view.visible_begin; begin < view.visible_end;)
      {
        const auto end = terminal::next_glyph(view.document, begin).end;
        if (end <= begin || end > view.visible_end)
          throw std::invalid_argument("REPL viewport splits a glyph");
        while (part != result.highlights.end() && part->span.end <= begin)
          ++part;
        auto style = part != result.highlights.end() && part->span.begin < end
            ? theme.style(*part) : terminal::Style{};
        if (view.selection && view.selection->first < end && view.selection->second > begin)
          style.inverse = true;
        output.span(view.document.substr(begin, end - begin), style);
        begin = end;
      }
    return output;
  };
}

inline EditHandler analysis_indenter(std::shared_ptr<const Analysis> analysis)
{
  if (!analysis)
    throw std::invalid_argument("missing REPL analysis");
  return [analysis = std::move(analysis)](Editor& editor, Command command, std::string_view)
  {
    if (command != Command::newline)
      return false;
    const auto result = analysis->inspect(editor.text(), editor.cursor());
    if (result.next_indentation > analysis->config().max_source_bytes
        || result.next_indentation >= editor.limits().input_bytes)
      return false;
    return editor.insert("\n" + std::string(result.next_indentation, ' '));
  };
}
} // namespace repl

namespace repl::scripts
{
// The wire format is a single S-expression on each stream (stdin/stdout).
// (libdsl-script 1 request ID OPERATION "payload")
// (libdsl-script 1 ok ID "payload") or (libdsl-script 1 error ID "diagnostic")
inline constexpr std::size_t max_wire_bytes = 4 * 1024 * 1024;

struct Limits
{
  std::chrono::milliseconds timeout{ 1000 };
  std::size_t input_bytes = 64 * 1024;
  std::size_t output_bytes = 64 * 1024; // stdout and stderr combined
};

struct Request
{
  std::string operation;
  std::string payload;
};

enum class Transport { streams, environment, pipes, shared_memory };
// streams: request stdin, response stdout.
// environment: request LIBDSL_SCRIPT_REQUEST, response stdout; no stdin data.
// pipes: child reads fd 3 and writes fd 4; the corresponding environment
// variables are LIBDSL_SCRIPT_INPUT_FD and LIBDSL_SCRIPT_OUTPUT_FD.
// shared_memory: fd 3 (LIBDSL_SCRIPT_SHM_FD), little-endian uint32 lengths at
// offsets 0 and 4, request at offset 8, then a bounded response slot. The child
// writes the response and its length before exiting; the parent waits for exit.
// For all transports LIBDSL_SCRIPT_TRANSPORT names the selected transport and
// LIBDSL_SCRIPT_OUTPUT_LIMIT is the response + diagnostic budget. In pipes
// and shared-memory modes both stdout and stderr are diagnostic streams.

enum class Hook
{
  completion, highlighting, indentation, prompt, directive, evaluation, session_event
};

enum class Status
{
  ok, script_error, invalid_request, protocol_error, spawn_error,
  process_error, timeout, output_limit, exit_failure, unsupported
};

struct Result
{
  Status status;
  std::string payload{};
  std::string stderr_data{}; // untrusted data, not terminal-ready
  int exit_code = -1;
};

namespace script_detail
{
inline bool
identifier (std::string_view value)
{
  return !value.empty () && value.size () <= 64
         && std::all_of (value.begin (), value.end (), [] (unsigned char c) {
              return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '_' || c == '-';
            });
}

inline bool
atom_is (const SExpr &node, std::string_view text)
{
  return node.is_atom && !node.quoted && node.text == text;
}
}

inline std::string
encode_request (const Request &request, std::string_view correlation,
                std::size_t limit = max_wire_bytes)
{
  if (limit > max_wire_bytes || request.payload.size () > limit
      || !script_detail::identifier (request.operation)
      || !script_detail::identifier (correlation)
      || request.payload.find ('\0') != std::string::npos)
    throw std::invalid_argument ("invalid script request");
  auto wire = SExpr::list ({ SExpr::atom ("libdsl-script"), SExpr::atom ("1"),
                             SExpr::atom ("request"),
                             SExpr::atom (std::string (correlation)),
                             SExpr::atom (request.operation),
                             SExpr::atom (request.payload, true) }).dump ();
  if (wire.size () >= limit)
    throw std::invalid_argument ("script request exceeds its byte limit");
  wire += '\n';
  return wire;
}

inline Result
decode_response (std::string_view wire, std::string_view correlation,
                 std::size_t limit = max_wire_bytes)
{
  if (limit > max_wire_bytes || wire.size () > limit)
    return { Status::output_limit, "script response exceeds its byte limit" };
  try
    {
      if (!script_detail::identifier (correlation)
          || wire.find ('\0') != std::string_view::npos)
        throw std::invalid_argument ("invalid script response");
      auto root = parse_bounded_sexpr (wire, SExprLimits{ limit, 6, 1 });
      const auto &fields = root.items;
      if (root.is_atom || fields.size () != 5
          || !script_detail::atom_is (fields[0], "libdsl-script")
          || !script_detail::atom_is (fields[1], "1")
          || !script_detail::atom_is (fields[3], correlation)
          || !fields[4].is_atom || !fields[4].quoted)
        throw std::invalid_argument ("invalid script response");
      if (script_detail::atom_is (fields[2], "ok"))
        return { Status::ok, std::move (root.items[4].text) };
      if (script_detail::atom_is (fields[2], "error"))
        return { Status::script_error, std::move (root.items[4].text) };
    }
  catch (const std::invalid_argument &)
    {
    }
  return { Status::protocol_error, "invalid libdsl-script v1 response" };
}

// This explicit native capability is never manufactured from a directive,
// S-expression, PATH lookup, or a protocol response. The host must trust the
// executable, its arguments, and the resources that it can access.
class Runner
{
  std::vector<std::string> command_;
  Limits limits_;
  Transport transport_;

  Runner (std::vector<std::string> command, Limits limits, Transport transport)
    : command_ (std::move (command)), limits_ (limits), transport_ (transport)
  {
  }

public:
  static Runner
  trusted (std::vector<std::string> command, Limits limits = {},
           Transport transport = Transport::streams)
  {
    if (command.empty () || command.size () > 128 || command[0].empty ()
        || command[0].front () != '/' || limits.timeout.count () <= 0
        || limits.timeout > std::chrono::minutes (5)
        || !limits.input_bytes || limits.input_bytes > max_wire_bytes
        || !limits.output_bytes || limits.output_bytes > max_wire_bytes)
      throw std::invalid_argument ("invalid trusted script configuration");
    std::size_t remaining = 64 * 1024;
    for (const auto &argument : command)
      {
        if (argument.size () >= remaining
            || argument.find ('\0') != std::string::npos)
          throw std::invalid_argument ("invalid trusted script argument");
        remaining -= argument.size () + 1;
      }
    switch (transport)
      {
      case Transport::streams: case Transport::environment:
      case Transport::pipes: case Transport::shared_memory:
        return Runner (std::move (command), limits, transport);
      }
    throw std::invalid_argument ("invalid trusted script transport");
  }

  Result run (const Request &request) const;
};

// An extension is installed explicitly by the host and only runs for hooks
// the host has selected. The REPL owns interpretation of returned payloads.
class Extension
{
  std::array<std::optional<Runner>, 7> hooks_{};

  static std::size_t
  index (Hook hook)
  {
    auto key = static_cast<std::size_t> (hook);
    if (key >= 7)
      throw std::invalid_argument ("invalid script hook");
    return key;
  }

public:
  void install (Hook hook, Runner runner) { hooks_[index (hook)] = std::move (runner); }
  bool has (Hook hook) const { return hooks_[index (hook)].has_value (); }

  Result
  invoke (Hook hook, std::string_view payload) const
  {
    constexpr std::string_view names[]{ "completion", "highlighting", "indentation",
                                        "prompt", "directive", "evaluation", "session-event" };
    const auto key = index (hook);
    if (!hooks_[key])
      return { Status::unsupported, "script hook is not installed" };
    if (payload.size () > max_wire_bytes)
      return { Status::invalid_request, "script hook payload exceeds its byte limit" };
    return hooks_[key]->run ({ std::string (names[key]), std::string (payload) });
  }
};

#if LIBDSL_LIVE_POSIX && defined(SOCK_CLOEXEC) && (defined(MSG_NOSIGNAL) || defined(SO_NOSIGPIPE))
namespace script_detail
{
inline constexpr int first_private_fd = 6;

struct Fd
{
  int value = -1;
  Fd () = default;
  Fd (const Fd &) = delete;
  Fd &operator= (const Fd &) = delete;
  ~Fd () { reset (); }
  void
  reset (int replacement = -1)
  {
    if (value >= 0)
      ::close (value); // do not retry close on EINTR
    value = replacement;
  }
};

inline bool
relocate (Fd &fd)
{
  if (fd.value < first_private_fd)
    {
      const auto duplicate = ::fcntl (fd.value, F_DUPFD_CLOEXEC, first_private_fd);
      if (duplicate == -1)
        return false;
      fd.reset (duplicate);
    }
  return true;
}

inline bool
nonblocking (int fd)
{
  const auto flags = ::fcntl (fd, F_GETFL);
  return flags != -1 && ::fcntl (fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

inline bool
make_channel (Fd &parent, Fd &child)
{
  int pair[2];
  if (::socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
    return false;
  parent.reset (pair[0]);
  child.reset (pair[1]);
  if (!relocate (parent) || !relocate (child) || !nonblocking (parent.value))
    return false;
#if !defined(MSG_NOSIGNAL)
  const int enable = 1;
  if (::setsockopt (parent.value, SOL_SOCKET, SO_NOSIGPIPE,
                    &enable, sizeof (enable)) == -1)
    return false;
#endif
  return true;
}

#if defined(__linux__)
inline bool
make_pipe (Fd &parent, Fd &child, bool parent_writes)
{
  int pair[2];
  if (::pipe2 (pair, O_CLOEXEC) == -1)
    return false;
  parent.reset (pair[parent_writes ? 1 : 0]);
  child.reset (pair[parent_writes ? 0 : 1]);
  return relocate (parent) && relocate (child) && nonblocking (parent.value);
}

// Mask SIGPIPE only in this thread and consume only a newly generated signal.
inline ssize_t
write_pipe (int fd, const char *data, std::size_t size)
{
  sigset_t one, old, pending;
  ::sigemptyset (&one);
  ::sigaddset (&one, SIGPIPE);
  if (auto error = ::pthread_sigmask (SIG_BLOCK, &one, &old))
    {
      errno = error;
      return -1;
    }
  ssize_t written = -1;
  if (::sigpending (&pending) == 0)
    {
      written = ::write (fd, data, size);
      if (written == -1 && errno == EPIPE && !::sigismember (&pending, SIGPIPE))
        {
          const struct timespec no_wait{ 0, 0 };
          while (::sigtimedwait (&one, nullptr, &no_wait) == -1 && errno == EINTR)
            {}
          errno = EPIPE;
        }
    }
  const auto saved = errno;
  ::pthread_sigmask (SIG_SETMASK, &old, nullptr);
  errno = saved;
  return written;
}

inline bool
file_io (int fd, char *bytes, std::size_t size, std::size_t offset, bool write)
{
  while (size)
    {
      auto n = write ? ::pwrite (fd, bytes, size, static_cast<off_t> (offset))
                     : ::pread (fd, bytes, size, static_cast<off_t> (offset));
      if (n == -1 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      bytes += n;
      size -= static_cast<std::size_t> (n);
      offset += static_cast<std::size_t> (n);
    }
  return true;
}

// Inherited fd 3: [u32le request bytes][u32le response bytes][request][response].
// The child writes its reply and length before exiting; the host reads after
// waiting. No name is exposed and unrelated processes cannot reopen the object.
inline bool
make_shared (Fd &shared, const std::string &wire, std::size_t limit,
             const std::string &correlation)
{
  auto name = "/libdsl-script-" + std::to_string (::getpid ()) + "-" + correlation;
  shared.reset (::shm_open (name.c_str (), O_RDWR | O_CREAT | O_EXCL, 0600));
  if (shared.value == -1)
    return false;
  int unlinked;
  do
    unlinked = ::shm_unlink (name.c_str ());
  while (unlinked == -1 && errno == EINTR);
  if (unlinked != 0 || !relocate (shared) || ::fchmod (shared.value, 0600) != 0
      || ::ftruncate (shared.value, static_cast<off_t> (8 + wire.size () + limit)) != 0)
    return false;
  char header[8]{};
  for (unsigned i = 0; i < 4; ++i)
    header[i] = static_cast<char> ((wire.size () >> (i * 8)) & 255);
  return file_io (shared.value, header, sizeof (header), 0, true)
         && file_io (shared.value, const_cast<char *> (wire.data ()), wire.size (), 8, true);
}
#endif

struct SpawnActions
{
  posix_spawn_file_actions_t value;
  int error = ::posix_spawn_file_actions_init (&value);
  ~SpawnActions () { if (!error) ::posix_spawn_file_actions_destroy (&value); }
};

struct SpawnAttributes
{
  posix_spawnattr_t value;
  int error = ::posix_spawnattr_init (&value);
  ~SpawnAttributes () { if (!error) ::posix_spawnattr_destroy (&value); }
};

struct Child
{
  pid_t pid = -1;

  // The host must not auto-reap or externally reap this runner's child.
  // Signal the group before reaping its leader, so its ID cannot be reused.
  int
  finish ()
  {
    if (pid <= 0)
      return -1;
    siginfo_t info{};
    int check;
    do
      check = ::waitid (P_PID, static_cast<id_t> (pid), &info,
                        WEXITED | WNOHANG | WNOWAIT);
    while (check == -1 && errno == EINTR);
    if (check == -1)
      {
        pid = -1;
        return -1;
      }
    ::kill (-pid, SIGKILL);
    if (!info.si_pid)
      ::kill (pid, SIGKILL);
    int status = 0;
    pid_t waited;
    do
      waited = ::waitpid (pid, &status, 0);
    while (waited == -1 && errno == EINTR);
    pid = -1;
    return waited == -1 ? -1 : status;
  }

  ~Child () { finish (); }
};

inline Result
execute (const std::vector<std::string> &command, const Limits &limits,
         const std::string &wire, const std::string &correlation, Transport transport)
{
  const bool pipes = transport == Transport::pipes;
  const bool memory = transport == Transport::shared_memory;
  const bool env = transport == Transport::environment;
  const bool streams = transport == Transport::streams;
  if (env && wire.size () > 64 * 1024)
    return { Status::invalid_request, "script environment request exceeds 64 KiB" };
#if !defined(__linux__)
  if (pipes || memory)
    return { Status::unsupported, "selected script transport is unavailable on this platform" };
#endif
  Fd parent[5], ends[5], shared;
  Child child;
  for (int i = 0; i < 3; ++i)
    if (!make_channel (parent[i], ends[i]))
      return { Status::spawn_error, "cannot open script channels" };
#if defined(__linux__)
  if (pipes && (!make_pipe (parent[3], ends[3], true)
                || !make_pipe (parent[4], ends[4], false)))
    return { Status::spawn_error, "cannot open dedicated script pipes" };
  if (memory && !make_shared (shared, wire, limits.output_bytes, correlation))
    return { Status::spawn_error, "cannot allocate private script shared memory" };
#endif

  SpawnActions actions;
  SpawnAttributes attributes;
  if (actions.error || attributes.error)
    return { Status::spawn_error, "cannot initialize script process" };
  for (int i = 0; i < (pipes ? 5 : 3); ++i)
    if (::posix_spawn_file_actions_adddup2 (&actions.value, ends[i].value, i)
        || ::posix_spawn_file_actions_addclose (&actions.value, ends[i].value)
        || ::posix_spawn_file_actions_addclose (&actions.value, parent[i].value))
      return { Status::spawn_error, "cannot configure script channels" };
  if (memory && (::posix_spawn_file_actions_adddup2 (&actions.value, shared.value, 3)
                 || ::posix_spawn_file_actions_addclose (&actions.value, shared.value)))
    return { Status::spawn_error, "cannot configure script shared memory" };
#if defined(__GLIBC__) && defined(__USE_GNU)
#if __GLIBC_PREREQ(2, 34)
  if (::posix_spawn_file_actions_addclosefrom_np (&actions.value, pipes ? 5 : memory ? 4 : 3))
    return { Status::spawn_error, "cannot isolate inherited script descriptors" };
#endif
#endif

  sigset_t mask, defaults;
  if (::sigemptyset (&mask) || ::sigemptyset (&defaults)
      || ::sigaddset (&defaults, SIGPIPE)
      || ::posix_spawnattr_setsigmask (&attributes.value, &mask)
      || ::posix_spawnattr_setsigdefault (&attributes.value, &defaults)
      || ::posix_spawnattr_setpgroup (&attributes.value, 0)
      || ::posix_spawnattr_setflags (&attributes.value, POSIX_SPAWN_SETPGROUP
                                      | POSIX_SPAWN_SETSIGMASK
                                      | POSIX_SPAWN_SETSIGDEF))
    return { Status::spawn_error, "cannot configure script process" };

  std::vector<char *> argv;
  argv.reserve (command.size () + 1);
  for (const auto &argument : command)
    argv.push_back (const_cast<char *> (argument.c_str ()));
  argv.push_back (nullptr);
  std::vector<std::string> environment{
    "PATH=/usr/bin:/bin", "LC_ALL=C",
    "LIBDSL_SCRIPT_OUTPUT_LIMIT=" + std::to_string (limits.output_bytes)
  };
  if (env)
    {
      environment.emplace_back ("LIBDSL_SCRIPT_TRANSPORT=environment");
      environment.push_back ("LIBDSL_SCRIPT_REQUEST=" + wire);
    }
  else if (pipes)
    {
      environment.emplace_back ("LIBDSL_SCRIPT_TRANSPORT=pipes");
      environment.emplace_back ("LIBDSL_SCRIPT_INPUT_FD=3");
      environment.emplace_back ("LIBDSL_SCRIPT_OUTPUT_FD=4");
    }
  else if (memory)
    {
      environment.emplace_back ("LIBDSL_SCRIPT_TRANSPORT=shared_memory");
      environment.emplace_back ("LIBDSL_SCRIPT_SHM_FD=3");
    }
  else
    environment.emplace_back ("LIBDSL_SCRIPT_TRANSPORT=streams");
  std::vector<char *> envp;
  for (auto &entry : environment)
    envp.push_back (entry.data ());
  envp.push_back (nullptr);
  const auto deadline = std::chrono::steady_clock::now () + limits.timeout;
  pid_t pid;
  if (::posix_spawn (&pid, argv[0], &actions.value, &attributes.value,
                     argv.data (), envp.data ()))
    return { Status::spawn_error, "cannot start trusted script" };
  child.pid = pid;
  for (auto &end : ends)
    end.reset ();
  if (!streams)
    parent[0].reset ();

  std::string output, errors;
  std::size_t sent = env || memory ? wire.size () : 0;
  const int input = pipes ? 3 : 0;
  auto fail = [&] (Status status, const char *message) -> Result {
    return { status, message, std::move (errors) };
  };
  for (;;)
    {
      const auto now = std::chrono::steady_clock::now ();
      if (now >= deadline)
        return fail (Status::timeout, "trusted script timed out");
      siginfo_t info{};
      if (::waitid (P_PID, static_cast<id_t> (child.pid), &info,
                    WEXITED | WNOHANG | WNOWAIT) == -1)
        {
          if (errno == EINTR)
            continue;
          return fail (Status::process_error, "cannot observe script process");
        }
      if (info.si_pid && parent[1].value == -1 && parent[2].value == -1
          && parent[4].value == -1)
        break;
      struct pollfd fds[]{ { parent[input].value, POLLOUT, 0 },
                           { parent[1].value, POLLIN, 0 },
                           { parent[2].value, POLLIN, 0 },
                           { parent[4].value, POLLIN, 0 } };
      // Periodic wakeups also observe children that close all channels and
      // then exit. Each ready stream gets just one operation per turn.
      const auto ms = std::chrono::ceil<std::chrono::milliseconds> (deadline - now);
      if (::poll (fds, 4, ms.count () > 20 ? 20 : static_cast<int> (ms.count ())) == -1)
        {
          if (errno == EINTR)
            continue;
          return fail (Status::process_error, "cannot poll script channels");
        }
      if (fds[0].revents & POLLNVAL)
        return fail (Status::process_error, "invalid script stdin");
      if (fds[0].revents)
        {
#if defined(MSG_NOSIGNAL)
          constexpr int send_flags = MSG_NOSIGNAL;
#else
          constexpr int send_flags = 0;
#endif
          ssize_t n;
#if defined(__linux__)
          if (pipes)
            n = write_pipe (parent[input].value, wire.data () + sent, wire.size () - sent);
          else
#endif
            n = ::send (parent[input].value, wire.data () + sent, wire.size () - sent, send_flags);
          if (n > 0)
            sent += static_cast<std::size_t> (n);
          else if (n == -1 && errno == EPIPE)
            parent[input].reset ();
          else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return fail (Status::process_error, "cannot write script request");
          if (sent == wire.size ())
            parent[input].reset ();
        }
      for (int i = 1; i < 4; ++i)
        if (fds[i].revents)
          {
            if (fds[i].revents & POLLNVAL)
              return fail (Status::process_error, "invalid script output channel");
            char buffer[4096];
            const auto remaining = limits.output_bytes - output.size () - errors.size ();
            auto &fd = parent[i == 3 ? 4 : i];
            const auto n = ::read (fd.value, buffer,
                                   std::min (sizeof (buffer), remaining + 1));
            if (!n)
              fd.reset ();
            else if (n > 0)
              {
                const auto count = static_cast<std::size_t> (n);
                if (count > remaining)
                  return fail (Status::output_limit, "script output exceeds its byte limit");
                (i == 3 || (i == 1 && (streams || env)) ? output : errors).append (buffer, count);
              }
            else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
              return fail (Status::process_error, "cannot read script output");
          }
    }

  const auto status = child.finish ();
  if (status < 0)
    return fail (Status::process_error, "cannot reap script process");
  if (!WIFEXITED (status) || WEXITSTATUS (status) != 0)
    {
      auto result = fail (Status::exit_failure, "trusted script exited unsuccessfully");
      if (WIFEXITED (status))
        result.exit_code = WEXITSTATUS (status);
      return result;
    }
  if (sent != wire.size ())
    return fail (Status::process_error, "script closed its request channel before the write completed");
#if defined(__linux__)
  if (memory)
    {
      char size_bytes[4];
      if (!file_io (shared.value, size_bytes, sizeof (size_bytes), 4, false))
        return fail (Status::protocol_error, "invalid shared script response size");
      std::uint32_t size = 0;
      for (unsigned i = 0; i < 4; ++i)
        size |= std::uint32_t (static_cast<unsigned char> (size_bytes[i])) << (i * 8);
      if (size > limits.output_bytes - errors.size ())
        return fail (Status::output_limit, "shared script response exceeds its byte limit");
      output.resize (size);
      if (!file_io (shared.value, output.data (), output.size (), 8 + wire.size (), false))
        return fail (Status::protocol_error, "truncated shared script response");
    }
#endif
  auto result = decode_response (output, correlation, limits.output_bytes);
  result.stderr_data = std::move (errors);
  result.exit_code = 0;
  return result;
}
}
#endif

inline bool process_supported (Transport transport = Transport::streams)
{
  switch (transport)
    {
    case Transport::streams: case Transport::environment:
      break;
    case Transport::pipes: case Transport::shared_memory:
#if !defined(__linux__)
      return false;
#else
      break;
#endif
    default: return false;
    }
#if LIBDSL_LIVE_POSIX && defined(SOCK_CLOEXEC) && (defined(MSG_NOSIGNAL) || defined(SO_NOSIGPIPE))
  return true;
#else
  return false;
#endif
}

inline Result
Runner::run (const Request &request) const
{
  static std::atomic<std::uint64_t> ids{ 0 };
  const auto correlation = std::to_string (ids.fetch_add (1, std::memory_order_relaxed));
  std::string wire;
  try
    {
      wire = encode_request (request, correlation, limits_.input_bytes);
    }
  catch (const std::invalid_argument &)
    {
      return { Status::invalid_request, "invalid script request or byte limit exceeded" };
    }
#if LIBDSL_LIVE_POSIX && defined(SOCK_CLOEXEC) && (defined(MSG_NOSIGNAL) || defined(SO_NOSIGPIPE))
  return script_detail::execute (command_, limits_, wire, correlation, transport_);
#else
  (void)transport_;
  return { Status::unsupported, "trusted process execution is unavailable on this build" };
#endif
}
}

namespace repl
{
class Directives
{
public:
  using Handler = std::function<Evaluation (const cli::ParseResult &)>;

private:
  struct Word
  {
    std::string value;
    std::size_t begin;
    bool plain;
  };

  cli::OptParser parser_;
  std::map<std::vector<std::string>, Handler> handlers_;
  static constexpr std::size_t byte_limit = 64 * 1024;

  explicit Directives (cli::OptParser parser) : parser_ (std::move (parser)) {}

  static bool space (char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

  static std::size_t
  colon (std::string_view line)
  {
    auto start = line.find_first_not_of (" \t");
    return start != line.npos && line[start] == ':' ? start : line.npos;
  }

  static std::vector<Word>
  words (std::string_view line, std::size_t start)
  {
    if (line.size () > byte_limit || line.find ('\0') != line.npos)
      throw std::invalid_argument ("invalid directive or directive byte limit exceeded");
    std::vector<Word> result;
    for (auto i = start; i < line.size ();)
      {
        if (space (line[i]))
          {
            ++i;
            continue;
          }
        if (result.size () >= 1024)
          throw std::invalid_argument ("directive argument limit exceeded");
        Word word{ {}, i, true };
        char quote = 0;
        do
          {
            auto c = line[i++];
            if (c == '\\' && quote != '\'')
              {
                if (i == line.size ())
                  throw std::invalid_argument ("unfinished directive escape");
                word.value += line[i++];
                word.plain = false;
              }
            else if (quote)
              {
                if (c == quote)
                  quote = 0;
                else
                  word.value += c;
              }
            else if (c == '\'' || c == '"')
              {
                quote = c;
                word.plain = false;
              }
            else
              word.value += c;
          }
        while (i < line.size () && (quote || !space (line[i])));
        if (quote)
          throw std::invalid_argument ("unterminated directive quote");
        result.push_back (std::move (word));
      }
    return result;
  }

public:
  explicit Directives (const cli::Command &root) : parser_ (root) {}

  static Directives from_sexpr (const SExpr &root)
  {
    return Directives (cli::OptParser::from_sexpr (root));
  }

  static Directives from_text (std::string_view root)
  {
    return Directives (cli::OptParser::from_text (root));
  }

  static bool recognizes (std::string_view line) { return colon (line) != line.npos; }

  void bind (std::vector<std::string> path, Handler handler)
  {
    if (!handler)
      throw std::invalid_argument ("directive handler is empty");
    auto *scope = &parser_.root ();
    for (auto &part : path)
      {
        auto child = std::find_if (scope->subcommands.begin (), scope->subcommands.end (),
          [&] (const cli::Command &command) {
            return command.name == part
                   || std::find (command.aliases.begin (), command.aliases.end (), part)
                      != command.aliases.end ();
          });
        if (child == scope->subcommands.end ())
          throw std::invalid_argument ("unknown directive binding");
        part = child->name;
        scope = &*child;
      }
    if (!handlers_.emplace (std::move (path), std::move (handler)).second)
      throw std::invalid_argument ("directive is already bound");
  }

  // Textual schemas describe arguments only. An executable capability is
  // attached to a validated path exclusively through this native method.
  void bind_script (std::vector<std::string> path, scripts::Runner runner)
  {
    bind (std::move (path), [runner = std::move (runner)] (const cli::ParseResult &parsed) {
      const auto atoms = [] (const std::vector<std::string> &values) {
        std::vector<SExpr> nodes;
        for (const auto &v : values)
          nodes.push_back (SExpr::atom (v, true));
        return SExpr::list (std::move (nodes));
      };
      // Sort keys for deterministic wire traces, including inherited/default options.
      std::map<std::string, const std::vector<std::string> *> options;
      for (const auto &[name, values] : parsed.options)
        options.emplace (name, &values);
      std::vector<SExpr> option_nodes;
      for (const auto &[name, values] : options)
        option_nodes.push_back (SExpr::list ({ SExpr::atom (name, true), atoms (*values) }));
      auto payload = SExpr::list ({ SExpr::atom ("directive"), atoms (parsed.command_path),
                                   SExpr::list (std::move (option_nodes)),
                                   atoms (parsed.positionals) }).dump ();
      auto result = runner.run ({ "directive", std::move (payload) });
      return Evaluation{ result.status == scripts::Status::ok, std::move (result.payload) };
    });
  }

  // Empty optional means the line is not a directive. Malformed or unknown
  // directives are consumed, never forwarded to the language evaluator.
  std::optional<Evaluation>
  dispatch (std::string_view line, const cli::Environment &environment = {}) const
  {
    const auto start = colon (line);
    if (start == line.npos)
      return std::nullopt;
    std::vector<std::string> argv;
    try
      {
        for (auto &word : words (line, start + 1))
          argv.push_back (std::move (word.value));
      }
    catch (const std::invalid_argument &error)
      {
        return Evaluation{ false, error.what () };
      }
    if (argv.empty ())
      return Evaluation{ true, parser_.help () };
    // A user-supplied schema never reads the host environment implicitly.
    auto parsed = parser_.parse (argv, environment ? environment
      : cli::Environment{ [] (std::string_view) -> std::optional<std::string> {
          return std::nullopt;
        } });
    if (!parsed)
      {
        std::string output;
        for (const auto &error : parsed.errors)
          {
            if (!output.empty ())
              output += '\n';
            output += error;
          }
        return Evaluation{ false, std::move (output) };
      }
    if (parsed.help_requested)
      return Evaluation{ true, parser_.help (parsed.command_path) };
    const auto handler = handlers_.find (parsed.command_path);
    if (handler == handlers_.end ())
      return Evaluation{ false, "directive has no installed handler" };
    auto callback = handler->second;
    return callback (parsed);
  }

  std::string
  help (const std::vector<std::string> &path = {},
        terminal::RenderMode mode = terminal::RenderMode::plain) const
  {
    return parser_.help (path, mode);
  }

  std::vector<Completion>
  complete (std::string_view line, std::size_t cursor) const
  {
    const auto start = colon (line);
    if (line.size () > byte_limit || start == line.npos || cursor <= start
        || cursor > line.size () || !Editor::is_boundary (line, cursor))
      return {};
    try
      {
        const auto preceding = line.substr (0, cursor);
        auto tokens = words (preceding, start + 1);
        auto begin = cursor, end = cursor;
        std::string prefix;
        // A token ending at cursor remains partial, even if the user is in
        // the middle of the full token. Do not edit a quoted or escaped token.
        if (!tokens.empty () && !space (preceding.back ()))
          {
            auto word = std::move (tokens.back ());
            tokens.pop_back ();
            if (!word.plain)
              return {};
            prefix = std::move (word.value);
            begin = word.begin;
          }
        while (end < line.size () && !space (line[end]))
          {
            if (line[end] == '\'' || line[end] == '"' || line[end] == '\\')
              return {};
            ++end;
          }
        std::vector<std::string> argv;
        argv.reserve (tokens.size ());
        for (auto &token : tokens)
          argv.push_back (std::move (token.value));
        std::vector<Completion> result;
        for (auto &candidate : parser_.complete (argv, prefix))
          {
            if (candidate.empty () || candidate.size () > byte_limit
                || terminal::sanitize_text (candidate) != candidate)
              continue;
            auto insertion = candidate;
            if (insertion.find_first_of (" \t\r\n\\\"'") != insertion.npos)
              {
                insertion = '"';
                for (char c : candidate)
                  {
                    if (c == '"' || c == '\\')
                      insertion += '\\';
                    insertion += c;
                  }
                insertion += '"';
              }
            if (insertion.size () <= byte_limit - line.size () + (end - begin))
              result.push_back ({ begin, end, std::move (insertion), std::move (candidate) });
          }
        return result;
      }
    catch (const std::invalid_argument &)
      {
        return {};
      }
  }

  static AnalysisCallbacks
  analysis (std::shared_ptr<const Directives> directives, AnalysisCallbacks fallback = {})
  {
    return {
      [directives, continuation = std::move (fallback.continuation)] (std::string_view source) {
        if (directives && recognizes (source))
          return live::SyntaxStatus::complete;
        return continuation ? continuation (source) : live::SyntaxStatus::complete;
      },
      [directives = std::move (directives), complete = std::move (fallback.complete)]
      (std::string_view source, std::size_t cursor) {
        if (directives && recognizes (source))
          return directives->complete (source, cursor);
        return complete ? complete (source, cursor) : std::vector<Completion>{};
      }
    };
  }

  static Evaluator
  route (std::shared_ptr<const Directives> directives, Evaluator fallback = {})
  {
    return [directives = std::move (directives), fallback = std::move (fallback)]
      (std::string_view source) -> Evaluation {
        if (directives)
          if (auto result = directives->dispatch (source))
            return std::move (*result);
        if (fallback)
          return fallback (source);
        return { false, "no language evaluator installed" };
      };
  }
};
}

// Canonical language syntax and structural macros. No evaluator or host
// capabilities are installed by reading a specification.
namespace synth
{
struct Form
{
  std::string name;
  std::size_t min_arity;
  std::optional<std::size_t> max_arity;
};

enum class MacroPhase { object, specification };

struct Macro
{
  SExpr pattern;
  SExpr replacement;
  MacroPhase phase = MacroPhase::object;
};

enum class DeclarationKind { state, expression, statement, attribute, metaproc,
                             diagnostic };

struct Declaration
{
  DeclarationKind kind;
  std::string name;
  SExpr body;
};

struct ExpansionLimits
{
  std::size_t max_depth = 128;
  std::size_t max_rewrites = 1024;
  std::size_t max_work = 1024 * 1024;
};

// ExprTemplates builds a native syntax tree without executing C++ arithmetic.
class SyntaxOperand : public DSL<SyntaxOperand, ExprTemplates>
{
  SExpr syntax_;

public:
  explicit SyntaxOperand (std::string name) : syntax_ (SExpr::atom (std::move (name))) {}
  const SExpr &syntax () const { return syntax_; }
};

namespace specification_detail
{
inline constexpr std::size_t max_source_bytes = 256 * 1024;
inline constexpr std::size_t max_tree_depth = 128;

inline bool symbol (const SExpr &expr, std::string_view value)
{
  return expr.is_atom && !expr.quoted && expr.text == value;
}

inline bool headed (const SExpr &expr, std::string_view head)
{
  return !expr.is_atom && !expr.items.empty () && symbol (expr.items[0], head);
}

inline bool bare (std::string_view text)
{
  if (text.empty ())
    return false;
  for (unsigned char c : text)
    if (c <= ' ' || c == 127 || c == '(' || c == ')' || c == '"' || c == ';'
        || c == '\\')
      return false;
  return true;
}

inline bool metavariable (const SExpr &expr)
{
  return expr.is_atom && !expr.quoted && expr.text.size () > 1
         && expr.text[0] == '$';
}

inline void require_name (std::string_view name)
{
  if (!bare (name) || name.front () == '$' || name == "_")
    throw std::invalid_argument ("invalid language, form, or macro name");
}

struct Budget
{
  std::size_t left;
  std::size_t depth;

  void spend (std::size_t amount = 1)
  {
    if (amount > left)
      throw std::length_error ("language syntax work limit exceeded");
    left -= amount;
  }

  void node (std::size_t level, std::size_t bytes = 0)
  {
    if (level > depth)
      throw std::length_error ("language syntax nesting limit exceeded");
    spend ();
    spend (bytes);
  }

  std::size_t child (std::size_t level) const
  {
    if (level >= depth)
      throw std::length_error ("language syntax nesting limit exceeded");
    return level + 1;
  }
};

inline void check_tree (const SExpr &expr, Budget &budget, std::size_t depth = 0)
{
  budget.node (depth, expr.text.size ());
  if (expr.is_atom)
    {
      if (!expr.items.empty () || (!expr.quoted && !bare (expr.text)))
        throw std::invalid_argument ("malformed S-expression atom");
    }
  else
    {
      if (!expr.text.empty () || expr.quoted)
        throw std::invalid_argument ("malformed S-expression list");
      for (const auto &child : expr.items)
        check_tree (child, budget, budget.child (depth));
    }
}

inline SExpr read (std::string_view source)
{
  if (source.size () > max_source_bytes)
    throw std::length_error ("language syntax source limit exceeded");
  return parse_bounded_sexpr (source, SExprLimits{ max_source_bytes, 65536, max_tree_depth });
}

inline void check_encoding (const SExpr &expr, Budget &bytes, std::size_t &nodes,
                            std::size_t depth = 0)
{
  if (!nodes)
    throw std::length_error ("serialized language node limit exceeded");
  --nodes;
  if (depth > bytes.depth)
    throw std::length_error ("serialized language nesting limit exceeded");
  if (expr.is_atom)
    {
      bytes.spend (expr.text.size ());
      if (expr.quoted)
        {
          bytes.spend (2);
          for (char c : expr.text)
            if (c == '\\' || c == '"' || c == '\n' || c == '\r' || c == '\t')
              bytes.spend ();
        }
      return;
    }
  bytes.spend (2);
  if (!expr.items.empty ())
    bytes.spend (expr.items.size () - 1);
  for (const auto &child : expr.items)
    check_encoding (child, bytes, nodes, bytes.child (depth));
}

inline std::size_t arity (const SExpr &expr)
{
  if (!expr.is_atom || expr.quoted || expr.text.empty ())
    throw std::invalid_argument ("expected a nonnegative form arity");
  std::size_t result = 0;
  for (char c : expr.text)
    {
      if (c < '0' || c > '9')
        throw std::invalid_argument ("expected a nonnegative form arity");
      auto digit = static_cast<std::size_t> (c - '0');
      if (result > (std::numeric_limits<std::size_t>::max () - digit) / 10)
        throw std::out_of_range ("form arity is too large");
      result = result * 10 + digit;
    }
  return result;
}

inline const std::string &name (const SExpr &expr)
{
  if (!expr.is_atom || expr.quoted)
    throw std::invalid_argument ("expected an unquoted identifier");
  return expr.text;
}

inline void check_variables (const SExpr &expr,
                             std::unordered_set<std::string_view> &bound,
                             bool pattern)
{
  if (!pattern && headed (expr, "fresh"))
    {
      if (expr.items.size () != 2)
        throw std::invalid_argument ("expected (fresh NAME) in macro template");
      require_name (name (expr.items[1]));
      return;
    }
  if (metavariable (expr))
    {
      if (pattern)
        bound.insert (expr.text);
      else if (!bound.contains (expr.text))
        throw std::invalid_argument ("unbound macro template variable: "
                                     + expr.text);
    }
  for (const auto &child : expr.items)
    check_variables (child, bound, pattern);
}

// Preserve quoting and child order; charge work before comparisons and copies.
inline bool equal (const SExpr &a, const SExpr &b, Budget &budget)
{
  budget.spend ();
  budget.spend (a.text.size ());
  budget.spend (b.text.size ());
  if (a.is_atom != b.is_atom || a.quoted != b.quoted || a.text != b.text
      || a.items.size () != b.items.size ())
    return false;
  for (std::size_t i = 0; i < a.items.size (); ++i)
    if (!equal (a.items[i], b.items[i], budget))
      return false;
  return true;
}

using Captures = std::unordered_map<std::string_view, const SExpr *>;

struct FreshNames
{
  std::unordered_set<std::string> reserved;
  std::size_t counter = 0;

  void reserve (const SExpr &expr)
  {
    if (expr.is_atom)
      reserved.insert (expr.text);
    for (const auto &child : expr.items)
      reserve (child);
  }

  std::string next (Budget &budget)
  {
    for (;;)
      {
        budget.spend ();
        if (counter == std::numeric_limits<std::size_t>::max ())
          throw std::length_error ("fresh macro name space exhausted");
        auto candidate = "__dsl_macro_" + std::to_string (++counter);
        if (reserved.insert (candidate).second)
          return candidate;
      }
  }
};

inline bool match (const SExpr &pattern, const SExpr &input, Captures &bindings,
                   Budget &budget)
{
  budget.spend ();
  budget.spend (pattern.text.size ());
  if (symbol (pattern, "_"))
    return true;
  if (metavariable (pattern))
    {
      auto [it, added] = bindings.emplace (pattern.text, &input);
      return added || equal (*it->second, input, budget);
    }
  budget.spend (input.text.size ());
  if (pattern.is_atom != input.is_atom || pattern.quoted != input.quoted
      || pattern.text != input.text || pattern.items.size () != input.items.size ())
    return false;
  for (std::size_t i = 0; i < pattern.items.size (); ++i)
    if (!match (pattern.items[i], input.items[i], bindings, budget))
      return false;
  return true;
}

inline SExpr substitute (const SExpr &expr, const Captures *bindings,
                         Budget &budget, std::size_t depth,
                         FreshNames *names = nullptr,
                         std::unordered_map<std::string_view, std::string> *fresh = nullptr)
{
  budget.node (depth, expr.text.size ());
  if (bindings && metavariable (expr))
    return substitute (*bindings->at (expr.text), nullptr, budget, depth);
  if (names && headed (expr, "fresh"))
    {
      budget.spend (expr.items[1].text.size ());
      auto [it, inserted] = fresh->try_emplace (expr.items[1].text);
      if (inserted)
        it->second = names->next (budget);
      budget.spend (it->second.size ());
      return SExpr::atom (it->second);
    }
  if (expr.is_atom)
    return expr;
  std::vector<SExpr> children;
  for (const auto &child : expr.items)
    children.push_back (
        substitute (child, bindings, budget, budget.child (depth), names, fresh));
  return SExpr::list (std::move (children));
}

inline void check_ast (const ASTNode &node, Budget &budget, std::size_t depth)
{
  budget.node (depth, node.tag ().size ());
  if (node.is_leaf ())
    {
      if (!node.children ().empty ())
        throw std::invalid_argument ("AST leaf cannot have children");
      budget.node (budget.child (depth), node.value ().size ());
    }
  for (const auto &child : node.children ())
    check_ast (child, budget, budget.child (depth));
}

inline SExpr lower_term (const Term &term, Budget &budget, std::size_t depth,
                         bool head = false)
{
  budget.node (depth, term.symbol.size ());
  if (term.is_variable)
    {
      if (head || !term.arguments.empty () || !bare (term.symbol))
        throw std::invalid_argument ("invalid term variable in macro");
      return SExpr::atom ("$" + term.symbol);
    }
  auto name = SExpr::atom (term.symbol, term.symbol.starts_with ('$')
                                        || term.symbol == "_");
  if (!head && term.arguments.empty ())
    return name;
  std::vector<SExpr> parts{ std::move (name) };
  for (const auto &arg : term.arguments)
    parts.push_back (lower_term (arg, budget, budget.child (depth)));
  return SExpr::list (std::move (parts));
}

struct ExprLowerer
{
  static SExpr lower (const SyntaxOperand &operand, Budget &budget, std::size_t depth)
  {
    return substitute (operand.syntax (), nullptr, budget, depth);
  }

  template <std::integral Number>
  static SExpr lower (Number value, Budget &budget, std::size_t depth)
  {
    auto text = std::to_string (value);
    budget.node (depth, text.size ());
    return SExpr::atom (std::move (text));
  }

  template <class Op, class Lhs, class Rhs>
  static SExpr lower (const BinExpr<Op, Lhs, Rhs> &expr, Budget &budget,
                      std::size_t depth)
  {
    budget.node (depth);
    const char *symbol = nullptr;
    if constexpr (std::same_as<Op, std::plus<>>) symbol = "+";
    else if constexpr (std::same_as<Op, std::minus<>>) symbol = "-";
    else if constexpr (std::same_as<Op, std::multiplies<>>) symbol = "*";
    else if constexpr (std::same_as<Op, std::divides<>>) symbol = "/";
    else throw std::invalid_argument ("unsupported native operator syntax");
    auto next = budget.child (depth);
    return SExpr::list ({ SExpr::atom (symbol), lower (expr.lhs, budget, next),
                          lower (expr.rhs, budget, next) });
  }

  template <class Op, class Value>
  static SExpr lower (const UnaryExpr<Op, Value> &expr, Budget &budget,
                      std::size_t depth)
  {
    if constexpr (!std::same_as<Op, std::negate<>>)
      throw std::invalid_argument ("unsupported native unary operator syntax");
    budget.node (depth);
    return SExpr::list ({ SExpr::atom ("-"),
                          lower (expr.operand, budget, budget.child (depth)) });
  }
};

inline constexpr std::array<std::string_view, 6> declaration_names{
  "state", "expression", "statement", "attribute", "metaproc", "diagnostic"
};

inline void check_schema (const SExpr &type)
{
  if (symbol (type, "syntax") || symbol (type, "atom") || symbol (type, "string")
      || symbol (type, "boolean") || symbol (type, "integer"))
    return;
  if (headed (type, "list") && type.items.size () == 2)
    {
      check_schema (type.items[1]);
      return;
    }
  if (headed (type, "record"))
    {
      std::unordered_set<std::string_view> fields;
      for (std::size_t i = 1; i < type.items.size (); ++i)
        {
          const auto &field = type.items[i];
          if (field.is_atom || field.items.size () != 2)
            throw std::invalid_argument ("expected (FIELD TYPE) in state record");
          const auto &id = name (field.items[0]);
          require_name (id);
          if (!fields.insert (id).second)
            throw std::invalid_argument ("duplicate state field: " + id);
          check_schema (field.items[1]);
        }
      return;
    }
  throw std::invalid_argument ("unknown textual state type");
}

inline bool state_value (const SExpr &type, const SExpr &value, Budget &budget)
{
  budget.spend ();
  if (symbol (type, "syntax"))
    return true;
  if (symbol (type, "atom"))
    return value.is_atom && !value.quoted;
  if (symbol (type, "string"))
    return value.is_atom && value.quoted;
  if (symbol (type, "boolean"))
    return symbol (value, "true") || symbol (value, "false");
  if (symbol (type, "integer"))
    {
      if (!value.is_atom || value.quoted || value.text.empty ())
        return false;
      auto digits = std::string_view (value.text);
      if (digits.front () == '-')
        digits.remove_prefix (1);
      return !digits.empty () &&
             std::all_of (digits.begin (), digits.end (),
                          [] (char c) { return c >= '0' && c <= '9'; });
    }
  if (value.is_atom)
    return false;
  if (headed (type, "list"))
    {
      for (const auto &element : value.items)
        if (!state_value (type.items[1], element, budget))
          return false;
      return true;
    }
  if (!headed (value, "record") || value.items.size () != type.items.size ())
    return false;
  std::unordered_map<std::string_view, const SExpr *> fields;
  for (std::size_t i = 1; i < value.items.size (); ++i)
    {
      const auto &field = value.items[i];
      if (field.is_atom || field.items.size () != 2 || !field.items[0].is_atom
          || field.items[0].quoted
          || !fields.emplace (field.items[0].text, &field.items[1]).second)
        return false;
    }
  for (std::size_t i = 1; i < type.items.size (); ++i)
    {
      const auto &field = type.items[i];
      budget.spend (field.items[0].text.size ());
      auto it = fields.find (field.items[0].text);
      if (it == fields.end () || !state_value (field.items[1], *it->second, budget))
        return false;
    }
  return true;
}
} // namespace specification_detail

class SpecBuilder;

// An immutable snapshot: native descriptors and textual declarations both
// pass through SpecBuilder before this type is published.
class Specification
{
  friend class SpecBuilder;
  std::string name_;
  std::vector<Form> forms_;
  std::vector<Macro> macros_;
  std::vector<Declaration> declarations_;
  std::optional<CFG> grammar_;

  explicit Specification (std::string_view name) : name_ (name) {}

public:
  Specification (const Specification &) = default;
  Specification &operator= (const Specification &) = default;
  const std::string &name () const { return name_; }
  const std::vector<Form> &forms () const { return forms_; }
  const std::vector<Macro> &macros () const { return macros_; }
  const std::vector<Declaration> &declarations () const { return declarations_; }
  const std::optional<CFG> &grammar () const { return grammar_; }
  static Specification from_sexpr (const SExpr &source);
  static Specification from_text (std::string_view source);
  SExpr to_sexpr () const;
  void validate_state (std::string_view schema, const SExpr &value) const;
};

class SpecBuilder
{
  Specification spec_;
  specification_detail::Budget budget_{ ExpansionLimits{}.max_work,
                                         specification_detail::max_tree_depth };

public:
  explicit SpecBuilder (std::string_view name) : spec_ ([&]
    {
      if (name.size () > specification_detail::max_source_bytes)
        throw std::length_error ("language name is too long");
      specification_detail::require_name (name);
      return name;
    } ())
  {
    budget_.spend (name.size ());
  }

  SpecBuilder &form (const Form &desc)
  {
    using namespace specification_detail;
    auto next = budget_;
    next.node (0, desc.name.size ());
    require_name (desc.name);
    if (desc.max_arity && *desc.max_arity < desc.min_arity)
      throw std::invalid_argument ("inverted form arity interval");
    for (const auto &entry : spec_.forms_)
      {
        next.node (0, entry.name.size ());
        if (entry.name == desc.name)
          throw std::invalid_argument ("duplicate form: " + desc.name);
      }
    spec_.forms_.push_back (desc);
    budget_ = next;
    return *this;
  }

  SpecBuilder &form (std::string_view name, std::size_t arity)
  {
    if (name.size () > budget_.left)
      throw std::length_error ("language syntax work limit exceeded");
    return form ({ std::string (name), arity, arity });
  }

  template <FixedString Name, std::size_t Arity> SpecBuilder &form ()
  {
    return form (Name.view (), Arity);
  }

  SpecBuilder &declare (DeclarationKind kind, std::string_view id, const SExpr &body)
  {
    using namespace specification_detail;
    auto next = budget_;
    next.node (0, id.size ());
    require_name (id);
    check_tree (body, next, 2);
    if (static_cast<std::size_t> (kind) >= declaration_names.size ())
      throw std::invalid_argument ("unknown declaration kind");
    if (kind == DeclarationKind::state)
      check_schema (body);
    if (kind == DeclarationKind::diagnostic && (!body.is_atom || !body.quoted))
      throw std::invalid_argument ("diagnostic requires a quoted message");
    for (const auto &entry : spec_.declarations_)
      {
        next.node (0, entry.name.size ());
        if (entry.kind == kind && entry.name == id)
          throw std::invalid_argument ("duplicate specification declaration");
      }
    spec_.declarations_.push_back ({ kind, std::string (id), body });
    budget_ = next;
    return *this;
  }

  SpecBuilder &declare (DeclarationKind kind, std::string_view id, const ASTNode &body)
  {
    auto budget = budget_;
    specification_detail::check_ast (body, budget, 2);
    return declare (kind, id, ast_to_sexpr (body));
  }

  template <class Expr>
  SpecBuilder &expression (std::string_view id, const Expr &expr)
  {
    auto budget = budget_;
    return declare (DeclarationKind::expression, id,
                    specification_detail::ExprLowerer::lower (expr, budget, 2));
  }

  SpecBuilder &macro (const SExpr &pattern, const SExpr &replacement,
                      MacroPhase phase = MacroPhase::object)
  {
    using namespace specification_detail;
    auto next = budget_;
    // Macro bodies occupy depth two in the serialized specification.
    check_tree (pattern, next, 2);
    check_tree (replacement, next, 2);
    if (pattern.is_atom || pattern.items.empty ())
      throw std::invalid_argument ("macro requires a literal-headed list pattern");
    auto &head = name (pattern.items[0]);
    require_name (head);
    if (head == "quote")
      throw std::invalid_argument ("quote is reserved by macro expansion");
    if (phase != MacroPhase::object && phase != MacroPhase::specification)
      throw std::invalid_argument ("unknown macro phase");
    std::unordered_set<std::string_view> bound;
    check_variables (pattern, bound, true);
    check_variables (replacement, bound, false);
    for (const auto &entry : spec_.macros_)
      if (entry.phase == phase && equal (entry.pattern, pattern, next))
        throw std::invalid_argument ("duplicate macro pattern for " + head);
    spec_.macros_.push_back ({ pattern, replacement, phase });
    budget_ = next;
    return *this;
  }

  SpecBuilder &macro (const TermRule &rule, MacroPhase phase = MacroPhase::object)
  {
    auto budget = budget_;
    auto pattern = specification_detail::lower_term (rule.lhs, budget, 2, true);
    auto replacement = specification_detail::lower_term (rule.rhs, budget, 2);
    return macro (pattern, replacement, phase);
  }

  SpecBuilder &grammar (const CFG &grammar)
  {
    using namespace specification_detail;
    if (spec_.grammar_)
      throw std::invalid_argument ("duplicate grammar declaration");
    auto next = budget_;
    next.node (0, grammar.start.size ());
    require_name (grammar.start);
    for (const auto &rule : grammar.productions)
      {
        next.node (0, rule.lhs.size ());
        require_name (rule.lhs);
        for (const auto &entry : rule.rhs)
          {
            next.node (0, entry.name.size ());
            if (!entry.terminal)
              require_name (entry.name);
          }
      }
    grammar.validate ();
    spec_.grammar_ = grammar;
    budget_ = next;
    return *this;
  }

  Specification build () const
  {
    auto result = spec_;
    std::stable_partition (result.macros_.begin (), result.macros_.end (),
                           [] (const Macro &rule)
                           { return rule.phase == MacroPhase::specification; });
    specification_detail::Budget bytes{ specification_detail::max_source_bytes,
                                        specification_detail::max_tree_depth };
    std::size_t nodes = 65536;
    specification_detail::check_encoding (result.to_sexpr (), bytes, nodes);
    return result;
  }
};

namespace specification_detail
{
inline SExpr expand_specification (const Specification &, const SExpr &);
} // namespace specification_detail

inline Specification Specification::from_sexpr (const SExpr &source)
{
  using namespace specification_detail;
  Budget budget{ ExpansionLimits{}.max_work, max_tree_depth };
  check_tree (source, budget);
  if (!headed (source, "language") || source.items.size () < 2)
    throw std::invalid_argument ("expected (language NAME DECLARATION ...)");
  const auto &id = specification_detail::name (source.items[1]);
  SpecBuilder early (id);
  std::vector<SExpr> expansions{ SExpr::atom ("__dsl_spec_batch", true) };
  for (std::size_t i = 2; i < source.items.size (); ++i)
    {
      const auto &part = source.items[i];
      if (headed (part, "spec-macro"))
        {
          if (part.items.size () != 3)
            throw std::invalid_argument ("expected (spec-macro PATTERN TEMPLATE)");
          early.macro (part.items[1], part.items[2], MacroPhase::specification);
        }
      else if (headed (part, "expand-spec"))
        {
          if (part.items.size () != 2)
            throw std::invalid_argument ("expected (expand-spec DECLARATION)");
          expansions.push_back (part.items[1]);
        }
    }
  auto expanded = expansions.size () == 1
                      ? SExpr::list (std::move (expansions))
                      : expand_specification (early.build (),
                                              SExpr::list (std::move (expansions)));
  auto builder = std::move (early);
  std::size_t expansion = 1;
  for (std::size_t i = 2; i < source.items.size (); ++i)
    {
      const auto &original = source.items[i];
      if (headed (original, "spec-macro"))
        continue;
      const auto &part = headed (original, "expand-spec")
                             ? expanded.items.at (expansion++) : original;
      const auto &items = part.items;
      if (headed (part, "form") && (items.size () == 3 || items.size () == 4))
        {
          auto min = arity (items[2]);
          auto max = std::optional{ min };
          if (items.size () == 4)
            max = symbol (items[3], "*") ? std::nullopt
                                           : std::optional{ arity (items[3]) };
          builder.form ({ specification_detail::name (items[1]), min, max });
        }
      else if (headed (part, "macro") && items.size () == 3)
        builder.macro (items[1], items[2]);
      else if (headed (part, "grammar"))
        {
          // Validate declaration shape before delegating to the existing CFG
          // adapter, which otherwise accepts multiple start declarations.
          bool start = false;
          for (std::size_t j = 1; j < items.size (); ++j)
            {
              const auto &decl = items[j];
              if (headed (decl, "start") && decl.items.size () == 2 && !start)
                {
                  specification_detail::name (decl.items[1]);
                  start = true;
                }
              else if (headed (decl, "rule") && decl.items.size () >= 3)
                specification_detail::name (decl.items[1]);
              else
                throw std::invalid_argument ("invalid language grammar declaration");
            }
          builder.grammar (sexpr_grammar (part));
        }
      else
        {
          bool recognized = false;
          if (items.size () == 3)
            for (std::size_t d = 0; d < declaration_names.size (); ++d)
              if (headed (part, declaration_names[d]))
                {
                  builder.declare (static_cast<DeclarationKind> (d),
                                   specification_detail::name (items[1]), items[2]);
                  recognized = true;
                  break;
                }
          if (!recognized)
            throw std::invalid_argument ("unknown or malformed language declaration");
        }
    }
  return builder.build ();
}

inline Specification Specification::from_text (std::string_view source)
{
  return from_sexpr (specification_detail::read (source));
}

inline SExpr Specification::to_sexpr () const
{
  std::vector<SExpr> out{ SExpr::atom ("language"), SExpr::atom (name_) };
  for (const auto &entry : forms_)
    {
      std::vector<SExpr> decl{ SExpr::atom ("form"), SExpr::atom (entry.name),
                             SExpr::atom (std::to_string (entry.min_arity)) };
      if (!entry.max_arity || *entry.max_arity != entry.min_arity)
        decl.push_back (SExpr::atom (entry.max_arity
                                       ? std::to_string (*entry.max_arity) : "*"));
      out.push_back (SExpr::list (std::move (decl)));
    }
  for (const auto &entry : macros_)
    out.push_back (SExpr::list ({ SExpr::atom (entry.phase == MacroPhase::object
                                                ? "macro" : "spec-macro"), entry.pattern,
                                  entry.replacement }));
  for (const auto &entry : declarations_)
    out.push_back (SExpr::list ({ SExpr::atom (std::string (
                                     specification_detail::declaration_names[
                                         static_cast<std::size_t> (entry.kind)])),
                                  SExpr::atom (entry.name), entry.body }));
  if (grammar_)
    {
      std::vector<SExpr> decl{ SExpr::atom ("grammar"),
                              SExpr::list ({ SExpr::atom ("start"),
                                             SExpr::atom (grammar_->start) }) };
      for (const auto &rule : grammar_->productions)
        {
          std::vector<SExpr> rhs;
          for (const auto &entry : rule.rhs)
            rhs.push_back (SExpr::atom (entry.name, entry.terminal));
          decl.push_back (SExpr::list ({ SExpr::atom ("rule"), SExpr::atom (rule.lhs),
                                         SExpr::list (std::move (rhs)) }));
        }
      out.push_back (SExpr::list (std::move (decl)));
    }
  return SExpr::list (std::move (out));
}

inline void Specification::validate_state (std::string_view schema,
                                           const SExpr &value) const
{
  using namespace specification_detail;
  Budget budget{ ExpansionLimits{}.max_work, max_tree_depth };
  budget.spend (schema.size ());
  check_tree (value, budget);
  for (const auto &decl : declarations_)
    if (decl.kind == DeclarationKind::state && decl.name == schema)
      {
        if (!state_value (decl.body, value, budget))
          throw std::invalid_argument ("value does not match the state schema");
        return;
      }
  throw std::invalid_argument ("unknown state schema");
}

struct MacroExpansion
{
  std::size_t macro_index;
  std::vector<std::size_t> path;
};

struct ExpansionResult
{
  SExpr syntax;
  std::vector<MacroExpansion> provenance;
};

class MetaLanguage
{
  Specification spec_;
  using MacroIndex = std::unordered_map<std::string, std::vector<std::size_t>>;
  MacroIndex object_macros_, spec_macros_;

  // Outermost first, then children in order, then reconsider the head if a
  // child rewrote. The first matching rule wins; quote is opaque to expansion.
  SExpr expand_node (SExpr node, const ExpansionLimits &limits,
                     specification_detail::Budget &budget,
                     std::size_t &rewrites, const MacroIndex &macros,
                     specification_detail::FreshNames &names,
                     std::vector<std::size_t> &path,
                     std::vector<MacroExpansion> *log) const
  {
    using namespace specification_detail;
    for (;;)
      {
        budget.node (path.size (), node.text.size ());
        if (node.is_atom || node.items.empty () || headed (node, "quote"))
          return node;
        bool rewritten = false;
        const auto &head = node.items[0];
        if (head.is_atom && !head.quoted)
          {
            budget.spend (head.text.size ());
            auto it = macros.find (head.text);
            if (it != macros.end ())
              for (auto index : it->second)
                {
                  Captures bindings;
                  const auto &entry = spec_.macros ()[index];
                  if (!specification_detail::match (entry.pattern, node, bindings, budget))
                    continue;
                  if (rewrites >= limits.max_rewrites)
                    throw std::length_error ("macro rewrite limit exceeded");
                  ++rewrites;
                  if (log)
                    {
                      budget.spend (path.size ());
                      log->push_back ({ index, path });
                    }
                  std::unordered_map<std::string_view, std::string> fresh;
                  node = substitute (entry.replacement, &bindings, budget,
                                     path.size (), &names, &fresh);
                  rewritten = true;
                  break;
                }
          }
        if (rewritten)
          continue;
        auto before = rewrites;
        for (std::size_t i = 0; i < node.items.size (); ++i)
          {
            budget.child (path.size ());
            path.push_back (i);
            node.items[i] = expand_node (std::move (node.items[i]), limits,
                                         budget, rewrites, macros, names, path, log);
            path.pop_back ();
          }
        if (before == rewrites)
          return node;
      }
  }

  SExpr expand_with (const SExpr &program, ExpansionLimits limits,
                    MacroPhase phase, std::vector<MacroExpansion> *log) const
  {
    using namespace specification_detail;
    Budget budget{ limits.max_work, std::min (limits.max_depth, max_tree_depth) };
    check_tree (program, budget);
    std::size_t rewrites = 0;
    std::vector<std::size_t> path;
    FreshNames names;
    const MacroIndex *index = nullptr;
    if (phase == MacroPhase::object)
      index = &object_macros_;
    else if (phase == MacroPhase::specification)
      index = &spec_macros_;
    else
      throw std::invalid_argument ("unknown macro phase");
    if (index->empty ())
      return program;
    names.reserve (program);
    for (const auto &macro : spec_.macros ())
      if (macro.phase == phase)
        {
          check_tree (macro.replacement, budget);
          names.reserve (macro.replacement);
        }
    return expand_node (program, limits, budget, rewrites, *index, names, path, log);
  }

public:
  MetaLanguage (const MetaLanguage &) = default;
  MetaLanguage &operator= (const MetaLanguage &) = default;
  explicit MetaLanguage (const Specification &spec) : spec_ (spec)
  {
    for (std::size_t i = 0; i < spec_.macros ().size (); ++i)
      {
        const auto &entry = spec_.macros ()[i];
        auto &index = entry.phase == MacroPhase::object ? object_macros_ : spec_macros_;
        index[entry.pattern.items[0].text].push_back (i);
      }
  }

  const Specification &specification () const { return spec_; }

  SExpr expand (const SExpr &program, ExpansionLimits limits = {}) const
  {
    return expand_with (program, limits, MacroPhase::object, nullptr);
  }

  ExpansionResult expand_report (const SExpr &program, ExpansionLimits limits = {},
                                 MacroPhase phase = MacroPhase::object) const
  {
    ExpansionResult result;
    result.syntax = expand_with (program, limits, phase, &result.provenance);
    return result;
  }

  SExpr expand (std::string_view program, ExpansionLimits limits = {}) const
  {
    if (program.size () > limits.max_work)
      throw std::length_error ("language syntax work limit exceeded");
    return expand (specification_detail::read (program), limits);
  }

  // Check one registered form, not its arguments. A language may choose its
  // own validation for literals, binder lists, quoted data or open calls.
  void validate_form (const SExpr &form) const
  {
    using namespace specification_detail;
    Budget budget{ ExpansionLimits{}.max_work, max_tree_depth };
    check_tree (form, budget);
    if (form.is_atom || form.items.empty ())
      throw std::invalid_argument ("expected a registered language form");
    const auto &name = specification_detail::name (form.items[0]);
    for (const auto &entry : spec_.forms ())
      if (entry.name == name)
        {
          auto arity = form.items.size () - 1;
          if (arity < entry.min_arity || (entry.max_arity && arity > *entry.max_arity))
            throw std::invalid_argument ("wrong arity for language form " + name);
          return;
        }
    throw std::invalid_argument ("unknown language form " + name);
  }
};

inline SExpr specification_detail::expand_specification (const Specification &spec,
                                                        const SExpr &syntax)
{
  return MetaLanguage (spec).expand_report (syntax, {}, MacroPhase::specification).syntax;
}

// A specification may be paired with any native state type. Registration is an
// explicit host action; declarations cannot manufacture executable callbacks.
template <class State> class NativeState
{
public:
  using Operation = std::function<SExpr (State &, std::span<const SExpr>)>;

private:
  Specification specification_;
  std::unordered_map<std::string, Operation> operations_;

public:
  explicit NativeState (const Specification &spec) : specification_ (spec) {}
  const Specification &specification () const { return specification_; }

  NativeState &operation (std::string name, Operation callable)
  {
    specification_detail::require_name (name);
    if (!callable || !operations_.emplace (std::move (name), std::move (callable)).second)
      throw std::invalid_argument ("empty or duplicate native state operation");
    return *this;
  }

  SExpr invoke (State &state, std::string_view name, std::span<const SExpr> arguments) const
  {
    using namespace specification_detail;
    Budget budget{ ExpansionLimits{}.max_work, max_tree_depth };
    budget.spend (name.size ());
    auto it = operations_.find (std::string (name));
    if (it == operations_.end ())
      throw std::invalid_argument ("unregistered native state operation");
    for (const auto &arg : arguments)
      check_tree (arg, budget);
    auto result = it->second (state, arguments);
    check_tree (result, budget);
    return result;
  }
};

struct EvaluationLimits
{
  std::size_t max_depth = 128;
  std::size_t max_work = 1024 * 1024;
};

namespace specification_detail
{
struct Closure;
}

class MetaValue;
using MetaPrimitive = std::function<MetaValue (std::span<const MetaValue>)>;

class MetaValue
{
  friend class MetaEvaluator;
  using Syntax = std::shared_ptr<const SExpr>;
  using Function = std::shared_ptr<const specification_detail::Closure>;
  using Primitive = std::shared_ptr<const MetaPrimitive>;
  using Storage = std::variant<Syntax, Function, Primitive>;
  Storage value_;

  explicit MetaValue (Function closure) : value_ (std::move (closure)) {}
  explicit MetaValue (Primitive primitive) : value_ (std::move (primitive)) {}

public:
  explicit MetaValue (const SExpr &syntax)
  {
    specification_detail::Budget budget{ ExpansionLimits{}.max_work,
                                         specification_detail::max_tree_depth };
    specification_detail::check_tree (syntax, budget);
    value_ = std::make_shared<const SExpr> (syntax);
  }

  explicit MetaValue (bool truth) : MetaValue (SExpr::atom (truth ? "true" : "false")) {}
  MetaValue (const MetaValue &) = default;
  MetaValue &operator= (const MetaValue &) = default;

  bool is_syntax () const { return std::holds_alternative<Syntax> (value_); }
  const SExpr &as_syntax () const
  {
    if (const auto *syntax = std::get_if<Syntax> (&value_))
      return **syntax;
    throw std::invalid_argument ("metalanguage callable is not syntax");
  }
};

namespace specification_detail
{
struct Environment
{
  std::shared_ptr<const Environment> parent;
  std::unordered_map<std::string, MetaValue> bindings;
};

struct Closure
{
  std::shared_ptr<const Environment> env;
  std::shared_ptr<const SExpr> owner;
  const SExpr *parameters;
  const SExpr *body;
};

inline bool literal (const SExpr &expr)
{
  if (!expr.is_atom)
    return false;
  if (expr.quoted || symbol (expr, "true") || symbol (expr, "false"))
    return true;
  auto digits = std::string_view (expr.text);
  if (!digits.empty () && digits.front () == '-')
    digits.remove_prefix (1);
  return !digits.empty () && std::all_of (digits.begin (), digits.end (),
                                           [] (char c) { return c >= '0' && c <= '9'; });
}

inline void check_pattern (const SExpr &pattern, std::unordered_set<std::string_view> &seen,
                           Budget &budget)
{
  budget.node (0, pattern.text.size ());
  if (pattern.is_atom)
    {
      if (literal (pattern) || symbol (pattern, "_"))
        return;
      require_name (pattern.text);
      if (!seen.insert (pattern.text).second)
        throw std::invalid_argument ("duplicate destructuring variable: " + pattern.text);
    }
  for (const auto &child : pattern.items)
    check_pattern (child, seen, budget);
}
} // namespace specification_detail

// Pure lexical interpreter over syntax and closures. Trusted primitives may
// be supplied by a host; their own resource usage is outside evaluator limits.
class MetaEvaluator
{
  using Budget = specification_detail::Budget;
  using Environment = specification_detail::Environment;
  using Env = std::shared_ptr<const Environment>;
  using Syntax = std::shared_ptr<const SExpr>;
  using Closure = specification_detail::Closure;
  std::unordered_map<std::string, MetaValue> primitives_;

  static MetaValue syntax_value (const SExpr &expr, Budget &budget)
  {
    specification_detail::check_tree (expr, budget);
    return MetaValue (expr);
  }

  static void bind (const SExpr &pattern, const MetaValue &value,
                    Environment &env, Budget &budget)
  {
    using namespace specification_detail;
    budget.node (0, pattern.text.size ());
    if (symbol (pattern, "_"))
      return;
    if (pattern.is_atom && !literal (pattern))
      {
        env.bindings.emplace (pattern.text, value);
        return;
      }
    const auto &syntax = value.as_syntax ();
    if (pattern.is_atom ? !equal (pattern, syntax, budget)
                        : syntax.is_atom || pattern.items.size () != syntax.items.size ())
      throw std::invalid_argument ("metalanguage destructuring mismatch");
    for (std::size_t i = 0; i < pattern.items.size (); ++i)
      bind (pattern.items[i], syntax_value (syntax.items[i], budget), env, budget);
  }

  static MetaValue eval (const SExpr &expr, const Syntax &owner, const Env &env,
                         Budget &budget, std::size_t depth)
  {
    using namespace specification_detail;
    budget.node (depth, expr.text.size ());
    if (expr.is_atom)
      {
        if (literal (expr))
          return syntax_value (expr, budget);
        for (auto scope = env; scope; scope = scope->parent)
          {
            budget.node (0, expr.text.size ());
            auto it = scope->bindings.find (expr.text);
            if (it != scope->bindings.end ())
              return it->second;
          }
        throw std::invalid_argument ("unbound metalanguage variable: " + expr.text);
      }
    const auto &items = expr.items;
    if (items.empty ())
      return syntax_value (expr, budget);
    auto next = [&] { return budget.child (depth); };
    if (symbol (items[0], "quote"))
      {
        if (items.size () != 2)
          throw std::invalid_argument ("expected (quote SYNTAX)");
        return syntax_value (items[1], budget);
      }
    if (symbol (items[0], "lambda"))
      {
        if (items.size () != 3 || items[1].is_atom)
          throw std::invalid_argument ("expected (lambda (PARAM ...) BODY)");
        std::unordered_set<std::string_view> seen;
        for (const auto &pattern : items[1].items)
          check_pattern (pattern, seen, budget);
        return MetaValue (std::make_shared<const Closure> (
            Closure{ env, owner, &items[1], &items[2] }));
      }
    if (symbol (items[0], "if"))
      {
        if (items.size () != 4)
          throw std::invalid_argument ("expected (if CONDITION THEN ELSE)");
        auto condition = eval (items[1], owner, env, budget, next ());
        const auto &value = condition.as_syntax ();
        if (symbol (value, "true"))
          return eval (items[2], owner, env, budget, next ());
        if (symbol (value, "false"))
          return eval (items[3], owner, env, budget, next ());
        throw std::invalid_argument ("metalanguage condition is not boolean");
      }
    if (symbol (items[0], "let"))
      {
        if (items.size () != 4)
          throw std::invalid_argument ("expected (let PATTERN VALUE BODY)");
        std::unordered_set<std::string_view> seen;
        check_pattern (items[1], seen, budget);
        auto value = eval (items[2], owner, env, budget, next ());
        auto local = std::make_shared<Environment> (Environment{ env, {} });
        bind (items[1], value, *local, budget);
        return eval (items[3], owner, local, budget, next ());
      }
    // The built-in syntax operators are budgeted at every copied node.
    if (symbol (items[0], "list"))
      {
        std::vector<SExpr> children;
        for (std::size_t i = 1; i < items.size (); ++i)
          {
            auto argument = eval (items[i], owner, env, budget, next ());
            children.push_back (substitute (argument.as_syntax (), nullptr, budget, 1));
          }
        return syntax_value (SExpr::list (std::move (children)), budget);
      }
    if (symbol (items[0], "equal?"))
      {
        if (items.size () != 3)
          throw std::invalid_argument ("expected (equal? A B)");
        auto a = eval (items[1], owner, env, budget, next ());
        auto b = eval (items[2], owner, env, budget, next ());
        return MetaValue (equal (a.as_syntax (), b.as_syntax (), budget));
      }
    auto callee = eval (items[0], owner, env, budget, next ());
    const auto *closure = std::get_if<MetaValue::Function> (&callee.value_);
    const auto *primitive = std::get_if<MetaValue::Primitive> (&callee.value_);
    if (!closure && !primitive)
      throw std::invalid_argument ("metalanguage application requires a callable");
    if (closure && (*closure)->parameters->items.size () != items.size () - 1)
      throw std::invalid_argument ("wrong number of metalanguage lambda arguments");
    std::vector<MetaValue> arguments;
    for (std::size_t i = 1; i < items.size (); ++i)
      arguments.push_back (eval (items[i], owner, env, budget, next ()));
    if (primitive)
      {
        auto result = (**primitive) (arguments);
        if (result.is_syntax ())
          check_tree (result.as_syntax (), budget);
        return result;
      }
    auto local = std::make_shared<Environment> (Environment{ (*closure)->env, {} });
    for (std::size_t i = 0; i < arguments.size (); ++i)
      bind ((*closure)->parameters->items[i], arguments[i], *local, budget);
    return eval (*(*closure)->body, (*closure)->owner, local, budget, next ());
  }

public:
  MetaEvaluator &primitive (std::string_view id, MetaPrimitive callable)
  {
    using namespace specification_detail;
    require_name (id);
    if (id == "quote" || id == "lambda" || id == "let" || id == "if"
        || id == "list" || id == "equal?" || literal (SExpr::atom (std::string (id))))
      throw std::invalid_argument ("cannot register a reserved metalanguage name");
    if (!callable || !primitives_.emplace (std::string (id),
          MetaValue (std::make_shared<const MetaPrimitive> (std::move (callable)))).second)
      throw std::invalid_argument ("empty or duplicate metalanguage primitive");
    return *this;
  }

  MetaValue evaluate (const SExpr &expr, EvaluationLimits limits = {}) const
  {
    using namespace specification_detail;
    Budget budget{ limits.max_work, std::min (limits.max_depth, max_tree_depth) };
    check_tree (expr, budget);
    for (const auto &entry : primitives_)
      budget.node (0, entry.first.size ());
    auto owner = std::make_shared<const SExpr> (expr);
    auto env = std::make_shared<const Environment> (Environment{ {}, primitives_ });
    return eval (*owner, owner, env, budget, 0);
  }

  MetaValue evaluate (std::string_view source, EvaluationLimits limits = {}) const
  {
    if (source.size () > limits.max_work)
      throw std::length_error ("metalanguage evaluation work limit exceeded");
    return evaluate (specification_detail::read (source), limits);
  }
};
} // namespace synth

struct LanguageSynthesizer
{
  template <class Derived> struct Mixin
  {
    static synth::MetaLanguage synthesize (const synth::Specification &spec)
    {
      return synth::MetaLanguage (spec);
    }
    static synth::MetaLanguage synthesize (const SExpr &spec)
    {
      return synthesize (synth::Specification::from_sexpr (spec));
    }
    static synth::MetaLanguage synthesize (std::string_view spec)
    {
      return synthesize (synth::Specification::from_text (spec));
    }
  };
};

// Declarative, evaluation-local attributes and lexical metaprocedures.
namespace synth
{
struct AttributeKey
{
  // IDs are supplied by the caller, not derived from AST object addresses.
  std::string node;
  std::string attribute;

  bool operator==(const AttributeKey&) const = default;
};

struct AttributeKeyHash
{
  std::size_t operator()(const AttributeKey& key) const noexcept
  {
    const auto node = std::hash<std::string>{}(key.node);
    return node ^ (std::hash<std::string>{}(key.attribute)
                   + (node << 6) + (node >> 2));
  }
};

enum class SynthesisError
{
  invalid_declaration, duplicate_declaration, missing_attribute,
  undeclared_dependency, circular_dependency, nonconvergent, budget,
  duplicate_binding, missing_binding, missing_procedure, argument_count,
  callback_failed, wrong_phase, inadmissible_state
};

class SynthesisFailure : public std::runtime_error
{
public:
  const SynthesisError code;
  const live::SourceSpan span;
  const std::vector<AttributeKey> trace;

  SynthesisFailure(SynthesisError kind, std::string message,
                   live::SourceSpan source = {},
                   std::vector<AttributeKey> path = {})
    : std::runtime_error(std::move(message)), code(kind), span(source),
      trace(std::move(path))
  {
  }

  live::Diagnostic diagnostic() const
  {
    const char* id = "unknown";
    switch (code)
      {
      case SynthesisError::invalid_declaration: id = "invalid-declaration"; break;
      case SynthesisError::duplicate_declaration: id = "duplicate-declaration"; break;
      case SynthesisError::missing_attribute: id = "missing-attribute"; break;
      case SynthesisError::undeclared_dependency: id = "undeclared-dependency"; break;
      case SynthesisError::circular_dependency: id = "circular-dependency"; break;
      case SynthesisError::nonconvergent: id = "nonconvergent"; break;
      case SynthesisError::budget: id = "budget"; break;
      case SynthesisError::duplicate_binding: id = "duplicate-binding"; break;
      case SynthesisError::missing_binding: id = "missing-binding"; break;
      case SynthesisError::missing_procedure: id = "missing-procedure"; break;
      case SynthesisError::argument_count: id = "argument-count"; break;
      case SynthesisError::callback_failed: id = "callback-failed"; break;
      case SynthesisError::wrong_phase: id = "wrong-phase"; break;
      case SynthesisError::inadmissible_state: id = "inadmissible-state"; break;
      }
    return {live::Severity::error, std::string("synth.") + id, what(), span};
  }
};

namespace attribute_detail
{
inline constexpr std::size_t source_limit = 1024 * 1024;

inline std::string describe(const AttributeKey& key)
{
  return key.node + ":" + key.attribute;
}

inline void name(std::string_view value, live::SourceSpan source)
{
  if (value.empty() || std::any_of(value.begin(), value.end(), [](unsigned char c)
      { return c <= ' ' || c == 0x7f || c == '(' || c == ')' || c == '"'
               || c == ';' || c == '\\'; }))
    throw SynthesisFailure(SynthesisError::invalid_declaration,
                           "expected a nonempty unquoted name", source);
}

inline void span(live::SourceSpan source)
{
  if (source.end < source.begin)
    throw SynthesisFailure(SynthesisError::invalid_declaration,
                           "reversed source span");
}

inline bool list(const SExpr& expression)
{
  return !expression.is_atom && expression.text.empty() && !expression.quoted;
}

inline std::string symbol(const SExpr& expression, live::SourceSpan source)
{
  if (!expression.is_atom || expression.quoted || !expression.items.empty())
    throw SynthesisFailure(SynthesisError::invalid_declaration,
                           "expected an unquoted name", source);
  name(expression.text, source);
  return expression.text;
}

inline SExpr parse(std::string_view text, live::SourceSpan source)
{
  if (text.size() > source_limit)
    throw SynthesisFailure(SynthesisError::budget,
                           "declaration source limit exceeded", source);
  try
    {
      return parse_bounded_sexpr(text, SExprLimits{});
    }
  catch (const std::invalid_argument& error)
    {
      throw SynthesisFailure(SynthesisError::invalid_declaration,
                             error.what(), source);
    }
}

template <class Action>
decltype(auto) call(Action&& action, live::SourceSpan source,
                    const AttributeKey* attribute = nullptr)
{
  try
    {
      return std::forward<Action>(action)();
    }
  catch (const SynthesisFailure&)
    {
      throw;
    }
  catch (const std::bad_alloc&)
    {
      throw;
    }
  catch (const std::exception& error)
    {
      throw SynthesisFailure(SynthesisError::callback_failed, error.what(), source,
                             attribute ? std::vector<AttributeKey>{*attribute}
                                       : std::vector<AttributeKey>{});
    }
  catch (...)
    {
      throw SynthesisFailure(SynthesisError::callback_failed,
                             "host callback threw an unknown exception", source,
                             attribute ? std::vector<AttributeKey>{*attribute}
                                       : std::vector<AttributeKey>{});
    }
}
} // namespace attribute_detail

struct AttributeLimits
{
  std::size_t max_steps = 100000;
  std::size_t max_rounds = 64;
};

enum class AttributeRole { inherited, synthesized, contextual };

template <class State, class Value = SExpr>
class AttributeEngine
{
public:
  class Context
  {
  public:
    const State& state() const noexcept { return state_; }
    const AttributeKey& key() const noexcept { return key_; }
    AttributeRole role() const noexcept { return role_; }
    std::span<const AttributeKey> dependencies() const noexcept { return dependencies_; }

    const Value& at(std::size_t index) const
    {
      if (index < values_.size())
        return *values_[index];
      throw SynthesisFailure(SynthesisError::undeclared_dependency,
                             "attribute dependency index is not declared", source_, {key_});
    }

    const Value& get(const AttributeKey& key) const
    {
      const auto found = indices_.find(key);
      if (found != indices_.end())
        return at(found->second);
      throw SynthesisFailure(SynthesisError::undeclared_dependency,
                             "attribute dependency not declared: "
                               + attribute_detail::describe(key), source_, {key_, key});
    }

  private:
    using Indices = std::unordered_map<AttributeKey, std::size_t, AttributeKeyHash>;

    Context(const State& state, const AttributeKey& key, AttributeRole role, live::SourceSpan source,
            std::span<const AttributeKey> dependencies, const Indices& indices,
            std::span<const Value* const> values)
      : state_(state), key_(key), role_(role), source_(source), dependencies_(dependencies),
        indices_(indices), values_(values)
    {
    }

    const State& state_;
    const AttributeKey& key_;
    AttributeRole role_;
    live::SourceSpan source_;
    std::span<const AttributeKey> dependencies_;
    const Indices& indices_;
    std::span<const Value* const> values_;
    friend class AttributeEngine;
  };

  // Native callbacks (including join/equivalence) must be pure. Const access
  // constrains the API, not arbitrary side effects in user-provided C++ code.
  using Rule = std::function<Value(const Context&)>;
  using Results = std::unordered_map<AttributeKey, Value, AttributeKeyHash>;
  class Session;

  struct FixedPoint
  {
    Value bottom;
    std::function<Value(const Value&, const Value&)> join;
    std::function<bool(const Value&, const Value&)> equivalent;
  };

  void declare(AttributeKey key, std::vector<AttributeKey> dependencies, Rule rule,
               live::SourceSpan source = {}, std::optional<FixedPoint> fixed = {},
               AttributeRole role = AttributeRole::synthesized)
  {
    attribute_detail::span(source);
    attribute_detail::name(key.node, source);
    attribute_detail::name(key.attribute, source);
    if (role != AttributeRole::inherited && role != AttributeRole::synthesized
        && role != AttributeRole::contextual)
      throw SynthesisFailure(SynthesisError::invalid_declaration, "invalid attribute role", source);
    if (!rule || (fixed && (!fixed->join || !fixed->equivalent)))
      throw SynthesisFailure(SynthesisError::invalid_declaration,
                             "attribute callbacks must be provided", source, {key});
    typename Context::Indices indices;
    for (std::size_t i = 0; i < dependencies.size(); ++i)
      {
        const auto& dependency = dependencies[i];
        attribute_detail::name(dependency.node, source);
        attribute_detail::name(dependency.attribute, source);
        if (!indices.emplace(dependency, i).second)
          throw SynthesisFailure(SynthesisError::invalid_declaration,
                                 "duplicate attribute dependency", source, {key, dependency});
      }
    if (!definitions_.try_emplace(key, Definition{std::move(dependencies),
          std::move(indices), std::move(rule), source, std::move(fixed), role}).second)
      throw SynthesisFailure(SynthesisError::duplicate_declaration,
                             "attribute already declared: " + attribute_detail::describe(key),
                             source, {key});
  }

  // (attribute NODE NAME (depends (NODE NAME) ...)); host rule is explicit.
  // (circular NODE NAME (depends ...)) also requires a host fixed-point policy.
  void declare(const SExpr& expression, Rule rule, live::SourceSpan source = {},
               std::optional<FixedPoint> fixed = {})
  {
    using namespace attribute_detail;
    if (!list(expression) || (expression.items.size() != 4 && expression.items.size() != 5))
      throw SynthesisFailure(SynthesisError::invalid_declaration,
                             "expected (attribute NODE NAME (depends ...))", source);
    const auto kind = symbol(expression.items[0], source);
    if ((kind != "attribute" && kind != "circular") || ((kind == "circular") != bool(fixed)))
      throw SynthesisFailure(SynthesisError::invalid_declaration,
                             "declaration must match its attribute policy", source);
    AttributeKey key{symbol(expression.items[1], source), symbol(expression.items[2], source)};
    const auto& inputs = expression.items[3];
    if (!list(inputs) || inputs.items.empty() || symbol(inputs.items.front(), source) != "depends")
      throw SynthesisFailure(SynthesisError::invalid_declaration,
                             "expected a depends clause", source, {key});
    std::vector<AttributeKey> dependencies;
    for (std::size_t i = 1; i < inputs.items.size(); ++i)
      {
        const auto& input = inputs.items[i];
        if (!list(input) || input.items.size() != 2)
          throw SynthesisFailure(SynthesisError::invalid_declaration,
                                 "expected dependency (NODE NAME)", source, {key});
        dependencies.push_back({symbol(input.items[0], source), symbol(input.items[1], source)});
      }
    auto role = AttributeRole::synthesized;
    if (expression.items.size() == 5)
      {
        const auto form = symbol(expression.items[4], source);
        if (form == "inherited") role = AttributeRole::inherited;
        else if (form == "contextual") role = AttributeRole::contextual;
        else if (form != "synthesized")
          throw SynthesisFailure(SynthesisError::invalid_declaration, "invalid attribute role", source);
      }
    declare(std::move(key), std::move(dependencies), std::move(rule), source, std::move(fixed), role);
  }

  void declare_text(std::string_view text, Rule rule, live::SourceSpan source = {},
                    std::optional<FixedPoint> fixed = {})
  {
    declare(attribute_detail::parse(text, source), std::move(rule), source, std::move(fixed));
  }

  // A complete graph can be checked without a State or any rule invocation.
  // Open engines may continue to diagnose missing dependencies on demand.
  void validate_dependencies() const
  {
    for (const auto& [key, definition] : definitions_)
      for (const auto& dependency : definition.dependencies)
        if (!definitions_.contains(dependency))
          throw SynthesisFailure(SynthesisError::missing_attribute,
                                 "missing attribute: " + attribute_detail::describe(dependency),
                                 definition.source, {key, dependency});
  }

  // Each evaluation is its own cache. A new State or changed host definitions
  // never see earlier results. Only the requested keys are returned.
  Results evaluate(const State& state, std::span<const AttributeKey> requested,
                   AttributeLimits limits = {}) const
  {
    return compute(state, requested, limits, nullptr);
  }

  Value evaluate(const State& state, const AttributeKey& requested,
                 AttributeLimits limits = {}) const
  {
    auto results = evaluate(state, std::span<const AttributeKey>(&requested, 1), limits);
    return std::move(results.find(requested)->second);
  }

private:
  Results compute(const State& state, std::span<const AttributeKey> requested,
                  AttributeLimits limits, Results* cache) const
  {
    struct Node
    {
      const AttributeKey* key;
      const Definition* definition;
      std::vector<std::size_t> edges;
      std::optional<Value> value;
    };
    struct Frame { std::size_t node; std::size_t next; };
    std::vector<Node> nodes;
    std::vector<Frame> frames;
    DirectedGraph<const AttributeKey*, bool> graph;
    std::unordered_map<const AttributeKey*, std::size_t> discovered;
    std::vector<std::size_t> roots;
    auto remaining = limits.max_steps;
    const auto step = [&](live::SourceSpan source)
      {
        if (!remaining)
          throw SynthesisFailure(SynthesisError::budget, "attribute work limit exceeded", source);
        --remaining;
      };
    const auto discover = [&](const AttributeKey& key) -> std::pair<std::size_t, bool>
      {
        const auto found = definitions_.find(key);
        if (found == definitions_.end())
          {
            std::vector<AttributeKey> trace;
            for (const auto& frame : frames)
              trace.push_back(*nodes[frame.node].key);
            trace.push_back(key);
            throw SynthesisFailure(SynthesisError::missing_attribute,
                                   "missing attribute: " + attribute_detail::describe(key),
                                   frames.empty() ? live::SourceSpan{}
                                                  : nodes[frames.back().node].definition->source,
                                   std::move(trace));
          }
        const auto [member, inserted] = discovered.emplace(&found->first, nodes.size());
        if (inserted)
          {
            nodes.push_back({&found->first, &found->second, {}, {}});
            graph.add_node(&found->first);
            frames.push_back({member->second, 0});
          }
        return {member->second, inserted};
      };

    // Build the reachable dependency graph (prerequisite -> user). Keep each
    // equation's input order separately so its positional access is stable.
    for (const auto& key : requested)
      {
        step({});
        roots.push_back(discover(key).first);
        while (!frames.empty())
          {
            const auto current = frames.back().node;
            const auto& definition = *nodes[current].definition;
            if (frames.back().next < definition.dependencies.size())
              {
                step(definition.source);
                const auto dependency = discover(definition.dependencies[frames.back().next++]).first;
                nodes[current].edges.push_back(dependency);
                graph.add_edge(dependency, current);
                continue;
              }
            frames.pop_back();
          }
      }

    // Validate the entire plan before invoking any equation. Schedule the
    // condensed DAG expressly rather than relying on incidental SCC order.
    auto schedule = graph.strongly_connected_components();
    std::vector<GraphNodeId> component_for(nodes.size());
    DirectedGraph<bool, bool> condensation;
    for (auto& component : schedule)
      {
        const auto id = condensation.add_node(false);
        for (const auto member : component)
          component_for[member] = id;
        std::sort(component.begin(), component.end());
        if (is_cycle(component, nodes))
          for (const auto member : component)
            if (!nodes[member].definition->fixed)
              throw SynthesisFailure(SynthesisError::circular_dependency,
                                     "cycle requires explicit policies on all members",
                                     nodes[member].definition->source,
                                     component_keys(component, nodes));
      }
    for (std::size_t i = 0; i < nodes.size(); ++i)
      for (const auto dependency : nodes[i].edges)
        if (component_for[dependency] != component_for[i])
          condensation.add_edge(component_for[dependency], component_for[i]);

    const auto run = [&](const Node& node)
      {
        step(node.definition->source);
        std::vector<const Value*> values;
        for (const auto dependency : node.edges)
          {
            step(node.definition->source);
            values.push_back(&nodes[dependency].value.value());
          }
        const Context context(state, *node.key, node.definition->role, node.definition->source,
                              node.definition->dependencies, node.definition->indices, values);
        return attribute_detail::call([&] { return node.definition->rule(context); },
                                      node.definition->source, node.key);
      };
    for (const auto id : condensation.topological_sort())
      {
        const auto& component = schedule[id];
        auto& first = nodes[component.front()];
        if (cache && std::all_of(component.begin(), component.end(), [&](const auto member)
              { return cache->contains(*nodes[member].key); }))
          {
            for (const auto member : component)
              nodes[member].value.emplace(cache->at(*nodes[member].key));
            continue;
          }
        if (!is_cycle(component, nodes))
          {
            first.value.emplace(run(first));
            continue;
          }

        // Jacobi iteration: all callbacks see the preceding round's snapshot.
        // The host must provide a lattice bottom, monotone rules and a sound
        // join/equivalence; the engine does not certify these properties.
        for (const auto member : component)
          nodes[member].value.emplace(nodes[member].definition->fixed->bottom);
        bool converged = false;
        for (std::size_t round = 0; round < limits.max_rounds; ++round)
          {
            std::vector<Value> next;
            converged = true;
            for (const auto member : component)
              {
                const auto& node = nodes[member];
                const auto& policy = *node.definition->fixed;
                const auto candidate = run(node);
                step(node.definition->source);
                converged = attribute_detail::call([&]
                  {
                    next.push_back(policy.join(*node.value, candidate));
                    return policy.equivalent(*node.value, next.back());
                  }, node.definition->source, node.key) && converged;
              }
            for (std::size_t i = 0; i < component.size(); ++i)
              nodes[component[i]].value.emplace(std::move(next[i]));
            if (converged)
              break;
          }
        if (!converged)
          throw SynthesisFailure(SynthesisError::nonconvergent,
                                 "attribute fixed point did not converge within its round limit",
                                 first.definition->source, component_keys(component, nodes));
      }
    Results results;
    for (const auto root : roots)
      results.try_emplace(*nodes[root].key, *nodes[root].value);
    if (cache)
      for (auto& node : nodes)
        cache->try_emplace(*node.key, std::move(*node.value));
    return results;
  }

  struct Definition
  {
    std::vector<AttributeKey> dependencies;
    typename Context::Indices indices;
    Rule rule;
    live::SourceSpan source;
    std::optional<FixedPoint> fixed;
    AttributeRole role;
  };
  std::unordered_map<AttributeKey, Definition, AttributeKeyHash> definitions_;

  template <class Nodes>
  static bool is_cycle(const std::vector<std::size_t>& component, const Nodes& nodes)
  {
    if (component.size() > 1)
      return true;
    const auto member = component.front();
    const auto& edges = nodes[member].edges;
    return std::find(edges.begin(), edges.end(), member) != edges.end();
  }

  template <class Nodes>
  static std::vector<AttributeKey> component_keys(const std::vector<std::size_t>& component,
                                                   const Nodes& nodes)
  {
    std::vector<AttributeKey> keys;
    for (const auto member : component)
      keys.push_back(*nodes[member].key);
    return keys;
  }
};

// A session owns a snapshot of the definitions and of the caller's state.
// Shareable/pointer members in State must also be logically immutable while
// cached, or the caller must explicitly invalidate all affected inputs.
template <class State, class Value>
class AttributeEngine<State, Value>::Session
{
public:
  Session(const AttributeEngine& engine, State state)
    : engine_(engine), state_(std::move(state))
  {
  }

  Results evaluate(std::span<const AttributeKey> requested, AttributeLimits limits = {})
  {
    try
      {
        return engine_.compute(state_, requested, limits, &cache_);
      }
    catch (...)
      {
        // A failed Value copy/move at commit cannot leave a partial cache.
        cache_.clear();
        throw;
      }
  }

  Value evaluate(const AttributeKey& requested, AttributeLimits limits = {})
  {
    auto results = evaluate(std::span<const AttributeKey>(&requested, 1), limits);
    return std::move(results.find(requested)->second);
  }

  void replace_state(State state)
  {
    cache_.clear();
    state_ = std::move(state);
  }

  void invalidate() noexcept { cache_.clear(); }

  // Invalidate a key and its transitive consumers, including a complete SCC.
  // This graph only needs cached keys: cached consumers imply cached inputs.
  void invalidate(const AttributeKey& requested)
  {
    DirectedGraph<const AttributeKey*, bool> graph;
    std::unordered_map<AttributeKey, GraphNodeId, AttributeKeyHash> ids;
    if (!cache_.contains(requested))
      return;
    for (const auto& entry : cache_)
      ids.emplace(entry.first, graph.add_node(&entry.first));
    for (const auto& [key, id] : ids)
      for (const auto& dependency : engine_.definitions_.at(key).dependencies)
        if (const auto found = ids.find(dependency); found != ids.end())
          graph.add_edge(found->second, id);
    for (const auto id : graph.breadth_first(ids.at(requested)))
      cache_.erase(*graph.value(id));
  }

private:
  AttributeEngine engine_;
  State state_;
  Results cache_;
};

struct SemanticFix
{
  live::SourceSpan span;
  std::string replacement;
};

struct SemanticDiagnostic
{
  live::Diagnostic diagnostic;
  std::vector<SemanticFix> fixes;
};

namespace attribute_detail
{
inline void validate(const SemanticDiagnostic& diagnostic)
{
  const auto& report = diagnostic.diagnostic;
  name(report.code, report.span);
  span(report.span);
  if (report.severity != live::Severity::note && report.severity != live::Severity::warning
      && report.severity != live::Severity::error)
    throw SynthesisFailure(SynthesisError::invalid_declaration, "invalid diagnostic severity", report.span);
  for (const auto& note : report.notes)
    span(note.span);
  for (const auto& fix : diagnostic.fixes)
    span(fix.span);
}

// Check malformed native IR and bound the recursion of existing pattern tools.
inline std::size_t check_expression(const SExpr& expression, live::SourceSpan source,
                                    std::size_t& work)
{
  const SExprLimits limits;
  std::size_t count = 0;
  auto bytes = limits.max_bytes;
  const auto visit = [&](const auto& self, const SExpr& node, std::size_t depth) -> void
    {
      if (!work || depth > limits.max_depth || count == limits.max_nodes || node.text.size() > bytes)
        throw SynthesisFailure(SynthesisError::budget, "diagnostic pattern limits exceeded", source);
      --work;
      ++count;
      bytes -= node.text.size();
      if (node.is_atom ? (!node.items.empty() || (!node.quoted && node.text.empty())) : !list(node))
        throw SynthesisFailure(SynthesisError::invalid_declaration, "malformed S-expression", source);
      for (const auto& child : node.items)
        self(self, child, depth + 1);
    };
  visit(visit, expression, 0);
  return count;
}
} // namespace attribute_detail

enum class DiagnosticPhase { syntax, semantic };

template <class State>
class PatternDiagnostics
{
public:
  using Guard = std::function<bool(const State&, const SExprBindings&)>;

  struct Rule
  {
    DiagnosticPhase phase;
    SExpr pattern;
    live::Severity severity;
    std::string code;
    std::string message;
    std::vector<live::DiagnosticNote> related;
    std::optional<std::string> replacement; // Replaces the matched subject span.
    Guard guard;
  };

  void add(const Rule& rule)
  {
    if ((rule.phase != DiagnosticPhase::syntax && rule.phase != DiagnosticPhase::semantic)
        || (rule.phase == DiagnosticPhase::syntax && rule.guard))
      throw SynthesisFailure(SynthesisError::invalid_declaration,
                             "syntax diagnostics cannot call a semantic guard");
    attribute_detail::validate({{rule.severity, rule.code, rule.message, {}, rule.related}, {}});
    auto work = SExprLimits{}.max_nodes;
    const auto count = attribute_detail::check_expression(rule.pattern, {}, work);
    rules_.push_back({rule, count});
  }

  // (diagnostic syntax|semantic note|warning|error CODE PATTERN "MESSAGE"
  //    ["REPLACEMENT"]). Structural patterns use LibDSL's ?variable syntax.
  void add(const SExpr& declaration, Guard guard = {}, live::SourceSpan source = {})
  {
    using namespace attribute_detail;
    span(source);
    if (!list(declaration) || (declaration.items.size() != 6 && declaration.items.size() != 7)
        || symbol(declaration.items[0], source) != "diagnostic")
      throw SynthesisFailure(SynthesisError::invalid_declaration, "invalid diagnostic declaration", source);
    const auto phase = symbol(declaration.items[1], source);
    const auto severity = symbol(declaration.items[2], source);
    if ((phase != "syntax" && phase != "semantic")
        || (severity != "note" && severity != "warning" && severity != "error"))
      throw SynthesisFailure(SynthesisError::invalid_declaration, "invalid diagnostic phase/severity", source);
    const auto string = [&](std::size_t i)
      {
        const auto& item = declaration.items[i];
        if (!item.is_atom || !item.quoted || !item.items.empty())
          throw SynthesisFailure(SynthesisError::invalid_declaration, "expected quoted diagnostic text", source);
        return item.text;
      };
    auto work = SExprLimits{}.max_nodes;
    check_expression(declaration.items[4], source, work); // Bound before deep copy.
    add({phase == "syntax" ? DiagnosticPhase::syntax : DiagnosticPhase::semantic,
         declaration.items[4],
         severity == "note" ? live::Severity::note
                            : severity == "warning" ? live::Severity::warning : live::Severity::error,
         symbol(declaration.items[3], source), string(5), {},
         declaration.items.size() == 7 ? std::optional<std::string>(string(6)) : std::nullopt,
         std::move(guard)});
  }

  void add_text(std::string_view declaration, Guard guard = {}, live::SourceSpan source = {})
  {
    add(attribute_detail::parse(declaration, source), std::move(guard), source);
  }

  std::vector<SemanticDiagnostic> syntax(const SExpr& subject, live::SourceSpan source,
                                         std::size_t max_work = 100000) const
  {
    return inspect(DiagnosticPhase::syntax, nullptr, subject, source, max_work);
  }

  std::vector<SemanticDiagnostic> semantic(const State& state, const SExpr& subject,
                                            live::SourceSpan source,
                                            std::size_t max_work = 100000) const
  {
    return inspect(DiagnosticPhase::semantic, &state, subject, source, max_work);
  }

private:
  struct Compiled { Rule rule; std::size_t nodes; };
  std::vector<Compiled> rules_;

  std::vector<SemanticDiagnostic> inspect(DiagnosticPhase phase, const State* state,
                                           const SExpr& subject, live::SourceSpan source,
                                           std::size_t work) const
  {
    attribute_detail::span(source);
    const auto nodes = attribute_detail::check_expression(subject, source, work);
    const auto spend = [&](std::size_t amount)
      {
        if (amount > work)
          throw SynthesisFailure(SynthesisError::budget, "diagnostic work limit exceeded", source);
        work -= amount;
      };
    std::vector<SemanticDiagnostic> reports;
    for (const auto& [rule, count] : rules_)
      {
        spend(1);
        if (rule.phase != phase)
          continue;
        spend(count);
        spend(nodes);
        SExprBindings bindings;
        if (!match_sexpr(rule.pattern, subject, bindings))
          continue;
        if (rule.guard && !attribute_detail::call([&] { return rule.guard(*state, bindings); }, source))
          continue;
        SemanticDiagnostic report{{rule.severity, rule.code, rule.message, source, rule.related}, {}};
        if (rule.replacement)
          report.fixes.push_back({source, *rule.replacement});
        reports.push_back(std::move(report));
      }
    return reports;
  }
};

template <class Value>
class Scope
{
public:
  struct Binding
  {
    std::string name;
    Value value;
    live::SourceSpan source{};
  };

  // Each extension is one immutable frame, so a captured Scope keeps the
  // precise definition-time environment and is safe to copy into a closure.
  Scope extend(std::vector<Binding> bindings, live::SourceSpan source = {}) const
  {
    const auto depth = frame_ ? frame_->depth : 0;
    if (depth >= 256)
      throw SynthesisFailure(SynthesisError::budget, "scope depth limit exceeded", source);
    attribute_detail::span(source);
    auto frame = std::make_shared<Frame>();
    frame->parent = frame_;
    frame->depth = depth + 1;
    for (auto& binding : bindings)
      {
        attribute_detail::name(binding.name, binding.source);
        attribute_detail::span(binding.source);
        if (frame->bindings.contains(binding.name))
          throw SynthesisFailure(SynthesisError::duplicate_binding,
                                 "binding already exists in this scope: " + binding.name,
                                 binding.source);
        auto key = binding.name;
        frame->bindings.emplace(std::move(key), std::move(binding));
      }
    Scope extended;
    extended.frame_ = std::move(frame);
    return extended;
  }

  const Binding* find(std::string_view name) const &
  {
    for (auto* frame = frame_.get(); frame; frame = frame->parent.get())
      if (const auto item = frame->bindings.find(name); item != frame->bindings.end())
        return &item->second;
    return nullptr;
  }
  const Binding* find(std::string_view) const && = delete;

  const Value& require(std::string_view name, live::SourceSpan source = {}) const &
  {
    if (const auto* binding = find(name))
      return binding->value;
    throw SynthesisFailure(SynthesisError::missing_binding,
                           "binding not found: " + std::string(name), source);
  }
  const Value& require(std::string_view, live::SourceSpan = {}) const && = delete;

private:
  struct Hash
  {
    using is_transparent = void;
    std::size_t operator()(std::string_view name) const { return std::hash<std::string_view>{}(name); }
  };
  struct Frame
  {
    std::shared_ptr<const Frame> parent;
    std::size_t depth;
    std::unordered_map<std::string, Binding, Hash, std::equal_to<>> bindings;
  };
  std::shared_ptr<const Frame> frame_;
};

enum class ScopePolicy { lexical, dynamic, custom };
enum class MetaPhase { analyze, validate, transform };

template <class State, class Value = SExpr>
class MetaProcedures
{
public:
  struct Context
  {
    const State& state;
    const Scope<Value>& environment;
    std::span<const Value> arguments;
    live::SourceSpan source;

    const Value& resolve(std::string_view name) const { return environment.require(name, source); }
  };

  // These signatures deliberately separate state transformations from
  // read-only callbacks. Hosts must honor the purity contract of readers and
  // selectors; untrusted source only selects capabilities already registered.
  using Analyze = std::function<Value(const Context&)>;
  using Validate = std::function<std::vector<SemanticDiagnostic>(const Context&)>;
  using Transform = std::function<State(const Context&)>;
  // Wrapper alternatives keep phase distinctions even when State == Value.
  struct Analyzer { Analyze callback; };
  struct Validator { Validate callback; };
  struct Transformer { Transform callback; };
  using Body = std::variant<Analyzer, Validator, Transformer>;
  using SelectScope = std::function<Scope<Value>(
    const State&, const Scope<Value>& definition, const Scope<Value>& caller)>;

  struct Definition
  {
    std::string name;
    std::vector<std::string> parameters;
    Body body;
    ScopePolicy scoping = ScopePolicy::lexical;
    Scope<Value> closure{};
    SelectScope select{};
    live::SourceSpan source{};
  };

  struct Invocation
  {
    std::string name;
    std::vector<Value> arguments;
    live::SourceSpan source{};
  };

  using Inspection = std::variant<Value, std::vector<SemanticDiagnostic>>;

  void define(Definition definition)
  {
    const auto source = definition.source;
    attribute_detail::span(source);
    attribute_detail::name(definition.name, source);
    if ((definition.scoping != ScopePolicy::lexical && definition.scoping != ScopePolicy::dynamic
         && definition.scoping != ScopePolicy::custom)
        || ((definition.scoping == ScopePolicy::custom) != bool(definition.select))
        || !std::visit([](const auto& body) { return bool(body.callback); }, definition.body))
      throw SynthesisFailure(SynthesisError::invalid_declaration, "invalid metaprocedure callbacks", source);
    std::unordered_set<std::string> names;
    for (const auto& parameter : definition.parameters)
      {
        attribute_detail::name(parameter, source);
        if (!names.insert(parameter).second)
          throw SynthesisFailure(SynthesisError::duplicate_binding,
                                 "duplicate metaprocedure parameter: " + parameter, source);
      }
    auto key = definition.name;
    if (!procedures_.try_emplace(key, std::move(definition)).second)
      throw SynthesisFailure(SynthesisError::duplicate_declaration,
                             "metaprocedure already defined: " + key, source);
  }

  // (metaproc NAME analyze|validate|transform lexical|dynamic|custom (PARAM ...))
  void define(const SExpr& expression, Body body, Scope<Value> closure = {},
              SelectScope select = {}, live::SourceSpan source = {})
  {
    using namespace attribute_detail;
    span(source);
    if (!list(expression) || expression.items.size() != 5
        || symbol(expression.items[0], source) != "metaproc")
      throw SynthesisFailure(SynthesisError::invalid_declaration, "invalid metaprocedure declaration", source);
    const auto form = symbol(expression.items[2], source);
    const auto policy = symbol(expression.items[3], source);
    const auto& params = expression.items[4];
    if (((form != "analyze" || !std::holds_alternative<Analyzer>(body))
         && (form != "validate" || !std::holds_alternative<Validator>(body))
         && (form != "transform" || !std::holds_alternative<Transformer>(body)))
        || (policy != "lexical" && policy != "dynamic" && policy != "custom") || !list(params))
      throw SynthesisFailure(SynthesisError::invalid_declaration, "metaprocedure phase/scope mismatch", source);
    std::vector<std::string> parameters;
    for (const auto& param : params.items)
      parameters.push_back(symbol(param, source));
    define({symbol(expression.items[1], source), std::move(parameters), std::move(body),
            policy == "lexical" ? ScopePolicy::lexical
                                : policy == "dynamic" ? ScopePolicy::dynamic : ScopePolicy::custom,
            std::move(closure), std::move(select), source});
  }

  void define_text(std::string_view declaration, Body body, Scope<Value> closure = {},
                   SelectScope select = {}, live::SourceSpan source = {})
  {
    define(attribute_detail::parse(declaration, source), std::move(body),
           std::move(closure), std::move(select), source);
  }

  MetaPhase phase(std::string_view name) const
  {
    return phase(find(name, {}).body);
  }

  Value analyze(const State& state, const Scope<Value>& caller, const Invocation& invocation) const
  {
    return run<Analyzer>(state, caller, checked(invocation, MetaPhase::analyze), invocation);
  }

  std::vector<SemanticDiagnostic> validate(const State& state, const Scope<Value>& caller,
                                            const Invocation& invocation) const
  {
    auto reports = run<Validator>(state, caller, checked(invocation, MetaPhase::validate), invocation);
    for (const auto& report : reports)
      attribute_detail::validate(report);
    return reports;
  }

  // Plan against the registry before entering any callback. In particular a
  // transform or bad arity anywhere causes this whole read-only plan to fail.
  std::vector<Inspection> inspect(const State& state, const Scope<Value>& caller,
                                  std::span<const Invocation> requests,
                                  std::size_t max_calls = 1024) const
  {
    if (requests.size() > max_calls)
      throw SynthesisFailure(SynthesisError::budget, "metaprocedure call limit exceeded");
    std::vector<MetaPhase> phases;
    for (const auto& request : requests)
      {
        const auto kind = phase(find(request.name, request.source).body);
        if (kind == MetaPhase::transform)
          throw SynthesisFailure(SynthesisError::wrong_phase,
                                 "live analysis cannot schedule a state transform", request.source);
        checked(request, kind);
        phases.push_back(kind);
      }
    std::vector<Inspection> results;
    for (std::size_t i = 0; i < requests.size(); ++i)
      if (phases[i] == MetaPhase::analyze)
        results.emplace_back(std::in_place_index<0>, analyze(state, caller, requests[i]));
      else
        results.emplace_back(std::in_place_index<1>, validate(state, caller, requests[i]));
    return results;
  }

  State transform(const State& state, const Scope<Value>& caller, const Invocation& invocation,
                  std::span<const Invocation> validators = {}) const
  {
    const auto& definition = checked(invocation, MetaPhase::transform);
    if (validators.size() > 1024)
      throw SynthesisFailure(SynthesisError::budget, "state validator limit exceeded", invocation.source);
    for (const auto& validator : validators)
      checked(validator, MetaPhase::validate);
    auto proposed = run<Transformer>(state, caller, definition, invocation);
    for (const auto& validator : validators)
      for (const auto& report : validate(proposed, caller, validator))
        if (report.diagnostic.severity == live::Severity::error)
          throw SynthesisFailure(SynthesisError::inadmissible_state,
                                 report.diagnostic.message, report.diagnostic.span);
    return proposed;
  }

private:
  std::unordered_map<std::string, Definition> procedures_;

  static MetaPhase phase(const Body& body) noexcept
  {
    if (std::holds_alternative<Analyzer>(body)) return MetaPhase::analyze;
    if (std::holds_alternative<Validator>(body)) return MetaPhase::validate;
    return MetaPhase::transform;
  }

  const Definition& find(std::string_view name, live::SourceSpan source) const
  {
    const auto found = procedures_.find(std::string(name));
    if (found == procedures_.end())
      throw SynthesisFailure(SynthesisError::missing_procedure,
                             "metaprocedure not found: " + std::string(name), source);
    return found->second;
  }

  const Definition& checked(const Invocation& invocation, MetaPhase expected) const
  {
    attribute_detail::span(invocation.source);
    const auto& definition = find(invocation.name, invocation.source);
    if (phase(definition.body) != expected)
      throw SynthesisFailure(SynthesisError::wrong_phase,
                             "metaprocedure belongs to a different phase", invocation.source);
    if (definition.parameters.size() != invocation.arguments.size())
      throw SynthesisFailure(SynthesisError::argument_count,
                             "wrong metaprocedure argument count: " + invocation.name, invocation.source);
    return definition;
  }

  template <class Callback>
  auto run(const State& state, const Scope<Value>& caller, const Definition& definition,
           const Invocation& invocation) const
  {
    return attribute_detail::call([&]
      {
        auto base = definition.closure;
        if (definition.scoping == ScopePolicy::dynamic)
          base = caller;
        if (definition.scoping == ScopePolicy::custom)
          base = definition.select(state, definition.closure, caller);
        std::vector<typename Scope<Value>::Binding> parameters;
        for (std::size_t i = 0; i < definition.parameters.size(); ++i)
          parameters.push_back({definition.parameters[i], invocation.arguments[i], invocation.source});
        const auto environment = base.extend(std::move(parameters), invocation.source);
        const Context context{state, environment, invocation.arguments, invocation.source};
        return std::get<Callback>(definition.body).callback(context);
      }, invocation.source);
  }
};
} // namespace synth

namespace synth
{
template <class State, class Value, class AttributeValue> class LinkedSpecification;

// Compiles source forms into host-defined operations. Registration and operation
// factories are trusted C++; source can only use the language they expose.
template <class State, class Value = std::int64_t>
class Executable
{
public:
  struct Limits
  {
    std::size_t source_bytes = 64 * 1024;
    std::size_t syntax_nodes = 8192;
    std::size_t syntax_depth = 128;
    std::size_t steps = 100000;
    std::size_t execution_depth = 128;
  };

  class Frame;
  class Builder;

  class Expression
  {
    friend class Builder;
    friend class Frame;
    std::shared_ptr<const std::function<Value (Frame &)>> operation;

    explicit Expression (std::function<Value (Frame &)> fn)
      : operation (std::make_shared<const std::function<Value (Frame &)>> (
            std::move (fn)))
    {
      if (!*operation)
        throw std::invalid_argument ("empty expression operation");
    }

  public:
    Value
    run (State &state, Limits limits = {}) const
    {
      Frame frame (state, limits);
      return frame.evaluate (*this);
    }
  };

  class Statement
  {
    friend class Builder;
    friend class Frame;
    std::shared_ptr<const std::function<void (Frame &)>> operation;

    explicit Statement (std::function<void (Frame &)> fn)
      : operation (std::make_shared<const std::function<void (Frame &)>> (
            std::move (fn)))
    {
      if (!*operation)
        throw std::invalid_argument ("empty statement operation");
    }

  public:
    void
    run (State &state, Limits limits = {}) const
    {
      Frame frame (state, limits);
      frame.execute (*this);
    }
  };

  class Frame
  {
    friend class Expression;
    friend class Statement;

    State &state_;
    std::size_t remaining_;
    std::size_t max_depth_;
    std::size_t depth_ = 0;

    explicit Frame (State &state, Limits limits)
      : state_ (state), remaining_ (limits.steps),
        max_depth_ (std::min (limits.execution_depth, std::size_t{ 256 }))
    {
    }

    struct Enter
    {
      Frame &frame;
      explicit Enter (Frame &frame) : frame (frame)
      {
        if (frame.depth_ >= frame.max_depth_)
          throw std::runtime_error ("execution depth limit exceeded");
        frame.tick ();
        ++frame.depth_;
      }
      ~Enter () { --frame.depth_; }
    };

  public:
    Frame (const Frame &) = delete;
    Frame &operator= (const Frame &) = delete;

    State &state () noexcept { return state_; }
    std::size_t remaining () const noexcept { return remaining_; }
    void
    tick ()
    {
      if (remaining_ == 0)
        throw std::runtime_error ("execution step limit exceeded");
      --remaining_;
    }

    Value
    evaluate (const Expression &expression)
    {
      auto operation = expression.operation;
      if (!operation)
        throw std::invalid_argument ("moved-from expression");
      Enter enter (*this);
      return (*operation) (*this);
    }

    void
    execute (const Statement &statement)
    {
      auto operation = statement.operation;
      if (!operation)
        throw std::invalid_argument ("moved-from statement");
      Enter enter (*this);
      (*operation) (*this);
    }
  };

  using ExpressionForm = std::function<Expression (Builder &, const SExpr &)>;
  using StatementForm = std::function<Statement (Builder &, const SExpr &)>;

private:
  template <class, class, class> friend class LinkedSpecification;
  std::unordered_map<std::string, ExpressionForm> expressions_;
  std::unordered_map<std::string, StatementForm> statements_;
  std::unordered_map<std::string, Form> form_constraints_;
  ExpressionForm atom_;
  ExpressionForm application_;

  static bool
  symbol (std::string_view text)
  {
    if (text.empty ())
      return false;
    for (unsigned char c : text)
      if (c <= ' ' || c == '(' || c == ')' || c == ';' || c == '"' || c == '\\')
        return false;
    return true;
  }

  static const SExpr *
  head (const SExpr &syntax)
  {
    if (syntax.is_atom || syntax.items.empty ())
      return nullptr;
    const auto &candidate = syntax.items.front ();
    return candidate.is_atom && !candidate.quoted ? &candidate : nullptr;
  }

  void check_form (const SExpr &syntax, const SExpr &name) const
  {
    if (const auto it = form_constraints_.find (name.text); it != form_constraints_.end ())
      {
        const auto arity = syntax.items.size () - 1;
        const auto &form = it->second;
        if (arity < form.min_arity || (form.max_arity && arity > *form.max_arity))
          throw std::invalid_argument ("wrong arity for language form " + name.text);
      }
  }

public:
  class Builder
  {
    friend class Executable;
    const Executable &language_;
    std::size_t nodes_;
    std::size_t bytes_;
    std::size_t dispatches_;
    std::size_t max_depth_;
    std::size_t dispatch_depth_ = 0;

    Builder (const Executable &language, Limits limits)
      : language_ (language), nodes_ (limits.syntax_nodes),
        bytes_ (limits.source_bytes), dispatches_ (limits.syntax_nodes),
        max_depth_ (std::min (limits.syntax_depth, std::size_t{ 256 }))
    {
    }

    void
    validate (const SExpr &syntax, std::size_t depth = 0)
    {
      if (depth >= max_depth_)
        throw std::invalid_argument ("syntax depth limit exceeded");
      if (nodes_ == 0)
        throw std::invalid_argument ("syntax node limit exceeded");
      --nodes_;
      if (syntax.is_atom)
        {
          if (syntax.text.size () > bytes_)
            throw std::invalid_argument ("syntax byte limit exceeded");
          bytes_ -= syntax.text.size ();
          if (!syntax.items.empty () || (!syntax.quoted && !symbol (syntax.text)))
            throw std::invalid_argument ("malformed executable atom");
        }
      else
        {
          if (syntax.quoted || !syntax.text.empty ())
            throw std::invalid_argument ("malformed executable list");
          for (const auto &child : syntax.items)
            validate (child, depth + 1);
        }
    }

    struct Dispatch
    {
      Builder &builder;
      explicit Dispatch (Builder &builder) : builder (builder)
      {
        if (builder.dispatch_depth_ >= builder.max_depth_ || builder.dispatches_ == 0)
          throw std::invalid_argument ("form compilation limit exceeded");
        --builder.dispatches_;
        ++builder.dispatch_depth_;
      }
      ~Dispatch () { --builder.dispatch_depth_; }
    };

  public:
    Builder (const Builder &) = delete;
    Builder &operator= (const Builder &) = delete;

    // Subform factories compile borrowed children here and capture owned
    // expressions/statements or copied literal data, never a borrowed SExpr.
    Expression
    expression (const SExpr &syntax)
    {
      Dispatch dispatch (*this);
      if (syntax.is_atom)
        {
          if (language_.atom_)
            return language_.atom_ (*this, syntax);
        }
      else
        {
          if (const auto *name = head (syntax))
            {
              language_.check_form (syntax, *name);
              if (auto it = language_.expressions_.find (name->text);
                  it != language_.expressions_.end ())
                return it->second (*this, syntax);
            }
          if (language_.application_)
            return language_.application_ (*this, syntax);
        }
      throw std::invalid_argument ("unknown expression form");
    }

    Statement
    statement (const SExpr &syntax)
    {
      Dispatch dispatch (*this);
      if (const auto *name = head (syntax))
        {
          language_.check_form (syntax, *name);
          if (auto it = language_.statements_.find (name->text);
              it != language_.statements_.end ())
            return it->second (*this, syntax);
        }
      throw std::invalid_argument ("unknown statement form");
    }

    static Expression expression (std::function<Value (Frame &)> fn)
    {
      return Expression (std::move (fn));
    }

    static Statement statement (std::function<void (Frame &)> fn)
    {
      return Statement (std::move (fn));
    }

    static void
    require_arity (const SExpr &form, std::size_t arguments)
    {
      if (form.is_atom || form.items.empty () || form.items.size () - 1 != arguments)
        throw std::invalid_argument ("wrong executable form arity");
    }
  };

  Executable &
  on_atom (ExpressionForm form)
  {
    if (!form)
      throw std::invalid_argument ("empty atom factory");
    atom_ = std::move (form);
    return *this;
  }

  Executable &
  on_application (ExpressionForm form)
  {
    if (!form)
      throw std::invalid_argument ("empty application factory");
    application_ = std::move (form);
    return *this;
  }

  Executable &
  on_expression (std::string name, ExpressionForm form)
  {
    if (!symbol (name) || !form
        || !expressions_.emplace (std::move (name), std::move (form)).second)
      throw std::invalid_argument ("invalid or duplicate expression factory");
    return *this;
  }

  Executable &
  on_statement (std::string name, StatementForm form)
  {
    if (!symbol (name) || !form
        || !statements_.emplace (std::move (name), std::move (form)).second)
      throw std::invalid_argument ("invalid or duplicate statement factory");
    return *this;
  }

  Expression
  compile_expression (const SExpr &syntax, Limits limits = {}) const
  {
    Builder builder (*this, limits);
    builder.validate (syntax);
    return builder.expression (syntax);
  }

  Statement
  compile_statement (const SExpr &syntax, Limits limits = {}) const
  {
    Builder builder (*this, limits);
    builder.validate (syntax);
    return builder.statement (syntax);
  }

  Expression
  compile_expression (std::string_view source, Limits limits = {}) const
  {
    return compile_expression (read (source, limits), limits);
  }

  Statement
  compile_statement (std::string_view source, Limits limits = {}) const
  {
    return compile_statement (read (source, limits), limits);
  }

  static SExpr
  read (std::string_view source, Limits limits = {})
  {
    if (limits.syntax_depth == 0)
      throw std::invalid_argument ("syntax depth limit exceeded");
    return parse_bounded_sexpr (
        source, { limits.source_bytes, limits.syntax_nodes,
                  std::min (limits.syntax_depth, std::size_t{ 256 }) - 1 });
  }

  // The frontend may be MetaLanguage (or another validated expansion provider).
  // Both the original syntax and the expanded result must meet our limits.
  template <class Frontend>
  Expression
  compile_expanded_expression (const Frontend &frontend, const SExpr &syntax,
                               Limits limits = {}) const
  {
    Builder builder (*this, limits);
    builder.validate (syntax);
    return compile_expression (frontend.expand (syntax), limits);
  }

  template <class Frontend>
  Expression
  compile_expanded_expression (const Frontend &frontend, std::string_view source,
                               Limits limits = {}) const
  {
    return compile_expanded_expression (frontend, read (source, limits), limits);
  }

  template <class Frontend>
  Statement
  compile_expanded_statement (const Frontend &frontend, const SExpr &syntax,
                              Limits limits = {}) const
  {
    Builder builder (*this, limits);
    builder.validate (syntax);
    return compile_statement (frontend.expand (syntax), limits);
  }

  template <class Frontend>
  Statement
  compile_expanded_statement (const Frontend &frontend, std::string_view source,
                              Limits limits = {}) const
  {
    return compile_expanded_statement (frontend, read (source, limits), limits);
  }
};

// Link named expression/statement bodies and a closed attribute graph from the
// same immutable Specification (whether built in C++ or read from source).
// Attribute declarations use (attribute ID (CALLBACK ATTRIBUTE-DEFINITION)),
// where ATTRIBUTE-DEFINITION is the AttributeEngine S-expression syntax:
//   (attribute result (compute (attribute root value (depends (leaf value)))))
// ID names the specification declaration; (NODE NAME) is its engine key.
// CALLBACK is looked up only in a host-supplied registry, never evaluated.
template <class State, class Value = std::int64_t, class AttributeValue = Value>
class LinkedSpecification
{
public:
  using Language = Executable<State, Value>;
  using Attributes = AttributeEngine<State, AttributeValue>;
  using Limits = typename Language::Limits;

  class TrustedCallbacks
  {
    friend class LinkedSpecification;
    struct AttributeBinding
    {
      typename Attributes::Rule rule;
      std::optional<typename Attributes::FixedPoint> fixed;
    };
    std::unordered_map<std::string, AttributeBinding> attributes_;

  public:
    Language executable;

    TrustedCallbacks &attribute (std::string name, typename Attributes::Rule rule,
                                 std::optional<typename Attributes::FixedPoint> fixed = {})
    {
      specification_detail::require_name (name);
      if (!rule || (fixed && (!fixed->join || !fixed->equivalent)))
        throw SynthesisFailure (SynthesisError::invalid_declaration,
                                "incomplete attribute binding: " + name);
      if (!attributes_.try_emplace (name, AttributeBinding{ std::move (rule),
                                                           std::move (fixed) }).second)
        throw SynthesisFailure (SynthesisError::duplicate_binding,
                                "duplicate attribute binding: " + name);
      return *this;
    }
  };

  LinkedSpecification (const Specification &spec, const TrustedCallbacks &callbacks,
                       Limits limits = {}, ExpansionLimits expansion = {})
  {
    for (const auto &decl : spec.declarations ())
      if (decl.kind == DeclarationKind::attribute)
        {
          const auto &body = decl.body;
          if (body.is_atom || body.items.size () != 2)
            throw SynthesisFailure (SynthesisError::invalid_declaration,
                                    "attribute " + decl.name + " needs (CALLBACK DEFINITION)");
          const auto callback = attribute_detail::symbol (body.items[0], {});
          const auto it = callbacks.attributes_.find (callback);
          if (it == callbacks.attributes_.end ())
            throw SynthesisFailure (SynthesisError::missing_binding,
                                    "missing attribute binding: " + callback);
          attributes_.declare (body.items[1], it->second.rule, {}, it->second.fixed);
        }
    attributes_.validate_dependencies ();

    auto executable = callbacks.executable;
    for (const auto &form : spec.forms ())
      executable.form_constraints_.emplace (form.name, form);
    const MetaLanguage frontend (spec);
    // Executable validates both the source tree and the expanded tree.
    struct Expander
    {
      const MetaLanguage &frontend;
      ExpansionLimits limits;
      SExpr expand (const SExpr &syntax) const { return frontend.expand (syntax, limits); }
    } expander{ frontend, expansion };
    for (const auto &decl : spec.declarations ())
      {
        if (decl.kind == DeclarationKind::expression)
          expressions_.emplace (decl.name,
              executable.compile_expanded_expression (expander, decl.body, limits));
        else if (decl.kind == DeclarationKind::statement)
          statements_.emplace (decl.name,
              executable.compile_expanded_statement (expander, decl.body, limits));
      }
  }

  typename Language::Expression expression (std::string_view name) const
  {
    return expressions_.at (std::string (name));
  }

  typename Language::Statement statement (std::string_view name) const
  {
    return statements_.at (std::string (name));
  }

  const Attributes &attributes () const & { return attributes_; }
  const Attributes &attributes () const && = delete;

private:
  std::unordered_map<std::string, typename Language::Expression> expressions_;
  std::unordered_map<std::string, typename Language::Statement> statements_;
  Attributes attributes_;
};

// Read-only expressions use a separate state type. This prevents mutation
// through the frame, not intentional side effects in trusted C++ callbacks.
template <class State, class Value = bool>
using PureExecutable = Executable<std::reference_wrapper<const State>, Value>;

enum class ContractPhase { precondition, invariant_entry, invariant_exit,
                           postcondition, frame_condition, exceptional_exit };

// Legacy name for a runtime clause descriptor, not a proposition or evidence.
struct VerificationObligation
{
  ContractPhase phase;
  std::string description;
};

// External verification records are independent of ExecutionContract. The
// scope must identify the exact program/model, domain and assumptions; the
// proposition must specify the claimed relation for that scope.
struct FormalContractObligation
{
  ContractPhase phase;
  std::string scope;
  std::string proposition;
};

enum class FormalContractVerdict { proved, disproved, unknown, trusted_assumption };

// An externally reported verdict and artifact reference, not validated here.
// Runtime checks never create or update these records.
struct FormalContractEvidence
{
  FormalContractObligation obligation;
  FormalContractVerdict verdict = FormalContractVerdict::unknown;
  std::string artifact;
};

class ExceptionalContractViolation : public std::runtime_error
{
  std::exception_ptr outcome_;

public:
  ExceptionalContractViolation (const std::string &description,
                                std::exception_ptr outcome)
      : std::runtime_error ("execution contract failed: " + description),
        outcome_ (std::move (outcome))
  {}

  const std::exception_ptr &
  outcome () const noexcept
  {
    return outcome_;
  }
};

template <class Snapshot>
class ExecutionContract
{
public:
  using Predicate = std::function<bool (const Snapshot &, const Snapshot &)>;
  using ExceptionalPredicate = std::function<bool (
      const Snapshot &, const Snapshot &, const std::exception_ptr &)>;

private:
  struct Condition
  {
    VerificationObligation obligation;
    ExceptionalPredicate predicate;
  };
  std::vector<Condition> conditions_;

  void
  check (ContractPhase phase, const Snapshot &before, const Snapshot &after,
         const std::exception_ptr &outcome = {}) const
  {
    for (const auto &condition : conditions_)
      if (condition.obligation.phase == phase
          && !condition.predicate (before, after, outcome))
        {
          if (outcome)
            throw ExceptionalContractViolation (condition.obligation.description,
                                                outcome);
          throw std::runtime_error ("execution contract failed: "
                                    + condition.obligation.description);
        }
  }

  template <class Program, class State, class Capture, class Limits>
  void
  run_body (const Program &statement, State &state, Capture &capture,
            Limits &limits, const Snapshot &before) const
  {
    try
      {
        statement.run (state, limits);
      }
    catch (...)
      {
        if (std::any_of (conditions_.begin (), conditions_.end (),
                         [] (const Condition &condition) {
                           return condition.obligation.phase
                                  == ContractPhase::exceptional_exit;
                         }))
          {
            const auto outcome = std::current_exception ();
            const Snapshot after = capture (std::as_const (state));
            check (ContractPhase::exceptional_exit, before, after, outcome);
          }
        throw;
      }
    const Snapshot after = capture (std::as_const (state));
    check (ContractPhase::invariant_exit, before, after);
    check (ContractPhase::postcondition, before, after);
    check (ContractPhase::frame_condition, before, after);
  }

public:
  ExecutionContract &
  require (ContractPhase phase, std::string description, Predicate predicate)
  {
    switch (phase)
      {
      case ContractPhase::precondition:
      case ContractPhase::invariant_entry:
      case ContractPhase::invariant_exit:
      case ContractPhase::postcondition:
      case ContractPhase::frame_condition:
        break;
      default:
        throw std::invalid_argument ("unknown execution contract phase");
      }
    if (description.empty () || !predicate)
      throw std::invalid_argument ("execution contract requires a label and predicate");
    conditions_.push_back (
        { { phase, std::move (description) },
          [predicate = std::move (predicate)] (const Snapshot &before,
                                               const Snapshot &after,
                                               const std::exception_ptr &) {
            return predicate (before, after);
          } });
    return *this;
  }

  // Opt in to checking exceptions escaping statement.run only. Clauses see
  // the state before rollback; normal exit clauses do not run. If all pass,
  // rethrow the original exception. A false clause throws a violation holding
  // that exception. An exception from capture or a predicate propagates as
  // an instrumentation error instead (not as a clause violation).
  ExecutionContract &
  require_exceptional (std::string description, ExceptionalPredicate predicate)
  {
    if (description.empty () || !predicate)
      throw std::invalid_argument ("execution contract requires a label and predicate");
    conditions_.push_back (
        { { ContractPhase::exceptional_exit, std::move (description) },
          std::move (predicate) });
    return *this;
  }

  // Runtime clause inventory only. Listing or passing these checks is not
  // formal evidence and does not discharge any universal obligation.
  std::vector<VerificationObligation>
  obligations () const
  {
    std::vector<VerificationObligation> result;
    result.reserve (conditions_.size ());
    for (const auto &condition : conditions_)
      result.push_back (condition.obligation);
    return result;
  }

  template <class Program, class State, class Capture, class Limits>
  void
  run (const Program &statement, State &state, Capture capture, Limits limits) const
  {
    const Snapshot before = capture (std::as_const (state));
    check (ContractPhase::precondition, before, before);
    check (ContractPhase::invariant_entry, before, before);
    run_body (statement, state, capture, limits, before);
  }

  // Commit only after normal exit checks pass. After initial capture, restore
  // once on any escaping C++ exception, including errors from the checks or
  // a later capture. Capture must be observational and the initial snapshot
  // must own all data needed by the supplied noexcept restore. The caller is
  // responsible for actually restoring the covered state; there is no isolation
  // or rollback of effects outside that state (e.g. I/O or shared aliases).
  template <class Program, class State, class Capture, class Limits, class Restore>
    requires std::is_nothrow_invocable_r_v<void, Restore &, State &, const Snapshot &>
  void
  run_transactional (const Program &statement, State &state, Capture capture,
                     Limits limits, Restore restore) const
  {
    const Snapshot before = capture (std::as_const (state));
    try
      {
        check (ContractPhase::precondition, before, before);
        check (ContractPhase::invariant_entry, before, before);
        run_body (statement, state, capture, limits, before);
      }
    catch (...)
      {
        std::invoke (restore, state, before);
        throw;
      }
  }
};

enum class MachineControl { next, halt };

// A native transition can implement a stack machine, CE(S)K, or any other
// abstract machine without changing the executable DSL or its scope model.
template <class State, class Step>
std::size_t
run_machine (State &state, Step step, std::size_t max_steps = 100000)
{
  for (std::size_t count = 0; count < max_steps;)
    {
      ++count;
      switch (step (state))
        {
        case MachineControl::halt: return count;
        case MachineControl::next: break;
        default: throw std::invalid_argument ("invalid machine transition result");
        }
    }
  throw std::runtime_error ("machine transition limit exceeded");
}

// An opt-in, non-growing adapter over the existing term-rewriting engine.
// The trusted proof gate must attest contextual equivalence for the intended
// language and domain, including effects, traps and order of evaluation.
class GuardedTermOptimizer
{
  struct Measure
  {
    std::size_t fixed = 0;
    std::unordered_map<std::string, std::size_t> occurrences;
  };

  TermRewriteSystem engine_;

  static void
  account (const Term &term, Measure &measure, std::size_t &budget,
           std::size_t depth = 0)
  {
    if (depth >= 32 || budget <= term.symbol.size ())
      throw std::invalid_argument ("optimizer term limit exceeded");
    budget -= 1 + term.symbol.size ();
    if (term.is_variable)
      ++measure.occurrences[term.symbol];
    else
      {
        if (term.symbol.empty () || term.symbol.front () == '?'
            || SExpr::atom (term.symbol).quoted)
          throw std::invalid_argument ("optimizer requires a plain symbol");
        measure.fixed += 1 + term.symbol.size ();
      }
    for (const auto &child : term.arguments)
      account (child, measure, budget, depth + 1);
  }

  static Measure
  measure (const Term &term)
  {
    std::size_t budget = 2048;
    Measure result;
    account (term, result, budget);
    return result;
  }

  static Term
  canonical_term (const SExpr &syntax, bool pattern, std::size_t &budget,
                  std::size_t depth = 0)
  {
    if (depth >= 32 || budget == 0)
      throw std::invalid_argument ("optimizer syntax limit exceeded");
    const SExpr *atom = &syntax;
    if (!syntax.is_atom)
      {
        if (!syntax.text.empty () || syntax.quoted || syntax.items.size () < 2)
          throw std::invalid_argument ("optimizer requires a non-nullary term form");
        atom = &syntax.items.front ();
      }
    if (!atom->is_atom || !atom->items.empty () || atom->quoted
        || atom->text.empty () || budget <= atom->text.size ())
      throw std::invalid_argument ("optimizer requires a bounded symbol");
    budget -= 1 + atom->text.size ();
    if (SExpr::atom (atom->text).quoted)
      throw std::invalid_argument ("optimizer requires a plain symbol");
    if (atom->text.front () == '?')
      {
        if (!pattern || !syntax.is_atom)
          throw std::invalid_argument ("unexpected optimizer variable");
        return Term::variable (atom->text.substr (1));
      }
    std::vector<Term> children;
    if (!syntax.is_atom)
      for (std::size_t i = 1; i < syntax.items.size (); ++i)
        children.push_back (canonical_term (syntax.items[i], pattern, budget, depth + 1));
    return Term::function (atom->text, std::move (children));
  }

public:
  using ProofGate = std::function<bool (const TermRule &)>;

  void
  add_rule (const TermRule &rule, const ProofGate &verify)
  {
    if (!verify || engine_.rules ().size () >= 32 || rule.name.empty ()
        || rule.name.size () > 128)
      throw std::invalid_argument ("optimizer rule limit or missing proof gate");
    const auto lhs = measure (rule.lhs);
    const auto rhs = measure (rule.rhs);
    rule.validate ();
    if (rhs.fixed > lhs.fixed)
      throw std::invalid_argument ("optimizer rule grows fixed syntax");
    for (const auto &[name, count] : rhs.occurrences)
      if (auto pos = lhs.occurrences.find (name);
          pos == lhs.occurrences.end () || pos->second < count)
        throw std::invalid_argument ("optimizer rule duplicates a substitution");
    if (!verify (rule))
      throw std::invalid_argument ("optimizer rule lacks equivalence evidence");
    engine_.add_rule (rule);
  }

  void
  add_rule (const SExpr &syntax, const ProofGate &verify)
  {
    if (syntax.is_atom || syntax.quoted || !syntax.text.empty ()
        || syntax.items.size () != 4 || syntax.items[0] != SExpr::atom ("rule"))
      throw std::invalid_argument ("optimizer rule must be (rule name lhs rhs)");
    const auto &name = syntax.items[1];
    if (!name.is_atom || !name.items.empty () || name.text.empty ()
        || name.text.size () > 128)
      throw std::invalid_argument ("optimizer rule requires a bounded name");
    std::size_t lhs_budget = 2048;
    std::size_t rhs_budget = 2048;
    add_rule ({ name.text, canonical_term (syntax.items[2], true, lhs_budget),
                canonical_term (syntax.items[3], true, rhs_budget) }, verify);
  }

  void
  add_rule (std::string_view source, const ProofGate &verify)
  {
    add_rule (parse_bounded_sexpr (source, { 8192, 4096, 33 }), verify);
  }

  SExpr
  optimize (const SExpr &syntax, std::size_t max_steps = 128) const
  {
    std::size_t budget = 2048;
    auto term = canonical_term (syntax, false, budget);
    max_steps = std::min (max_steps, std::size_t{ 128 });
    for (std::size_t steps = 0; auto next = engine_.rewrite_once (term); ++steps)
      {
        if (steps >= max_steps)
          throw std::runtime_error ("optimizer step limit exceeded");
        (void) measure (next->term);
        term = std::move (next->term);
      }
    return term.sexpr ();
  }
};

// Adapt the same single-source command to CLI dispatch and REPL evaluation.
// EvaluationResult may be repl::Evaluation; the frontend owns output handling.
template <class EvaluationResult, class Run>
auto
make_evaluation_adapter (Run run)
{
  return [run = std::move (run)] (std::string_view source) mutable -> EvaluationResult {
    try
      {
        return { true, terminal::sanitize_text (run (source)) };
      }
    catch (const std::exception &error)
      {
        return { false, terminal::sanitize_text (error.what ()) };
      }
  };
}
} // namespace synth

namespace synth
{
enum class OptimizationBackend { ast_root, term_graph, equality_saturation };

struct OptimizationClaim
{
  OptimizationBackend backend;
  std::string_view domain, evidence;
  std::span<const ERewriteRule> rules;
  const SExpr &subject;
};

// Trusted callbacks have separate jobs: certify the exact rules over the
// stated domain and subject (in every reachable context), then check the
// actual candidate. Neither callback is a default, nor does a passing
// observation certify a rule. Claims and candidates are borrowed for the call.
// The caller must account for results, effects, traps, evaluation order, and
// graph sharing in that domain; callback work itself is not budgeted here.
struct OptimizationReview
{
  std::string_view domain, evidence;
  std::function<bool (const OptimizationClaim &)> certify;
  std::function<bool (const OptimizationClaim &, const SExpr &)> observe;
};

namespace reviewed_optimization_detail
{
inline constexpr std::size_t max_nodes = 64, max_depth = 12, max_text = 64;

inline void
enter (std::size_t depth, std::size_t &remaining)
{
  if (depth >= max_depth || remaining == 0)
    throw std::length_error ("reviewed optimizer syntax limit exceeded");
  --remaining;
}

inline void
check_syntax (const SExpr &syntax, bool pattern, std::size_t &remaining,
              std::size_t depth = 0)
{
  enter (depth, remaining);
  if (syntax.is_atom)
    {
      if (syntax.text.size () > max_text || !syntax.items.empty ()
          || (!syntax.quoted && SExpr::atom (syntax.text).quoted)
          || (!pattern && is_pattern_variable (syntax)))
        throw std::invalid_argument ("invalid reviewed optimizer atom");
      return;
    }
  if (syntax.quoted || !syntax.text.empty () || syntax.items.empty ()
      || syntax.items.size () > 5 || !syntax.items.front ().is_atom
      || is_pattern_variable (syntax.items.front ()))
    throw std::invalid_argument ("invalid reviewed optimizer application");
  for (const auto &child : syntax.items)
    check_syntax (child, pattern, remaining, depth + 1);
}

inline void
check_syntax (const SExpr &syntax, bool pattern)
{
  auto remaining = max_nodes;
  check_syntax (syntax, pattern, remaining);
}

inline void
check_rule (const ERewriteRule &rule)
{
  if (rule.name.empty () || rule.name.size () > max_text
      || is_pattern_variable (rule.lhs))
    throw std::invalid_argument ("invalid reviewed optimization rule");
  check_syntax (rule.lhs, true);
  check_syntax (rule.rhs, true);
  // Check bindings with the same rule parser used by the e-graph.
  (void) sexpr_rule (SExpr::list ({ SExpr::atom ("rewrite"), SExpr::atom (rule.name),
                                  rule.lhs, rule.rhs }));
}

inline std::vector<ERewriteRule>
bounded_rules (std::span<const ERewriteRule> rules)
{
  if (rules.empty () || rules.size () > 4)
    throw std::invalid_argument ("expected 1-4 reviewed optimization rules");
  for (const auto &rule : rules)
    check_rule (rule);
  return { rules.begin (), rules.end () };
}

inline void
approved (const OptimizationReview &review, const OptimizationClaim &claim)
{
  if (review.domain.empty () || review.domain.size () > 256
      || review.evidence.empty () || review.evidence.size () > 256
      || !review.certify || !review.observe || !review.certify (claim))
    throw std::invalid_argument ("reviewed optimization needs whole-domain approval");
}

inline void
observed (const OptimizationReview &review, const OptimizationClaim &claim,
          const SExpr &candidate)
{
  check_syntax (candidate, false);
  if (!review.observe (claim, candidate))
    throw std::runtime_error ("reviewed optimization candidate failed observation");
}

inline void
check_ast (const ASTNode &node, std::size_t &remaining, std::size_t depth = 0)
{
  enter (depth, remaining);
  if (node.tag ().size () > max_text || node.value ().size () > max_text
      || node.size () > 4 || (node.is_leaf () && node.size () != 0))
    throw std::invalid_argument ("invalid reviewed optimizer AST");
  for (const auto &child : node.children ())
    check_ast (child, remaining, depth + 1);
}

inline Term
exact_term (const SExpr &syntax)
{
  auto term = term_from_sexpr (syntax);
  if (term.sexpr () != syntax)
    throw std::invalid_argument ("term graph would change the syntax representation");
  return term;
}

inline void
check_graph_unfolding (const TermGraph &graph, GraphNodeId root,
                       std::size_t &remaining, std::size_t depth = 0)
{
  enter (depth, remaining);
  for (auto child : graph.graph ().successors (root))
    check_graph_unfolding (graph, child, remaining, depth + 1);
}
} // namespace reviewed_optimization_detail

inline ERewriteRule
read_reviewed_optimization_rule (std::string_view source)
{
  using namespace reviewed_optimization_detail;
  auto rule = sexpr_rule (parse_bounded_sexpr (source, { 8192, 2 * max_nodes + 3, max_depth }));
  check_rule (rule);
  return rule;
}

// One root attempt via RewriteRule, not an unbounded bottom-up pass.
inline std::optional<ASTNode>
reviewed_ast_candidate (const ASTNode &subject, const ERewriteRule &rule,
                        OptimizationReview review)
{
  using namespace reviewed_optimization_detail;
  auto remaining = max_nodes;
  check_ast (subject, remaining);
  const auto input = subject;
  const auto before = ast_to_sexpr (input);
  check_syntax (before, false);
  const auto rules = bounded_rules (std::span<const ERewriteRule> (&rule, 1));
  const OptimizationClaim claim{ OptimizationBackend::ast_root, review.domain,
                                 review.evidence, rules, before };
  approved (review, claim);
  SExprBindings bindings;
  const auto engine = dsl::rule<"reviewed-root"> (
      [&] (const ASTNode &node) {
        return match_sexpr (rules.front ().lhs, ast_to_sexpr (node), bindings);
      },
      [&] (const ASTNode &) {
        // This temporary is at most RHS nodes times input nodes, before the
        // strict output limit is checked and before conversion to an AST.
        auto result = substitute_sexpr (rules.front ().rhs, bindings);
        check_syntax (result, false);
        auto ast = sexpr_to_ast (result);
        if (ast_to_sexpr (ast) != result)
          throw std::invalid_argument ("AST rewrite candidate would change representation");
        return ast;
      });
  if (auto candidate = engine.try_apply (input))
    {
      observed (review, claim, ast_to_sexpr (*candidate));
      return candidate;
    }
  return std::nullopt;
}

inline SExpr
reviewed_graph_reduce (const SExpr &subject, std::span<const ERewriteRule> rules,
                       OptimizationReview review, std::size_t max_steps = 8)
{
  using namespace reviewed_optimization_detail;
  check_syntax (subject, false);
  const auto before = subject;
  const auto checked = bounded_rules (rules);
  auto input = exact_term (before);
  TermRewriteSystem system;
  for (const auto &rule : checked)
    system.add_rule ({ rule.name, exact_term (rule.lhs), exact_term (rule.rhs) });
  const OptimizationClaim claim{ OptimizationBackend::term_graph, review.domain,
                                 review.evidence, checked, before };
  approved (review, claim);
  TermGraph graph;
  const auto root = graph.add (input, true);
  const auto result = graph.reduce (root, system, std::min (max_steps, std::size_t{ 8 }));
  if (!result.normal_form ())
    throw std::runtime_error ("reviewed graph reduction step limit exceeded");
  // Graph storage grows by at most RHS nodes per step. Check the unfolding
  // separately: even a small DAG can denote an exponentially large tree.
  auto remaining = max_nodes;
  check_graph_unfolding (graph, result.root, remaining);
  auto candidate = graph.to_term (result.root).sexpr ();
  observed (review, claim, candidate);
  return candidate;
}

inline SExpr
reviewed_equality_saturate (const SExpr &subject, std::span<const ERewriteRule> rules,
                            OptimizationReview review,
                            SaturationLimits limits = { 4, 64, 32 })
{
  using namespace reviewed_optimization_detail;
  check_syntax (subject, false);
  const auto before = subject;
  const auto checked = bounded_rules (rules);
  for (const auto &rule : checked)
    if (!rule.lhs.is_atom
        && std::any_of (rule.lhs.items.begin () + 1, rule.lhs.items.end (),
                        [] (const SExpr &arg) { return !arg.is_atom; }))
      throw std::invalid_argument ("reviewed e-matching requires flat left sides");
  const OptimizationClaim claim{ OptimizationBackend::equality_saturation,
                                 review.domain, review.evidence, checked, before };
  approved (review, claim);
  EGraph graph;
  auto root = graph.add (before);
  limits = { std::min (limits.iterations, std::size_t{ 4 }),
             std::min (limits.nodes, max_nodes),
             std::min (limits.matches, std::size_t{ 32 }) };
  if (!graph.saturate (checked, limits).saturated ())
    throw std::runtime_error ("reviewed equality saturation did not reach a fixpoint");
  // Min-node extraction always has the bounded original as an alternative.
  auto candidate = graph.extract (root);
  observed (review, claim, candidate);
  return candidate;
}
} // namespace synth

// END LIBDSL LIVE FACULTIES

} // namespace dsl
