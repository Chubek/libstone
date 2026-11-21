package LLSynthCompanions;

use strict;
use warnings;
use File::Spec ();
use File::Basename qw(dirname);
use Digest::SHA qw(sha256_hex);
use Encode qw(decode FB_CROAK);
use File::Temp ();
use Config ();

# Providers run at generation time. Their declarations pass through precisely
# the same linking, type checks and backend compilation as handwritten ones.
sub declarations {
    my ($flavor, $name, $origin, $ln, $deps, $loaded, $blocks) = @_;
    my $path = File::Spec->rel2abs($name, dirname(File::Spec->rel2abs($origin)));
    -f $path or main::fail("extern companion '$name' not found", $origin, $ln);
    $path = main::canonical_path($path);
    $flavor ||= $path =~ /\.(?:pm|pl)$/i ? 'perl' : 'c';
    if (exists $loaded->{$path}) {
        $loaded->{$path} eq $flavor or main::fail("conflicting companion formats for '$name'", $origin, $ln);
        return ();
    }
    $loaded->{$path} = $flavor;
    $deps->{$path} = sha256_hex(main::slurp($path));

    my $text = eval {
        my $result;
        if ($flavor eq 'perl') {
            local ($@, $!);
            $result = do $path;
            defined $result or die $@ || $! || 'provider returned undef';
            if (ref($result) eq 'HASH') {
                # Alternative to returning declarations: provide named block
                # expanders. Their scope is the declaring unit and its direct
                # importers, never the generator process.
                for my $key (sort keys %$result) {
                    $key =~ /^[A-Za-z_]\w*$/ && ref($result->{$key}) eq 'CODE'
                        or die 'expected a block name and code reference';
                    exists $blocks->{$key} and die "duplicate block provider for .$key";
                    $blocks->{$key} = {path => $path, expand => $result->{$key}};
                }
                $result = '';
            } elsif (ref($result) eq 'ARRAY') {
                grep { !defined($_) || ref($_) } @$result and die 'expected declaration lines';
                $result = join "\n", @$result;
            }
        } else {
            eval { require FFI::Platypus; 1 }
                or die "C companions require Perl FFI::Platypus: $@";
            my ($dir, $library);
            if ($path =~ /\.c$/i) {
                require ExtUtils::CBuilder;
                $dir = File::Temp->newdir;
                my $builder = ExtUtils::CBuilder->new(quiet => 1);
                my $obj = $builder->compile(source => $path,
                    object_file => File::Spec->catfile("$dir", "provider$Config::Config{obj_ext}"));
                $library = $builder->link(objects => $obj, module_name => 'LLSynthProvider',
                    lib_file => File::Spec->catfile("$dir", "provider.$Config::Config{dlext}"));
            }
            my $ffi = FFI::Platypus->new(api => 2, lib => $library // $path);
            $result = $ffi->function('llsynth_declarations' => [] => 'string')->call;
        }
        defined($result) && !ref($result) or die 'expected LLSynth declaration text';
        $result = decode('UTF-8', $result, FB_CROAK) unless utf8::is_utf8($result);
        index($result, "\0") < 0 or die 'NUL in companion declarations';
        $result;
    };
    my $error = $@;
    main::fail("extern companion '$name': $error", $origin, $ln) if length $error;
    return declaration_lines($text, $path, 1, 1);
}

sub declaration_lines {
    my ($text, $path, $line, $numbered) = @_;
    $text =~ s/\r\n?/\n/g;
    my @lines = split /\n/, $text;
    my @entries;
    for my $i (0 .. $#lines) {
        $lines[$i] =~ /^\s*\.(?:(?:module|library|include|import)\b|extern\s+(?:perl\s+|c\s+)?")/
            and main::fail('companion must provide declarations; place imports and companions in the specification',
                           $path, $line + ($numbered ? $i : 0));
        push @entries, [$lines[$i], $path, $line + ($numbered ? $i : 0)];
    }
    return @entries;
}

sub bind_blocks {
    my ($scope, $provided, $file, $line) = @_;
    for my $key (sort keys %$provided) {
        my $old = $scope->{$key};
        $old && $old->{path} ne $provided->{$key}{path}
            and main::fail("ambiguous block provider for .$key", $file, $line);
        $scope->{$key} = $provided->{$key};
    }
}

sub expand_block {
    my ($provider, $lines, $index, $deps) = @_;
    my ($head, $file, $line) = @{$lines->[$$index]};
    my @body;
    while (++$$index < @$lines && main::strip_ws($lines->[$$index][0]) ne '.end') {
        push @body, $lines->[$$index];
    }
    $$index < @$lines or main::fail("unterminated block: $head (expected .end)", $file, $line);
    my $read = sub {
        my ($path) = @_;
        -f $path or main::fail("block input '$path' not found", $file, $line);
        my $bytes = main::slurp($path);
        $deps->{main::canonical_path($path)} = sha256_hex($bytes);
        return $bytes;
    };
    my $text = eval { $provider->{expand}->(main::strip_ws($head), \@body, $file, $line, $read) };
    if (my $error = $@) {
        die $error if $error =~ /^.*?:\d+: error: /;
        main::fail($error, $file, $line);
    }
    defined($text) && !ref($text) or main::fail('expected block expansion text', $file, $line);
    return declaration_lines($text, $file, $line, 0);
}

1;
