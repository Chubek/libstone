#!/usr/bin/env perl
=pod

=head1 NAME

backends.t - cross-target execution, resource and CLI conformance tests

=head1 SYNOPSIS

  prove acsem/*.t
  ACSEM_REQUIRE_TARGETS=1 prove acsem/*.t

=head1 DESCRIPTION

Compiles standalone C99, C++11, Free Pascal, Rust 2021 and Java 8-compatible
interpreters and executes them alongside generated Python and Perl. All targets
run the same independently specified examples, malformed programs, arithmetic
boundaries, environment/scoping cases, and resource-limit cases. Generated
artifacts execute in a temporary directory without their specification file.

Missing toolchains are reported as skips. Set C<ACSEM_REQUIRE_TARGETS=1> to
make missing toolchains fail the suite. Executable paths can be supplied in
C<CC>, C<CXX>, C<FPC>, C<PYTHON>, C<RUSTC>, C<JAVAC>, and C<JAVA>.
Tests use only core Perl modules, perform no network access, and place compiler
outputs in temporary directories. The tests do not install toolchains.

=head1 REFERENCES

Peter D. Mosses, I<Action Semantics>, Cambridge University Press, 1992.
L<https://doi.org/10.1017/CBO9780511569869>.

=cut
use strict;
use warnings;
use 5.014;
use FindBin;
use Test::More;
use File::Temp qw(tempdir tempfile);
use File::Spec;
use IPC::Open3;
use Cwd qw(getcwd);
require "$FindBin::Bin/acsem.pl";

my $tmp = tempdir(CLEANUP => 1);
my $generator = File::Spec->catfile($FindBin::Bin, 'acsem.pl');
sub invoke {
    my ($input, @command) = @_;
    my ($in) = tempfile(DIR=>$tmp, UNLINK=>1);
    my ($out) = tempfile(DIR=>$tmp, UNLINK=>1);
    my ($err) = tempfile(DIR=>$tmp, UNLINK=>1);
    binmode $_ for ($in, $out, $err);
    print {$in} $input; seek($in, 0, 0) or die $!;
    my $pid = open3('<&'.fileno($in), '>&'.fileno($out), '>&'.fileno($err), @command);
    waitpid($pid, 0);
    my $status = $? & 127 ? 128 + ($? & 127) : $? >> 8;
    seek($_, 0, 0) or die $! for ($out, $err);
    local $/;
    return ($status, scalar(<$out>) // '', scalar(<$err>) // '');
}
sub tool {
    my ($env, $default) = @_;
    my $name = $ENV{$env} // $default;
    return File::Spec->rel2abs($name) if -f $name && -x $name;
    for my $dir (File::Spec->path) {
        my $path = File::Spec->catfile($dir, $name);
        return File::Spec->rel2abs($path) if -f $path && -x $path;
    }
    return;
}
sub tree {
    my ($ctor, @terms) = @_;
    return $terms[0] if @terms == 1;
    my @left = splice(@terms, 0, int(@terms/2));
    return $ctor.'('.tree($ctor,@left).','.tree($ctor,@terms).')';
}
sub doubles { return ('Double(' x $_[0]).$_[1].(')' x $_[0]) }
sub commands {
    my ($name,$tools,$file,$binary,$dir,$class) = @_;
    return ([],[$tools->[0],$file]) if $name eq 'perl' || $name eq 'python';
    return ([$tools->[0],$name eq 'c' ? '-std=c99' : '-std=c++11',
             '-Wall','-Wextra','-Werror','-pedantic',$file,'-o',$binary],[$binary])
        if $name eq 'c' || $name eq 'cpp';
    return ([$tools->[0],'-Cr','-Co',"-o$binary",$file],[$binary]) if $name eq 'pascal';
    return ([$tools->[0],'--edition=2021','-D','warnings',$file,'-o',$binary],[$binary]) if $name eq 'rust';
    return ([$tools->[0],'-source','8','-target','8','-Xlint:all,-options','-Werror','-cp',$dir,'-d',$dir,$file],
            [$tools->[1],'-cp',$dir,$class]);
}

my $source = Acsem::slurp_file("$FindBin::Bin/examples/expressions.acsem");
my %extra = (
    Literal => 'give "hello"',
    Compare => '(give -3 < 0) and then (give 2 == 2) and then (give 2 == 3)',
    Precedence => 'give 2 + 3 * 4 - 8 / 2',
    Grouping => 'give (2 + 3) * 4',
    Collision => 'bind "x" to 1 and then bind "x" to 2',
    SameKey => 'bind "x" to 1 and then bind "x" to 1',
    Scope => 'bind "x" to 3 hence give bound("x")',
    NotInherited => 'bind "x" to 3 then give bound("x")',
    ReplaceEnv => 'bind "x" to 1 hence (bind "y" to 2 hence give bound("x"))',
    Overlay => 'bind "x" to 1 hence (furthermore bind "y" to 2 hence (give bound("x") and then give bound("y")))',
    ThenInputs => 'give 7 then (give given(1) and then give given(1) + 1)',
    AndInputs => 'give 3 then ((give 9 then give given(1)) and then give given(1))',
    HenceInputs => 'give 7 then ((give 3 and then bind "x" to 5) hence (give given(1) and then give bound("x")))',
    MissingGiven => 'give given(64)',
    BadBound => 'give 1 then give bound(given(1))',
    BadContents => 'give 1 then give contents(given(1))',
    BadStore => 'give 1 then store 7 in given(1)',
    BadBind => 'give 1 then bind given(1) to 2',
    DirectFail => 'fail',
    FailureStore => '(allocate cell then bind "c" to given(1)) hence ((store 7 in bound("c") then fail) otherwise give contents(bound("c")))',
    EscapeStore => '(allocate cell then bind "c" to given(1)) hence ((store 9 in bound("c") then escape 5) trapping give contents(bound("c")))',
    FullOut => join(' and then ', ('give 1') x 64),
    TooManyOut => join(' and then ', ('give 1') x 65),
    FullBind => join(' and then ', map { qq(bind "k$_" to $_) } 1..64),
    TooManyBind => join(' and then ', map { qq(bind "k$_" to $_) } 1..65),
    OverlayFull => '('.join(' and then ', map { qq(bind "k$_" to $_) } 1..64).') hence furthermore bind "k1" to 9',
    OverlayOverflow => '('.join(' and then ', map { qq(bind "k$_" to $_) } 1..64).') hence furthermore bind "new" to 9',
);
my $constructors = join(' | ', map { "$_()" } sort keys %extra);
$source =~ s/\| Skip\(\);/| Skip() | $constructors | Double(child: Expr) | Via(child: Other);
  syntax Other = Leaf(value: integer);/;
my $rules = join("\n", map { "  rule $_ = $extra{$_};" } sort keys %extra);
$source =~ s/\}\s*\z/$rules
  rule Double = evaluate(child) then evaluate(child);
  rule Via = evaluate(child);
  rule Leaf = give value;
}/;
my $model = Acsem::parse_spec($source);
# Exactly 4096 semantic nodes, with a table larger than a JVM string constant.
# Target-language keywords remain constructor data, including the Java class
# name String, which must not shadow java.lang.String inside generated code.
my @large_names = (qw(int class match begin def null), map { "N$_" } 6..63);
my $large_source = 'language Large { start E; syntax E = '.join(' | ',map { "$_()" } @large_names).'; '.
    join(' ', map { 'rule '.$_.' = give '.join(' + ',('0') x 32).';' } @large_names).'}';
my $large_model = Acsem::parse_spec($large_source);
my @cases = (
    ['Add(Num(20),Num(22))', "completed 42\n"],
    ['Sub(Num(40),Num(2))', "completed 38\n"],
    ['Mul(Num(6),Num(7))', "completed 42\n"],
    ['Div(Num(-7),Num(2))', "completed -3\n"],
    ['Div(Num(7),Num(-2))', "completed -3\n"],
    ['Div(Num(-7),Num(-2))', "completed 3\n"],
    ['Num(-2147483648)', "completed -2147483648\n"],
    ['Num(2147483647)', "completed 2147483647\n"],
    ['Num(-00000000)', "completed 0\n"],
    ['Num('.('0' x 62).'42)', "completed 42\n", '64-digit zero-padded integer'],
    ['Num(-'.('0' x 54).'2147483648)', "completed -2147483648\n", 'zero-padded minimum'],
    ['Div(Num(1),Num(0))', "failed\n"],
    ['Add(Num(2147483647),Num(1))', "failed\n"],
    ['Sub(Num(-2147483648),Num(1))', "failed\n"],
    ['Mul(Num(2147483647),Num(2147483647))', "failed\n"],
    ['Div(Num(-2147483648),Num(-1))', "failed\n"],
    ['Let(x,Num(4),Let(x,Num(5),Var(x)))', "completed 5\n"],
    ['Let(x,Num(1),Let(y,Num(2),Add(Var(x),Var(y))))', "completed 3\n"],
    ['Let(x,New(),Seq(Set(Var(x),Num(40)),Add(Get(Var(x)),Num(2))))', "completed 42\n"],
    ['Let(x,New(),Add(Var(x),Num(1)))', "failed\n"],
    ['Let(x,Token(y),Var(x))', "completed token:y\n"],
    ['Var(missing)', "failed\n"],
    ['Given()', "failed\n"],
    ['Catch(Raise(Num(7)),Given())', "completed 7\n"],
    ['Catch(Num(1),Num(9))', "completed 1\n"],
    ['Catch(DirectFail(),Num(9))', "failed\n"],
    ['Fallback(Require(Num(0)),Num(9))', "completed 9\n"],
    ['Fallback(Raise(Num(5)),Num(9))', "escaped 5\n"],
    ['Catch(Raise(Token(problem)),Given())', "completed token:problem\n"],
    ['Pair(Num(1),Raise(Num(2)))', "escaped 2\n"],
    ['Pair(Num(1),DirectFail())', "failed\n"],
    ['Fallback(Num(1),New())', "completed 1\n"],
    ['Pair(Num(1),Num(2))', "completed 1 2\n"],
    ['Seq(Num(1),Num(2))', "completed 2\n"],
    ['Get(New())', "failed\n"],
    ['Pair(New(),New())', "completed cell:0 cell:1\n"],
    ['Set(New(),Num(4))', "completed\n"],
    ['Set(New(),Token(x))', "failed\n"],
    ['Require(Token(x))', "failed\n"],
    ['Require(Num(-1))', "completed\n"],
    ['Skip()', "completed\n"],
    ['Literal()', "completed token:hello\n"],
    ['Compare()', "completed 1 1 0\n"],
    ['Precedence()', "completed 10\n"],
    ['Grouping()', "completed 20\n"],
    ['Scope()', "completed 3\n"],
    ['Overlay()', "completed 1 2\n"],
    ['ThenInputs()', "completed 7 8\n"],
    ['AndInputs()', "completed 9 3\n"],
    ['HenceInputs()', "completed 3 7 5\n"],
    ['FailureStore()', "completed 7\n"],
    ['EscapeStore()', "completed 9\n"],
    ['FullOut()', 'completed'.(' 1' x 64)."\n"],
    ['FullBind()', "completed\n"],
    ['OverlayFull()', "completed\n"],
    ['Via(Leaf(42))', "completed 42\n"],
    ['Token('.('x' x 64).')', 'completed token:'.('x' x 64)."\n"],
    [tree('Seq',map {"Token(s$_)"} 1..256), "completed token:s256\n", '256 symbols'],
    [tree('Seq',('Skip()') x 1024), "completed\n", '2047 syntax nodes'],
    [doubles(10,'New()'), "completed cell:1023\n", '1024 allocations'],
    [(' ' x (65536 - 6)).'Skip()', "completed\n", '64 KiB source'],
);
push @cases, ["$_()", "failed\n"] for qw(Collision SameKey NotInherited ReplaceEnv MissingGiven BadBound BadContents BadStore BadBind DirectFail);
my @errors = (
    ['', 'ACSEM_ERROR', 'empty term'],
    ['Unknown()', 'ACSEM_ERROR', 'unknown constructor'],
    ['Num()', 'ACSEM_ERROR', 'missing scalar'],
    ['Num(1,2)', 'ACSEM_ERROR', 'extra argument'],
    ['Add(Num(1))', 'ACSEM_ERROR', 'missing syntax argument'],
    ['Add(Num(1),Num(2)', 'ACSEM_ERROR', 'missing parenthesis'],
    ['Num(2147483648)', 'ACSEM_ERROR', 'positive range'],
    ['Num(-2147483649)', 'ACSEM_ERROR', 'negative range'],
    ['Num('.('9' x 64).')', 'ACSEM_ERROR', 'long numeric range'],
    ['Num('.('9' x 65).')', 'ACSEM_LIMIT', 'numeric token length'],
    ['Num("1")', 'ACSEM_ERROR', 'quoted integer'],
    ['Token("x")', 'ACSEM_ERROR', 'quoted term identifier'],
    ['Skip() trailing', 'ACSEM_ERROR', 'trailing input'],
    ["Skip()\0", 'ACSEM_ERROR', 'NUL source'],
    ['Skip() // comment', 'ACSEM_ERROR', 'term comments rejected'],
    ['Via(Num(1))', 'ACSEM_ERROR', 'wrong child sort'],
    ['Leaf(1)', 'ACSEM_ERROR', 'wrong root sort'],
    ['Token('.('x' x 65).')', 'ACSEM_LIMIT', 'identifier length'],
    [' ' x 65537, 'ACSEM_LIMIT', 'source size'],
    [',' x 20001, 'ACSEM_LIMIT', 'token count'],
    [('Double(' x 128).'Skip()'.(')' x 128), 'ACSEM_LIMIT', 'parse depth'],
    [('Double(' x 64).'Skip()'.(')' x 64), 'ACSEM_LIMIT', 'evaluation nesting'],
    [tree('Seq', ('Skip()') x 1025), 'ACSEM_LIMIT', 'syntax node count'],
    [tree('Seq',map {"Token(s$_)"} 1..257), 'ACSEM_LIMIT', 'parsed symbol count'],
    [tree('Seq',(map {"Token(s$_)"} 1..256),'Literal()'), 'ACSEM_LIMIT', 'evaluated symbol count'],
    ['Seq('.doubles(10,'New()').',New())', 'ACSEM_LIMIT', 'allocation count'],
    ['TooManyOut()', 'ACSEM_LIMIT', 'transient capacity'],
    ['TooManyBind()', 'ACSEM_LIMIT', 'binding capacity'],
    ['OverlayOverflow()', 'ACSEM_LIMIT', 'furthermore binding capacity'],
    ['Fallback(TooManyOut(),Num(9))', 'ACSEM_LIMIT', 'otherwise cannot catch limits'],
    ['Catch(TooManyOut(),Num(9))', 'ACSEM_LIMIT', 'trapping cannot catch limits'],
    [doubles(16,'Skip()'), 'ACSEM_LIMIT', 'evaluation step count'],
);

subtest 'reference semantics and bounds' => sub {
    for my $case (@cases) {
        is(Acsem::result_text(Acsem::run_term($model,$case->[0])), $case->[1], $case->[2] // $case->[0]);
    }
    for my $case (@errors) {
        my $ok = eval { Acsem::run_term($model,$case->[0]); 1 };
        ok(!$ok && $@ =~ /^\Q$case->[1]\E:/, $case->[2]) or diag($@);
    }
};

my @targets = (
    ['perl','pl',[$^X]], ['c','c',[tool('CC','cc')]], ['cpp','cpp',[tool('CXX','c++')]],
    ['pascal','pas',[tool('FPC','fpc')]], ['python','py',[tool('PYTHON','python3')]],
    ['rust','rs',[tool('RUSTC','rustc')]], ['java','java',[tool('JAVAC','javac'),tool('JAVA','java')]],
);
for my $target (@targets) {
    my ($name,$extension,$tools) = @$target;
    subtest "$name standalone conformance" => sub {
        if (grep {!defined($_)} @$tools) {
            if ($ENV{ACSEM_REQUIRE_TARGETS}) { fail("$name toolchain required"); return }
            plan skip_all => "$name toolchain unavailable";
        }
        my $dir = File::Spec->catdir($tmp,$name);
        mkdir $dir or die $!;
        my $spec = File::Spec->catfile($dir,'input.acsem');
        my $file = File::Spec->catfile($dir,"AcsemProgram.$extension");
        my $binary = File::Spec->catfile($dir,'program');
        Acsem::write_file($spec,$source);
        my ($status,$stdout,$stderr) = invoke('', $^X,$generator,'--emit',$name,$spec,'-o',$file);
        is($status,0,'synthesis succeeds') or diag($stderr);
        is($stdout.$stderr,'','synthesis is quiet');
        return unless $status == 0;
        my $body = do { open my $fh,'<:raw',$file or die $!; local $/; <$fh> };
        is($body,Acsem::synthesize($model,$name,$file,undef),'deterministic source');
        unlink $spec or die $!;
        my ($compile,$run) = commands($name,$tools,$file,$binary,$dir,'AcsemProgram');
        my @run = @$run;
        if (@$compile) {
            ($status,$stdout,$stderr) = invoke('',@$compile);
            is($status,0,'compiles with strict checks') or diag($stdout.$stderr);
            return unless $status == 0;
        }
        my $cwd = getcwd(); chdir $dir or die $!;
        for my $case (@cases) {
            ($status,$stdout,$stderr) = invoke($case->[0],@run);
            my $expected_status = $case->[1] =~ /^completed/ ? 0 : 1;
            is_deeply([$status,$stdout,$stderr],[$expected_status,$case->[1],''], $case->[2] // $case->[0]);
        }
        for my $case (@errors) {
            ($status,$stdout,$stderr) = invoke($case->[0],@run);
            ok($status == 2 && $stdout eq '' && $stderr =~ /^\Q$case->[1]\E:/, $case->[2])
                or diag("status=$status stdout=$stdout stderr=$stderr");
        }
        for my $args (['--term','Num(42)'],['--term=Num(42)'],['--run','--term','Num(42)']) {
            is_deeply([invoke('',@run,@$args)],[0,"completed 42\n",''],'inline term option');
        }
        my $term_file = File::Spec->catfile($dir,'program term.txt');
        my $result_file = File::Spec->catfile($dir,'program result.txt');
        Acsem::write_file($term_file,'Num(42)');
        is_deeply([invoke('',@run,'--term-file',$term_file,'--output',$result_file)],[0,'',''],'term file and output file');
        is(Acsem::slurp_file($result_file),"completed 42\n",'output file contents');
        is_deeply([invoke('',@run,'--term','Num(42)','-o','-')],[0,"completed 42\n",''],'stdout output path');
        for my $args (['--term','Skip()','--term-file',$term_file], ['--unknown'], ['--term'], ['--check'],
                      ['--term-file',"$dir/missing"], ['--term','Skip()','-o',$dir]) {
            ($status,$stdout,$stderr) = invoke('',@run,@$args);
            ok($status == 2 && $stdout eq '' && length($stderr),'invalid CLI or I/O returns 2') or diag($stderr);
        }
        ($status,$stdout,$stderr) = invoke("Num(\n)",@run);
        like($stderr,qr/at 2:1/,'source line and column');
        for my $flag ('--help','--version') {
            ($status,$stdout,$stderr) = invoke('',@run,$flag);
            ok($status == 0 && length($stdout) && $stderr eq '',"$flag supported");
        }
        chdir $cwd or die $!;
        my $large_file = File::Spec->catfile($dir,"String.$extension");
        Acsem::write_file($large_file,Acsem::synthesize($large_model,$name,$large_file,undef));
        ($compile,$run) = commands($name,$tools,$large_file,"$binary-large",$dir,'String');
        if (@$compile) {
            ($status,$stdout,$stderr) = invoke('',@$compile);
            is($status,0,'maximum-node model compiles') or diag($stdout.$stderr);
            return unless $status == 0;
        }
        for my $term ('int()','class()','N63()') {
            is_deeply([invoke($term,@$run)],[0,"completed 0\n",''],'maximum-node model: '.$term);
        }
        if ($name eq 'java') {
            my $java_file = File::Spec->catfile($dir,'java.java');
            my $small_model = Acsem::parse_spec('language L { start E; syntax E = N(); rule N = give 42; }');
            Acsem::write_file($java_file,Acsem::synthesize($small_model,'java',$java_file,undef));
            ($compile,$run) = commands($name,$tools,$java_file,$binary,$dir,'java');
            ($status,$stdout,$stderr) = invoke('',@$compile);
            is($status,0,'class named java compiles') or diag($stdout.$stderr);
            is_deeply([invoke('N()',@$run)],[0,"completed 42\n",''],'class named java executes') if $status == 0;
        }
    };
}

subtest 'generator CLI validation' => sub {
    my $spec = "$FindBin::Bin/examples/calculator.acsem";
    for my $args (['--emit','unknown'],['--emit','java','--class-name','class'],
                  ['--emit','java','--class-name','Other','-o',"$tmp/Main.java"],
                  ['--emit','c','--class-name','Main'],['--check','--term','Num(1)'],
                  ['--emit','python','--term-file','x'],['--check','-o','x'],
                  ['--check','--run'],['--ch'],['--CHECK']) {
        my ($status,$stdout,$stderr) = invoke('',$^X,$generator,@$args,$spec);
        ok($status == 2 && $stdout eq '' && length($stderr),'invalid generator options rejected') or diag($stderr);
    }
    my ($status,$stdout,$stderr) = invoke('',$^X,$generator,'--emit','c++',$spec);
    ok($status == 0 && length($stdout) && $stderr eq '','c++ alias');
    ($status,$stdout,$stderr) = invoke('',$^X,$generator,'--emit','java','--class-name','Example',$spec);
    ok($status == 0 && $stdout =~ /public final class Example/ && $stderr eq '','explicit Java class');
};

done_testing();
