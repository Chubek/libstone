#!/usr/bin/env perl
=pod

=head1 NAME

acsem.pl - executable Action Semantics and standalone interpreter synthesis

=head1 SYNOPSIS

  # Run these commands from the acsem directory.
  perl acsem.pl --check examples/calculator.acsem
  perl acsem.pl --run examples/calculator.acsem \
    --term 'Add(Num(20),Num(22))'
  perl acsem.pl --emit python examples/calculator.acsem -o calc.py
  python3 calc.py --term 'Mul(Num(6),Num(7))'
  perl acsem.pl --emit c examples/calculator.acsem -o calc.c
  perl acsem.pl --emit cpp examples/calculator.acsem -o calc.cpp
  perl acsem.pl --emit pascal examples/calculator.acsem -o calc.pas
  perl acsem.pl --emit rust examples/calculator.acsem -o calc.rs
  perl acsem.pl --emit java examples/calculator.acsem -o Calculator.java
  perl acsem.pl --emit perl examples/calculator.acsem -o calc.pl
  perl acsem.pl --emit json examples/calculator.acsem -o calc.json
  prove acsem.t backends.t

=head1 DESCRIPTION

An executable, bounded dialect of Action Notation inspired by the Action
Semantics work of Peter D. Mosses and David A. Watt. A specification gives
abstract syntax and a semantic equation for each constructor. C<acsem>
checks the specification, executes constructor-form programs, and generates
standalone interpreters in Perl, C, C++, Pascal, Python, Rust, or Java.
JSON output exports the validated model for inspection and tooling.

The generator requires Perl 5.14 or later and core modules only. Keep
C<acsem.pl>, C<Acsem/Emit.pm>, and C<runtime/> together when installing it.
Each emitted source contains its own model and runtime, uses its target's
standard library, and runs independently of the generator and specification.
Emission requires no target compiler; compiling or running an artifact requires
the corresponding toolchain. C and C++ share a C99/C++11-compatible kernel.

Action Semantics separates a language's syntax-to-action mapping from the
meaning of the actions themselves. This makes mechanisms such as data flow,
lexical bindings, and mutable storage explicit and reusable. The literature
in L</REFERENCES> describes the broader formal framework. This implementation
defines the concrete subset below; it does not implement every notation or
data algebra from those publications.

=head1 QUICK START: BUILD A CALCULATOR

Save this specification as C<calculator.acsem>:

  language Calculator {
    start Expr;
    syntax Expr = Num(value: integer) | Add(left: Expr, right: Expr);
    rule Num = give value;
    rule Add = (evaluate(left) and then evaluate(right))
               then give given(1) + given(2);
  }

Check the specification and run a program:

  perl acsem.pl --check calculator.acsem
  # OK: Calculator
  perl acsem.pl --run calculator.acsem --term 'Add(Num(20),Num(22))'
  # completed 42

C<start Expr> chooses the sort required at the root of a program. C<syntax>
declares constructors and their ordered, typed fields. The rule for C<Num>
yields its integer field. The rule for C<Add> evaluates its two syntax-valued
fields, collects their results with C<and then>, and passes those results to
the addition action with C<then>. Transient positions are numbered from one.

Programs are abstract syntax trees written with constructors and parentheses:
C<Num(42)> and C<Add(Num(20),Num(22))>. The text C<20 + 22> is not a program
in this input format. A source-language parser can construct the equivalent
constructor-form text before invoking the interpreter.

C<examples/calculator.acsem> extends this example with subtraction,
multiplication, division, equality, and less-than. For example:

  perl acsem.pl --run examples/calculator.acsem \
    --term 'Less(Mul(Num(6),Num(7)),Num(100))'
  # completed 1

=head1 GENERATING AND RUNNING EVERY TARGET

The commands below use C<examples/calculator.acsem> and run from the
C<acsem> directory. Every example computes C<completed 42>.

=head2 C (C99 or later)

  perl acsem.pl --emit c examples/calculator.acsem -o calc.c
  cc -std=c99 -O2 -Wall -Wextra calc.c -o calc-c
  ./calc-c --term 'Add(Num(20),Num(22))'

=head2 C++ (C++11 or later)

  perl acsem.pl --emit cpp examples/calculator.acsem -o calc.cpp
  c++ -std=c++11 -O2 -Wall -Wextra calc.cpp -o calc-cpp
  ./calc-cpp --term 'Add(Num(20),Num(22))'

C<c++> is an alias for C<cpp>. The shared portable runtime is compiled
directly by the C++ compiler; no C library or foreign-language runtime is needed.

=head2 Pascal (Free Pascal 3.2 or later)

  perl acsem.pl --emit pascal examples/calculator.acsem -o calc.pas
  fpc -O2 -ocalc-pascal calc.pas
  ./calc-pascal --term 'Add(Num(20),Num(22))'

The source selects ObjFPC mode and uses the standard C<SysUtils> and
C<Classes> units. Range and overflow checks can be enabled with C<-Cr -Co>.

=head2 Python (Python 3.8 or later)

  perl acsem.pl --emit python examples/calculator.acsem -o calc.py
  python3 calc.py --term 'Add(Num(20),Num(22))'

=head2 Rust (Rust 2021 edition)

  perl acsem.pl --emit rust examples/calculator.acsem -o calc.rs
  rustc --edition=2021 -O calc.rs -o calc-rust
  ./calc-rust --term 'Add(Num(20),Num(22))'

Cargo and external crates are unnecessary.

=head2 Java (Java 8 or later)

  perl acsem.pl --emit java examples/calculator.acsem -o Calculator.java
  javac Calculator.java
  java Calculator --term 'Add(Num(20),Num(22))'

The public class name is inferred from an output filename ending in C<.java>.
For stdout output the default is C<AcsemProgram>; C<--class-name NAME>
selects another name. A class name must be a non-reserved ASCII Java identifier
and must match the C<.java> filename when one is supplied. The generator emits
one source file; C<javac> produces the main class and its nested helper classes.
Distribute all of those class files, or package them together in a JAR.

=head2 Perl and JSON

  perl acsem.pl --emit perl examples/calculator.acsem -o calc.pl
  perl calc.pl --term 'Add(Num(20),Num(22))'
  perl acsem.pl --emit json examples/calculator.acsem -o calc.json

The Perl artifact embeds the reference runtime and canonical JSON model.
JSON output is data, not an executable interpreter. Model version 1 includes
C<name>, C<start>, C<sorts>, C<sort_order>, C<constructors>, and C<rules>.
C<at> members are zero-based byte offsets into the specification. There is
currently no CLI mode for loading arbitrary JSON models.

