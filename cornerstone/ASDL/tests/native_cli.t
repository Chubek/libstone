#!/usr/bin/env perl
use strict;
use warnings;
use Cwd qw(abs_path);
use File::Temp qw(tempdir);
use FindBin qw($Bin);
use IPC::Open3;
use JSON::PP;
use Test::More;

my $native = $ENV{ASDLC} // shift @ARGV;
plan skip_all => 'pass asdlc path or set ASDLC' unless defined $native && -x $native;
$native = abs_path($native);
my $fixture = "$Bin/fixtures/calculator.asdl";
my $dir = tempdir(CLEANUP => 1);

sub run {
    my ($command, $input, @args) = @_;
    # Capture output in files, not pipes, so even large generated headers
    # can't deadlock the test runner.
    open my $stdout, '>', "$dir/stdout" or die $!;
    open my $stderr, '>', "$dir/stderr" or die $!;
    my $pid = open3(my $stdin, '>&' . fileno($stdout), '>&' . fileno($stderr),
                    @$command, @args);
    print {$stdin} $input if defined $input;
    close $stdin;
    waitpid($pid, 0);
    my $status = $?;
    close $stdout;
    close $stderr;
    return ($status, slurp("$dir/stdout"), slurp("$dir/stderr"));
}

sub slurp {
    open my $file, '<', $_[0] or die "$_[0]: $!";
    local $/;
    return <$file> // '';
}

sub put {
    open my $file, '>', $_[0] or die "$_[0]: $!";
    print {$file} $_[1];
    close $file or die "$_[0]: $!";
}

my ($status, $stdout, $stderr) = run([$native], undef, '--check', '--schema=-', $fixture);
is($status, 0, 'native validates the legacy calculator fixture') or diag $stderr;
my ($oracle_status, $oracle, $oracle_error) =
    run([$^X, "$Bin/../scripts/asdl.pl"], undef, '--check', '--schema=-', $fixture);
is($oracle_status, 0, 'Perl oracle validates calculator fixture') or diag $oracle_error;
is_deeply(decode_json($stdout), decode_json($oracle), 'legacy JSON metadata matches')
    if !$status && !$oracle_status;

($status, $stdout, $stderr) = run([$native],
    "module Demo { flag = Off | On }\n",
    '--check', '--schema=-', '--prefix=Changed', '--no-enums', '-');
is($status, 0, 'stdin and option overrides') or diag $stderr;
if (!$status) {
    my $schema = decode_json($stdout);
    is($schema->{prefix}, 'Changed_', 'prefix override used');
    is($schema->{types}[0]{representation}, 'pointer', 'enum override used');
}

my $header = "$dir/generated.h";
my $hardlink = "$dir/input-link.asdl";
my $input = "$dir/input.asdl";
put($input, "expr = Token(int x)\n");
put($header, "keep me\n");

($status, $stdout, $stderr) =
    run([$native], undef, '--header', $header, '--schema', "$dir/missing/schema.json", $input);
ok($status, 'nonexistent destination directory rejected');
like($stderr, qr/output directory does not exist/, 'missing-directory diagnostic');
is(slurp($header), "keep me\n", 'failed preflight preserves previous output');

($status, $stdout, $stderr) = run([$native], undef, '--header', $header, '--schema', $header, $input);
ok($status, 'duplicate destinations rejected');
like($stderr, qr/aliases output/, 'duplicate diagnostic');
is(slurp($header), "keep me\n", 'duplicate preflight preserves previous output');

SKIP: {
    skip "cannot make hard link: $!", 3 unless link $input, $hardlink;
    ($status, $stdout, $stderr) = run([$native], undef, '--header', $hardlink, $input);
    ok($status, 'input hardlink rejected');
    like($stderr, qr/aliases input/, 'hardlink diagnostic');
    is(slurp($input), "expr = Token(int x)\n", 'input unchanged');
}

($status, $stdout, $stderr) = run([$native], undef, '--check', '--visitor', "$dir/walk.h", $input);
is($status, 0, '--check does not render visitor or require an AST header path') or diag $stderr;
ok(!-e "$dir/walk.h", 'visitor not published during check');

chmod 0600, $header or die $!;
($status, $stdout, $stderr) = run([$native], undef,
    '--header', $header, '--source', "$dir/generated.c", $input);
is($status, 0, 'split output wrote both artifacts') or diag $stderr;
is((stat($header))[2] & 0777, 0600, 'replacement preserves existing permissions');
ok(-s "$dir/generated.c", 'split implementation is nonempty');

done_testing();
