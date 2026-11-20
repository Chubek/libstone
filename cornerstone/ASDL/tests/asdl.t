use 5.014;
use strict;
use warnings;
use Cwd qw(abs_path);
use File::Path qw(make_path);
use File::Spec;
use File::Temp qw(tempdir tempfile);
use FindBin qw($Bin);
use IPC::Open3 qw(open3);
use JSON::PP qw(decode_json);
use Test::More;
use Text::ParseWords qw(shellwords);

my $generator = "$Bin/../scripts/asdl.pl";
my $fixture = "$Bin/fixtures/calculator.asdl";
my $workspace = tempdir('asdl-test-XXXXXXXX', TMPDIR => 1, CLEANUP => 1);

sub read_handle {
    my ($handle) = @_;
    local $/;
    return <$handle> // '';
}

sub read_file {
    my ($path) = @_;
    open my $file, '<:raw', $path or die "open $path: $!";
    return read_handle($file);
}

sub write_file {
    my ($path, $text) = @_;
    open my $file, '>:raw', $path or die "open $path: $!";
    print {$file} $text or die "write $path: $!";
    close $file or die "close $path: $!";
}

# Files rather than pipes avoid deadlocks when emitted C and diagnostics grow.
sub run {
    my ($input, @command) = @_;
    my ($stdin) = tempfile(DIR => $workspace);
    my ($stdout) = tempfile(DIR => $workspace);
    my ($stderr) = tempfile(DIR => $workspace);
    print {$stdin} $input or die "write stdin: $!";
    seek $stdin, 0, 0 or die "rewind stdin: $!";
    my $pid = open3(['&', $stdin], ['&', $stdout], ['&', $stderr], @command);
    waitpid($pid, 0) == $pid or die "wait for $command[0]: $!";
    my $status = $?;
    seek $_, 0, 0 or die "rewind process output: $!" for $stdout, $stderr;
    return ($status, read_handle($stdout), read_handle($stderr));
}

sub asdl {
    return run('', $^X, $generator, @_);
}