=head1 TUTORIAL: LEXICAL VARIABLES

C<examples/expressions.acsem> includes these constructors and rules:

  Var(name: identifier)
  Let(name: identifier, value: Expr, body: Expr)

  rule Var = give bound(name);
  rule Let = furthermore (evaluate(value) then bind name to given(1))
             hence evaluate(body);

First, C<evaluate(value)> computes the initializer using the incoming
environment. C<then> makes its result available to C<bind>. The binding's
key comes from the identifier field C<name>. C<furthermore> overlays the new
binding on the incoming environment, and C<hence> supplies that extended
environment to the body.

  perl acsem.pl --run examples/expressions.acsem \
    --term 'Let(x,Num(10),Let(y,Num(32),Add(Var(x),Var(y))))'
  # completed 42
  perl acsem.pl --run examples/expressions.acsem \
    --term 'Let(x,Num(4),Let(x,Num(5),Var(x)))'
  # completed 5

Bare C<x> in the program is an identifier field value. In a specification,
C<"x"> denotes a literal identifier token, while C<x> denotes a field named
C<x>. Tokens contain only letters, digits, and underscores and start with a
letter or underscore; they are not general strings.

Omitting C<furthermore> replaces the incoming environment with just the new
binding. Replacing C<hence> with C<then> does not pass the binding to the body.
These are useful distinctions when defining scope. A missing binding yields
semantic failure, which C<otherwise> can handle.

=head1 TUTORIAL: MUTABLE STORAGE

The expression example also includes C<New()>, C<Get(cell)>, C<Set(cell,value)>,
and C<Seq(first,second)>:

  rule New = allocate cell;
  rule Get = evaluate(cell) then give contents(given(1));
  rule Set = (evaluate(cell) and then evaluate(value))
             then store given(2) in given(1);
  rule Seq = evaluate(first) then evaluate(second);

An allocated cell is a distinct typed identity, not an integer address.
Allocation yields a fresh, initially uninitialized cell. C<store> accepts
only integer contents and completes with no transients. C<contents> fails
until the cell has been initialized.

  perl acsem.pl --run examples/expressions.acsem \
    --term 'Let(c,New(),Seq(Set(Var(c),Num(40)),Add(Get(Var(c)),Num(2))))'
  # completed 42
  perl acsem.pl --run examples/expressions.acsem --term 'Pair(New(),New())'
  # completed cell:0 cell:1
  perl acsem.pl --run examples/expressions.acsem --term 'Get(New())'
  # failed (exit 1)

Each invocation starts with an empty store and cell numbering at zero.
Bindings can hold integers, tokens, or cells; storing into a cell changes
the store rather than the binding that names the cell.

=head1 TUTORIAL: FAILURE AND ESCAPE

There are three action outcomes: completion, failure, and escape. Failure
signals that an action cannot complete, including an undefined yielder or a
false C<check>. Escape carries transients out to a handler.

  perl acsem.pl --run examples/expressions.acsem \
    --term 'Fallback(Div(Num(1),Num(0)),Num(42))'
  # completed 42
  perl acsem.pl --run examples/expressions.acsem \
    --term 'Catch(Raise(Num(42)),Given())'
  # completed 42
  perl acsem.pl --run examples/expressions.acsem \
    --term 'Fallback(Raise(Num(7)),Num(42))'
  # escaped 7 (exit 1)

C<otherwise> handles only failure. C<trapping> handles only escape and
supplies the escaped transients to its handler. Neither combinator handles
malformed programs or resource-limit errors.

Storage changes persist across failure and escape. In
C<examples/recovery.acsem>, C<Failure()> stores 7 and then fails; its fallback
reads 7. C<Escape()> stores 9 and escapes; its handler reads 9:

  perl acsem.pl --run examples/recovery.acsem --term 'Failure()'
  # completed 7
  perl acsem.pl --run examples/recovery.acsem --term 'Escape()'
  # completed 9

Recovery does not implement a transaction or undo earlier allocations.

=head1 SPECIFICATION AND ACTION REFERENCE

=head2 Specification structure

  language Name {
    start Sort;
    syntax Sort = Constructor(field: Type, ...) | Empty();
    syntax AnotherSort = Other(child: Sort);
    rule Constructor = Action;
    rule Empty = Action;
    rule Other = Action;
  }

Declare all syntax sorts before rules. Every constructor, including a
nullary one, uses parentheses in both declarations and programs. Fields
are positional in programs and named in rules. Types are C<integer>,
C<identifier>, or a declared syntax sort. Sorts may be mutually recursive
provided every sort has some finite inhabitant. Names are case-sensitive
ASCII identifiers. C<#> and C<//> introduce line comments in specifications;
program terms accept whitespace but no comments. Quoted token literals are
available only in specifications. C<given>, C<bound>, and C<contents> are
reserved yielder names; avoid those names for fields that rules must access.

=head2 Primitives and yielders

The B<basic> facet controls completion, failure, escape, and sequencing;
the B<functional> facet passes transient data; the B<declarative> facet
handles bindings; the B<imperative> facet changes storage.

  complete              Complete with no outputs or bindings.
  fail                  Fail with no outputs or bindings.
  give Y                Complete with one transient value.
  check Y               Complete if Y is a nonzero integer; otherwise fail.
  escape Y              Escape with one transient value.
  bind K to Y           Complete with one token-to-value binding.
  allocate cell         Give a fresh, uninitialized cell identity.
  store Y in C          Store an integer in an allocated cell; give nothing.
  evaluate(field)       Execute the rule for a syntax-valued child field.
  furthermore A         Overlay A's completed bindings on the received ones.

C<evaluate> receives the current transients and environment. C<furthermore>
leaves the body's transients unchanged and propagates failure or escape.
It applies to the next primitive or parenthesized action.

  42, -7                Signed 32-bit integer literals.
  "name"                Literal identifier token.
  field                 Scalar constructor field.
  given(n)              Received transient n; literal index 1..64.
  bound(K)              Value bound to the token yielded by K.
  contents(C)           Integer currently stored in the cell yielded by C.
  X + Y, X - Y          Integer addition and subtraction.
  X * Y, X / Y          Integer multiplication and division.
  X == Y, X < Y         Integer comparisons yielding 0 or 1.
  (Y)                   Parenthesized yielder.

Multiplication and division bind tighter than addition and subtraction;
comparison has lowest precedence. Arithmetic operators associate left;
comparisons cannot be chained. A minus sign can precede an integer literal;
use C<0 - Y> to negate another yielder. Division truncates toward zero.
Overflow, division by zero, missing transients/bindings, and uninitialized
cells make a yielder undefined and its containing primitive fail. Tokens
and cells cannot be used as integers, including in comparisons.

