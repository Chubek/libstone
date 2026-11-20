use strict;
use warnings;
use File::Spec;
use File::Temp qw(tempdir);
use FindBin qw($Bin);
use IPC::Open3 qw(open3);
use JSON::PP qw(decode_json);
use Symbol qw(gensym);
use Test::More;

my $cli = File::Spec->catfile($Bin, '..', '..', 'scripts', 'llsynth.pl');
my $tmp = tempdir(CLEANUP => 1);
my $include = File::Spec->catdir($tmp, 'includes');
mkdir $include or die "mkdir $include: $!";

sub write_spec {
    my ($dir, $name, $text) = @_;
    my $path = File::Spec->catfile($dir, $name);
    open my $out, '>:raw', $path or die "open $path: $!";
    print {$out} $text or die "write $path: $!";
    close $out or die "close $path: $!";
    return $path;
}

sub check_spec {
    my ($path, @options) = @_;
    my $err = gensym;
    my $pid = open3(undef, my $out, $err, $^X, $cli, '--check', @options, '--', $path);
    my ($stdout, $stderr);
    {
        local $/;
        $stdout = <$out> // '';
        $stderr = <$err> // '';
    }
    waitpid($pid, 0);
    return ($? >> 8, $stdout, $stderr);
}

my $source = <<'SPEC';
.language "Frontend"
.start Start
.option parser = lalr1
.include "pair.llsynth"

.rule Start
'syntax: Pair Pair EOF
'semantics: return 0;
'lower: return nil;
..
SPEC
my $root = write_spec($tmp, 'main.llsynth', $source);
write_spec($tmp, 'pair.llsynth', <<'SPEC');
.include "tokens.llsynth"
.rule Pair
'syntax: prefix:"c"* value:END
..
SPEC
write_spec($include, 'pair.llsynth', ".should_not_be_read\n");
write_spec($include, 'tokens.llsynth', qq{.literal END = "d"\r\n.skip /[ \\t]+/\r\n});

subtest 'local and nested includes, EBNF, and LR table compilation' => sub {
    my @includes = ('--include-path', $include);
    my ($status, $grammar, $err) = check_spec($root, @includes, '--dump-grammar');
    is($status, 0, 'nested include falls back to the include path') or diag $err;
    like($err, qr/^OK: Frontend /, 'default parser accepts the specification');
    like($grammar, qr/^0: (__lls_\d+) -> \n1: \1 -> \1 LIT_\d+$/m,
        'zero-or-more is expanded into empty and recursive productions');
    like($grammar, qr/^\d+: Start -> Pair Pair EOF$/m, 'start rule retains its EOF');

    my %state_count;
    for my $algorithm (qw(lalr1 lr1)) {
        my ($code, $dump, $diag) = check_spec($root, @includes, "--parser=$algorithm", '--dump-tables');
        is($code, 0, "$algorithm table generation") or diag $diag;
        my $tables = eval { decode_json($dump) };
        ok($tables, "$algorithm dump is JSON") or next;
        $state_count{$algorithm} = @{$tables->{actions}};
        is(scalar @{$tables->{gotos}}, $state_count{$algorithm}, "$algorithm has one goto row per state");
        is_deeply($tables->{actions}[0]{END}, ['r', 0], "$algorithm reduces the empty repetition on END");
        is_deeply($tables->{productions}[2]{labels}, [qw(prefix value)], "$algorithm retains capture labels");
        is(scalar grep({ (($_->{'$'} || [])->[0] || '') eq 'a' } @{$tables->{actions}}),
            1, "$algorithm has one accept action");
    }
    if (defined $state_count{lr1} && defined $state_count{lalr1}) {
        cmp_ok($state_count{lr1}, '>', $state_count{lalr1}, 'LALR merges compatible LR(1) states');
    }
};

subtest 'include diagnostics' => sub {
    my ($status, undef, $err) = check_spec($root, '--diagnostics=json');
    is($status, 1, 'missing nested include is rejected');
    my $diag = eval { decode_json($err) };
    ok($diag, 'missing include returns JSON diagnostic') or return diag $err;
    is($diag->{location}{file}, File::Spec->catfile($tmp, 'pair.llsynth'), 'diagnostic uses containing file');
    is($diag->{location}{line}, 1, 'diagnostic uses include directive line');
    like($diag->{message}, qr/include 'tokens\.llsynth' not found/, 'diagnostic names missing include');

    my $cycle = write_spec($tmp, 'cycle.llsynth', ".include \"./cycle.llsynth\"\n");
    ($status, undef, $err) = check_spec($cycle);
    is($status, 1, 'cycle through a path alias is rejected');
    like($err, qr/include cycle involving /, 'cycle diagnostic explains the failure');
};

subtest 'grammar and action validation' => sub {
    my @includes = ('--include-path', $include);
    my @cases = (
        ['missing-eof', 'Pair Pair EOF', 'Pair Pair', qr/every start-rule derivation must end in EOF/],
        ['unknown-symbol', 'Pair Pair EOF', 'Pair UNKNOWN EOF', qr/undefined grammar symbol 'UNKNOWN'/],
        ['capture', 'return 0;', 'return $unbound;', qr/unbound capture '\$unbound'/],
        ['type', 'return 0;', 'let x: Int = "wrong"; return x;', qr/type mismatch in local initializer/],
        ['wide-int', 'return 0;', 'return 9223372036854775808;', qr/Int literal out of signed 64-bit range/],
        ['missing-facet', "'lower: return nil;", '', qr/start rule requires 'lower facet for mode both/],
    );
    for my $case (@cases) {
        my ($name, $from, $to, $msg) = @$case;
        (my $text = $source) =~ s/\Q$from\E/$to/ or die "test pattern not found: $from";
        my $path = write_spec($tmp, "$name.llsynth", $text);
        my ($status, undef, $err) = check_spec($path, @includes);
        is($status, 1, "$name is rejected");
        like($err, $msg, "$name has the expected diagnostic");
    }

    my $unreachable = write_spec($tmp, 'unreachable.llsynth', $source . <<'SPEC');
.rule Unused
'syntax: END
..
SPEC
    my ($ok, undef, $warning) = check_spec($unreachable, @includes);
    is($ok, 0, 'unreachable production normally only warns');
    like($warning, qr/warning W001: unreachable rule Unused/, 'unreachable production emits W001');
    my ($bad, undef, $error) = check_spec($unreachable, @includes, '--warnings-as-errors');
    is($bad, 1, 'warnings-as-errors rejects the unreachable production');
    like($error, qr/error: unreachable rule Unused/, 'unreachable production is reported as an error');
};

done_testing;
