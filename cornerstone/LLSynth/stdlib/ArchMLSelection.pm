package LLSynth::Library::ISel;
use strict;
use warnings;
use File::Basename qw(dirname);
use File::Spec ();

# Read the v1 list structure; quoted strings and comments are opaque. Only
# the selection pass's fields are interpreted; ArchML owns encoder validation.
sub profile {
    my ($bytes, $path) = @_;
    my @top;
    my @stack = (\@top);
    while ((pos($bytes) // 0) < length($bytes)) {
        if ($bytes =~ /\G(?:\s+|;[^\r\n]*)/gc) { next }
        if ($bytes =~ /\G\(/gc) {
            my $list = [];
            push @{$stack[-1]}, $list;
            push @stack, $list;
        } elsif ($bytes =~ /\G\)/gc) {
            @stack > 1 or die "$path: unmatched ')' near byte ".pos($bytes)."\n";
            pop @stack;
        } elsif ($bytes =~ /\G("([^"\\\r\n]|\\[^\r\n])*"|[^()";\s]+)/gc) {
            push @{$stack[-1]}, $1;
        } else {
            die "$path: invalid ArchML list or token near byte ".(pos($bytes) // 0)."\n";
        }
    }
    my $root = $top[0];
    @stack == 1 && @top == 1 && ref($root) && @$root >= 3 &&
        $root->[0] eq 'archml' && $root->[1] eq '1'
        or die "$path: expected one ArchML v1 profile\n";
    my %index;
    for my $record (@$root[3 .. $#$root]) {
        ref($record) && @$record >= 2 or die "$path: invalid profile record\n";
        my ($kind, $name, @fields) = @$record;
        next unless $kind =~ /^(?:target|type|feature|instruction|selection)$/;
        my %fields;
        for (@fields) {
            ref($_) && @$_ && !ref($_->[0]) or die "$path: invalid field of $kind '$name'\n";
            exists $fields{$_->[0]} and die "$path: duplicate field '$_->[0]' of $kind '$name'\n";
            $fields{$_->[0]} = [@$_[1 .. $#$_]];
        }
        exists $index{$kind}{$name} and die "$path: duplicate $kind '$name'\n";
        $index{$kind}{$name} = \%fields;
    }
    return ($root->[2], \%index);
}

sub references {
    my ($values, $kind) = @_;
    return map {
        ref($_) && @$_ == 3 && $_->[0] eq 'ref' && $_->[1] eq $kind
            or die "expected ArchML (ref $kind NAME)\n";
        $_->[2];
    } @$values;
}

sub expand {
    my ($header, $body, $file, $line, $read) = @_;
    my $id = qr/[A-Za-z_]\w*/;
    $header =~ /^\.isel\s+($id)\s+for\s+($id(?:\.$id)*)\s+from\s+(?:($id)|"([^"]+)")(?:\s+features\s+($id(?:\s*,\s*$id)*))?$/
        or main::fail('expected .isel TARGET for IR from PROFILE [features NAME,...]', $file, $line);
    my ($target, $ir, $bundled, $path, $features) = ($1,$2,$3,$4,$5);
    if (defined $bundled) {
        my $dir = File::Spec->catdir(dirname(__FILE__), qw(.. .. .. sandstone archml stdspec));
        ($path) = grep {-f $_} map {File::Spec->catfile($dir, "$bundled.$_")} qw(aml am);
        defined $path or main::fail("unknown ArchML stdspec profile '$bundled'", $file, $line);
    } else {
        $path = File::Spec->rel2abs($path, dirname(File::Spec->rel2abs($file)));
    }
    my ($profile, $records) = profile($read->($path), $path);
    my $arch = $records->{target}{$profile} or main::fail("missing ArchML target '$profile'", $file, $line);
    my %enabled = map { $_ => 1 } defined($features) ? split(/\s*,\s*/, $features)
                            : references($arch->{'default-features'} || [], 'feature');
    for my $name (sort keys %enabled) {
        my $feature = $records->{feature}{$name} or main::fail("unknown feature '$name' for $profile", $file, $line);
        for (references($feature->{requires} || [], 'feature')) {
            $enabled{$_} or main::fail("feature '$name' requires '$_'", $file, $line);
        }
    }
    keys %{$records->{selection} || {}} or main::fail("ArchML profile '$profile' has no selection records", $file, $line);
    my @out = (".backend $target for $ir");
    my %seen;
    for my $entry (@$body) {
        my ($raw, $origin, $ln) = @$entry;
        my $rule = main::strip_ws($raw);
        next if !$rule || $rule =~ /^(?:#(?:\s|$)|\/\/)/;
        $rule =~ /^\.select\s+($id)\s*\(\s*($id(?:\s*,\s*$id)*)?\s*\)\s*=>\s*([\w.-]+)\s+([\w.-]+)$/
            or main::fail('expected .select OPCODE(operand,...) => ir-op type', $origin, $ln);
        my ($opcode, $args, $op, $type) = ($1,$2,$3,$4);
        $seen{$opcode}++ and main::fail("duplicate selection for $opcode", $origin, $ln);
        my @args = split /\s*,\s*/, $args // '';
        my %args;
        for (@args) { $args{$_}++ and main::fail("duplicate operand '$_'", $origin, $ln) }
        my (@choices, @reasons);
        for my $name (sort keys %{$records->{selection}}) {
            my $sel = $records->{selection}{$name};
            my ($pattern) = @{$sel->{pattern} || []};
            next unless ref($pattern) && @$pattern == 3 && $pattern->[0] eq 'ir-op'
                     && $pattern->[1] eq $op && $pattern->[2] eq $type;
            my @types = references($sel->{type} || [], 'type');
            @types == 1 && $types[0] eq $type && $records->{type}{$type}
                or main::fail("invalid ArchML type for selection '$name'", $origin, $ln);
            my @forms = references($sel->{instruction} || [], 'instruction');
            @forms == 1 or main::fail("expected one instruction for selection '$name'", $origin, $ln);
            my ($form) = @forms;
            my $inst = $records->{instruction}{$form};
            $inst && $inst->{encoding} && $form =~ /^[A-Za-z_][\w.-]*$/
                or main::fail("expected encodable instruction for selection '$name'", $origin, $ln);
            my @missing = grep {!$enabled{$_}} references($inst->{requires} || [], 'feature');
            if (@missing) { push @reasons, "$form requires feature ".join(',', @missing); next }
            my @fields = map { $_->[0] } @{$inst->{operands}};
            if (@fields != @args || grep {!$args{$_}} @fields) {
                push @reasons, "$form needs operands (".join(',', @fields).')'; next;
            }
            my ($cost) = @{$sel->{cost} || []};
            defined($cost) && $cost =~ /^\d+$/ or main::fail("invalid selection cost for '$name'", $origin, $ln);
            push @choices, [$cost,$name,$form,\@fields];
        }
        @choices or main::fail("no selection for '$op $type' in $profile".(@reasons ? ': '.join('; ', @reasons) : ''), $origin, $ln);
        # Stable tie-break independent of profile record order.
        my ($best) = sort { $a->[0] <=> $b->[0] || $a->[1] cmp $b->[1] } @choices;
        my $asm = join(', ', $best->[2], map {'${'.$_.'}'} @{$best->[3]});
        push @out, ".emit $opcode(".join(', ', map {"$_:Int"} @args).qq{) => ".insn $asm\\n"};
    }
    keys %seen or main::fail('empty .isel block', $file, $line);
    return join("\n", @out, '.end');
}

return { isel => \&expand };