=head2 Combinator data flow

All five binary combinators have B<equal precedence> and associate B<left>.
Use parentheses to express the intended grouping. For C<A OP B>, the rules
on successful completion are:

  OP          B's transients    B's environment    Result transients
  then        A's outputs       original           B's outputs
  and then    original          original           A's then B's outputs
  hence       original          A's bindings       A's then B's outputs

C<then> and C<and then> join the two binding results. Any duplicate key
causes failure, even if the associated values are equal. C<hence> returns
only B's bindings. Both C<then> and C<and then> execute left-to-right;
C<and then> is sequential, not concurrent. If A fails or escapes, B is
skipped; if B fails or escapes, its outcome becomes the combined outcome.

C<A otherwise B> returns A's outcome unless A failed; in that case B
receives the original transients and environment. C<A trapping B> returns
A's outcome unless A escaped; in that case B receives A's escaped transients
and the original environment. Storage is shared throughout all combinations.

=head2 Deliberate boundaries

The supported data domains are signed 32-bit integers, identifier tokens,
and allocated cell identities. Nondeterministic choice, concurrency,
communication, closures, C<unfolding>, user-defined data algebras, general
strings, and concrete source grammars are outside this dialect. The targets
generate interpreters for the same subset, not optimizing compilers for
individual input programs.

=head1 TYPECHECKING

Every sort must be productive; constructors have unique global names, fields
have known sorts, and each constructor has exactly one rule. The static checker
rejects syntax nodes used as scalar data and incompatible known operand types.
C<given> and C<bound> have dynamic types: their values are checked at execution.
The checker does not infer transient tuple shapes, binding availability,
cell initialization, or termination. Thus C<give given(1)> passes checking
but fails when run without a received transient. Diagnostics for malformed
sources include one-based line and byte-column positions.

For a new constructor: declare its fields, write exactly one rule, check the
specification, and run one completing example and one boundary/failure case.
If a rule says C<give left> for a syntax field, use C<evaluate(left)> instead.
If a variable becomes unbound inside a nested scope, examine the distinction
between C<then>, C<hence>, and C<furthermore> before changing the variable rule.

=head1 COMMAND LINE AND RESULT PROTOCOL

=over 4

=item C<--check SPEC>

Parse and typecheck, printing C<OK: Name>. Term and output-file options are
rejected in this mode.

=item C<--run SPEC>

Check and execute a program. Execution is also the default when only a
specification path is supplied. C<--check>, C<--run>, and C<--emit> are exclusive.

=item C<--emit TARGET SPEC>

Emit C<perl>, C<c>, C<cpp> (or C<c++>), C<pascal>, C<python>, C<rust>,
C<java>, or C<json>. Write to stdout unless C<-o FILE> is supplied.

=item C<--term PROGRAM>, C<--term-file PATH>

Choose exactly one program source. When neither is supplied, read all stdin
as one program. These options apply to execution, including generated
interpreters. Stdin is not a multi-program REPL. C<--term=PROGRAM> also works.

=item C<-o FILE>, C<--output FILE>

Write emitted source or the execution result to a file. C<-> means stdout.
Diagnostics always go to stderr. Generated interpreters support these options.

=item C<--class-name NAME>

Choose the public Java class name; valid only with C<--emit java>.

=item C<--help>, C<-h>, C<--version>

Show usage or version. Generated interpreters also provide these options.

=back

Execution prints exactly one result line: C<completed>, C<failed>, or
C<escaped>, followed by space-separated transients. Integers print in decimal,
tokens as C<token:name>, and cells as C<cell:0>. Binding results and the store
are internal; yield their values explicitly to make them visible. For example:

  printf '%s\n' 'Add(Num(20),Num(22))' | python3 calc.py
  # completed 42
  python3 calc.py --term-file program.term -o result.txt

=head1 LIMITS AND EXIT STATUS

All targets apply the same fixed limits per specification or execution:

  Specification/program source     65536 bytes each
  Identifier or digit token        64 bytes
  Lexed source tokens              20000
  Constructors                     256
  Fields per constructor           16
  Action/yielder nodes             4096 across all rules
  Program syntax nodes             2048
  Parse/elaboration/evaluation     128 nesting levels
  Transient tuple                  64 values
  Binding result/environment       64 entries (including furthermore)
  Distinct runtime tokens          256
  Allocated cells                  1024
  Evaluation steps                 100000

The source limit applies to input specifications and programs, not to the
size of generated artifacts. Numeric literals are decimal, allow leading
zeroes, and must lie in -2147483648..2147483647. A leading minus is a separate
source token. Runtime token accounting includes identifiers anywhere in the
program and literal tokens actually evaluated by rules. Each executed syntax
node, action, and yielder consumes a step. Evaluation depth is consequently
not the same as syntax-tree depth.

Exit B<0> means completion (or successful checking/emission), B<1> means
semantic failure or an uncaught escape, and B<2> means invalid options,
malformed input, I/O failure, or resource exhaustion. Errors have an
C<ACSEM_ERROR:> or C<ACSEM_LIMIT:> prefix. They do not produce a result line
and cannot be intercepted by C<otherwise> or C<trapping>.

=head1 TESTING AND IMPLEMENTATION NOTES

  perl -c acsem.pl
  podchecker acsem.pl
  prove acsem.t backends.t
  ACSEM_REQUIRE_TARGETS=1 prove acsem.t backends.t

C<acsem.t> checks the parser, typechecker, reference semantics, and Perl/JSON
synthesis. C<backends.t> compiles and runs the available targets against the
same completion, failure, escape, arithmetic, storage, scope, malformed-input,
and resource-bound examples. It reports missing toolchains as skips; setting
C<ACSEM_REQUIRE_TARGETS=1> makes them failures. Set C<CC>, C<CXX>, C<FPC>,
C<PYTHON>, C<RUSTC>, C<JAVAC>, or C<JAVA> to override executable paths.

The front end and reference evaluator live in C<acsem.pl>.
C<Acsem/Emit.pm> resolves field names to indexes and lowers checked actions
into deterministic instruction tables. Templates in C<runtime/> contain
native term parsers and evaluators. Constructor and token names are data in
those tables, so target-language keywords do not become variable names.
Java class naming is checked separately. A new semantic operation requires
updates to the front end, reference evaluator, lowering, each runtime, and
shared behavioral cases. Re-emitting an unchanged model with the same version,
target, and class name produces identical source.

