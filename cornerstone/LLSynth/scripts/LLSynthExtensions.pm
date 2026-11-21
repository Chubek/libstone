package LLSynthExtensions;
use strict;
use warnings;
use File::Spec ();
use File::Basename qw(dirname);
use File::Path qw(make_path);
use Encode qw(encode);

# A module is loaded only when selected. Never search @INC or cwd by name.
sub discover {
    my (@dirs) = @_;
    my %modules;
    for my $dir (@dirs) {
        opendir my $dh, $dir or die "extension directory '$dir': $!\n";
        for my $file (sort readdir $dh) {
            next unless $file =~ /\A([A-Za-z][A-Za-z0-9_]*)\.pm\z/;
            my $name = $1;
            my $path = File::Spec->rel2abs(File::Spec->catfile($dir, $file));
            next unless -f $path;
            exists $modules{$name} and die "duplicate extension '$name' in $dir\n";
            $modules{$name} = $path;
        }
        closedir $dh;
    }
    return \%modules;
}

# Return source-level terminal and nonterminal nodes (including separators).
sub symbols {
    my @work = @_;
    my @symbols;
    while (my $n = pop @work) {
        if ($n->{k} eq 'sym') {
            push @symbols, $n;
        } elsif (ref $n->{v} eq 'ARRAY') {
            push @work, @{$n->{v}};
        } else {
            push @work, $n->{v};
            push @work, $n->{sep} if $n->{sep};
        }
    }
    return @symbols;
}

sub generate {
    my ($modules, $names, $spec, $opts) = @_;
    my $stem = lc $spec->{language};
    $stem =~ s/[^a-z0-9]+/_/g;
    $stem =~ s/^_+|_+$//g;
    $stem ||= 'language';
    my %files;
    for my $name (@$names) {
        my $path = $modules->{$name} or die "unknown extension '$name' (see --list-extensions)\n";
        require $path;
        my $render = "LLSynth::Extension::$name"->can('render')
            or die "extension '$name' does not implement render\n";
        my $artifacts = $render->($spec, $stem);
        ref $artifacts eq 'HASH' && keys %$artifacts
            or die "extension '$name' returned no artifacts\n";
        for my $file (sort keys %$artifacts) {
            $file =~ m{\A[A-Za-z0-9_-][A-Za-z0-9_.-]*(?:/[A-Za-z0-9_-][A-Za-z0-9_.-]*)*\z}
                or die "invalid artifact name '$file' from extension '$name'\n";
            my $value = $artifacts->{$file};
            defined $value && !ref $value or die "invalid text for '$file'\n";
            exists $files{$file} and die "output path collision: $file\n";
            $files{$file} = encode('UTF-8', $value);
        }
    }
    my %out;
    for my $file (sort keys %files) {
        my $dest = $file;
        if (defined $opts->{output}) {
            keys(%files) == 1 or die "-o requires exactly one extension artifact; use --out-dir\n";
            $dest = $opts->{output};
        }
        my $path = main::outpath($dest);
        make_path(dirname($path)) unless defined $opts->{output};
        $out{$path} = $files{$file};
    }
    my ($first) = sort keys %out;
    return main::write_outputs(\%out, $first);
}

1;
