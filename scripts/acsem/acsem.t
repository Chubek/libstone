#!/usr/bin/env perl
=pod

=head1 NAME

acsem.t - Typechecking, synthesis, and execution conformance tests for acsem.pl

=head1 SYNOPSIS

  perl acsem.t
  prove -v acsem.t

=head1 DESCRIPTION

This is the companion Perl C<.t> typecheck test file for C<acsem.pl>.
It exercises the static typechecker, sort productivity and rule coverage,
Mosses/Watt-style action combinator semantics, storage, failure, escape,
resource bounds, and standalone Perl interpreter synthesis. C<backends.t>
additionally compiles and executes all available target languages. The authoritative
checker is contained in C<acsem.pl>; this file tests it directly through its
public Perl functions in package C<Acsem>.

All tests use C<Test::More>, C<File::Temp>, and other Perl core modules.
No CPAN dependency is required. The semantic examples exercise the functional,
declarative, and imperative facets described in the Action Semantics literature.

=head1 FOCUS OF THE TYPECHECKER

The checker distinguishes C<integer>, C<identifier>, C<cell>,
C<dynamic>, and abstract syntax sorts. Syntax-valued constructor fields
must be evaluated, not yielded. Integer arithmetic/check predicates are
typed; identifier keys and cell operations are checked; dynamic transients
and environment values are verified at runtime. Action typechecking checks
syntax and operand compatibility, not termination or memory safety.

=head1 REFERENCES

Peter D. Mosses, I<Action Semantics>, Cambridge University Press (1992).
L<https://doi.org/10.1017/CBO9780511569869>.

=cut

use strict;
use warnings;
no warnings 'once';
use 5.014;
use FindBin ();
use Test::More;
use File::Temp qw(tempdir);
use File::Spec;

require "$FindBin::Bin/acsem.pl";

sub parse { return Acsem::parse_spec($_[0]) }
sub render {
    my ($model,$term)=@_;
    my $r=Acsem::run_term($model,$term);
    return Acsem::result_text($r);
}
sub rejects {
    my ($source,$pattern,$label)=@_;
    my $result=eval{parse($source);1};
    ok(!$result,$label);
    like($@,qr/$pattern/,'  rejects with '.$pattern) unless $result;
}
sub rejects_term {
    my ($model,$term,$pattern,$label)=@_;
    my $result=eval{render($model,$term);1};
    ok(!$result,$label);
    like($@,qr/$pattern/,'  runtime parser reports '.$pattern) unless $result;
}
sub spec_rule {
    my ($rule)=@_;
    return 'language Test { start E; syntax E = N(n: integer) | V(v: identifier); rule N = '
      .$rule.'; rule V = give bound(v); }';
}
my $calc = <<'CALC';
language Calculator {
    start Expr;
    syntax Expr = Num(value: integer) | Add(left: Expr, right: Expr);
    rule Num = give value;
    rule Add = (evaluate(left) and then evaluate(right)) then give given(1) + given(2);
}
CALC
my $model=parse($calc);
is($model->{name},'Calculator','language name parsed');
is($model->{start},'Expr','start sort parsed');
is(scalar(keys %{$model->{constructors}}),2,'constructors enumerated');
is(render($model,'Add(Num(20),Num(22))'),"completed 42\n",'evaluate nested expression');
is(render($model,'Num(-2147483648)'),"completed -2147483648\n",'minimum signed integer');
is(render($model,'Num(2147483647)'),"completed 2147483647\n",'maximum signed integer');
is(render($model,"Add(Num(9),Num(-3))"),"completed 6\n",'negative literal');