The reference API can be used from Perl:

  require './acsem.pl';
  my $model = Acsem::parse_spec(Acsem::slurp_file('calculator.acsem'));
  my $result = Acsem::run_term($model, 'Add(Num(20),Num(22))');
  print Acsem::result_text($result);

Model hashes should come from C<parse_spec>; this API is not an importer for
untrusted or hand-built model hashes. C<run_term> creates fresh execution state
on every call. Its result contains C<status>, C<out> (typed values), and
C<bind> (a token-to-value mapping).

=head1 REFERENCES

=over 4

=item Peter D. Mosses, I<Action Semantics>.

Cambridge Tracts in Theoretical Computer Science 26, Cambridge University
Press, 1992. The principal reference for actions, yielders, data, and the
facets of Action Notation. L<https://doi.org/10.1017/CBO9780511569869>.

=item Peter D. Mosses, I<An Introduction to Action Semantics>.

In I<Logic and Algebra of Specification>, Springer, 1993, pp. 247-288.
A tutorial introduction to the approach and its semantic building blocks.
L<https://doi.org/10.1007/978-3-642-58041-3_7>.

=item Peter D. Mosses and David A. Watt, I<The Use of Action Semantics>.

In I<Formal Description of Programming Concepts III>, North-Holland, 1987.
An early account of using action descriptions to specify programming languages.

=item Peter D. Mosses, I<Action Semantics> project overview.

Historical context and links to the book and subsequent semantic frameworks:
L<https://pdmosses.github.io/research/action-semantics/>.

=back

=head1 LICENSE

MIT-0. This script can be copied, modified, and redistributed.

=cut

use strict;
use warnings;
use 5.014;
use JSON::PP ();
use Getopt::Long ();

package Acsem;
use strict;
use warnings;
no warnings 'recursion'; # Explicit parse, visitor and execution depth budgets apply.
use 5.014;

our $VERSION = '0.2.0';
our $MAX_SOURCE = 65_536;
our $MAX_NODES = 4096;
our $MAX_DEPTH = 128;
our $MAX_STEPS = 100_000;
our $MIN = -2147483648;
our $MAX = 2147483647;