subtest 'CLI and schema' => sub {
    my ($status, $stdout, $stderr) = asdl('--check', $fixture);
    is($status, 0, 'check accepts standalone fixture') or diag $stderr;
    is($stdout, '', 'check writes no C');
    is($stderr, '', 'check writes no diagnostics for valid syntax');

    ($status, $stdout, $stderr) = asdl('--check', '--schema=-', $fixture);
    is($status, 0, 'schema export succeeds') or do { diag $stderr; return };
    my $schema = decode_json($stdout);
    is($schema->{format_version}, 1, 'schema interface version');
    is($schema->{module}, 'Calculator', 'module name');
    is($schema->{prefix}, 'calc_', 'preamble overrides module default prefix');
    my ($doc, $expr, $op) = @{$schema->{types}};
    is($doc->{builder}, 'calc_make_document', 'product C builder name');
    is($doc->{fields}[0]{qualifier}, '+', 'nonempty repetition preserved');
    is($expr->{attributes}[0]{qualifier}, '?', 'optional sum attribute preserved');
    is_deeply($expr->{constructors}[0]{location},
        { file => $fixture, line => 11, column => 12 }, 'source location includes preamble');
    is_deeply([map { [$_->{c_tag}, $_->{tag}] } @{$op->{constructors}}],
        [['calc_Plus', 1], ['calc_Minus', 2]], 'enum C names and ABI tags');
    my ($repeat_status, $repeat) = asdl('--check', '--schema=-', $fixture);
    is($repeat_status, 0, 'repeated schema export succeeds');
    is($repeat, $stdout, 'schema is deterministic');

    ($status, $stdout, $stderr) = run("%prefix input\n%option enums builtins\n%%\ntoken = Left | Right\n", $^X, $generator,
        '--no-enums', '--no-builtins', '--prefix=cli', '-');
    is($status, 0, 'stdin to stdout succeeds') or diag $stderr;
    like($stdout, qr/\bcli_token_t cli_Left\(/, 'CLI overrides enable pointer-backed sum');
    unlike($stdout, qr/ASDL_RUNTIME_COMMON_H/, 'runtime omitted');
};

subtest 'errors and output preservation' => sub {
    my $header = "$workspace/protected.h";
    my $schema = "$workspace/not-written.json";
    write_file($header, "keep existing header\n");
    for my $case (
        ['syntax-error.asdl', qr/2:18: expected a lowercase field type/],
        ['unknown-type.asdl', qr/4:18: undefined type 'missing'/],
        ['recursive-product.asdl', qr/2:17: recursive product type: item -> branch -> item/],
    ) {
        my ($file, $message) = @$case;
        my ($status, $stdout, $stderr) =
            asdl('--header', $header, '--schema', $schema, "$Bin/fixtures/$file");
        is($status, 1 << 8, "$file rejected with exit 1");
        like($stderr, qr/\Q$file\E:$message/, "$file diagnostic includes location and reason");
        is($stdout, '', "$file writes no stdout");
        is(read_file($header), "keep existing header\n", "$file preserves existing header");
        ok(!-e $schema, "$file does not publish schema");
    }
    my ($status, $stdout, $stderr) = asdl('--header', $header, '--schema', $header, $fixture);
    is($status, 1 << 8, 'duplicate output paths rejected');
    like($stderr, qr/aliases output/, 'duplicate output path diagnostic');
    is($stdout, '', 'no stdout on duplicate output path');
    is(read_file($header), "keep existing header\n", 'duplicate output preserves header');

    ($status, $stdout, $stderr) = asdl('--header', $header,
        '--schema', "$workspace/absent/schema.json", $fixture);
    is($status, 1 << 8, 'missing output directory rejected');
    like($stderr, qr/output directory does not exist/, 'missing directory diagnostic');
    is(read_file($header), "keep existing header\n", 'another failing output preserves header');
};

subtest 'repository consumer schemas (when present)' => sub {
    my $root = abs_path("$Bin/../../..");
    for my $case (
        ['yellowstone/ld/ldscript.asdl', 'qldexpr_', 'expr'],
        ['limestone/traceml/TraceSource.asdl', 'traceml_ast_', 'document'],
    ) {
        my ($path, $prefix, $first_type) = @$case;
        SKIP: {
            skip "$path not in standalone checkout", 3 unless defined $root && -f "$root/$path";
            my ($status, $stdout, $stderr) = asdl('--check', '--schema=-', "$root/$path");
            is($status, 0, "$path accepted") or diag $stderr;
            my $schema = $status ? {} : decode_json($stdout);
            is($schema->{prefix}, $prefix, "$path preserves C namespace");
            is($schema->{types}[0]{name}, $first_type, "$path preserves declared type order");
        }
    }
};

my @cc = shellwords($ENV{CC} || 'cc');
my @cflags = shellwords($ENV{CFLAGS} // '');
my $have_cc = @cc && (File::Spec->file_name_is_absolute($cc[0]) || $cc[0] =~ m{/}
    ? -f $cc[0] && -x $cc[0]
    : grep { my $path = File::Spec->catfile($_, $cc[0]); -f $path && -x $path }
        File::Spec->path());
BAIL_OUT("CC is set but no compiler was found: $ENV{CC}")
    if !$have_cc && $ENV{CC};

for my $mode ('header-only', 'split') {
    subtest "$mode C11 generation and runtime" => sub {
        my $dir = "$workspace/$mode";
        make_path("$dir/include", "$dir/src", "$dir/runtime");
        my @source = $mode eq 'split' ? ("$dir/src/calc.c") : ();
        my ($status, $stdout, $stderr) = asdl(
            '--header', "$dir/include/calc.h",
            '--visitor', "$dir/include/walk.h",
            '--listener', "$dir/include/events.h",
            '--builtins-header', "$dir/runtime/asdl.h",
            (@source ? ('--source', @source) : ()), $fixture,
        );
        is($status, 0, 'generator succeeds') or do { diag $stderr; return };
        is($stdout, '', 'named outputs do not use stdout');
        is($stderr, '', 'generation emits no diagnostics');
        ok(-s $_, "generated $_") for map { "$dir/$_" }
            qw(include/calc.h include/walk.h include/events.h runtime/asdl.h);
        ok(-s $_, "generated $_") for @source;

        SKIP: {
            skip 'C11 compiler not found (set CC to select one)', 2 unless $have_cc;
            my $exe = "$dir/run-calc";
            my $input = "$dir/events.log";
            write_file($input, "one\ntwo");
            ($status, $stdout, $stderr) = run('', @cc, @cflags,
                qw(-std=c11 -Wall -Wextra -Werror -pedantic -UNDEBUG),
                "-I$dir/include", "$Bin/runtime.c", "$Bin/runtime_other.c", @source, '-o', $exe);
            is($status, 0, 'strict C11 compilation and multi-unit linking') or diag $stdout, $stderr;
            skip 'generated C did not compile', 1 if $status;
            ($status, $stdout, $stderr) = run('', $exe, $input);
            is($status, 0, 'generated C runtime assertions pass') or diag $stdout, $stderr;
        }
    };
}

done_testing;
