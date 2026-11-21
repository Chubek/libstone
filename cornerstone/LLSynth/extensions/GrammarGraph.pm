package LLSynth::Extension::GrammarGraph;
use strict;
use warnings;

sub render {
    my ($spec, $stem) = @_;
    my %rule = map { $_->{name} => 1 } @{$spec->{rules}};
    my %edges;
    # Walk the grammar AST rather than expanded productions so captures,
    # repetitions and alternatives lead back to the source-level rule.
    for my $r (@{$spec->{rules}}) {
        for my $n (LLSynthExtensions::symbols($r->{grammar_ast})) {
            $edges{$r->{name}}{$n->{name}} = 1 if !$n->{literal} && $rule{$n->{name}};
        }
    }
    my $dot = "digraph grammar {\n  rankdir=LR;\n";
    for my $name (sort keys %rule) {
        $dot .= qq{  "$name"} . ($name eq $spec->{start} ? ' [shape=doublecircle]' : '') . ";\n";
        $dot .= qq{  "$name" -> "$_";\n} for sort keys %{$edges{$name} || {}};
    }
    return {"$stem.dot" => $dot . "}\n"};
}
1;