my @invalid = (
 [spec_rule('give missing'),qr/unknown field/,'unknown field'],
 [spec_rule('evaluate(n)'),qr/syntax field/,'cannot evaluate scalar'],
 [spec_rule('give 2147483648'),qr/32-bit/,'positive integer overflow'],
 [spec_rule('give -2147483649'),qr/32-bit/,'negative integer overflow'],
 [spec_rule('give given(0)'),qr/1\.\.64/,'zero transient index'],
 [spec_rule('give given(65)'),qr/1\.\.64/,'out-of-range transient index'],
 [spec_rule('give bound(1)'),qr/type mismatch/,'bound integer key'],
 [spec_rule('give contents(0)'),qr/type mismatch/,'contents integer pointer'],
 [spec_rule('bind n to 1'),qr/type mismatch/,'bind key integer'],
 [spec_rule('give "x" + 1'),qr/type mismatch/,'identifier arithmetic'],
 [spec_rule('check "x"'),qr/type mismatch/,'check token'],
 [spec_rule('store "x" in given(1)'),qr/type mismatch/,'store string value'],
 [spec_rule('store 1 in 0'),qr/type mismatch/,'store integer pointer'],
 [spec_rule('give n junk'),qr/expected|syntax/,'trailing rule tokens'],
 [spec_rule('give "x\\"; malicious"'),qr/syntax/,'escaped quotes rejected'],
 [spec_rule('unfold'),qr/expected action/,'unsupported unfold'],
 [spec_rule('complete and complete'),qr/expected 'then'/,'and needs then'],
 [spec_rule('give 1 < 2 < 3'),qr/expected|syntax/,'chained comparison rejected'],
 ["language L { start E; syntax E = N(); }",qr/rule/,'rules required'],
 ["language L { start Missing; syntax E = N(); rule N = complete; }",qr/start sort/,'bad start sort'],
 ["language L { start E; syntax E = N(); rule X = complete; }",qr/unknown constructor/,'unknown rule constructor'],
 ["language L { start E; syntax E = N() | M(); rule N = complete; }",qr/missing rule/,'rule coverage'],
 ["language L { start E; syntax E = N(); rule N = complete; rule N = fail; }",qr/duplicate rule/,'duplicate rule'],
 ["language L { start E; syntax E = N(); syntax E = M(); rule N = complete; }",qr/duplicate sort/,'duplicate sort'],
 ["language L { start E; syntax E = N() | N(); rule N = complete; }",qr/duplicate constructor/,'duplicate ctor'],
 ["language L { start E; syntax E = N(x: integer, x: integer); rule N = complete; }",qr/duplicate field/,'duplicate field'],
 ["language L { start E; syntax E = N(x: Missing); rule N = complete; }",qr/unknown field sort/,'unknown field sort'],
 ["language L { start E; syntax E = N(x: E); rule N = evaluate(x); }",qr/uninhabited/,'unproductive recursive sort'],
 ["language L { start E; syntax E = N() | M(x: E); rule N = complete; rule M = give x; }",qr/syntax field/,'syntax-valued yield is illegal'],
 ["language L { start E; syntax E = N(); rule N = give 1;",qr/expected/,'missing closing brace'],
 ["language L { start E; syntax E = N(); rule N = give 1; } malicious",qr/trailing/,'trailing spec input'],
);
for my $test(@invalid){rejects(@$test)}
rejects(' ' x 65537,qr/64 KiB/,'spec file source limit');
rejects("language L { start E; \0 syntax E = N(); rule N = complete; }",qr/NUL/,'NUL source rejected');

