#!/usr/bin/env perl
use strict;
use warnings;
use 5.014;

=pod

=head1 NAME

asdl.pl - generate C99 AST builders and visitors from Zephyr ASDL

=head1 SYNOPSIS

  perl asdl/asdl.pl tree.asdl > tree.h
  perl asdl/asdl.pl --header tree.h --visitor walk.h tree.asdl
  perl asdl/asdl.pl --header tree.h --source tree.c tree.asdl
  perl asdl/asdl.pl --check tree.asdl
  perl asdl/asdl.pl --check --schema tree.json tree.asdl
  perl asdl/asdl.pl --help
  perldoc asdl/asdl.pl

=head1 DESCRIPTION

C<asdl.pl> is a self-contained, deterministic compiler for the Abstract
Syntax Description Language described by Wang, Appel, Korn and Serra in
I<The Zephyr Abstract Syntax Description Language> (1997), distributed here
as F<asdl97.txt>. It requires Perl 5.14 or newer and only core modules.
Generated code needs a C99 implementation providing C<int64_t> and the C
standard library.

The compiler reads the whole description, checks names and type references,
and generates readable types, constructors and support functions. Its default
output is a header with C<static inline> implementations, safe to include in
multiple translation units. Alternatively it writes an interface header and
an ordinary C implementation. A separate, optional visitor header supplies
macro-selected, depth-first walkers; the same header can instantiate several
independent passes in one translation unit.

Additional facilities include arena ownership, allocator overrides, interned
identifiers, binary strings, typed sequences and optional values, structural
validation, constructor-name queries, X-macro inventories, and a JSON schema
with source locations and C name mappings.

=head1 THE ASDL LANGUAGE

The core grammar follows Figures 1 and 2 of the paper:

  definitions = { type_id "=" type }
  type        = fields | constructor { "|" constructor }
                         [ "attributes" fields ]
  constructor = constructor_id [ fields ]
  fields      = "(" field { "," field } ")"
  field       = type_id [ "?" | "*" ] [ identifier ]

A type name begins with an ASCII lowercase letter, a constructor with an
ASCII uppercase letter. Subsequent characters may be ASCII letters, digits
or underscores. A field name can begin with either case. Definitions and
constructors are unique within the description; field names are unique
within their own field list. Forward references are supported.

C<int>, C<string>, and C<identifier> are the only predefined types. They
cannot be redefined. There are no implicit C types, nested anonymous products,
inheritance, or stacked qualifiers. Parenthesized field lists are nonempty;
write C<Empty>, rather than C<Empty()>, for a nullary constructor.

A sum has one or more constructors. A product has a field list instead of
constructors. Attributes belong to the sum as a whole, including its nullary
constructors. Products cannot have attributes. Recursive sums are supported.
The paper's restriction on recursive products is enforced: a product cannot
depend transitively on itself, including through sums, attributes, C<?> or
C<*>. Use a named constructor to express a recursive tree.

C<*> denotes an ordered sequence of zero or more values. C<?> denotes zero
or one value. Missing optional values are different from present zero,
empty-string, or empty-sequence values.

As convenience extensions, comments may use C<-->, C<#>, C<//> or C</* */>;
definitions may end in semicolons; and one C<module Name { ... }> wrapper is
accepted. The wrapper supplies the default C prefix, but does not add imports
or change ASDL's type system. Bare, unwrapped 1997 descriptions are accepted.

=head1 FILE SECTIONS AND EMBEDDED C

An extended input has this layout:

  %{
  /* optional C prologue */
  %}
  %prefix demo
  %option enums line-directives
  %%
  pos = (string? file, int line, int column)
  expr = Number(int value)
       | Add(expr left, expr right)
       | Block(expr* items)
       attributes (pos? location)
  %%
  /* optional C epilogue */

The prologue must precede the options. C<%}> is recognized outside C string
and character literals and comments. C<%%> separators occupy a line by
themselves, with optional surrounding horizontal whitespace. Separators in
comments or the prologue have no effect. Everything after the SECOND separator
is copied literally, including any further C<%%> text.

The FIRST separator always starts ASDL definitions; it never starts C code.
A file with no prologue or options can omit both separators. A file with a
prologue or options must include the first separator. The second separator
and epilogue are optional.

The prologue goes in the AST header, after standard includes and before
generated declarations. Define allocator macros or application declarations
there. The epilogue follows all generated implementations: in the header for
header-only output, or in the C file for split output. Use C<static inline>
for epilogue functions that will be included by multiple translation units.
C<#line> directives preserve diagnostics for injected C unless disabled.

=head1 COMMAND-LINE OPTIONS

=over 2

=item C<--header FILE>, C<-o FILE>

Write the AST header to FILE. The default, or C<->, writes to standard output.

=item C<--source FILE>

Generate separate implementation code in FILE. Requires a named header.
Without this option, output is header-only.

=item C<--header-only>

Explicitly select header-only output, including when overriding a C<%source>
directive. An explicitly supplied command-line C<--source> conflicts with
command-line C<--header-only>.

=item C<--visitor FILE>

Write a re-includable visitor template. Supply C<--header> or C<--include>
so it can include the AST interface. Walkers are always C<static inline>.

=item C<--include NAME>

Use NAME in the generated source/visitor's quoted C<#include>. By default
the compiler computes a path to the header relative to each output file.
This option is useful for an installed include tree or a redirected header.

=item C<--prefix NAME>

Prefix generated identifiers with NAME. A trailing underscore is added if
absent. The default is the lowercase module name followed by an underscore,
or C<asdl_> for a bare description. NAME must begin with an ASCII letter and
otherwise contain only letters, digits and underscores. Uppercase NAME is
used for configuration macros. Each independently generated AST needs its
own prefix and guard.

=item C<--guard NAME>

Override the AST include guard, normally C<E<lt>PREFIXE<gt>AST_H>.

=item C<--enums>, C<--no-enums>

Enable (default) or disable compact enums for sums with only nullary
constructors and no attributes. Disabling enums gives these sums the same
pointer-and-builder API as other sums.

=item C<--line-directives>, C<--no-line-directives>

Enable (default) or disable C<#line> directives around embedded C.

=item C<--check>

Parse and validate without writing C files. May be combined with C<--schema>.

=item C<--schema FILE>

Write a canonical, pretty-printed JSON description in addition to any C
output. It contains the module, prefix, definitions, fields, attributes,
constructor tags and C spellings. C<--check --schema FILE> exports only JSON.
C<-> selects standard output, provided no other output uses it.

=item C<--help>, C<-h>; C<--man>; C<--version>, C<-V>

Show a usage summary, the complete manual, or the generator version.

=back

Exactly one input file is required. C<-> reads standard input. C<--> ends
option processing. Unknown options, unknown directives and malformed inputs
are errors. Command-line settings override corresponding input settings.
Relative output paths are relative to the working directory.

=head1 PREAMBLE OPTIONS

Single-option directives are C<%prefix>, C<%guard>, C<%header>, C<%source>,
C<%visitor>, C<%include>, C<%schema>, C<%enums>, C<%line-directives>, and
C<%header-only>. Boolean values accept C<on/off>, C<yes/no>, C<true/false>,
and C<1/0>; an omitted boolean means true.

C<%option> accepts several whitespace-separated options:

  %option prefix=calc header="generated tree.h" visitor=walk.h
  %option header-only no-enums no-line-directives

Use single or double quotes around values containing spaces. In quotes,
backslash escapes the following character. Outside quotes, C<#>, C<-->,
and C<//> at the beginning of a word introduce a line comment. Options take
one logical line each. A later input directive replaces an earlier setting.

=head1 GENERATED TYPES AND BUILDERS

The following examples assume C<%prefix demo>.

=head2 Nodes and constructors

  expr = Number(int value) | Add(expr left, expr right)
         attributes (int line)
  pair = (int first, int second)
  op = Plus | Minus

This provides:

  typedef struct demo_expr_node *demo_expr_t;
  typedef struct demo_pair_node *demo_pair_t;
  /* demo_op_t is an enum with demo_Plus=1, demo_Minus=2. */

  demo_expr_t demo_Number(demo_arena *, demo_int_t value, demo_int_t line);
  demo_expr_t demo_Add(demo_arena *, demo_expr_t left, demo_expr_t right,
                       demo_int_t line);
  demo_pair_t demo_make_pair(demo_arena *, demo_int_t first, demo_int_t second);

A sum node has a C<kind> member (C<demo_Number_kind>, C<demo_Add_kind>, ...),
common attributes directly on the node, and a named union C<v>. Access a
field as C<node-E<gt>v.Number.value>, or an attribute as C<node-E<gt>line>.
Product fields are direct members. Nullary alternatives need no union member.
Tags start at one in declaration order, independently for each sum.

Constructor parameters are fields in declaration order, followed by
attributes in declaration order. Enum constructors are constants, not
allocation functions. C<--no-enums> instead provides C<demo_Plus(arena)> and
C<demo_Plus_kind>, for example.

