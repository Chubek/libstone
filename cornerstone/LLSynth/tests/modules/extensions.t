use strict;
use warnings;
use Cwd qw(abs_path);
use File::Spec;
use File::Temp qw(tempdir);
use FindBin qw($Bin);
use Test::More ();

# Exercise the linker independently: these declarations are parsed by the other
# feature workers' frontends, not the baseline compiler in this worktree.
require "$Bin/../../scripts/LLSynthCompiler.pm";
require "$Bin/../../scripts/LLSynthModules.pm";
require "$Bin/../../scripts/LLSynthActionSemantics.pm";

sub canonical_path { return abs_path($_[0]) }
sub slurp {
    open my $in, '<:raw', $_[0] or die "$_[0]: $!";
    local $/;
    return <$in>;
}
sub fail { die "$_[1]:$_[2]: error: $_[0]\n" }

my $tmp = tempdir(CLEANUP => 1);
sub source {
    my ($name, $text) = @_;
    my $path = File::Spec->catfile($tmp, $name);
    open my $out, '>:raw', $path or die "$path: $!";
    print {$out} $text;
    close $out or die "$path: $!";
    return $path;
}

source('lib.llsynth', <<'SPEC');
.module Lib
.export cfun, parse_native, double, invoke, fetch
.extern c cfun(x: Int) -> Int from "/path/with cfun and invoke/libcfun.so" symbol "cfun"
.sandstone parse_native bank "parse_native.bank" syntax "parse_native.banksyn" cli "/path/parse_native"
.template double(x) = x + cfun(x);
.macro invoke(x)
return double(cfun(x));
.end
.async function fetch(x: Int) -> Int
return &cfun(x);
.end
SPEC
my $main = source('main.llsynth', <<'SPEC');
.import "lib.llsynth" as L
.template apply(x) = L__double(x);
.macro run(x)
return L__invoke(x);
.end
.rule Start
'syntax: EOF
'semantics: return L__double(&L__cfun(1));
.end
SPEC
my %deps;
my @lines = read_module_spec_lines($main, \%deps);
my $text = join "\n", map { $_->[0] } @lines;
Test::More::like($text, qr/^\.extern c (LLSMod_\w+_cfun)\(x: Int\) -> Int from "\/path\/with cfun and invoke\/libcfun\.so" symbol "cfun"$/m,
    'C local name is isolated without changing the library path or ABI symbol');
my ($cfun) = $text =~ /^\.extern c (\w+)/m;
Test::More::like($text, qr/^\.sandstone LLSMod_\w+_parse_native bank "parse_native\.bank" syntax "parse_native\.banksyn" cli "\/path\/parse_native"$/m,
    'Sandstone local name is isolated but bank, syntax, and executable paths are opaque');
Test::More::like($text, qr/^\.template (LLSMod_\w+_double)\(x\) = x \+ \Q$cfun\E\(x\);$/m,
    'single-line template expression binds the private foreign function');
my ($double) = $text =~ /^\.template (\w+_double)/m;
Test::More::like($text, qr/^\.macro LLSMod_\w+_invoke\(x\)\nreturn \Q$double\E\(\Q$cfun\E\(x\)\);\n\.end$/m,
    'macro body binds module-local template and foreign calls');
Test::More::like($text, qr/^\.async function LLSMod_\w+_fetch\(x: Int\) -> Int\nreturn &\Q$cfun\E\(x\);$/m,
    'async function name and foreign function reference are isolated');
Test::More::like($text, qr/^\.template LLSMod_\w+_apply\(x\) = \Q$double\E\(x\);$/m,
    'root template binds an imported template');
Test::More::like($text, qr/^'semantics: return \Q$double\E\(&\Q$cfun\E\(1\)\);$/m,
    'imported metaprogram and foreign references bind in ordinary rule facets');
Test::More::is(scalar(keys %deps), 2, 'only specification source files are loaded and fingerprinted');

Test::More::done_testing();
