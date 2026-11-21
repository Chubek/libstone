package Acsem::Emit;
use strict;
use warnings;
use 5.014;
use File::Basename qw(dirname basename);
use File::Spec;

# One compact, deterministic instruction format for all standalone backends.
# References are zero-based; -1 denotes an unused operand. Field names are
# resolved here, so the target runtimes never need to elaborate a specification.
my @ops = qw(integer token field given bound contents + - * / == <
             complete fail give check escape bind allocate store evaluate
             furthermore then and_then hence otherwise trapping);
my %opcode = map { $ops[$_] => $_ + 1 } 0..$#ops;

sub lower {
    my ($model) = @_;
    my @sorts = @{$model->{sort_order}};
    my %sort = map { $sorts[$_] => $_ } 0..$#sorts;
    my @ctors = map { @{$model->{sorts}{$_}} } @sorts;
    my (@nodes, @records);
    for my $name (@ctors) {
        my $ctor = $model->{constructors}{$name};
        my %field = map { $ctor->{fields}[$_]{name} => $_ } 0..$#{$ctor->{fields}};
        my $visit;
        $visit = sub {
            my ($node) = @_;
            my $op = $node->{op};
            my ($x, $y, $n, $text) = (-1, -1, 0, '-');
            if ($op eq 'integer') { $n = $node->{value} }
            elsif ($op eq 'token') { $text = $node->{value} }
            elsif ($op eq 'field' || $op eq 'evaluate') {
                $n = $field{$node->{$op eq 'field' ? 'name' : 'field'}};
            } elsif ($op eq 'given') { $n = $node->{arg}{value} - 1 }
            elsif ($op eq 'binary' || $op eq 'combine') {
                $op = $node->{symbol};
                $op = 'and_then' if $op eq 'and then';
                $x = $visit->($node->{left}); $y = $visit->($node->{right});
            } elsif ($op eq 'bind') {
                $x = $visit->($node->{key}); $y = $visit->($node->{value});
            } elsif ($op eq 'store') {
                $x = $visit->($node->{value}); $y = $visit->($node->{cell});
            } elsif ($op eq 'furthermore') { $x = $visit->($node->{body}) }
            elsif (exists $node->{arg}) { $x = $visit->($node->{arg}) }
            die "ACSEM_ERROR: unsupported instruction $op\n" unless exists $opcode{$op};
            push @nodes, join(' ', $opcode{$op}, $x, $y, $n, $text);
            return $#nodes;
        };
        my $rule = $visit->($model->{rules}{$name});
        undef $visit; # Break the recursive closure's reference cycle.
        my @types = map { $_->{type} eq 'integer' ? -1 :
                         $_->{type} eq 'identifier' ? -2 : $sort{$_->{type}} } @{$ctor->{fields}};
        push @records, join(' ', $name, $sort{$ctor->{sort}}, scalar(@types), $rule, @types);
    }
    return join("\n", join(' ', $sort{$model->{start}}, scalar(@ctors), scalar(@nodes)),
                @records, @nodes)."\n";
}

sub java_class {
    my ($output, $requested) = @_;
    my $filename = defined($output) && $output =~ /\.java\z/ ? basename($output, '.java') : undef;
    my $name = $requested // $filename // 'AcsemProgram';
    my %reserved = map { $_ => 1 } qw(abstract assert boolean break byte case catch char
        class const continue default do double else enum extends final finally float for
        goto if implements import instanceof int interface long native new package private
        protected public return short static strictfp super switch synchronized this throw
        throws transient try void volatile while true false null _ var yield record sealed permits);
    die "ACSEM_ERROR: invalid Java class name '$name'\n"
        unless $name =~ /\A[A-Za-z_][A-Za-z_0-9]*\z/ && !$reserved{$name};
    die "ACSEM_ERROR: Java class name must match output filename '$filename.java'\n"
        if defined($filename) && $filename ne $name;
    return $name;
}

sub synthesize {
    my ($model, $target, $output, $class) = @_;
    my %templates = (c=>'portable.c', cpp=>'portable.c', python=>'python.py',
                     rust=>'rust.rs', java=>'java.java', pascal=>'pascal.pas');
    die "ACSEM_ERROR: unsupported target '$target'\n" unless exists $templates{$target};
    my $path = File::Spec->catfile(dirname(__FILE__), '..', 'runtime', $templates{$target});
    open my $fh, '<:raw', $path or die "ACSEM_ERROR: cannot read backend $path: $!\n";
    local $/; my $body = <$fh>; close $fh;
    my $data = lower($model);
    my @lines = split /\n/, $data;
    # The only textual operands are validated ASCII identifiers. No source
    # fragments from the input language are executed or pasted into target code.
    my $literal;
    if ($target eq 'python') { $literal = '"""'.$data.'"""' }
    elsif ($target eq 'rust') { $literal = '"'.$data.'"' }
    elsif ($target eq 'pascal') { $literal = join(" +\n", map { "'$_ '" } @lines) }
    elsif ($target eq 'java') {
        # Separate array elements avoid the JVM's 65535-byte string constant limit.
        $literal = join(",\n", map { '"'.$_.'"' } @lines);
    } else {
        # Separate records avoid C's minimum limit on concatenated string size.
        $literal = join(",\n", map { '"'.$_.'"' } @lines);
    }
    $body =~ s/\@MODEL\@/$literal/;
    if ($target eq 'java') {
        my $name = java_class($output, $class);
        # A class literally named java shadows the root package. Imports are
        # unambiguous in this case, while all other class names use qualified
        # library types (including class names such as String or System).
        if ($name eq 'java') {
            my %imports;
            $body =~ s{\b(java\.(?:lang|io|util|nio\.charset)\.([A-Za-z]+))}{$imports{$1}=1; $2}ge;
            $body = join('', map { "import $_;\n" } sort keys %imports).$body;
        }
        $body =~ s/\@CLASS\@/$name/g;
    }
    $body =~ s/\@VERSION\@/$Acsem::VERSION/g;
    return $body;
}

1;