Unnamed fields receive a type-and-position name, such as C<expr1> and
C<expr2>. Explicit names are preserved where possible. C keywords and standard
object-like macros from the generated includes (such as C<NULL>) receive an
underscore; collisions receive numeric suffixes. The sum's attribute names
C<kind> and C<v> are similarly escaped. Constructor fields and attributes
have separate name spaces. Exact C mappings appear in comments and JSON.
Conflicts between generated global C symbols are diagnosed.

=head2 Primitive values

C<demo_int_t> is C<int64_t>, a signed finite-precision representation allowed
by section 2.2 of the paper. Applications converting larger integers must
check their range before passing them to builders.

C<demo_string_t> is C<{ size_t length; const char *data; }>. Embedded NUL
bytes are supported. C<demo_string_view(data, length)> borrows storage;
C<demo_string_copy(arena, data, length)> copies it and appends a convenience
NUL byte not counted in the length. A zero-length string may have NULL data.

C<demo_identifier_t> is C<const char *>. C<demo_identifier(arena, text)>
interns a NUL-terminated name: equal names in one arena have equal addresses.
The implementation uses an arena-local linked intern table. Builders also
accept borrowed, non-NULL identifier strings. Pointer equality is meaningful
only for names interned in the same arena.

=head2 Sequences and optional values

Every primitive and declared type T has:

  demo_T_seq { size_t length; demo_T_t *items; }
  demo_T_opt { int has_value; demo_T_t value; }

C<demo_T_seq_new(arena, count)> allocates a typed, zero-initialized array.
Fill its elements before passing it to a builder. C<demo_T_seq_copy(arena,
items, count)> validates and shallow-copies an existing array. Empty sequences
need no allocation. Failure returns an empty sequence and sets the arena error.

C<demo_T_none()> returns an absent option; C<demo_T_some(value)> returns a
present one. The wrapper is used even for pointer types, so presence is never
confused with a zero integer or enum. C<has_value> must be exactly zero or one;
the value of an absent option is ignored. A present node or identifier cannot
be NULL. Both the builders and validators enforce these rules.

=head1 OWNERSHIP AND ERRORS

  demo_arena arena;
  demo_arena_init(&arena);
  demo_expr_t node = demo_Number(&arena, 42, 1);
  if (!node) { /* inspect arena.error */ }
  demo_arena_destroy(&arena);

An arena owns nodes, copied sequence arrays, copied strings and interned
identifiers. Destruction frees every allocation once, without recursively
following tree edges. Shared subtrees and even manually introduced cycles
do not cause double frees. Independent arenas have no shared mutable state.
Do not copy a live arena, initialize it twice, or free its objects individually.
Destroying an arena resets it and makes it reusable.

Builders copy values, not entire subtrees or borrowed strings. Every referenced
object must outlive its parents. Mixing arenas is permitted with that lifetime
discipline. No automatic deep clone or recursive ownership transfer occurs.

C<arena.error> is one of C<DEMO_OK>, C<DEMO_OOM>, C<DEMO_INVALID>, or
C<DEMO_OVERFLOW>. The first error is sticky: further allocating/building
operations fail until C<demo_arena_clear_error(&arena)> or destruction.
Failed operations may leave earlier allocations in the arena; destruction
reclaims them. Invalid arguments and arithmetic overflow are distinguished
from allocator failure. A NULL arena fails without dereferencing it.
C<arena.allocations> and C<arena.bytes> count live allocation blocks and
requested payload bytes. C<demo_error_string(error)> describes a status.

C<demo_arena_alloc(arena, size)> returns uninitialized storage suitably aligned
for generated types. A zero-sized request reserves one byte. Allocation uses
one bookkeeping block per request; this favors predictable cleanup and
allocator integration over a bump allocator's throughput.

Define C<DEMO_MALLOC(size)> and C<DEMO_FREE(pointer)> before the header, or
in the prologue, to replace allocation. They must be a matched pair with
malloc-compatible alignment. In header-only builds use consistent overrides
in every translation unit sharing an arena. In split builds overrides affect
the implementation translation unit.

Constructors check immediate field values, sequence elements and option
flags before allocating. They do not traverse children. They never exit the
process on an error. The caller checks return values and arena status.

=head1 VALIDATION AND INTROSPECTION

C<int demo_validate_T(demo_T_t node, size_t max_depth)> checks an entire
declared type, including tags, attributes, optional flags, enum values,
sequence shapes and required pointers. It returns one for valid data and
zero otherwise. Each declared-type visit consumes one depth level; zero
depth fails. Bounded recursion terminates on cycles. Choose a depth limit
appropriate to the program's C stack. Validators cannot check dangling
pointers, allocated array bounds, or arena membership.

C<const char *demo_T_name(demo_T_t node)> returns the constructor name
(or product type name), and NULL for a NULL node or invalid tag.

The AST header also defines:

  DEMO_TYPE_COUNT
  DEMO_CONSTRUCTOR_COUNT
  DEMO_FOR_EACH_TYPE(X)          /* X(asdl_name, c_type, SUM|PRODUCT|ENUM) */
  DEMO_FOR_EACH_CONSTRUCTOR(X)   /* X(type_name, constructor_name, tag) */

Inventories preserve description order. They include user-defined types;
primitive types are not counted. JSON schema format version 1 is a tooling
interface, not an ASDL pickle.

=head1 MACRO-SELECTED VISITORS

Generate F<walk.h> with C<--visitor walk.h>. Before including it, choose a
function prefix, context type and callbacks:

  #include "tree.h"
  struct counter { size_t numbers; };
  static int count_number(struct counter *ctx, demo_expr_t node) {
      (void)node;
      ++ctx->numbers;
      return DEMO_WALK_CONTINUE;
  }
  #define DEMO_VISITOR_NAME count
  #define DEMO_VISITOR_CONTEXT struct counter
  #define DEMO_VISIT_Number count_number
  #include "walk.h"
  #undef DEMO_VISIT_Number
  #undef DEMO_VISITOR_CONTEXT
  #undef DEMO_VISITOR_NAME

This defines C<int count_expr(demo_expr_t node, struct counter *ctx)> and
an entry point for every other declared type. C<VISITOR_NAME> is mandatory;
C<VISITOR_CONTEXT> defaults to C<void>. Each instantiation needs a distinct
name. All public walkers take the node first, context second; callbacks take
context first, value second. Callback macros may name functions or be
function-like macros.

Available callbacks, all optional, are:

  DEMO_ENTER_expr(ctx, node)    /* before a declared type */
  DEMO_LEAVE_expr(ctx, node)    /* after a declared type */
  DEMO_VISIT_Number(ctx, node)  /* before a constructor */
  DEMO_LEAVE_Number(ctx, node)  /* after a constructor */
  DEMO_VALUE_int(ctx, value)
  DEMO_VALUE_string(ctx, value)
  DEMO_VALUE_identifier(ctx, value)

Products have type callbacks but no constructor callbacks. Enum callbacks
receive the enum value. Other type/constructor callbacks receive a node
pointer. Primitive callbacks receive a value, not a pointer to the field.
Strings count as one primitive value, not a sequence of byte visits.

Callback return values are C<DEMO_WALK_CONTINUE> (0), C<DEMO_WALK_SKIP>
(1), C<DEMO_WALK_STOP> (2), and C<DEMO_WALK_ERROR> (-1). SKIP in ENTER
skips the constructor and descendants but still calls the type's LEAVE.
SKIP in VISIT skips descendants but still calls both LEAVEs. SKIP in a
primitive or LEAVE callback is treated as CONTINUE. STOP and ERROR propagate
immediately, without unwinding LEAVE callbacks. Unknown callback results
produce ERROR. The public walker returns CONTINUE, STOP or ERROR.

Visit order is type ENTER, constructor VISIT, attributes left to right,
constructor fields left to right, constructor LEAVE, type LEAVE. Product
fields and sequence elements retain declaration/index order. Absent options
are skipped. This attribute-before-field order follows the paper's traversal
convention. Shared subtrees are visited once per incoming edge.

C<DEMO_VISITOR_MAX_DEPTH> defaults to 1024 declared-type visits. Override it
before including the visitor. A cycle, invalid tag, required NULL node,
malformed field or exhausted depth returns ERROR. Pruned descendants are
not validated. Callbacks may update scalar payloads but must not change a
node's kind, sequence storage or structural edges during traversal.

The visitor has deliberately no include guard. Its private helper macros
and default settings are cleaned up after each inclusion. User-defined
configuration and callback macros remain defined; undefine them before
configuring another pass. Generated C<NAME__depth_T> helpers are internal
implementation details; C<NAME_T> is the public entry point.

=head1 COMPLETE EXAMPLE

Save as F<calc.asdl>:

  %prefix calc
  %%
  expr = Integer(int value) | Sum(expr* terms)
  %%
  static inline calc_expr_t calc_answer(calc_arena *arena) {
      return calc_Integer(arena, 42);
  }

Generate C<perl asdl/asdl.pl --header calc.h calc.asdl>, then compile:

  #include "calc.h"
  int main(void) {
      calc_arena arena;
      calc_arena_init(&arena);
      calc_expr_t answer = calc_answer(&arena);
      int ok = answer && calc_validate_expr(answer, 64)
               && answer->v.Integer.value == 42;
      calc_arena_destroy(&arena);
      return ok ? 0 : 1;
  }