my $expression= <<'EXPR';
// Expression-and-storage examples of the imperative and declarative facets.
language Expressions {
  start Expr;
  syntax Expr = Num(value: integer)
    | Add(left: Expr, right: Expr)
    | Sub(left: Expr, right: Expr)
    | Mul(left: Expr, right: Expr)
    | Div(left: Expr, right: Expr)
    | Var(name: identifier)
    | Let(name: identifier, value: Expr, body: Expr)
    | Seq(first: Expr, second: Expr)
    | Pair(left: Expr, right: Expr)
    | Raise(value: Expr)
    | Catch(body: Expr, handler: Expr)
    | Given()
    | Require(value: Expr)
    | Fallback(first: Expr, second: Expr)
    | New()
    | Get(cell: Expr)
    | Set(cell: Expr, value: Expr)
    | Skip();
  rule Num = give value;
  rule Add = (evaluate(left) and then evaluate(right)) then give given(1) + given(2);
  rule Sub = (evaluate(left) and then evaluate(right)) then give given(1) - given(2);
  rule Mul = (evaluate(left) and then evaluate(right)) then give given(1) * given(2);
  rule Div = (evaluate(left) and then evaluate(right)) then give given(1) / given(2);
  rule Var = give bound(name);
  rule Let = furthermore (evaluate(value) then bind name to given(1)) hence evaluate(body);
  rule Seq = evaluate(first) then evaluate(second);
  rule Pair = evaluate(left) and then evaluate(right);
  rule Raise = evaluate(value) then escape given(1);
  rule Catch = evaluate(body) trapping evaluate(handler);
  rule Given = give given(1);
  rule Require = evaluate(value) then check given(1);
  rule Fallback = evaluate(first) otherwise evaluate(second);
  rule New = allocate cell;
  rule Get = evaluate(cell) then give contents(given(1));
  rule Set = (evaluate(cell) and then evaluate(value)) then store given(2) in given(1);
  rule Skip = complete;
}
EXPR
my $e=parse($expression);
my @executions = (
 ['Let(x, New(), Seq(Set(Var(x), Num(40)), Add(Get(Var(x)), Num(2))))',"completed 42\n",'lexically scoped storage example'],
 ['Add(Num(20),Num(22))',"completed 42\n",'addition'],
 ['Sub(Num(40),Num(2))',"completed 38\n",'subtraction'],
 ['Mul(Num(6),Num(7))',"completed 42\n",'multiplication'],
 ['Div(Num(-7),Num(2))',"completed -3\n",'division truncates toward zero'],
 ['Div(Num(1),Num(0))',"failed\n",'division by zero'],
 ['Add(Num(2147483647),Num(1))',"failed\n",'addition overflow'],
 ['Sub(Num(-2147483648),Num(1))',"failed\n",'subtraction overflow'],
 ['Mul(Num(2147483647),Num(2147483647))',"failed\n",'multiplication overflow'],
 ['Div(Num(-2147483648),Num(-1))',"failed\n",'division overflow'],
 ['Let(x,Num(8),Add(Var(x),Num(2)))',"completed 10\n",'lexical binding'],
 ['Let(x,Num(4),Let(x,Num(5),Var(x)))',"completed 5\n",'lexical shadowing'],
 ['Let(x,Num(1),Let(y,Num(2),Add(Var(x),Var(y))))',"completed 3\n",'outer environment preserved'],
 ['Var(x)',"failed\n",'unbound identifier'],
 ['Given()',"failed\n",'missing transient'],
 ['Catch(Raise(Num(7)),Given())',"completed 7\n",'escape handler receives transient'],
 ['Catch(Num(1),Num(9))',"completed 1\n",'escape handler skipped'],
 ['Fallback(Require(Num(0)),Num(9))',"completed 9\n",'failure fallback'],
 ['Fallback(Raise(Num(5)),Num(9))',"escaped 5\n",'otherwise does not catch escape'],
 ['Pair(Num(1),Num(2))',"completed 1 2\n",'and then joins transients'],
 ['Seq(Num(1),Num(2))',"completed 2\n",'then replaces transients'],
 ['Get(New())',"failed\n",'uninitialized cell'],
 ['Pair(New(),New())',"completed cell:0 cell:1\n",'fresh distinct cells'],
 ['Set(New(),Num(4))',"completed\n",'storage write'],
 ['Let(c,New(),Seq(Set(Var(c),Num(6)),Get(Var(c))))',"completed 6\n",'storage update then read'],
 ['Skip()',"completed\n",'empty completed action'],
);
for my $case(@executions){is(render($e,$case->[0]),$case->[1],$case->[2])}

my $bindings=parse(<<'BINDINGS');
language Bindings {
  start E;
  syntax E = Collision() | Scope() | NotInherited() | SameKey();
  rule Collision = bind "x" to 1 and then bind "x" to 2;
  rule Scope = bind "x" to 3 hence give bound("x");
  rule NotInherited = bind "x" to 3 then give bound("x");
  rule SameKey = bind "x" to 1 and then bind "x" to 1;
}
BINDINGS
is(render($bindings,'Collision()'),"failed\n",'duplicate binding conflicts');
is(render($bindings,'SameKey()'),"failed\n",'equal duplicate binding still conflicts');
is(render($bindings,'Scope()'),"completed 3\n",'hence passes environment');
is(render($bindings,'NotInherited()'),"failed\n",'then does not implicitly pass environment');

my @badterms=(
 ['Add(Num(2))',qr/expected ','/,'missing ctor argument'],
 ['Add(Num(2), Num(3)',qr/expected '\)'/,'missing close paren'],
 ['Num(2147483648)',qr/32-bit/,'term integer overflow'],
 ['Unknown()',qr/unknown constructor/,'unknown ctor'],
 ['Num()',qr/integer field/,'missing integer'],
 ['Add(Num(1),Num(2)) gibberish',qr/trailing/,'trailing program text'],
 ['Num(2) // comment',qr/trailing|invalid/,'term comments are not in constructor dialect'],
 ['Num(1,2)',qr/expected '\)'/,'extra field'],
 ['Num("bad")',qr/invalid|integer field/,'string term disallowed'],
 ['Add(Num(1),Var(x))',qr/unknown constructor/,'constructor not declared in model'],
);
for my $case(@badterms){rejects_term($model,@$case)}

