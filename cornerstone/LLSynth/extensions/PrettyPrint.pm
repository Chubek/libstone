package LLSynth::Extension::PrettyPrint;
use strict;
use warnings;

my %escape = (
    '\\' => '\\textbackslash{}', '~' => '\\textasciitilde{}', '^' => '\\textasciicircum{}',
    map { $_ => "\\$_" } ('{', '}', '#', '$', '%', '&', '_')
);
sub tex {
    my ($s) = @_;
    $s =~ s/([\\~^{}#\$%&_])/$escape{$1}/g;
    return $s;
}

sub code {
    my ($s) = @_;
    $s =~ s/^\s+|\s+$//g;
    return join "\\\\\n", map { '\texttt{' . tex($_) . '}' } split /\n/, $s;
}

sub render {
    my ($s, $stem) = @_;
    my $doc = "\\documentclass{article}\n\\usepackage[T1]{fontenc}\n\\begin{document}\n";
    $doc .= "\\section*{" . tex($s->{language}) . "}\n";
    $doc .= 'Version: ' . tex($s->{version}) . "\\par\n" if defined $s->{version};
    $doc .= tex($s->{summary}) . "\\par\n" if defined $s->{summary};
    $doc .= 'Start rule: ' . code($s->{start}) . "\\par\n";
    $doc .= "\\section*{Lexical rules}\n";
    for my $tok (grep { $_->{line} } @{$s->{tokens}}) {
        my $pattern = defined $tok->{literal} ? qq{"$tok->{literal}"} : "/$tok->{regex}/";
        $doc .= code("$tok->{name}: $tok->{type} = $pattern") . "\\par\n";
    }
    $doc .= code("skip /$_->{regex}/") . "\\par\n" for @{$s->{skips}};
    if (@{$s->{types} || []}) {
        $doc .= "\\section*{Types}\n";
        for my $type (@{$s->{types}}) {
            if ($type->{members}) {
                $doc .= code(".$type->{kind} %$type->{name}") . "\\par\n";
                $doc .= code("$_->{name}: $_->{type}") . "\\par\n" for @{$type->{members}};
            } else {
                $doc .= code("%$type->{name} = $type->{type}") . "\\par\n";
            }
        }
    }
    if (@{$s->{functions}}) {
        $doc .= "\\section*{Functions}\n";
        for my $fn (@{$s->{functions}}) {
            $doc .= code("$fn->{name}($fn->{args}) -> $fn->{returns}") . "\\par\n";
        }
    }
    $doc .= "\\section*{Grammar}\n";
    for my $rule (@{$s->{rules}}) {
        $doc .= "\\subsection*{" . tex($rule->{name}) . "}\n";
        for my $facet (qw(syntax semantics lower analyze emit)) {
            next unless defined $rule->{facets}{$facet};
            $doc .= "\\noindent\\textbf{$facet}\\par\n" . code($rule->{facets}{$facet}) . "\\par\n";
        }
    }
    return {"$stem.tex" => $doc . "\\end{document}\n"};
}
1;
