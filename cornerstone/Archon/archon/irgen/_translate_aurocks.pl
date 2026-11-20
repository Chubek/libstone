# Archon Translate.py's data-only entrypoint, linked to the embedded Aurocks
# source instead of its C generator main(). Reuse the actual grammar reader,
# terminal decoder, recursion checks and Thompson NFA compiler. This additional
# envelope check closes the permissive C/metadata regions of the upstream
# grammar reader and ensures it cannot silently discard pieces of our input.
use JSON::PP ();

sub translate_grammar {
    my ($text) = @_;
    my $length = length $text;
    my $error = sub {
        my ($at, $message) = @_;
        my $before = substr($text, 0, $at);
        my $line = 1 + ($before =~ tr/\n//);
        my $column = $at - rindex($before, "\n");
        die "$line:$column: $message\n";
    };
    my @tokens;
    my $at = 0;
    while ($at < $length) {
        pos($text) = $at;
        if ($text =~ /\G(?:[ \t\r\n]+|\/\/[^\n]*)/gc) {
            $at = pos($text);
            next;
        }
        my $pair = substr($text, $at, 2);
        if ($pair eq '/*') {
            my $end = index($text, '*/', $at + 2);
            $error->($at, 'unclosed grammar comment') if $end < 0;
            $at = $end + 2;
            next;
        }
        my $ch = substr($text, $at, 1);
        my $end;
        if ($pair eq '%{' || $pair eq '%}' || $ch eq '{' || $ch eq '}') {
            $error->($at, 'native grammar actions and C sections are not supported');
        } elsif ($ch eq '"' || $ch eq "'" || $ch eq '/') {
            $end = $ch eq '/' ? scan_regex_literal($text, $at)
                               : scan_quoted_literal($text, $ch, $at);
            my $token = substr($text, $at, $end - $at);
            $error->($at, 'unterminated terminal')
                if length($token) < 2 || substr($token, -1) ne $ch || $token =~ /[\r\n]/;
            # Upstream finds section delimiters before scanning terminals.
            $error->($at, 'section delimiter inside terminal (use a regex byte escape)')
                if $token =~ /%%|%\{/;
            $error->($at, 'terminal exceeds 512 bytes') if length($token) > 512;
        } elsif ($text =~ /\G(?:%%|[:|;]|%?[A-Za-z_][A-Za-z0-9_]*|-?[0-9]+)/gc) {
            $end = pos($text);
            $error->($at, 'grammar token exceeds 64 bytes') if $end - $at > 64;
        } else {
            $error->($at, 'unsupported grammar text (C epilogues are not supported)');
        }
        push @tokens, [substr($text, $at, $end - $at), $at];
        $at = $end;
    }
    push @tokens, ['', $length];
    my $index = 0;
    my $peek = sub { $tokens[$index][0] };
    my $fail = sub { $error->($tokens[$index][1], @_); };
    my $take = sub { $tokens[$index++][0] };
    $take->() if $peek->() eq '%%';
    my (%directives, @lines, @expected);
    while ($peek->() =~ /\A%(?!%)/) {
        my $at = $tokens[$index][1];
        my $directive = $take->();
        $error->($at, "unsupported grammar directive $directive")
            unless $directive eq '%start' || $directive eq '%skip';
        $error->($at, "duplicate grammar directive $directive") if $directives{$directive}++;
        my $line_end = index($text, "\n", $at);
        $line_end = $length if $line_end < 0;
        my $arg = $peek->();
        $fail->("expected an argument to $directive on the same line")
            if $tokens[$index][1] >= $line_end
            || ($directive eq '%start' ? $arg !~ /\A[A-Za-z_]\w*\z/ : $arg !~ m{\A/.+/\z});
        $take->();
        $fail->("$directive must occupy its own line") if $tokens[$index][1] < $line_end;
        push @lines, "$directive $arg";
    }
    my %names;
    while ($peek->() ne '' && $peek->() ne '%%') {
        my $lhs = $peek->();
        $fail->('expected a production name') unless $lhs =~ /\A[A-Za-z_]\w*\z/;
        $names{$take->()} = 1;
        $fail->('too many productions (limit: 64)') if keys(%names) > 64;
        $fail->("expected ':' after $lhs") unless $peek->() eq ':';
        $take->();
        my @alternatives;
        while (1) {
            my @rhs;
            my $empty = $peek->() eq '%empty';
            if ($empty) {
                $take->();
            } else {
                while ($peek->() =~ /\A(?:[A-Za-z_]\w*\z|['"\/])/) {
                    push @rhs, $take->();
                    $fail->('right-hand side exceeds 16 symbols') if @rhs > 16;
                }
                $fail->('expected a grammar symbol or %empty') unless @rhs;
            }
            my $precedence = 0;
            my $suffix = '';
            if ($peek->() eq '%dprec') {
                $take->();
                $precedence = $peek->();
                $fail->('expected a signed 32-bit %dprec value')
                    unless $precedence =~ /\A-?[0-9]{1,10}\z/
                    && $precedence >= -2147483648 && $precedence <= 2147483647;
                $suffix = ' %dprec ' . $take->();
            }
            push @expected, [$lhs, \@rhs, 0 + $precedence];
            $fail->('too many alternatives (limit: 256)') if @expected > 256;
            push @alternatives, ($empty ? '%empty' : join(' ', @rhs)) . $suffix;
            last unless $peek->() eq '|';
            $take->();
        }
        $fail->("expected ';' after production $lhs") unless $peek->() eq ';';
        $take->();
        push @lines, "$lhs: " . join(' | ', @alternatives) . ';';
    }
    $take->() if $peek->() eq '%%';
    $fail->('unsupported grammar epilogue or misplaced %%') if $peek->() ne '';

    my $grammar = parse_grammar(join("\n", @lines) . "\n");
    my $rules = $grammar->{rules};
    die "unsupported grammar section\n" if $grammar->{prologue} ne '' || $grammar->{epilogue} ne '';
    die "Aurocks did not preserve the grammar alternatives\n" if @$rules != @expected;
    for my $i (0 .. $#expected) {
        my ($lhs, $rhs, $prec) = @{$expected[$i]};
        my $r = $rules->[$i];
        die "Aurocks did not preserve alternative for [$lhs]\n"
            if $r->{lhs} ne $lhs || @{$r->{rhs}} != @$rhs || $r->{dprec} != $prec
                || $r->{action} ne '' || grep { $r->{rhs}[$_] ne $rhs->[$_] } 0 .. $#$rhs;
    }
    return $grammar;
}

# Estimate the expanded NFA size before interval repetitions allocate states.
sub translate_nfa_size {
    my ($node, $depth) = @_;
    die "regex nesting exceeds 48 levels\n" if $depth > 48;
    my $type = $node->{type};
    my $count = 1;
    if ($type eq 'literal') {
        $count = length($node->{text}) || 1;
    } elsif ($type eq 'concat' || $type eq 'alt') {
        $count = $type eq 'alt' ? @{$node->{parts}} - 1 : 0;
        $count += translate_nfa_size($_, $depth + 1) for @{$node->{parts}};
    } elsif ($type eq 'repeat') {
        my ($min, $max) = @{$node}{qw(min max)};
        die "invalid regex repetition range\n" if defined($max) && $max < $min;
        die "regex repetition bound exceeds 512\n" if $min > 512 || (defined($max) && $max > 512);
        my $child = translate_nfa_size($node->{child}, $depth + 1);
        $count = defined($max)
            ? $max * $child + ($max - $min) + ($min ? 0 : 1)
            : ($min + 1) * $child + 1 + ($min ? 0 : 1);
    }
    die "regex expands to too many states (limit: 2048)\n" if $count >= 2048;
    return $count;
}

sub translate_machine {
    my ($desc) = @_;
    return undef if !defined $desc;
    my $nfa = regex_compile_nfa($desc->{ast}, $desc);
    return {
        start => $nfa->{start},
        states => [map { [@{$_}{qw(kind out1 out2 arg)}] } @{$nfa->{states}}],
        classes => [map { unpack('H*', regex_class_mask($_)) } @{$nfa->{classes}}],
    };
}

sub translate_data_main {
    my $grammar = translate_grammar(slurp($ARGV[0]));
    my $rules = $grammar->{rules};
    my $nonterm = $grammar->{nonterm};
    my $nullable = compute_nullable($rules, $nonterm);
    check_recursion($rules, $nonterm, $nullable);
    my $skip = $grammar->{directives}{skip};
    my ($terms, $layout, $ids) = collect_terminals($rules, $skip ? substr($skip->[0], 1, -1) : '');
    my (%has_seed, %left_recursive);
    for my $rule (@$rules) {
        my ($lhs, $rhs) = @{$rule}{qw(lhs rhs)};
        for my $symbol (@$rhs) {
            die "unknown grammar symbol [$symbol]\n"
                unless $nonterm->{$symbol} || exists $ids->{$symbol};
        }
        if (@$rhs && $lhs eq $rhs->[0]) {
            $left_recursive{$lhs} = 1;
            die "left-recursive tail of [$lhs] must consume input\n"
                unless grep { !$nullable->{$_} } @$rhs[1 .. $#$rhs];
        } else {
            $has_seed{$lhs} = 1;
        }
    }
    for my $lhs (sort keys %left_recursive) {
        die "production [$lhs] has no non-left-recursive seed\n" unless $has_seed{$lhs};
    }
    my $size = 0;
    for my $desc (grep { defined $_ } (@$terms, $layout)) {
        $size += translate_nfa_size($desc->{ast}, 0) + 1;
        die "total NFA size exceeds 8192 states\n" if $size > 8192;
    }
    my $unicode = sub { decode('UTF-8', $_[0], FB_CROAK | Encode::LEAVE_SRC) };
    print JSON::PP->new->ascii->canonical->encode({
        start => $grammar->{start},
        rules => [map {
            +{ lhs => $_->{lhs}, rhs => [map { $unicode->($_) } @{$_->{rhs}}],
               precedence => 0 + $_->{dprec} }
        } @$rules],
        terminals => {map { $unicode->($_) => $ids->{$_} } keys %$ids},
        matchers => [map { translate_machine($_) } @$terms],
        skip => translate_machine($layout),
    });
}

translate_data_main();