{
    local $Acsem::MAX_STEPS=5;
    my $ok=eval{render($model,'Add(Num(1),Num(2))');1};
    ok(!$ok && $@=~/step budget/,'evaluation step limit is enforced');
}
{
    local $Acsem::MAX_DEPTH=2;
    my $ok=eval{render($model,'Add(Num(1),Num(2))');1};
    ok(!$ok && $@=~/depth|nesting/,'parse or evaluation depth enforced');
}


# Large-result boundaries are resource errors, not ordinary action failure.
{
    my $large_action = join(' and then ', ('give 1') x 65);
    my $large = parse("language Large { start E; syntax E = N(); rule N = $large_action; }");
    my $ok = eval {render($large,'N()');1};
    ok(!$ok && $@ =~ /transient result capacity/,'65 transients exceed result capacity');
}
{
    my $large_action = join(' and then ',map {qq(bind "k$_" to $_)} 1..65);
    my $large = parse("language Large { start E; syntax E = N(); rule N = $large_action; }");
    my $ok = eval {render($large,'N()');1};
    ok(!$ok && $@ =~ /binding result capacity/,'65 bindings exceed result capacity');
}
my $persistent=parse(<<'PERSISTENT');
language Persistent {
  start E;
  syntax E = Failure() | Escape();
  rule Failure = (allocate cell then bind "c" to given(1)) hence
    ((store 7 in bound("c") then fail) otherwise give contents(bound("c")));
  rule Escape = (allocate cell then bind "c" to given(1)) hence
    ((store 9 in bound("c") then escape 5) trapping give contents(bound("c")));
}
PERSISTENT
is(render($persistent,'Failure()'),"completed 7\n",'store persists after failed branch');
is(render($persistent,'Escape()'),"completed 9\n",'store persists after escape');
is(render($e,'Let(x, New(), Add(Var(x), Num(1)))'),"failed\n",'dynamic cell-to-integer mismatch fails');
my $mutual=parse(<<'MUTUAL');
language MutuallyRecursive {
  start E;
  syntax E = Stop() | More(next: F);
  syntax F = Go(next: E);
  rule Stop = give 0;
  rule More = evaluate(next);
  rule Go = evaluate(next);
}
MUTUAL
is(render($mutual,'More(Go(Stop()))'),"completed 0\n",'productive mutually recursive sorts');

my $tmp=tempdir(CLEANUP=>1);
my $spec_path=File::Spec->catfile($tmp,'calc.acsem');
my $out_path=File::Spec->catfile($tmp,'calc.pl');
Acsem::write_file($spec_path,$calc);
my $command=$^X.' '.quoted(File::Spec->catfile($FindBin::Bin,'acsem.pl')).' --emit perl '.quoted($spec_path).' -o '.quoted($out_path);
is(system($command),0,'CLI synthesizes stand-alone Perl');
ok(-s $out_path,'generated output is nonempty');
my $generated=Acsem::slurp_file($out_path);
like($generated,qr/^__DATA__$/m,'generated program contains serialized model');
my $runtime_output=`$^X @{[quoted($out_path)]} --term 'Add(Num(10),Num(32))'`;
is($? >> 8,0,'generated program completes successfully');
is($runtime_output,"completed 42\n",'generated program runs without original spec');
my $json_path=File::Spec->catfile($tmp,'calc.json');
is(system($^X,File::Spec->catfile($FindBin::Bin,'acsem.pl'),'--emit','json',$spec_path,'-o',$json_path),0,'CLI JSON synthesis');
my $decoded=JSON::PP->new->decode(Acsem::slurp_file($json_path));
is($decoded->{name},'Calculator','synthesized JSON model can be decoded');
is(Acsem::canonical_json(parse($calc)),Acsem::canonical_json(parse($calc)),'canonical model deterministic');

sub quoted {
    my ($x)=@_;
    $x =~ s/'/'"'"'/g;
    return "'$x'";
}

done_testing();