For split output, epilogue functions intended for other translation units
need a declaration in the prologue and external rather than static linkage.

=head1 DIAGNOSTICS AND OUTPUT GUARANTEES

Syntax and semantic errors include the input filename, line and column.
The compiler finishes parsing, validation and generation before publishing
any output. Files are staged in their destination directories and renamed
into place; existing outputs survive a grammar or staging error. An I/O
failure during the final sequence of renames can still publish a subset.
Input/output aliases, duplicate output paths, and multiple standard-output
destinations are rejected. Output contains no timestamps or random identifiers.

=head1 SCOPE

This implementation supplies the paper's description language and a C99
in-memory interface. It does not implement the pickling protocol, arbitrary
precision arithmetic, module imports, or other target languages. In
particular, JSON schema output is not advertised as interoperable Zephyr
serialization. Structural operations use bounded C recursion; the arena's
destructor is iterative. Data structures are mutable C records, so callers
remain responsible for the validity and lifetime of manually supplied memory.

=head1 TESTING

  prove -v asdl/asdl.t
  perl -c asdl/asdl.pl
  podchecker asdl/asdl.pl

The tests generate, compile and execute C99 programs, including multiple
translation units, visitor passes, invalid inputs and allocator failure.
Set C<CC> to choose a compiler and C<CFLAGS> to add compiler/sanitizer flags.

=head1 REFERENCES

Daniel C. Wang, Andrew W. Appel, Jeff L. Korn, Christopher S. Serra,
I<The Zephyr Abstract Syntax Description Language>, DSL'97, October 1997.
See F<asdl97.txt>, especially sections 2.1--2.7 and Figures 1, 2 and 15.

=cut

use Getopt::Long qw(GetOptionsFromArray);
use File::Basename qw(dirname);
use File::Spec;
use File::Temp qw(tempfile);
use Cwd qw(abs_path);
use JSON::PP;

my $VERSION = '1.0';
my %PRIMITIVE = map { $_ => 1 } qw(int string identifier);
my %C_KEYWORD = map { $_ => 1 } qw(auto break case char const continue default
    do double else enum extern float for goto if inline int long register
    restrict return short signed sizeof static struct switch typedef union
    unsigned void volatile while _Bool _Complex _Imaginary);
my %C_FIELD_MACRO = map { $_ => 1 } qw(NULL EXIT_SUCCESS EXIT_FAILURE RAND_MAX
    MB_CUR_MAX SIZE_MAX PTRDIFF_MIN PTRDIFF_MAX SIG_ATOMIC_MIN SIG_ATOMIC_MAX
    WCHAR_MIN WCHAR_MAX WINT_MIN WINT_MAX);
for my $width (qw(8 16 32 64 LEAST8 LEAST16 LEAST32 LEAST64 FAST8 FAST16 FAST32 FAST64 PTR MAX)) {
    $C_FIELD_MACRO{"INT${width}_MIN"} = 1;
    $C_FIELD_MACRO{"INT${width}_MAX"} = 1;
    $C_FIELD_MACRO{"UINT${width}_MAX"} = 1;
}

# ---------------------------------------------------------------------------
# Input scanning and source locations
# ---------------------------------------------------------------------------