sub error {
    my ($s, $message, $at) = @_;
    $at //= (ref($s) eq 'HASH' && $s->{tokens} ? $s->{tokens}[$s->{pos}]{at} : 0);
    my $source = ref($s) eq 'HASH' ? ($s->{source} // '') : '';
    my $before = substr($source, 0, $at);
    my $line = 1 + (() = $before =~ /\n/g);
    my $last = rindex($before, "\n");
    my $col = length($before) - $last;
    die "ACSEM_ERROR: $message at $line:$col\n";
}
sub limit { die "ACSEM_LIMIT: $_[0]\n" }
sub bytes_check { limit('source exceeds 64 KiB') if length($_[0]) > $MAX_SOURCE; error({source=>$_[0]},'NUL in source',index($_[0],"\0")) if index($_[0],"\0") >= 0 }
sub int32 {
    return 0 unless defined $_[0] && $_[0] =~ /\A-?[0-9]+\z/;
    my $digits=$_[0];$digits=~s/^-//;$digits=~s/^0+//;
    return length($digits)<=10 && $_[0]>=$MIN && $_[0]<=$MAX;
}
sub lex {
    my ($source, $mode) = @_;
    bytes_check($source);
    my @tokens;
    pos($source) = 0;
    while (pos($source) < length($source)) {
        if ($source =~ /\G[ \t\r\n]+/gc) { next }
        if ($mode ne 'term' && $source =~ /\G(?:\#[^\r\n]*|\/\/[^\r\n]*)/gc) { next }
        my $at = pos($source);
        if ($source =~ /\G([A-Za-z_][A-Za-z_0-9]*)/gc) {
            limit('identifier exceeds 64 bytes') if length($1) > 64;
            push @tokens, {k=>'id',v=>$1,at=>$at};
        } elsif ($source =~ /\G([0-9]+)/gc) {
            limit('integer spelling exceeds 64 bytes') if length($1) > 64;
            push @tokens, {k=>'num',v=>$1,at=>$at};
        } elsif ($mode ne 'term' && $source =~ /\G"([A-Za-z_][A-Za-z_0-9]*)"/gc) {
            limit('token exceeds 64 bytes') if length($1) > 64;
            push @tokens, {k=>'str',v=>$1,at=>$at};
        } elsif ($source =~ /\G(==|[{}(),;:=|+*\/-]|<)/gc) {
            push @tokens, {k=>'sym',v=>$1,at=>$at};
        } else { error({source=>$source}, 'invalid '.$mode.' syntax', $at) }
        limit('too many tokens') if @tokens > 20_000;
    }
    push @tokens, {k=>'eof',v=>'<eof>',at=>length($source)};
    return {source=>$source,tokens=>\@tokens,pos=>0,depth=>0,nodes=>0,mode=>$mode};
}
sub peek { $_[0]{tokens}[$_[0]{pos}]{v} }
sub kind { $_[0]{tokens}[$_[0]{pos}]{k} }
sub take { my ($s)=@_; $s->{pos}++; return $s->{tokens}[$s->{pos}-1] }
sub eat { my ($s,$v)=@_; return 0 if peek($s) ne $v; take($s); return 1 }
sub expect {
    my ($s,$v)=@_;
    error($s,"expected '$v'") unless eat($s,$v);
}
sub ident {
    my ($s)=@_;
    error($s,'expected identifier') unless kind($s) eq 'id';
    return take($s)->{v};
}
sub node {
    my ($s,$type,%args)=@_;
    $s->{nodes}++; limit('too many actions or yielders') if $s->{nodes}>$MAX_NODES;
    return {op=>$type,at=>$s->{tokens}[$s->{pos}]{at},%args};
}
sub nested {
    my ($s,$sub)=@_;
    limit('parse depth exceeded') if ++$s->{depth}>$MAX_DEPTH;
    my $v=$sub->();
    $s->{depth}--;
    return $v;
}
sub parse_expr {
    my ($s)=@_;
    my $left=parse_sum($s);
    if (peek($s) eq '==' || peek($s) eq '<') {
        my $op=take($s)->{v}; my $right=parse_sum($s);
        $left=node($s,'binary',symbol=>$op,left=>$left,right=>$right);
    }
    return $left;
}
sub parse_sum {
    my ($s)=@_;
    my $left=parse_product($s);
    while (peek($s) eq '+' || peek($s) eq '-') {
        my $op=take($s)->{v}; my $right=parse_product($s);
        $left=node($s,'binary',symbol=>$op,left=>$left,right=>$right);
    }
    return $left;
}
sub parse_product {
    my ($s)=@_;
    my $left=parse_atom($s);
    while (peek($s) eq '*' || peek($s) eq '/') {
        my $op=take($s)->{v}; my $right=parse_atom($s);
        $left=node($s,'binary',symbol=>$op,left=>$left,right=>$right);
    }
    return $left;
}
sub parse_atom {
    my ($s)=@_;
    my $at=$s->{tokens}[$s->{pos}]{at};
    return nested($s,sub{ my $v=parse_expr($s); expect($s,')'); $v }) if eat($s,'(');
    if (kind($s) eq 'num' || peek($s) eq '-') {
        my $neg=eat($s,'-');
        error($s,'expected digits after minus') unless kind($s) eq 'num';
        my $str=($neg?'-':'').take($s)->{v};
        error($s,'integer outside signed 32-bit range') unless int32($str);
        return node($s,'integer',value=>0+$str,at=>$at);
    }
    return node($s,'token',value=>take($s)->{v},at=>$at) if kind($s) eq 'str';
    if (kind($s) eq 'id') {
        my $name=ident($s);
        if ($name eq 'given' || $name eq 'bound' || $name eq 'contents') {
            expect($s,'(');
            my $arg;
            if ($name eq 'given') {
                error($s,'given index must be integer literal 1..64') unless kind($s) eq 'num';
                my $n=take($s)->{v};
                error($s,'given index must be integer literal 1..64') unless int32($n) && $n>=1 && $n<=64;
                $arg=node($s,'integer',value=>0+$n);
            } else { $arg=nested($s,sub{parse_expr($s)}) }
            expect($s,')');
            return node($s,$name,arg=>$arg,at=>$at);
        }
        return node($s,'field',name=>$name,at=>$at);
    }
    error($s,'expected yielder');
}
sub parse_primitive {
    my ($s)=@_;
    my $at=$s->{tokens}[$s->{pos}]{at};
    my $t=peek($s);
    if (eat($s,'(')) {
        return nested($s,sub{my $a=parse_action($s);expect($s,')');$a});
    }
    if (eat($s,'furthermore')) {
        return node($s,'furthermore',body=>nested($s,sub{parse_primitive($s)}),at=>$at);
    }
    if ($t eq 'complete' || $t eq 'fail') {take($s);return node($s,$t,at=>$at)}
    if ($t eq 'give' || $t eq 'check' || $t eq 'escape') {
        take($s);return node($s,$t,arg=>parse_expr($s),at=>$at);
    }
    if ($t eq 'bind') {
        take($s);my $key=parse_expr($s);expect($s,'to');
        return node($s,'bind',key=>$key,value=>parse_expr($s),at=>$at);
    }
    if ($t eq 'store') {
        take($s);my $value=parse_expr($s);expect($s,'in');
        return node($s,'store',value=>$value,cell=>parse_expr($s),at=>$at);
    }
    if ($t eq 'allocate') {take($s);expect($s,'cell');return node($s,'allocate',at=>$at)}
    if ($t eq 'evaluate') {
        take($s);expect($s,'(');my $field=ident($s);expect($s,')');
        return node($s,'evaluate',field=>$field,at=>$at);
    }
    error($s,'expected action');
}
sub parse_action {
    my ($s)=@_;
    my $left=parse_primitive($s);
    while (1) {
        my $op=peek($s);
        if ($op eq 'and' ) {take($s);expect($s,'then');$op='and then'}
        elsif ($op =~ /\A(?:then|hence|otherwise|trapping)\z/) {take($s)}
        else {last}
        my $right=parse_primitive($s);
        $left=node($s,'combine',symbol=>$op,left=>$left,right=>$right);
    }
    return $left;
}
sub parse_spec {
    my ($source)=@_;
    my $s=lex($source,'spec');
    expect($s,'language'); my $name=ident($s);expect($s,'{');
    expect($s,'start');my $start=ident($s);expect($s,';');
    my (%sorts,%constructors,%rules);my @sort_order;
    while(eat($s,'syntax')) {
        my $sort=ident($s);
        error($s,'duplicate sort '.$sort) if exists $sorts{$sort};
        error($s,'sort collides with built-in') if $sort eq 'integer' || $sort eq 'identifier';
        expect($s,'=');my @cons;
        do {
            my $at=$s->{tokens}[$s->{pos}]{at};
            my $ctor=ident($s);
            error($s,'duplicate constructor '.$ctor) if exists $constructors{$ctor};
            limit('too many constructors') if keys(%constructors)>=256;
            expect($s,'('); my @fields; my %seen;
            unless (eat($s,')')) {
                do {
                    my $field=ident($s);error($s,'duplicate field '.$field) if $seen{$field}++;
                    expect($s,':');my $type=ident($s);
                    limit('too many fields') if @fields>=16;
                    push @fields,{name=>$field,type=>$type};
                } while(eat($s,','));
                expect($s,')');
            }
            my $c={name=>$ctor,sort=>$sort,fields=>\@fields,at=>$at};
            $constructors{$ctor}=$c;push @cons,$ctor;
        } while(eat($s,'|'));
        expect($s,';');
        $sorts{$sort}=\@cons;push @sort_order,$sort;
    }
    error($s,'at least one syntax declaration required') unless @sort_order;
    while(eat($s,'rule')) {
        my $at=$s->{tokens}[$s->{pos}]{at};my $ctor=ident($s);
        error($s,'unknown constructor '.$ctor) unless exists $constructors{$ctor};
        error($s,'duplicate rule '.$ctor) if exists $rules{$ctor};
        expect($s,'=');$rules{$ctor}=parse_action($s);
        expect($s,';');
    }
    error($s,'at least one rule required') unless keys(%rules);
    expect($s,'}');error($s,'trailing specification syntax') unless kind($s) eq 'eof';
    my $model={version=>1,name=>$name,start=>$start,sorts=>\%sorts,
       sort_order=>\@sort_order,constructors=>\%constructors,rules=>\%rules};
    typecheck($model,$s);
    return $model;
}
sub field_type {
    my ($c,$name,$s,$at)=@_;
    for my $f (@{$c->{fields}}) {return $f->{type} if $f->{name} eq $name}
    error($s,'unknown field '.$name,defined($at)?$at:$c->{at});
}
sub require_type {
    my ($s,$actual,$expected,$message,$at)=@_;
    error($s,'type mismatch: '.$message,$at) if $actual ne 'dynamic' && $actual ne $expected;
}
sub type_expr {
    my ($expr,$ctor,$s)=@_;
    local $s->{visitor_depth}=($s->{visitor_depth}//0)+1;
    limit('elaboration visitor depth exceeded') if $s->{visitor_depth}>$MAX_DEPTH;
    my $op=$expr->{op};
    return 'integer' if $op eq 'integer';
    return 'identifier' if $op eq 'token';
    if ($op eq 'field') {
        my $t=field_type($ctor,$expr->{name},$s,$expr->{at});
        error($s,'syntax field must be evaluated with evaluate(field)',$expr->{at})
             if $t ne 'integer' && $t ne 'identifier';
        return $t;
    }
    return 'dynamic' if $op eq 'given';
    if ($op eq 'bound') {
        my $t=type_expr($expr->{arg},$ctor,$s);
        require_type($s,$t,'identifier','bound expects token',$expr->{at});
        return 'dynamic';
    }
    if ($op eq 'contents') {
        my $t=type_expr($expr->{arg},$ctor,$s);
        require_type($s,$t,'cell','contents expects cell',$expr->{at});
        return 'integer';
    }
    if ($op eq 'binary') {
        for my $child ($expr->{left},$expr->{right}) {
            require_type($s,type_expr($child,$ctor,$s),'integer','arithmetic/comparison requires integer',$expr->{at});
        }
        return 'integer';
    }
    error($s,'unknown yielder node '.$op,$expr->{at});
}
sub type_action {
    my ($a,$ctor,$s)=@_;
    local $s->{visitor_depth}=($s->{visitor_depth}//0)+1;
    limit('elaboration visitor depth exceeded') if $s->{visitor_depth}>$MAX_DEPTH;
    my $op=$a->{op};
    return if $op eq 'complete' || $op eq 'fail' || $op eq 'allocate';
    if ($op eq 'evaluate') {
        my $t=field_type($ctor,$a->{field},$s,$a->{at});
        error($s,'evaluate(field) requires a syntax field',$a->{at})
            if $t eq 'integer' || $t eq 'identifier';
        return;
    }
    if ($op eq 'furthermore') {type_action($a->{body},$ctor,$s);return}
    if ($op eq 'combine') {
        type_action($a->{left},$ctor,$s);type_action($a->{right},$ctor,$s);return;
    }
    if ($op eq 'give' || $op eq 'escape') {type_expr($a->{arg},$ctor,$s);return}
    if ($op eq 'check') {
        require_type($s,type_expr($a->{arg},$ctor,$s),'integer','check expects integer',$a->{at});return;
    }
    if ($op eq 'bind') {
        require_type($s,type_expr($a->{key},$ctor,$s),'identifier','bind key expects token',$a->{at});
        type_expr($a->{value},$ctor,$s);return;
    }
    if ($op eq 'store') {
        require_type($s,type_expr($a->{value},$ctor,$s),'integer','store value expects integer',$a->{at});
        require_type($s,type_expr($a->{cell},$ctor,$s),'cell','store target expects cell',$a->{at});return;
    }
    error($s,'unknown action '.$op,$a->{at});
}
sub typecheck {
    my ($model,$s)=@_;
    my $sorts=$model->{sorts};my $ctors=$model->{constructors};my $rules=$model->{rules};
    error($s,'unknown start sort '.$model->{start}) unless exists $sorts->{$model->{start}};
    for my $name (sort keys %$ctors) {
        my $ctor=$ctors->{$name};
        for my $f (@{$ctor->{fields}}) {
            error($s,'unknown field sort '.$f->{type},$ctor->{at})
                unless $f->{type} eq 'integer' || $f->{type} eq 'identifier' || exists $sorts->{$f->{type}};
        }
        error($s,'missing rule for '.$ctor->{name},$ctor->{at}) unless exists $rules->{$ctor->{name}};
        type_action($rules->{$ctor->{name}},$ctor,$s);
    }
    my %productive;
    for(1..scalar(keys %$sorts)+1) {
        my $changed=0;
        for my $sn (sort keys %$sorts) {
            next if $productive{$sn};
            for my $cn (@{$sorts->{$sn}}) {
                my $ok=1;
                for my $f (@{$ctors->{$cn}{fields}}) {
                    if ($f->{type} ne 'integer' && $f->{type} ne 'identifier' && !$productive{$f->{type}}) {$ok=0;last}
                }
                if($ok){$productive{$sn}=1;$changed=1;last}
            }
        }
        last unless $changed;
    }
    for my $sn (sort keys %$sorts) {error($s,'uninhabited sort '.$sn) unless $productive{$sn}}
    return 1;
}
sub validate_model {
    my ($model)=@_;
    die "ACSEM_ERROR: invalid model\n" unless ref($model) eq 'HASH' && ($model->{version}//0)==1
       && ref($model->{constructors}) eq 'HASH' && ref($model->{rules}) eq 'HASH';
    return $model;
}
sub parse_term {
    my ($model,$source)=@_;
    my $s=lex($source,'term');$s->{terms}=0;$s->{symbols}={};
    my $root=term_node($model,$s,$model->{start});
    error($s,'trailing term input') unless kind($s) eq 'eof';
    return wantarray ? ($root, $s->{symbols}) : $root;
}
sub term_node {
    my ($m,$s,$sort)=@_;
    return nested($s,sub {
        limit('program syntax node limit') if ++$s->{terms}>2048;
        my $name=ident($s);
        my $c=$m->{constructors}{$name};
        error($s,'unknown constructor '.$name) unless $c;
        error($s,'constructor '.$name.' has wrong sort') unless $c->{sort} eq $sort;
        expect($s,'(');
        my @values;
        my $fs=$c->{fields};
        for my $i (0..$#$fs) {
            expect($s,',') if $i;
            my $t=$fs->[$i]{type};
            if($t eq 'integer') {
                my $neg=eat($s,'-');error($s,'integer field expected') unless kind($s) eq 'num';
                my $v=($neg?'-':'').take($s)->{v};error($s,'integer field outside 32-bit range') unless int32($v);
                push @values, {type=>'integer',value=>0+$v};
            } elsif($t eq 'identifier') {
                my $v=ident($s);
                $s->{symbols}{$v}=1;
                limit('runtime symbol limit') if keys(%{$s->{symbols}})>256;
                push @values, {type=>'identifier',value=>$v};
            } else {
                push @values,term_node($m,$s,$t);
            }
        }
        expect($s,')');
        return {ctor=>$name,values=>\@values};
    });
}
sub val { {type=>$_[0],value=>$_[1]} }
sub good {
    my ($out,$bind)=@_;$out||=[];$bind||={};
    limit('transient result capacity') if @$out>64;
    limit('binding result capacity') if keys(%$bind)>64;
    return {status=>'completed',out=>$out,bind=>$bind};
}
sub failed { {status=>'failed',out=>[],bind=>{}} }
sub escaped { {status=>'escaped',out=>$_[0]||[],bind=>{}} }
sub is_tag { ref($_[0]) eq 'HASH' && ($_[0]{type}//'') eq $_[1] }
sub bump {
    my ($state)=@_;
    limit('evaluation step budget exceeded') if ++$state->{steps}>$MAX_STEPS;
    limit('evaluation nesting exceeded') if $state->{depth}>$MAX_DEPTH;
}
sub run_expr {
    my ($e,$node,$inputs,$env,$state)=@_;
    local $state->{depth} = $state->{depth}+1;
    bump($state);
    my $op=$e->{op};
    return val('integer',$e->{value}) if $op eq 'integer';
    if($op eq 'token') {
        $state->{symbols}{$e->{value}}=1;
        limit('runtime symbol limit') if keys(%{$state->{symbols}})>256;
        return val('identifier',$e->{value});
    }
    if($op eq 'field') {
        my $f=$state->{model}{constructors}{$node->{ctor}}{fields};
        for(my $i=0;$i<@$f;$i++){return $node->{values}[$i] if $f->[$i]{name} eq $e->{name}}
        return undef;
    }
    if($op eq 'given') {return $inputs->[$e->{arg}{value}-1]}
    if($op eq 'bound') {
        my $key=run_expr($e->{arg},$node,$inputs,$env,$state);
        return undef unless is_tag($key,'identifier');
        return $env->{$key->{value}};
    }
    if($op eq 'contents') {
        my $cell=run_expr($e->{arg},$node,$inputs,$env,$state);
        return undef unless is_tag($cell,'cell') && $cell->{value} < @{$state->{cells}};
        return $state->{cells}[$cell->{value}];
    }
    if($op eq 'binary') {
        my $left=run_expr($e->{left},$node,$inputs,$env,$state);
        my $right=run_expr($e->{right},$node,$inputs,$env,$state);
        return undef unless is_tag($left,'integer') && is_tag($right,'integer');
        my ($a,$b)=($left->{value},$right->{value});my $v;
        if($e->{symbol} eq '+'){$v=$a+$b}
        elsif($e->{symbol} eq '-'){$v=$a-$b}
        elsif($e->{symbol} eq '*'){$v=$a*$b}
        elsif($e->{symbol} eq '/'){
            return undef unless $b && !($a==$MIN && $b == -1);
            $v=int($a/$b);
        } elsif($e->{symbol} eq '=='){$v=$a==$b?1:0}
        elsif($e->{symbol} eq '<'){$v=$a<$b?1:0}
        else {die "ACSEM_ERROR: unknown binary operator\n"}
        return undef if $v<$MIN || $v>$MAX;
        return val('integer',$v);
    }
    die "ACSEM_ERROR: invalid expression op\n";
}
sub join_bind {
    my ($a,$b)=@_;my %out=%$a;
    for my $key(keys %$b){return undef if exists $out{$key};$out{$key}=$b->{$key}}
    return \%out;
}
sub run_action {
    my ($a,$node,$inputs,$env,$state)=@_;
    local $state->{depth} = $state->{depth}+1;
    bump($state);
    my $op=$a->{op};
    return good() if $op eq 'complete';
    return failed() if $op eq 'fail';
    if($op eq 'give' || $op eq 'escape' || $op eq 'check') {
        my $v=run_expr($a->{arg},$node,$inputs,$env,$state);
        return failed() unless defined $v;
        return good([$v]) if $op eq 'give';
        return escaped([$v]) if $op eq 'escape';
        return (is_tag($v,'integer') && $v->{value})?good():failed();
    }
    if($op eq 'allocate') {
        limit('allocated cell limit') if @{$state->{cells}}>=1024;
        my $n=@{$state->{cells}};push @{$state->{cells}}, undef;
        return good([val('cell',$n)]);
    }
    if($op eq 'bind') {
        my $key=run_expr($a->{key},$node,$inputs,$env,$state);
        my $v=run_expr($a->{value},$node,$inputs,$env,$state);
        return failed() unless is_tag($key,'identifier') && defined $v;
        return good([],{ $key->{value}=>$v });
    }
    if($op eq 'store') {
        my $v=run_expr($a->{value},$node,$inputs,$env,$state);
        my $cell=run_expr($a->{cell},$node,$inputs,$env,$state);
        return failed() unless is_tag($v,'integer') && is_tag($cell,'cell') && $cell->{value}<@{$state->{cells}};
        $state->{cells}[$cell->{value}]=$v;
        return good();
    }
    if($op eq 'evaluate') {
        my $f=$state->{model}{constructors}{$node->{ctor}}{fields};
        for(my $i=0;$i<@$f;$i++) {
            return run_node($node->{values}[$i],$inputs,$env,$state) if $f->[$i]{name} eq $a->{field};
        }
        die "ACSEM_ERROR: field lookup failed\n";
    }
    if($op eq 'furthermore') {
        my $r=run_action($a->{body},$node,$inputs,$env,$state);
        return $r unless $r->{status} eq 'completed';
        return good($r->{out},{%$env,%{$r->{bind}}});
    }
    if($op eq 'combine') {
        my $comb=$a->{symbol};
        my $left=run_action($a->{left},$node,$inputs,$env,$state);
        if ($comb eq 'otherwise') {
            return $left unless $left->{status} eq 'failed';
            return run_action($a->{right},$node,$inputs,$env,$state);
        }
        if ($comb eq 'trapping') {
            return $left unless $left->{status} eq 'escaped';
            return run_action($a->{right},$node,$left->{out},$env,$state);
        }
        return $left unless $left->{status} eq 'completed';
        my $right=run_action($a->{right},$node,
            $comb eq 'then' ? $left->{out} : $inputs,
            $comb eq 'hence' ? $left->{bind} : $env, $state);
        return $right unless $right->{status} eq 'completed';
        my $bindings=$comb eq 'hence' ? $right->{bind} : join_bind($left->{bind},$right->{bind});
        return failed() unless defined $bindings;
        limit('binding result capacity') if keys(%$bindings)>64;
        my $outs=$comb eq 'then' ? $right->{out} : [@{$left->{out}},@{$right->{out}}];
        limit('transient result capacity') if @$outs>64;
        return good($outs,$bindings);
    }
    die "ACSEM_ERROR: invalid action op\n";
}
sub run_node {
    my ($node,$inputs,$env,$state)=@_;
    local $state->{depth}=$state->{depth}+1;
    bump($state);
    return run_action($state->{model}{rules}{$node->{ctor}},$node,$inputs,$env,$state);
}
sub run_term {
    my ($model,$source)=@_;
    validate_model($model);
    my ($root,$symbols)=parse_term($model,$source);
    my $state={model=>$model,cells=>[],symbols=>$symbols,steps=>0,depth=>0};
    return run_node($root,[],{},$state);
}
sub result_text {
    my ($r)=@_;
    my $s=$r->{status};
    for my $v(@{$r->{out}}){
        $s.=' '.($v->{type} eq 'integer' ? $v->{value} : ($v->{type} eq 'identifier'?'token:':'cell:').$v->{value});
    }
    return "$s\n";
}
sub canonical_json { JSON::PP->new->canonical->utf8->encode($_[0]) }
sub read_bounded {
    my ($fh)=@_;my $buf='';
    while (1){my $n=read($fh,my $chunk,4096);die "ACSEM_ERROR: input read failed: $!\n" unless defined $n;
        last unless $n;$buf.=$chunk;limit('input exceeds 64 KiB') if length($buf)>$MAX_SOURCE;
    }
    return $buf;
}
sub slurp_file {
    my ($path)=@_;open my $fh,'<:raw',$path or die "ACSEM_ERROR: cannot open $path: $!\n";
    my $s=read_bounded($fh);close $fh;return $s;
}
sub write_file {
    my ($path,$body)=@_;
    if (defined $path && $path ne '-') {
        open my $fh,'>:raw',$path or die "ACSEM_ERROR: cannot write $path: $!\n";
        print {$fh} $body or die "ACSEM_ERROR: cannot write $path: $!\n";
        close $fh or die "ACSEM_ERROR: cannot close $path: $!\n";
    } else {print STDOUT $body or die "ACSEM_ERROR: stdout write failed\n"}
}
sub synthesize_perl {
    my ($model,$origin)=@_;
    # Reuse the exact implementation so synthesized artifacts need no source files.
    # Only a validated JSON model is embedded, never executable user snippets.
    open my $fh,'<:raw',$origin or die "ACSEM_ERROR: cannot read own source: $!\n";
    local $/;my $program=<$fh>;close $fh;
    die "ACSEM_ERROR: cannot synthesize nested generated script\n" if $program =~ /^__DATA__\s*$/m;
    my $json=canonical_json($model);
    return $program."\n__DATA__\n".$json."\n";
}
sub synthesize {
    my ($model,$target,$output,$class)=@_;
    return canonical_json($model)."\n" if $target eq 'json';
    return synthesize_perl($model,__FILE__) if $target eq 'perl';
    require File::Basename;
    require File::Spec;
    my $module=File::Spec->rel2abs(File::Spec->catfile(File::Basename::dirname(__FILE__),'Acsem','Emit.pm'));
    require $module;
    return Acsem::Emit::synthesize($model,$target,$output,$class);
}
sub usage {
    return <<'HELP';
acsem.pl --check SPEC
acsem.pl --run SPEC [--term PROGRAM | --term-file PATH]
acsem.pl --emit perl|c|cpp|pascal|python|rust|java|json SPEC [-o FILE]
         [--class-name NAME (Java only; defaults to the .java filename)]
standalone generated.pl [--term PROGRAM | --term-file PATH]
Exit: completed=0, failed/escaped=1, errors=2.
HELP
}
sub cli {
    my @args=@_;
    my ($check,$run,$emit,$output,$term,$term_file,$help,$version,$class);
    Getopt::Long::Configure('no_auto_abbrev','no_ignore_case','permute');
    my $ok=Getopt::Long::GetOptionsFromArray(\@args,
        'check'=>\$check,'run'=>\$run,'emit=s'=>\$emit,'o|output=s'=>\$output,
       'term=s'=>\$term,'term-file=s'=>\$term_file,'help|h'=>\$help,'version'=>\$version,
       'class-name=s'=>\$class);
    die "ACSEM_ERROR: invalid options\n" unless $ok;
    if($help){print usage();return 0}
    if($version){print "acsem.pl $VERSION\n";return 0}
    die "ACSEM_ERROR: choose one of --check, --run, --emit\n" if ($check?1:0)+($run?1:0)+(defined($emit)?1:0)>1;
    die "ACSEM_ERROR: use only one of --term and --term-file\n" if defined $term && defined $term_file;
    die "ACSEM_ERROR: --term/--term-file require execution mode\n"
        if ($check || defined($emit)) && (defined($term) || defined($term_file));
    die "ACSEM_ERROR: --output is not supported with --check\n" if $check && defined($output);
    if (defined $emit) {
        $emit='cpp' if $emit eq 'c++';
        die "ACSEM_ERROR: target must be perl, c, cpp, pascal, python, rust, java or json\n"
            unless $emit =~ /\A(?:perl|c|cpp|pascal|python|rust|java|json)\z/;
    }
    die "ACSEM_ERROR: --class-name requires --emit java\n" if defined($class) && ($emit//'') ne 'java';
    my $embedded=defined fileno(*main::DATA);
    my $model;
    if($embedded) {
        die "ACSEM_ERROR: generated interpreter does not accept spec paths or --emit/--check\n"
            if @args || defined($emit) || $check;
        local $/;my $text=<main::DATA>;
        $model=validate_model(JSON::PP->new->utf8->decode($text));
        $run=1;
    } else {
        die "ACSEM_ERROR: expected exactly one specification filename\n" unless @args==1;
        $model=parse_spec(slurp_file($args[0]));
    }
    if($check){print "OK: ".$model->{name}."\n";return 0}
    if(defined $emit) {
        my $body=synthesize($model,$emit,$output,$class);
        write_file($output,$body);return 0;
    }
    if(defined $term_file){$term=slurp_file($term_file)}
    if(!defined $term){binmode STDIN,':raw';$term=read_bounded(*STDIN)}
    my $result=run_term($model,$term);
    write_file($output,result_text($result));
    return $result->{status} eq 'completed'?0:1;
}

package main;
unless(caller){
    my $code=eval{Acsem::cli(@ARGV)};
    if($@){my $msg=$@;$msg=~s/ at \S+ line \d+.*//s if $msg!~/^ACSEM_/;
        print STDERR $msg=~/\n\z/?$msg:"$msg\n";exit 2}
    exit $code;
}
1;