sub location {
    my ($s, $at) = @_;
    $at = $s->{pos} unless defined $at;
    my $before = substr($s->{text}, 0, $at);
    my $line = 1 + ($before =~ tr/\n//);
    my $last = rindex($before, "\n");
    return { file => $s->{file}, line => $line, column => $at - $last };
}

sub fail {
    my ($where, $message) = @_;
    $where = location($where) if exists $where->{text};
    die "$where->{file}:$where->{line}:$where->{column}: $message\n";
}

sub skip_layout {
    my ($s) = @_;
    my $text = $s->{text};
    pos($text) = $s->{pos};
    while (pos($text) < length $text) {
        next if $text =~ /\G\s+/gc;
        next if $text =~ /\G(?:--|\#|\/\/)[^\n]*(?:\n|\z)/gc;
        if (substr($text, pos($text), 2) eq '/*') {
            my $start = pos($text);
            my $end = index($text, '*/', $start + 2);
            fail(location($s, $start), 'unterminated block comment') if $end < 0;
            pos($text) = $end + 2;
            next;
        }
        last;
    }
    $s->{pos} = pos($text);
}

sub next_token {
    my ($s) = @_;
    skip_layout($s);
    my $at = $s->{pos};
    my $loc = location($s);
    return { %$loc, value => '', kind => 'eof' } if $at == length $s->{text};
    my $text = $s->{text};
    pos($text) = $at;
    my ($kind, $value);
    if ($text =~ /\G([A-Za-z][A-Za-z0-9_]*)/gc) {
        ($kind, $value) = ('id', $1);
    } elsif ($text =~ /\G(%%)/gc) {
        ($kind, $value) = ('separator', $1);
        my $start = rindex($text, "\n", $at - 1) + 1;
        my $end = index($text, "\n", $at);
        $end = length($text) if $end < 0;
        fail($loc, '%% must occupy a line by itself')
            unless substr($text, $start, $end - $start) =~ /\A[ \t]*%%[ \t\r]*\z/;
    } elsif ($text =~ /\G([{}()=|,*?;])/gc) {
        ($kind, $value) = ('punct', $1);
    } else {
        my $c = substr($text, $at, 1);
        fail($loc, "unexpected character '$c'");
    }
    $s->{pos} = pos($text);
    return { %$loc, value => $value, kind => $kind };
}

# The closing delimiter is recognized only in C's normal lexical state.
sub read_prologue {
    my ($s) = @_;
    my $opening = location($s);
    $s->{pos} += 2;
    my $start = $s->{pos};
    my $where = location($s);
    my $text = $s->{text};
    my $i = $start;
    while ($i < length $text) {
        my $pair = substr($text, $i, 2);
        if ($pair eq '%}') {
            $s->{pos} = $i + 2;
            return { text => substr($text, $start, $i - $start), %$where };
        }
        if ($pair eq '/*') {
            my $end = index($text, '*/', $i + 2);
            fail(location($s, $i), 'unterminated C block comment') if $end < 0;
            $i = $end + 2;
            next;
        }
        if ($pair eq '//') {
            # Backslash-newline splicing continues a C line comment.
            my $continued = 1;
            while ($continued && $i < length $text) {
                my $end = index($text, "\n", $i);
                $end = length($text) if $end < 0;
                $continued = substr($text, $i, $end - $i) =~ /\\\r?\z/;
                $i = $end < length($text) ? $end + 1 : $end;
            }
            next;
        }
        my $c = substr($text, $i, 1);
        if ($c eq '"' || $c eq "'") {
            my $quote = $c;
            my $qstart = $i++;
            my $closed;
            while ($i < length $text) {
                my $d = substr($text, $i++, 1);
                if ($d eq '\\') {
                    $i++ if substr($text, $i, 2) eq "\r\n";
                    $i++;
                } elsif ($d eq $quote) { $closed = 1; last; }
                elsif ($d eq "\n") { last; }
            }
            fail(location($s, $qstart), 'unterminated C literal in prologue') unless $closed;
            next;
        }
        $i++;
    }
    fail($opening, 'unterminated %{ prologue; expected %}');
}

sub directive_words {
    my ($line, $where) = @_;
    my (@words, $word, $active);
    $word = '';
    for (my $i = 0; $i < length $line; $i++) {
        my $c = substr($line, $i, 1);
        if ($c =~ /\s/) {
            if ($active) { push @words, $word; $word = ''; $active = 0; }
            next;
        }
        last if !$active && ($c eq '#' || substr($line, $i, 2) =~ /\A(?:--|\/\/)\z/);
        $active = 1;
        if ($c eq '"' || $c eq "'") {
            my $quote = $c;
            my $closed;
            while (++$i < length $line) {
                $c = substr($line, $i, 1);
                if ($c eq $quote) { $closed = 1; last; }
                if ($c eq '\\') {
                    fail($where, 'unfinished escape in option') if ++$i == length $line;
                    $c = substr($line, $i, 1);
                }
                $word .= $c;
            }
            fail($where, 'unterminated quoted option') unless $closed;
        } else { $word .= $c; }
    }
    push @words, $word if $active;
    return @words;
}

sub set_option {
    my ($options, $name, $value, $where) = @_;
    my %bool = map { $_ => 1 } qw(enums line-directives header-only);
    my %string = map { $_ => 1 } qw(prefix guard header source visitor include schema);
    if ($name =~ s/\Ano-//) {
        fail($where, "unknown boolean option '$name'") unless $bool{$name};
        fail($where, "no-$name does not take a value") if defined $value;
        $value = 'off';
    }
    if ($bool{$name}) {
        $value = 'on' unless defined $value;
        fail($where, "invalid boolean value '$value' for $name")
            unless $value =~ /\A(?:on|off|yes|no|true|false|1|0)\z/;
        $options->{$name} = $value =~ /\A(?:on|yes|true|1)\z/ ? 1 : 0;
    } elsif ($string{$name}) {
        fail($where, "$name requires a nonempty value") unless defined $value && length $value;
        $options->{$name} = $value;
        $options->{'header-only'} = 0 if $name eq 'source';
    } else { fail($where, "unknown option '$name'"); }
}

sub parse_envelope {
    my ($text, $file) = @_;
    my $s = { text => $text, file => $file, pos => 0 };
    my ($prologue, $epilogue, %options, $preamble);
    skip_layout($s);
    if (substr($text, $s->{pos}, 2) eq '%{') {
        $prologue = read_prologue($s);
        $preamble = 1;
    }
    while (1) {
        skip_layout($s);
        last unless substr($text, $s->{pos}, 1) eq '%'
                    && substr($text, $s->{pos}, 2) ne '%%';
        my $where = location($s);
        my $end = index($text, "\n", $s->{pos});
        $end = length($text) if $end < 0;
        my $line = substr($text, $s->{pos} + 1, $end - $s->{pos} - 1);
        $s->{pos} = $end;
        my ($name, @words) = directive_words($line, $where);
        fail($where, 'expected an option directive') unless defined $name;
        if ($name eq 'option') {
            fail($where, '%option requires at least one option') unless @words;
            for my $word (@words) {
                my ($key, $value) = split /=/, $word, 2;
                set_option(\%options, $key, $value, $where);
            }
        } else {
            fail($where, "%$name takes at most one value") if @words > 1;
            set_option(\%options, $name, $words[0], $where);
        }
        $preamble = 1;
    }
    my @tokens;
    my $first = next_token($s);
    my $delimited = $first->{kind} eq 'separator';
    fail($first, 'expected initial %% after preamble') if $preamble && !$delimited;
    push @tokens, $first unless $delimited;
    while (!@tokens || $tokens[-1]{kind} ne 'eof') {
        my $token = next_token($s);
        if ($token->{kind} eq 'separator') {
            fail($token, 'epilogue requires an initial %% before definitions') unless $delimited;
            $epilogue = { text => substr($text, $s->{pos}), %{location($s)} };
            push @tokens, { %$token, kind => 'eof', value => '' };
            last;
        }
        push @tokens, $token;
    }
    return { tokens => \@tokens, options => \%options, prologue => $prologue,
             epilogue => $epilogue, file => $file };
}

# ---------------------------------------------------------------------------
# Recursive-descent ASDL reader
# ---------------------------------------------------------------------------

sub peek { return $_[0]{tokens}[$_[0]{index}]; }
sub take { return $_[0]{tokens}[$_[0]{index}++]; }
sub accept_token {
    my ($p, $value) = @_;
    return unless peek($p)->{value} eq $value;
    return take($p);
}
sub expect {
    my ($p, $value) = @_;
    return take($p) if peek($p)->{value} eq $value;
    fail(peek($p), "expected '$value', found '" . (peek($p)->{value} || 'end of input') . "'");
}
sub expect_id {
    my ($p, $case, $description) = @_;
    my $t = peek($p);
    my $pattern = $case eq 'lower' ? qr/\A[a-z]/ : $case eq 'upper' ? qr/\A[A-Z]/ : qr/\A[A-Za-z]/;
    fail($t, "expected $description") unless $t->{kind} eq 'id' && $t->{value} =~ $pattern;
    return take($p);
}

sub parse_fields {
    my ($p) = @_;
    expect($p, '(');
    my @fields;
    while (1) {
        my $type = expect_id($p, 'lower', 'a lowercase field type');
        my $qualifier = '';
        $qualifier = take($p)->{value} if peek($p)->{value} =~ /\A[?*]\z/;
        my $name;
        $name = take($p)->{value} if peek($p)->{kind} eq 'id';
        push @fields, { type => $type->{value}, qualifier => $qualifier,
                        name => $name, location => $type };
        last unless accept_token($p, ',');
    }
    expect($p, ')');
    return \@fields;
}

sub parse_asdl {
    my ($g) = @_;
    my $p = { tokens => $g->{tokens}, index => 0 };
    if (peek($p)->{value} eq 'module' && $p->{tokens}[1]{value} ne '=') {
        take($p);
        $g->{module} = expect_id($p, 'any', 'a module name')->{value};
        expect($p, '{');
    }
    my @definitions;
    while (peek($p)->{kind} ne 'eof' && peek($p)->{value} ne '}') {
        my $name = expect_id($p, 'lower', 'a lowercase type name');
        expect($p, '=');
        my $d = { name => $name->{value}, location => $name, attributes => [], constructors => [] };
        if (peek($p)->{value} eq '(') {
            $d->{kind} = 'product';
            $d->{fields} = parse_fields($p);
        } else {
            $d->{kind} = 'sum';
            do {
                my $ctor = expect_id($p, 'upper', 'an uppercase constructor name');
                my $fields = peek($p)->{value} eq '(' ? parse_fields($p) : [];
                push @{$d->{constructors}}, { name => $ctor->{value}, location => $ctor, fields => $fields };
            } while (accept_token($p, '|'));
            # A contextual keyword can also be a type name: attributes = C.
            if (peek($p)->{value} eq 'attributes' && $p->{tokens}[$p->{index} + 1]{value} ne '=') {
                take($p);
                $d->{attributes} = parse_fields($p);
            }
        }
        accept_token($p, ';');
        push @definitions, $d;
    }
    expect($p, '}') if defined $g->{module};
    fail(peek($p), 'unexpected text after definitions') unless peek($p)->{kind} eq 'eof';
    fail(peek($p), 'no ASDL definitions found') unless @definitions;
    $g->{definitions} = \@definitions;
    delete $g->{tokens};
    return $g;
}

sub field_lists {
    my ($d) = @_;
    return ($d->{fields}) if $d->{kind} eq 'product';
    return ($d->{attributes}, map { $_->{fields} } @{$d->{constructors}});
}

sub name_fields {
    my ($fields, $reserved) = @_;
    my (%used, %original);
    $used{$_} = 1 for @$reserved;
    for my $f (@$fields) {
        next unless defined $f->{name};
        fail($f->{location}, "duplicate field '$f->{name}'") if $original{$f->{name}}++;
    }
    my $i = 0;
    for my $f (@$fields) {
        ++$i;
        if (!defined $f->{name}) {
            my $name = $f->{type} . $i;
            $name .= '_' while $original{$name};
            $f->{name} = $name;
            $original{$name} = 1;
            $f->{unnamed} = 1;
        }
        my $base = $f->{name};
        $base .= '_' if $C_KEYWORD{$base} || $C_FIELD_MACRO{$base} || grep { $_ eq $base } @$reserved;
        my $name = $base;
        my $suffix = 2;
        $name = $base . '_' . $suffix++ while $used{$name};
        $used{$name} = 1;
        $f->{c_name} = $name;
    }
}

sub validate_asdl {
    my ($g) = @_;
    my (%types, %constructors);
    for my $d (@{$g->{definitions}}) {
        fail($d->{location}, "cannot redefine primitive '$d->{name}'") if $PRIMITIVE{$d->{name}};
        fail($d->{location}, "duplicate type '$d->{name}'") if $types{$d->{name}};
        $types{$d->{name}} = $d;
        my $tag = 0;
        for my $c (@{$d->{constructors}}) {
            fail($c->{location}, "duplicate constructor '$c->{name}'") if $constructors{$c->{name}}++;
            $c->{tag} = ++$tag;
        }
        $d->{enum} = $g->{options}{enums} && $d->{kind} eq 'sum'
            && !@{$d->{attributes}} && !(grep { @{$_->{fields}} } @{$d->{constructors}}) ? 1 : 0;
        name_fields($d->{attributes}, [qw(kind v)]);
        name_fields($_, []) for $d->{kind} eq 'product'
            ? ($d->{fields}) : (map { $_->{fields} } @{$d->{constructors}});
    }
    for my $d (@{$g->{definitions}}) {
        for my $list (field_lists($d)) {
            for my $f (@$list) {
                fail($f->{location}, "undefined type '$f->{type}'") unless $PRIMITIVE{$f->{type}} || $types{$f->{type}};
            }
        }
    }
    # A cycle reachable from a product back to itself violates section 2.6.
    for my $root (grep { $_->{kind} eq 'product' } @{$g->{definitions}}) {
        my %visited;
        my $visit;
        $visit = sub {
            my ($d, $path) = @_;
            return if $visited{$d->{name}}++;
            for my $list (field_lists($d)) {
                for my $f (@$list) {
                    next if $PRIMITIVE{$f->{type}};
                    fail($f->{location}, 'recursive product type: ' . join(' -> ', @$path, $f->{type}))
                        if $f->{type} eq $root->{name};
                    $visit->($types{$f->{type}}, [@$path, $f->{type}]);
                }
            }
        };
        $visit->($root, [$root->{name}]);
        undef $visit; # break the recursive closure's reference cycle
    }
    $g->{types} = \%types;
}

# ---------------------------------------------------------------------------
# C emission utilities
# ---------------------------------------------------------------------------

sub c_string {
    my ($s) = @_;
    $s =~ s/([\\"])/\\$1/g;
    $s =~ s/\n/\\n/g;
    $s =~ s/\r/\\r/g;
    $s =~ s/\t/\\t/g;
    return '"' . $s . '"';
}

sub c_type { return $_[0]{prefix} . $_[1] . '_t'; }
sub field_type {
    my ($g, $f) = @_;
    return $g->{prefix} . $f->{type} . ($f->{qualifier} eq '*' ? '_seq'
        : $f->{qualifier} eq '?' ? '_opt' : '_t');
}
sub tag_name {
    my ($g, $d, $c) = @_;
    return $g->{prefix} . $c->{name} . ($d->{enum} ? '' : '_kind');
}
sub all_types { return (qw(int string identifier), map { $_->{name} } @{$_[0]{definitions}}); }

sub register_symbol {
    my ($g, $name, $description) = @_;
    die "$g->{file}: generated C symbol '$name' conflicts: $g->{symbols}{$name} / $description\n"
        if exists $g->{symbols}{$name};
    $g->{symbols}{$name} = $description;
}

sub add_function {
    my ($g, $signature, $body) = @_;
    $signature =~ /([A-Za-z][A-Za-z0-9_]*)\(/ or die "internal error: bad signature $signature\n";
    register_symbol($g, $1, 'function');
    push @{$g->{functions}}, { signature => $signature, body => $body };
}

sub template {
    my ($g, $text) = @_;
    $text =~ s/\@P\@/$g->{prefix}/g;
    $text =~ s/\@U\@/$g->{upper}/g;
    return $text;
}

sub shallow_test {
    my ($g, $type, $v) = @_;
    return '1' if $type eq 'int';
    return "(($v).length == 0 || ($v).data != NULL)" if $type eq 'string';
    return "(($v) != NULL)" if $type eq 'identifier' || !$g->{types}{$type}{enum};
    my $d = $g->{types}{$type};
    my $first = tag_name($g, $d, $d->{constructors}[0]);
    my $last = tag_name($g, $d, $d->{constructors}[-1]);
    return "(($v) >= $first && ($v) <= $last)";
}

sub emit_fields {
    my ($g, $fields, $indent) = @_;
    return join '', map {
        "$indent" . field_type($g, $_) . " $_->{c_name}; /* ASDL: $_->{name} */\n"
    } @$fields;
}

sub emit_runtime_types {
    my ($g) = @_;
    my $p = $g->{prefix};
    my $u = $g->{upper};
    register_symbol($g, $p . $_, 'runtime type') for qw(error visit_result block intern arena int_t string_t identifier_t);
    register_symbol($g, $u . $_, 'runtime constant') for qw(OK OOM INVALID OVERFLOW WALK_CONTINUE WALK_SKIP WALK_STOP WALK_ERROR);
    register_symbol($g, $u . $_, 'configuration macro') for qw(MALLOC FREE VISITOR_NAME
        VISITOR_CONTEXT VISITOR_MAX_DEPTH PRIVATE_CAT_I PRIVATE_CAT PRIVATE_FN
        PRIVATE_DEFAULT_CONTEXT PRIVATE_DEFAULT_DEPTH);
    return template($g, <<'C');
#ifndef @U@MALLOC
#define @U@MALLOC(size) malloc(size)
#endif
#ifndef @U@FREE
#define @U@FREE(pointer) free(pointer)
#endif

typedef enum @P@error {
    @U@OK = 0, @U@OOM, @U@INVALID, @U@OVERFLOW
} @P@error;
typedef enum @P@visit_result {
    @U@WALK_ERROR = -1, @U@WALK_CONTINUE = 0,
    @U@WALK_SKIP = 1, @U@WALK_STOP = 2
} @P@visit_result;

/* The union aligns block+1 for every type emitted by this generator. */
typedef struct @P@block {
    struct @P@block *next;
    union { long double floating; void *pointer; intmax_t integer; } alignment;
} @P@block;
typedef struct @P@intern {
    struct @P@intern *next;
    char text[];
} @P@intern;
typedef struct @P@arena {
    @P@block *blocks;
    @P@intern *identifiers;
    size_t allocations;
    size_t bytes;
    @P@error error;
} @P@arena;

typedef int64_t @P@int_t;
typedef struct @P@string { size_t length; const char *data; } @P@string_t;
typedef const char *@P@identifier_t;

C
}

sub emit_ast_types {
    my ($g) = @_;
    my $p = $g->{prefix};
    my $out = "/* ---- ASDL types (forward declarations permit recursive sums) ---- */\n";
    for my $d (@{$g->{definitions}}) {
        my $t = c_type($g, $d->{name});
        register_symbol($g, $t, "type $d->{name}");
        if ($d->{kind} eq 'sum') {
            my $kind = $p . $d->{name} . '_kind';
            register_symbol($g, $kind, 'sum tag type');
            $out .= "typedef enum ${p}$d->{name}_tag {\n";
            for my $c (@{$d->{constructors}}) {
                my $tag = tag_name($g, $d, $c);
                register_symbol($g, $tag, "tag $c->{name}");
                $out .= "    $tag = $c->{tag},\n";
            }
            $out .= "} $kind;\n";
            $out .= "typedef $kind $t;\n" if $d->{enum};
        }
        $out .= "typedef struct ${p}$d->{name}_node *$t;\n" unless $d->{enum};
    }
    $out .= "\n/* Typed collections and explicit presence, including scalar options. */\n";
    for my $name (all_types($g)) {
        my $t = c_type($g, $name);
        my $stem = $p . $name;
        register_symbol($g, $stem . $_, 'collection type') for qw(_seq _opt);
        $out .= "typedef struct { size_t length; $t *items; } ${stem}_seq;\n";
        $out .= "typedef struct { int has_value; $t value; } ${stem}_opt;\n";
    }
    for my $d (@{$g->{definitions}}) {
        next if $d->{enum};
        $out .= "\nstruct ${p}$d->{name}_node {\n";
        if ($d->{kind} eq 'product') {
            $out .= emit_fields($g, $d->{fields}, '    ');
        } else {
            $out .= "    ${p}$d->{name}_kind kind;\n";
            $out .= emit_fields($g, $d->{attributes}, '    ');
            my @with_fields = grep { @{$_->{fields}} } @{$d->{constructors}};
            if (@with_fields) {
                $out .= "    union {\n";
                for my $c (@with_fields) {
                    $out .= "        struct {\n" . emit_fields($g, $c->{fields}, '            ')
                          . "        } $c->{name};\n";
                }
                $out .= "    } v;\n";
            }
        }
        $out .= "};\n";
    }
    return $out . "\n";
}

sub emit_inventory {
    my ($g) = @_;
    my $u = $g->{upper};
    my @d = @{$g->{definitions}};
    my $count = 0;
    $count += @{$_->{constructors}} for @d;
    register_symbol($g, $u . $_, 'inventory macro') for qw(TYPE_COUNT CONSTRUCTOR_COUNT FOR_EACH_TYPE FOR_EACH_CONSTRUCTOR);
    my $out = "#define ${u}TYPE_COUNT " . scalar(@d) . "\n#define ${u}CONSTRUCTOR_COUNT $count\n";
    my @types = map { 'X(' . $_->{name} . ', ' . c_type($g, $_->{name}) . ', '
        . ($_->{enum} ? 'ENUM' : uc($_->{kind})) . ')' } @d;
    my @ctors;
    for my $d (@d) {
        push @ctors, map { "X($d->{name}, $_->{name}, $_->{tag})" } @{$d->{constructors}};
    }
    $out .= "#define ${u}FOR_EACH_TYPE(X)" . join('', map { " \\\n    $_" } @types) . "\n";
    $out .= "#define ${u}FOR_EACH_CONSTRUCTOR(X)" . join('', map { " \\\n    $_" } @ctors) . "\n\n";
    return $out;
}

sub add_runtime_functions {
    my ($g) = @_;
    my $runtime = template($g, <<'C');
FUNCTION void @P@arena_init(@P@arena *arena)
{
    if (arena) *arena = (@P@arena){0};
}
FUNCTION void @P@set_error(@P@arena *arena, @P@error error)
{
    if (arena && arena->error == @U@OK) arena->error = error;
}
FUNCTION void @P@arena_clear_error(@P@arena *arena)
{
    if (arena) arena->error = @U@OK;
}
FUNCTION const char *@P@error_string(@P@error error)
{
    switch (error) {
    case @U@OK: return "success";
    case @U@OOM: return "out of memory";
    case @U@INVALID: return "invalid argument";
    case @U@OVERFLOW: return "size overflow";
    default: return "unknown error";
    }
}
FUNCTION void *@P@arena_alloc(@P@arena *arena, size_t size)
{
    @P@block *block;
    if (!arena || arena->error != @U@OK) return NULL;
    if (size == 0) size = 1;
    if (size > SIZE_MAX - sizeof(*block) || size > SIZE_MAX - arena->bytes
        || arena->allocations == SIZE_MAX) {
        @P@set_error(arena, @U@OVERFLOW);
        return NULL;
    }
    block = (@P@block *)@U@MALLOC(sizeof(*block) + size);
    if (!block) {
        @P@set_error(arena, @U@OOM);
        return NULL;
    }
    block->next = arena->blocks;
    arena->blocks = block;
    arena->bytes += size;
    ++arena->allocations;
    return (void *)(block + 1);
}
FUNCTION void @P@arena_destroy(@P@arena *arena)
{
    @P@block *block;
    if (!arena) return;
    block = arena->blocks;
    while (block) {
        @P@block *next = block->next;
        @U@FREE(block);
        block = next;
    }
    @P@arena_init(arena);
}
FUNCTION @P@string_t @P@string_view(const char *data, size_t length)
{
    @P@string_t value = {length, data};
    return value;
}
FUNCTION @P@string_t @P@string_copy(@P@arena *arena, const char *data, size_t length)
{
    @P@string_t value = {0, NULL};
    char *copy;
    if (!arena || arena->error != @U@OK) return value;
    if (length && !data) { @P@set_error(arena, @U@INVALID); return value; }
    if (length == SIZE_MAX) { @P@set_error(arena, @U@OVERFLOW); return value; }
    copy = (char *)@P@arena_alloc(arena, length + 1);
    if (!copy) return value;
    if (length) memcpy(copy, data, length);
    copy[length] = '\0';
    value.length = length;
    value.data = copy;
    return value;
}
FUNCTION @P@identifier_t @P@identifier(@P@arena *arena, const char *text)
{
    @P@intern *entry;
    size_t length;
    if (!arena || arena->error != @U@OK) return NULL;
    if (!text) { @P@set_error(arena, @U@INVALID); return NULL; }
    for (entry = arena->identifiers; entry; entry = entry->next)
        if (strcmp(entry->text, text) == 0) return entry->text;
    length = strlen(text);
    if (length > SIZE_MAX - sizeof(*entry) - 1) {
        @P@set_error(arena, @U@OVERFLOW);
        return NULL;
    }
    entry = (@P@intern *)@P@arena_alloc(arena, sizeof(*entry) + length + 1);
    if (!entry) return NULL;
    entry->next = arena->identifiers;
    memcpy(entry->text, text, length + 1);
    arena->identifiers = entry;
    return entry->text;
}
C
    while ($runtime =~ /^FUNCTION ([^\n]+)\n\{\n(.*?)^\}\n/msg) {
        add_function($g, $1, $2);
    }
}

sub add_collection_functions {
    my ($g) = @_;
    my ($p, $u) = @{$g}{qw(prefix upper)};
    for my $name (all_types($g)) {
        my $stem = $p . $name;
        my $t = c_type($g, $name);
        add_function($g, "${stem}_opt ${stem}_none(void)",
            "    ${stem}_opt result = {0};\n    return result;\n");
        add_function($g, "${stem}_opt ${stem}_some($t value)",
            "    ${stem}_opt result = {1, value};\n    return result;\n");
        add_function($g, "${stem}_seq ${stem}_seq_new(${p}arena *arena, size_t length)", <<C);
    ${stem}_seq result = {0, NULL};
    size_t i;
    if (!arena || arena->error != ${u}OK) return result;
    if (length > SIZE_MAX / sizeof(*result.items)) {
        ${p}set_error(arena, ${u}OVERFLOW);
        return result;
    }
    if (!length) return result;
    result.items = ($t *)${p}arena_alloc(arena, length * sizeof(*result.items));
    if (!result.items) return result;
    result.length = length;
    for (i = 0; i < length; ++i) result.items[i] = ($t){0};
    return result;
C
        my $check = shallow_test($g, $name, 'items[i]');
        my $checks = $check eq '1' ? ''
            : "    for (i = 0; i < length; ++i) {\n        if (!($check)) { ${p}set_error(arena, ${u}INVALID); return result; }\n    }\n";
        my $i_decl = $check eq '1' ? '' : "    size_t i;\n";
        add_function($g, "${stem}_seq ${stem}_seq_copy(${p}arena *arena, const $t *items, size_t length)", <<C);
    ${stem}_seq result = {0, NULL};
${i_decl}    if (!arena || arena->error != ${u}OK) return result;
    if (length > SIZE_MAX / sizeof(*result.items)) {
        ${p}set_error(arena, ${u}OVERFLOW);
        return result;
    }
    if (length && !items) { ${p}set_error(arena, ${u}INVALID); return result; }
${checks}    result = ${stem}_seq_new(arena, length);
    if (result.items) memcpy(result.items, items, length * sizeof(*items));
    return result;
C
    }
}

# Apply an operation to the value(s) carried by a field. Each collection
# gets its own C scope, so nested operations can safely reuse index names.
sub field_operation {
    my ($g, $f, $expr, $invalid, $operation, $indent) = @_;
    $indent //= '    ';
    my $q = $f->{qualifier};
    if ($q eq '?') {
        return "$indent" . "if (($expr).has_value != 0 && ($expr).has_value != 1) { $invalid }\n"
            . "$indent" . "if (($expr).has_value) {\n"
            . $operation->($f->{type}, "($expr).value", $indent . '    ')
            . "$indent}\n";
    }
    if ($q eq '*') {
        return "$indent" . "if (($expr).length && !($expr).items) { $invalid }\n"
            . "$indent" . "if (($expr).length > SIZE_MAX / sizeof(*($expr).items)) { $invalid }\n"
            . "$indent\{\n$indent    size_t index;\n"
            . "$indent    for (index = 0; index < ($expr).length; ++index) {\n"
            . $operation->($f->{type}, "($expr).items[index]", $indent . '        ')
            . "$indent    }\n$indent}\n";
    }
    return $operation->($f->{type}, $expr, $indent);
}

sub shallow_operation {
    my ($g, $invalid) = @_;
    return sub {
        my ($type, $value, $indent) = @_;
        my $test = shallow_test($g, $type, $value);
        return '' if $test eq '1';
        return "${indent}if (!($test)) { $invalid }\n";
    };
}

sub add_builders {
    my ($g) = @_;
    my ($p, $u) = @{$g}{qw(prefix upper)};
    for my $d (@{$g->{definitions}}) {
        next if $d->{enum};
        my @constructors = $d->{kind} eq 'product'
            ? ({ name => 'make_' . $d->{name}, fields => $d->{fields} }) : @{$d->{constructors}};
        for my $c (@constructors) {
            my (@params, @assignments, $checks);
            $checks = '';
            for my $pair ([$c->{fields}, 'arg_', $d->{kind} eq 'product' ? '' : "v.$c->{name}."],
                          [$d->{attributes}, 'attr_', '']) {
                my ($fields, $arg_prefix, $member_prefix) = @$pair;
                for my $f (@$fields) {
                    my $arg = $arg_prefix . $f->{c_name};
                    push @params, field_type($g, $f) . " $arg";
                    push @assignments, "    node->$member_prefix$f->{c_name} = $arg;\n";
                    my $invalid = "${p}set_error(arena, ${u}INVALID); return NULL;";
                    $checks .= field_operation($g, $f, $arg, $invalid, shallow_operation($g, $invalid));
                }
            }
            my $type = c_type($g, $d->{name});
            my $args = join(', ', "${p}arena *arena", @params);
            my $body = "    $type node;\n    if (!arena || arena->error != ${u}OK) return NULL;\n$checks"
                . "    node = ($type)${p}arena_alloc(arena, sizeof(*node));\n    if (!node) return NULL;\n"
                . "    *node = (struct ${p}$d->{name}_node){0};\n";
            $body .= '    node->kind = ' . tag_name($g, $d, $c) . ";\n" if $d->{kind} eq 'sum';
            $body .= join('', @assignments) . "    return node;\n";
            add_function($g, "$type ${p}$c->{name}($args)", $body);
        }
    }
}

sub add_validators {
    my ($g) = @_;
    my $p = $g->{prefix};
    my $operation = sub {
        my ($type, $value, $indent) = @_;
        my $test = $PRIMITIVE{$type} ? shallow_test($g, $type, $value)
            : "${p}validate_$type($value, depth - 1)";
        return '' if $test eq '1';
        return "${indent}if (!($test)) return 0;\n";
    };
    for my $d (@{$g->{definitions}}) {
        my $t = c_type($g, $d->{name});
        my $body = "    if (!depth) return 0;\n";
        $body .= "    if (!node) return 0;\n" unless $d->{enum};
        if ($d->{kind} eq 'product') {
            for my $f (@{$d->{fields}}) {
                $body .= field_operation($g, $f, "node->$f->{c_name}", 'return 0;', $operation);
            }
        } else {
            my $selector = $d->{enum} ? 'node' : 'node->kind';
            $body .= "    switch ($selector) {\n";
            for my $c (@{$d->{constructors}}) {
                $body .= '    case ' . tag_name($g, $d, $c) . ":\n";
                for my $f (@{$d->{attributes}}) {
                    $body .= field_operation($g, $f, "node->$f->{c_name}", 'return 0;', $operation, '        ');
                }
                for my $f (@{$c->{fields}}) {
                    $body .= field_operation($g, $f, "node->v.$c->{name}.$f->{c_name}", 'return 0;', $operation, '        ');
                }
                $body .= "        break;\n";
            }
            $body .= "    default: return 0;\n    }\n";
        }
        $body .= "    return 1;\n";
        add_function($g, "int ${p}validate_$d->{name}($t node, size_t depth)", $body);
        my $name_body;
        if ($d->{kind} eq 'product') {
            $name_body = '    return node ? ' . c_string($d->{name}) . " : NULL;\n";
        } else {
            $name_body = $d->{enum} ? '' : "    if (!node) return NULL;\n";
            $name_body .= '    switch (' . ($d->{enum} ? 'node' : 'node->kind') . ") {\n";
            for my $c (@{$d->{constructors}}) {
                $name_body .= '    case ' . tag_name($g, $d, $c) . ': return ' . c_string($c->{name}) . ";\n";
            }
            $name_body .= "    default: return NULL;\n    }\n";
        }
        add_function($g, "const char *${p}$d->{name}_name($t node)", $name_body);
    }
}

sub injected_c {
    my ($g, $fragment, $output_name) = @_;
    return '' unless $fragment && length $fragment->{text};
    my $out = "\n";
    $out .= '#line ' . $fragment->{line} . ' ' . c_string($g->{file}) . "\n" if $g->{options}{'line-directives'};
    $out .= $fragment->{text};
    $out .= "\n" unless $out =~ /\n\z/;
    $out .= '#line 1 ' . c_string($output_name) . "\n" if $g->{options}{'line-directives'};
    return $out;
}

sub header_include {
    my ($g, $destination) = @_;
    my $name = $g->{options}{include};
    if (!defined $name) {
        my $header = $g->{options}{header};
        die "--visitor requires --header or --include when the header is written to stdout\n"
            unless defined $header && $header ne '-';
        $name = File::Spec->abs2rel(File::Spec->rel2abs($header),
                                  File::Spec->rel2abs(dirname($destination)));
    }
    die "invalid quoted C include path '$name'\n" if $name =~ /["\\\r\n\0]/;
    return "#include \"$name\"\n";
}

sub emit_ast {
    my ($g) = @_;
    my $o = $g->{options};
    $g->{symbols} = {};
    $g->{functions} = [];
    register_symbol($g, $g->{guard}, 'header guard');
    my $types = emit_runtime_types($g) . emit_ast_types($g) . emit_inventory($g);
    add_runtime_functions($g);
    add_collection_functions($g);
    add_builders($g);
    add_validators($g);
    my $linkage = $o->{'header-only'} ? 'static inline ' : '';
    my $prototypes = join '', map { "$linkage$_->{signature};\n" } @{$g->{functions}};
    my $bodies = join "\n", map { "$linkage$_->{signature}\n{\n$_->{body}}\n" } @{$g->{functions}};
    my $header = "/* Generated by asdl.pl $VERSION. C99; edit the ASDL input. */\n"
        . "#ifndef $g->{guard}\n#define $g->{guard}\n\n"
        . "#include <stddef.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n"
        . injected_c($g, $g->{prologue}, $o->{header} // '<asdl-header>')
        . "\n$types/* ---- API ---- */\n$prototypes\n";
    my $source;
    if ($o->{'header-only'}) {
        $header .= "/* ---- implementation ---- */\n$bodies"
                 . injected_c($g, $g->{epilogue}, $o->{header} // '<asdl-header>');
    } else {
        $source = "/* Generated by asdl.pl $VERSION. */\n" . header_include($g, $o->{source})
                . "\n$bodies" . injected_c($g, $g->{epilogue}, $o->{source});
    }
    $header .= "\n#endif /* $g->{guard} */\n";
    return ($header, $source);
}

# ---------------------------------------------------------------------------
# Re-includable, compile-time selected walker template
# ---------------------------------------------------------------------------

sub visitor_callback {
    my ($g, $hook, $value, $indent, $skip_label) = @_;
    my $u = $g->{upper};
    my $macro = $u . $hook;
    my $code = "#ifdef $macro\n${indent}action = $macro(context, $value);\n";
    if (defined $skip_label) {
        $code .= "${indent}if (action == ${u}WALK_SKIP) goto $skip_label;\n"
               . "${indent}if (action != ${u}WALK_CONTINUE) return ${u}PRIVATE_FN(__result)(action);\n";
    } else {
        $code .= "${indent}if (action != ${u}WALK_CONTINUE && action != ${u}WALK_SKIP)\n"
               . "$indent    return ${u}PRIVATE_FN(__result)(action);\n";
    }
    return $code . "#endif\n";
}

sub emit_visitor {
    my ($g, $destination) = @_;
    my ($p, $u) = @{$g}{qw(prefix upper)};
    my $out = "/* Generated by asdl.pl $VERSION: intentionally re-includable. */\n"
        . header_include($g, $destination) . "\n" . template($g, <<'C');
#ifndef @U@VISITOR_NAME
#error "Define @U@VISITOR_NAME before including this visitor template"
#endif
#ifndef @U@VISITOR_CONTEXT
#define @U@VISITOR_CONTEXT void
#define @U@PRIVATE_DEFAULT_CONTEXT
#endif
#ifndef @U@VISITOR_MAX_DEPTH
#define @U@VISITOR_MAX_DEPTH 1024
#define @U@PRIVATE_DEFAULT_DEPTH
#endif
#define @U@PRIVATE_CAT_I(a, b) a##b
#define @U@PRIVATE_CAT(a, b) @U@PRIVATE_CAT_I(a, b)
#define @U@PRIVATE_FN(suffix) @U@PRIVATE_CAT(@U@VISITOR_NAME, suffix)

static inline int @U@PRIVATE_FN(__result)(int action)
{
    return action == @U@WALK_STOP ? @U@WALK_STOP : @U@WALK_ERROR;
}

C
    for my $d (@{$g->{definitions}}) {
        my $t = c_type($g, $d->{name});
        $out .= "static inline int ${u}PRIVATE_FN(__depth_$d->{name})($t node, ${u}VISITOR_CONTEXT *context, size_t depth);\n";
    }
    $out .= "\n";
    my $operation = sub {
        my ($type, $value, $indent) = @_;
        if ($PRIMITIVE{$type}) {
            my $check = shallow_test($g, $type, $value);
            my $code = $check eq '1' ? '' : "${indent}if (!($check)) return ${u}WALK_ERROR;\n";
            return $code . visitor_callback($g, "VALUE_$type", $value, $indent);
        }
        return "${indent}action = ${u}PRIVATE_FN(__depth_$type)($value, context, depth - 1);\n"
             . "${indent}if (action != ${u}WALK_CONTINUE) return action;\n";
    };
    for my $d (@{$g->{definitions}}) {
        my $name = $d->{name};
        my $t = c_type($g, $name);
        my $body = "    int action = ${u}WALK_CONTINUE;\n    (void)action;\n    (void)context;\n"
                 . "    if (!depth || !${p}${name}_name(node)) return ${u}WALK_ERROR;\n"
                 . visitor_callback($g, "ENTER_$name", 'node', '    ', 'leave_type');
        if ($d->{kind} eq 'product') {
            for my $f (@{$d->{fields}}) {
                $body .= field_operation($g, $f, "node->$f->{c_name}", "return ${u}WALK_ERROR;", $operation);
            }
        } else {
            my $selector = $d->{enum} ? 'node' : 'node->kind';
            $body .= "    switch ($selector) {\n";
            for my $c (@{$d->{constructors}}) {
                $body .= '    case ' . tag_name($g, $d, $c) . ": {\n"
                       . visitor_callback($g, "VISIT_$c->{name}", 'node', '        ', "leave_ctor_$c->{tag}");
                for my $f (@{$d->{attributes}}) {
                    $body .= field_operation($g, $f, "node->$f->{c_name}", "return ${u}WALK_ERROR;", $operation, '        ');
                }
                for my $f (@{$c->{fields}}) {
                    $body .= field_operation($g, $f, "node->v.$c->{name}.$f->{c_name}", "return ${u}WALK_ERROR;", $operation, '        ');
                }
                $body .= "#ifdef ${u}VISIT_$c->{name}\n    leave_ctor_$c->{tag}: ;\n#endif\n"
                       . visitor_callback($g, "LEAVE_$c->{name}", 'node', '        ')
                       . "        break;\n    }\n";
            }
            $body .= "    default: return ${u}WALK_ERROR;\n    }\n";
        }
        $body .= "#ifdef ${u}ENTER_$name\nleave_type: ;\n#endif\n"
               . visitor_callback($g, "LEAVE_$name", 'node', '    ')
               . "    return ${u}WALK_CONTINUE;\n";
        $out .= "static inline int ${u}PRIVATE_FN(__depth_$name)($t node, ${u}VISITOR_CONTEXT *context, size_t depth)\n{\n$body}\n\n";
    }
    for my $d (@{$g->{definitions}}) {
        my $name = $d->{name};
        my $t = c_type($g, $name);
        $out .= "static inline int ${u}PRIVATE_FN(_$name)($t node, ${u}VISITOR_CONTEXT *context)\n{\n"
              . "    return ${u}PRIVATE_FN(__depth_$name)(node, context, (size_t)${u}VISITOR_MAX_DEPTH);\n}\n\n";
    }
    $out .= template($g, <<'C');
#undef @U@PRIVATE_FN
#undef @U@PRIVATE_CAT
#undef @U@PRIVATE_CAT_I
#ifdef @U@PRIVATE_DEFAULT_CONTEXT
#undef @U@VISITOR_CONTEXT
#undef @U@PRIVATE_DEFAULT_CONTEXT
#endif
#ifdef @U@PRIVATE_DEFAULT_DEPTH
#undef @U@VISITOR_MAX_DEPTH
#undef @U@PRIVATE_DEFAULT_DEPTH
#endif
C
    return $out;
}

# ---------------------------------------------------------------------------
# Machine-readable schema and output publication
# ---------------------------------------------------------------------------

sub schema_location {
    my ($loc) = @_;
    return { map { $_ => $loc->{$_} } qw(file line column) };
}

sub emit_schema {
    my ($g) = @_;
    my $fields = sub {
        my ($list) = @_;
        return [map { +{
            name => $_->{name}, type => $_->{type}, qualifier => $_->{qualifier},
            c_name => $_->{c_name}, c_type => field_type($g, $_),
            unnamed => $_->{unnamed} ? JSON::PP::true : JSON::PP::false,
            location => schema_location($_->{location}),
        } } @$list];
    };
    my @types;
    for my $d (@{$g->{definitions}}) {
        my $type = { name => $d->{name}, kind => $d->{kind},
            representation => $d->{enum} ? 'enum' : 'pointer',
            c_type => c_type($g, $d->{name}), location => schema_location($d->{location}),
            attributes => $fields->($d->{attributes}),
        };
        if ($d->{kind} eq 'product') {
            $type->{fields} = $fields->($d->{fields});
            $type->{builder} = $g->{prefix} . 'make_' . $d->{name};
        } else {
            $type->{constructors} = [map { +{
                name => $_->{name}, tag => $_->{tag},
                c_name => $g->{prefix} . $_->{name}, c_tag => tag_name($g, $d, $_),
                fields => $fields->($_->{fields}), location => schema_location($_->{location}),
            } } @{$d->{constructors}}];
        }
        push @types, $type;
    }
    return JSON::PP->new->canonical->pretty->ascii->encode({
        format_version => 1, generator_version => $VERSION,
        module => $g->{module}, prefix => $g->{prefix}, guard => $g->{guard},
        primitives => [qw(int string identifier)], types => \@types,
    });
}

sub canonical_output {
    my ($path) = @_;
    my $absolute = abs_path($path);
    return $absolute if defined $absolute;
    my $parent = abs_path(dirname($path));
    die "output directory does not exist for '$path'\n" unless defined $parent && -d $parent;
    my (undef, undef, $file) = File::Spec->splitpath($path);
    return File::Spec->catfile($parent, $file);
}

sub publish_outputs {
    my ($input, $outputs) = @_;
    my (%seen, %inodes);
    if ($input ne '-') {
        $seen{abs_path($input)} = 'input';
        my @st = stat($input);
        $inodes{"$st[0]:$st[1]"} = 'input' if @st;
    }
    for my $output (@$outputs) {
        my ($path) = @$output;
        my $canonical = $path eq '-' ? '<stdout>' : canonical_output($path);
        die "output '$path' aliases $seen{$canonical}\n" if exists $seen{$canonical};
        $seen{$canonical} = "output '$path'";
        next if $path eq '-';
        die "output '$path' is not a regular file\n" if -e $path && !-f $path;
        my @st = stat($path);
        if (@st) {
            my $inode = "$st[0]:$st[1]";
            die "output '$path' aliases $inodes{$inode}\n" if exists $inodes{$inode};
            $inodes{$inode} = "output '$path'";
        }
    }
    my @staged;
    my $ok = eval {
        for my $output (@$outputs) {
            my ($path, $text) = @$output;
            next if $path eq '-';
            my ($fh, $temporary) = tempfile('.asdl-XXXXXXXX', DIR => dirname($path), UNLINK => 0);
            push @staged, [$temporary, $path];
            binmode $fh;
            print {$fh} $text or die "cannot write '$path': $!\n";
            close $fh or die "cannot close '$path': $!\n";
            my @st = stat($path);
            my $mode = @st ? $st[2] & 0777 : 0666 & ~umask;
            chmod $mode, $temporary or die "cannot set mode for '$path': $!\n";
        }
        for my $file (@staged) {
            rename $file->[0], $file->[1] or die "cannot publish '$file->[1]': $!\n";
        }
        for my $output (@$outputs) {
            next unless $output->[0] eq '-';
            print STDOUT $output->[1] or die "cannot write stdout: $!\n";
        }
        1;
    };
    my $error = $@;
    unlink $_->[0] for grep { -e $_->[0] } @staged;
    die $error unless $ok;
}

sub slurp_input {
    my ($path) = @_;
    my $fh;
    if ($path eq '-') { $fh = *STDIN; }
    else { open $fh, '<', $path or die "cannot open '$path': $!\n"; }
    binmode $fh;
    local $/;
    my $text = <$fh>;
    die "cannot read '$path': $!\n" if !defined($text) && $!;
    close $fh or die "cannot close '$path': $!\n" if $path ne '-';
    return $text // '';
}

sub usage {
    return <<'TEXT';
usage: asdl.pl [options] input.asdl
  --header FILE, -o FILE  AST header (default: stdout)
  --source FILE          separate C implementation (requires --header)
  --header-only          static-inline header (default)
  --visitor FILE         re-includable, macro-configured walker header
  --include NAME         AST include path for source/visitor
  --prefix NAME          C namespace (default: module name or asdl_)
  --guard NAME           override the AST include guard
  --[no-]enums           optimize nullary sums to enums (default: on)
  --[no-]line-directives preserve injected C locations (default: on)
  --check               validate without generating C files
  --schema FILE         export JSON schema; '-' selects stdout
  --help, -h            this summary
  --man                 full POD manual
  --version, -V         generator version
Use '-' as the input filename to read standard input.
TEXT
}

sub main {
    my @args = @_;
    my (%cli, $help, $man, $version, $check);
    Getopt::Long::Configure(qw(no_auto_abbrev no_ignore_case));
    GetOptionsFromArray(\@args,
        'header|o=s' => \$cli{header}, 'source=s' => \$cli{source},
        'header-only' => \$cli{'header-only'}, 'visitor=s' => \$cli{visitor},
        'include=s' => \$cli{include}, 'prefix=s' => \$cli{prefix},
        'guard=s' => \$cli{guard}, 'enums!' => \$cli{enums},
        'line-directives!' => \$cli{'line-directives'}, 'schema=s' => \$cli{schema},
        'check' => \$check, 'help|h' => \$help, 'man' => \$man, 'version|V' => \$version,
    ) or die usage();
    if ($help) { print usage(); return; }
    if ($version) { print "asdl.pl $VERSION\n"; return; }
    if ($man) {
        require Pod::Usage;
        Pod::Usage::pod2usage(-verbose => 2, -exitval => 0, -noperldoc => 1);
    }
    die usage() unless @args == 1;
    my $file = $args[0];
    my $g = parse_asdl(parse_envelope(slurp_input($file), $file));
    die "--source and --header-only cannot be combined on the command line\n"
        if defined $cli{source} && $cli{'header-only'};
    $cli{'header-only'} = 0 if defined $cli{source};
    my %options = (enums => 1, 'line-directives' => 1, 'header-only' => 1,
        %{$g->{options}}, map { $_ => $cli{$_} } grep { defined $cli{$_} } keys %cli);
    for my $key (qw(header source visitor include prefix guard schema)) {
        die "--$key requires a nonempty value\n" if defined $options{$key} && !length $options{$key};
    }
    if ($options{'header-only'}) { delete $options{source}; }
    else {
        die "split output requires --source FILE and --header FILE\n"
            unless defined $options{source} && defined $options{header}
                && $options{header} ne '-' && $options{source} ne '-';
    }
    die "--visitor requires a filename, not stdout\n" if defined $options{visitor} && $options{visitor} eq '-';
    my $prefix = $options{prefix} // (defined $g->{module} ? lc($g->{module}) : 'asdl');
    die "invalid C prefix '$prefix'\n" unless $prefix =~ /\A[A-Za-z][A-Za-z0-9_]*\z/;
    $prefix .= '_' unless $prefix =~ /_\z/;
    $g->{prefix} = $prefix;
    $g->{upper} = uc($prefix);
    $g->{guard} = $options{guard} // $g->{upper} . 'AST_H';
    die "invalid C header guard '$g->{guard}'\n" unless $g->{guard} =~ /\A[A-Za-z][A-Za-z0-9_]*\z/;
    $g->{options} = \%options;
    validate_asdl($g);
    # Generation also checks global target-language name collisions in --check.
    my ($header, $source) = emit_ast($g);
    my @outputs;
    if (!$check) {
        push @outputs, [$options{header} // '-', $header];
        push @outputs, [$options{source}, $source] if defined $source;
        push @outputs, [$options{visitor}, emit_visitor($g, $options{visitor})] if defined $options{visitor};
    }
    push @outputs, [$options{schema}, emit_schema($g)] if defined $options{schema};
    publish_outputs($file, \@outputs);
}

unless (caller) {
    my $ok = eval { main(@ARGV); 1 };
    if (!$ok) { print STDERR "asdl.pl: $@"; exit 1; }
}
