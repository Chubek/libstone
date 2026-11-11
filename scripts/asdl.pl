#!/usr/bin/env perl
use strict;
use warnings;
use 5.014;

=pod

=head1 NAME

asdl.pl - generate C11 AST builders, visitors, runtimes and file listeners

=head1 SYNOPSIS

  perl asdl/asdl.pl tree.asdl > tree.h
  perl asdl/asdl.pl --header tree.h --visitor walk.h tree.asdl
  perl asdl/asdl.pl --header tree.h --source tree.c tree.asdl
  perl asdl/asdl.pl --header tree.h --listener events.h tree.asdl
  perl asdl/asdl.pl --header tree.h --builtins-header runtime.h tree.asdl
  perl asdl/asdl.pl --no-builtins tree.asdl > tree.h
  perl asdl/asdl.pl --check tree.asdl
  perl asdl/asdl.pl --check --schema tree.json tree.asdl
  perl asdl/asdl.pl --help
  perldoc asdl/asdl.pl

=head1 DESCRIPTION

C<asdl.pl> is a self-contained, deterministic compiler for the Abstract
Syntax Description Language described by Wang, Appel, Korn and Serra in
I<The Zephyr Abstract Syntax Description Language> (1997), distributed here
as F<asdl97.txt>. It requires Perl 5.14 or newer and only core modules.
Generated code uses C11. The extended runtime also uses POSIX facilities:
C<ssize_t>, filesystem operations, C<regex.h>, and C<mmap> for listeners.

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
  field       = field_type [ "?" | "*" | "+" ] [ identifier ]
  field_type  = type_id | "list" "[" field_type "]"
              | "map" "[" field_type "," field_type "]"
              | "array" "[" [ field_type "," ] positive_integer "]"

A type name begins with an ASCII lowercase letter, a constructor with an
ASCII uppercase letter. Subsequent characters may be ASCII letters, digits
or underscores. A field name can begin with either case. Definitions and
constructors are unique within the description; field names are unique
within their own field list. Forward references are supported.

C<int>, C<string>, C<identifier>, C<bool>, C<uint>, C<real>, C<size>,
C<usize>, C<list>, C<array>, C<map>, C<port>, C<path>, C<buffer>, C<sexpr>,
C<regexp>, C<parsec>, and C<event> are predefined and cannot be redefined.
There are no implicit C types, nested anonymous products,
inheritance, or stacked qualifiers. Parenthesized field lists are nonempty;
write C<Empty>, rather than C<Empty()>, for a nullary constructor.

A sum has one or more constructors. A product has a field list instead of
constructors. Attributes belong to the sum as a whole, including its nullary
constructors. Products cannot have attributes. Recursive sums are supported.
The paper's restriction on recursive products is enforced: a product cannot
depend transitively on itself, including through sums, attributes, C<?> or
C<*>, C<+>, or parameterized containers. Use a named constructor to express
a recursive tree.

C<*> denotes an ordered sequence of zero or more values. C<+> uses the same
typed sequence representation but requires at least one element. Builders,
validators and visitors reject an empty C<+> field, including attributes.
C<?> denotes zero or one value. Missing optional values are different from present zero,
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

Use NAME in the generated source/visitor/listener's quoted C<#include>. By default
the compiler computes a path to the header relative to each output file.
This option is useful for an installed include tree or a redirected header.

=item C<--listener FILE>

Write a guarded, static-inline mmap file listener header. Like a visitor, this
requires a named C<--header> or C<--include>. See L</FILE LISTENERS>.

=item C<--builtins>, C<--no-builtins>

Emit (default) or omit all C<ASDL...> runtime definitions and operations.
This does not remove the types from the ASDL language, nor suppress the
prefix-specific AST, arena, sequence, option or typed-container interfaces.
Supply replacement runtime declarations and operations before the AST header
or through its C prologue. Scalar typedefs remain available.

=item C<--omit-builtin NAME>, C<--no-builtin NAME>

Omit one runtime component, using its ASDL name or C struct name, for example
C<--omit-builtin list> or C<--omit-builtin ASDLVector>. Repeat the option or
use comma-separated names. Available components are C<list>, C<array>, C<map>,
C<buffer>, C<port>, C<path>, C<event>, C<sexpr>, C<regexp> and C<parsec>.
Omission leaves dependent components enabled; provide a compatible replacement.
C<port>, C<path>, C<regexp> and C<parsec> depend on C<buffer>; C<sexpr> depends
on C<buffer>, C<port> and C<event>. A listener needs C<event> and the common
runtime allocator macros. Invalid component names are errors.

=item C<--builtins-header FILE>

Write enabled runtime components to a separate header and include it from
the AST header using a relative path. Component guards allow multiple ASTs
and runtime headers in one translation unit. Runtime functions always use
C<static inline>, including in split-source mode. Allocator configuration
must precede the first runtime inclusion. This option participates in output
alias checks and staged publication. With C<--no-builtins> it writes an empty
runtime header.

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
C<%visitor>, C<%listener>, C<%include>, C<%schema>, C<%enums>,
C<%line-directives>, C<%header-only>, C<%builtins>, C<%builtins-header>,
and C<%omit-builtin> (also C<%no-builtin>). Boolean values accept C<on/off>, C<yes/no>, C<true/false>,
and C<1/0>; an omitted boolean means true.

C<%option> accepts several whitespace-separated options:

  %option prefix=calc header="generated tree.h" visitor=walk.h
  %option header-only no-enums no-line-directives

Use single or double quotes around values containing spaces. In quotes,
backslash escapes the following character. Outside quotes, C<#>, C<-->,
and C<//> at the beginning of a word introduce a line comment. Options take
one logical line each. A later input directive replaces an earlier setting,
except C<omit-builtin>, which accumulates names. A command-line omit list
replaces the input's omit list.

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

C<demo_int_t> is C<intmax_t>, a signed finite-precision representation allowed
by section 2.2 of the paper. Applications converting larger integers must
check their range before passing them to builders.

C<demo_uint_t> is C<uintmax_t>, C<demo_bool_t> is C<bool> from C<stdbool.h>
(use C<true> and C<false>), C<demo_real_t> is C<long double>, C<demo_size_t>
is C<ssize_t>, and C<demo_usize_t> is C<size_t>. New builtin typedefs and
sequence/option wrappers are emitted when the schema uses them. The original
three primitives retain their unconditional wrappers.

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

=head1 EXTENDED RUNTIME

Runtime structs are value types. Their operations use the C struct name as
the function prefix. Initialize each owning object once, then destroy it
explicitly. AST builders copy these records shallowly: an AST reference does
not acquire ownership. Keep the original object and its storage alive until
all AST uses finish, and destroy the owner exactly once. Mutating container
length or reallocating its storage after copying its record into an AST does
not update the AST copy. Reassign the field when necessary. Regex records
share a pointer to compiled regex state under the same shallow ownership rule.

The arena does not own extended runtime allocations, file handles, regex
state, or mappings. The shared runtime uses C<ASDL_MALLOC(size)> and
C<ASDL_FREE(pointer)>, defaulting to C<malloc/free>, independently of the
prefix-specific arena allocator. Use compatible overrides in all translation
units. Allocation operations return C<false> or NULL on failure. Container
growth preserves existing storage on failure; C<_init> and C<_new> require inspection of their
return value or resulting storage. Do not reinitialize a live owner.
Container copies, removal and destruction do not recursively destroy elements.
Zero-filled resized or newly allocated container storage must be populated
with valid values before passing it to an AST builder.

=head2 Typed lists, fixed arrays and maps

  node = Value(int n) | Children(list[node] children)
       | Index(map[int,node] values) | Packet(array[64] bytes)
       | Pair(array[node,2] children)

C<list[T]> maps to C<struct ASDLVector>, C<array[N]> to a byte-sized
C<struct ASDLFixedArray>, C<array[T,N]> to a typed fixed array, and
C<map[K,V]> to C<struct ASDLHashMap>. Array lengths are positive decimal
integers, checked against allocation-size overflow at runtime. Generics nest:
C<list[map[int,node]]> is valid. Qualifiers apply to the whole container.
The C alias replaces punctuation by underscores, e.g. C<demo_list_node_t>,
C<demo_array_node_2_t> and C<demo_map_int_node_t>. Ambiguous flattened names
are diagnosed as C symbol collisions. The JSON C mapping is authoritative.

Typed list/array helpers are C<STEM_new()>, C<STEM_at(collection*, index)>
and C<STEM_set(collection*, index, value)>; lists also have C<STEM_push>.
For example:

  demo_list_node_t children = demo_list_node_new();
  if (!demo_list_node_push(&children, child)) { /* allocation failed */ }
  demo_node_t tree = demo_Children(arena, children);
  /* use tree while children storage remains alive */
  ASDLVector_destroy(&children);

Common operations (inspect generated prototypes for exact C signatures):

  ASDLVector_init(v, element_size)       ASDLVector_destroy(v)
  ASDLVector_reserve(v, capacity)        ASDLVector_resize(v, length)
  ASDLVector_at(v, index)                ASDLVector_set(v, index, value_ptr)
  ASDLVector_push(v, value_ptr)          ASDLVector_pop(v, optional_out)
  ASDLVector_insert(v, index, value_ptr) ASDLVector_erase(v, index, count)
  ASDLVector_clear(v)                   ASDLVector_reverse(v)
  ASDLVector_sort(v, compare)

  ASDLFixedArray_init(a, element_size, count)
  ASDLFixedArray_destroy(a)             ASDLFixedArray_at(a, index)
  ASDLFixedArray_set(a, index, value_ptr)
  ASDLFixedArray_fill(a, value_ptr)      ASDLFixedArray_copy(to, from)
  ASDLFixedArray_sort(a, compare)

C<_at> returns NULL outside the collection. Returned element pointers are
invalidated by resizing, insertion, removal or destruction. Array length is
fixed until destruction; it is runtime-allocated rather than an inline C array.
Builders and validators check declared array length and container element
sizes. Visitors descend through list/array elements and both map keys and
values; maps use slot order, not insertion order. Nested container traversal
does not consume a declared-node depth level.

=head2 Hashing and map operations

Every map key type T requires an application symbol C<T_hashfn(value)> returning
a value convertible to C<size_t>. It may be a function or a function-like
macro. T is the unprefixed ASDL name (flattened for a generic key). The
generator calls the symbol with the typed key by value, never with a C<void *>.
Declare it before generated implementations, usually in the prologue:

  %{
  static inline size_t int_hashfn(intmax_t value) {
      return (size_t)(uintmax_t)value;
  }
  %}

For node-key functions, a prologue can forward-declare C<struct demo_node_node>
and a hash function taking a pointer to that tag; the implementation may follow
the AST header. For a string key, forward-declare C<struct demo_string> and
use that value type in the hash prototype. A macro can refer directly to the
key's members. No default hash silently replaces the application function.

C<demo_map_K_V_new(equal)> installs the generated hash adapter. The optional
equality callback has type C<int (*)(const void *, const void *)>; it receives
pointers to typed keys and returns nonzero for equality. NULL selects bytewise
C<memcmp> equality. Supply semantic equality for strings, nodes or structs
with padding, and ensure equal keys hash equally. Keys must not change their
hash/equality while stored. Map keys and values are copied shallowly into
separate, suitably aligned allocations.

Typed helpers are C<STEM_set(map*, key, value)>, C<STEM_get(map*, key)>
(returns a value pointer or NULL), and C<STEM_remove(map*, key)>.
The shared API also offers C<ASDLHashMap_init>, C<_reserve>, C<_contains>,
C<_clear>, C<_destroy> and C<_next(map, cursor*, key_out, value_out)>.
Initialize an iteration cursor to zero. Do not mutate the map while iterating.
Collisions use open addressing with tombstones; replacement keeps length
unchanged. Rehash preserves key/value allocations, but removal frees them.

=head2 Buffers, ports and paths

C<buffer> is C<struct ASDLByteBuffer> with C<data>, C<length>, C<capacity>
and a read C<position>. Operations are C<ASDLByteBuffer_init>, C<_destroy>,
C<_reserve>, C<_append(buffer, bytes, length)>, C<_put(buffer, byte)>,
C<_read(buffer, output, length)>, C<_seek(buffer, position)>,
C<_truncate(buffer, length)>, C<_clear>, and C<_cstr>. C<_cstr> adds an
uncounted trailing NUL; embedded NUL bytes remain valid. Read returns the
number of bytes copied. Append supports aliased/self input.

C<port> is C<struct ASDLPort>. C<ASDLPort_open(port, path, mode)> owns an
C<fopen> stream. C<_file(port, FILE*, owned)> wraps a stream with explicit
ownership. C<_input(port, bytes, length)> borrows an input slice, and
C<_output(port)> creates an owned growable output buffer, exposed as
C<port.output>. Operations include C<_read>, C<_write>, C<_get>, C<_put>,
C<_read_line(port, buffer*)> (retains newline), C<_seek(port, long, whence)>,
C<_eof>, C<_flush> and C<_close>. Memory output is append-only. C<port.error>
stores an errno-style failure; byte operations use EOF or short counts.
C<_close> frees memory output and closes only owned streams.

C<path> is C<struct ASDLPath> containing an owned NUL-terminated C<text>.
C<ASDLPath_init(path, text)> copies a path; C<_destroy> frees it.
C<_join(out, base, part)> and C<_parent(out, path)> replace an initialized
output (zero initialization is sufficient) and permit in-place use.
C<_name> and C<_suffix> return borrowed strings. Filesystem operations are
C<_exists>, C<_is_file>, C<_is_dir>, C<_is_absolute>, C<_mkdir(path, mode)>,
C<_remove>, C<_rename(from, to)>, C<_read(path, buffer*)>, and
C<_write(path, bytes, length)>. Read replaces an initialized output buffer
only after success; write truncates the destination. These use POSIX paths;
join and parent are lexical and do not resolve symbolic links.

=head2 Continuation-based S-expressions

C<sexpr> is C<struct ASDLSexpr>. Initialize with
C<ASDLSexpr_init(reader, handler)> and call
C<ASDLSexpr_feed(reader, bytes, length, final, consumed*)>. Input may split at
any byte boundary, including within strings, escapes, atoms and comments.
Reader state is the saved continuation; no whole input buffer is required.
Events are OPEN, CLOSE, ATOM, STRING, QUOTE and EOF using the event types
described below. Strings decode C<\n>, C<\r>, C<\t>, C<\\> and C<\">.
Semicolon comments end at newline. Atom text is uninterpreted, so numeric,
boolean and dotted-pair policies belong to the application. List nesting,
string termination and dangling quotes are checked.

Callbacks return CONTINUE, PAUSE or ERROR. After PAUSE, resume with the
unconsumed suffix given by C<consumed>; the event causing the pause has
already been delivered. With C<final=true>, an atom event may pause before
EOF; resume with an empty final chunk. EOF is delivered once. Event token
storage is borrowed only during the callback. ERROR makes the reader sticky
until C<ASDLSexpr_reset>. C<ASDLSexpr_destroy> frees token storage.

C<ASDLSexpr_write> is a handler callback whose context is an C<ASDLPort *>.
It writes canonical whitespace-separated events, escaping strings and
preserving binary string contents. A reader can stream directly to a writer
using C<{ ASDLSexpr_write, &output_port }> as its handler. An application
handler can write one event and then return PAUSE to control scheduling.

=head2 Regular expressions and parser combinators

C<regexp> is C<struct ASDLRegexp>. Zero-initialize or use C<ASDLRegexp_init>;
C<_compile(regexp, pattern, flags)> uses POSIX C<regcomp>, C<_exec(regexp,
text, count, matches, flags)> exposes captures and C<regexec> status,
C<_test(regexp, text)> returns a boolean, C<_error(regexp, buffer, size)>
formats the latest status, and C<_destroy> calls C<regfree>.
C<_replace(regexp, text, replacement, output_buffer)> replaces the first
match with literal text. Use a pattern compiled without C<REG_NOSUB> for
captures/replacement. Inputs are NUL-terminated, as required by C<regex.h>.

C<parsec> is C<struct ASDLParsec>, owning combinator nodes and named rules.
Initialize/destroy with C<ASDLParsec_init> and C<_destroy>. Compile EBNF-like
grammars with C<ASDLParsec_compile(parser, grammar, optional_start_rule)>:

  root = '(' item { ',' item } ')';
  item = 'a' | '(' item ')';

Rules use C<name = expression;>. Quoted literals, references, grouping,
ordered choice C<|>, concatenation (optional comma), optional C<[...]>,
repetition C<{...}>, postfix C<?/*/+>, and any byte C<.> are supported.
Grammar whitespace is ignored; input whitespace must be matched explicitly.
Forward references are resolved after compilation. The first rule is the
default start. A failed compile preserves the previous grammar and sets
C<error> and C<error_offset>.

The API offers C<ASDLParsec_literal(parser, bytes, length)>, C<_range(parser,
low, high)>, C<_any>, C<_end>, C<_sequence(parser, left, right)>,
C<_choice(parser, left, right)>, C<_repeat(parser, child, minimum, maximum)>,
C<_ref(parser, name)> and C<_rule(parser, name, node)>. Nodes belong to their
parser. Set C<parser.root> directly for an unnamed API grammar, or use the
first named rule. C<SIZE_MAX> is the unbounded repetition maximum.

C<ASDLParsec_match(parser, bytes, length, complete, consumed*)> returns 1
for success, 0 for mismatch and -1 for an invalid grammar or resource limit.
C<complete> requires all input to match. Choice is ordered and repetition
greedy/possessive (PEG semantics); it does not enumerate ambiguous EBNF parses.
C<max_depth> defaults to 256 and C<max_steps> to 1,000,000. Left recursion
terminates with a resource error; unbounded zero-width repetitions are errors.
No automatic parse tree is built. C<error_offset> reports the furthest failing
input position, or the grammar position after a compile error.

=head1 FILE LISTENERS

C<--listener events.h> emits C<struct demo_listener> and functions
C<demo_listener_init>, C<_open(listener, path)>, C<_add(listener, handler)>,
C<_remove(listener, handler_index)>, C<_poll(listener, final)> and C<_close>.
Initialize/open once and close before reuse. The file must be a regular file;
polling maps its current contents read-only and remaps when size changes.
An empty file is supported without a zero-length mapping.

The event framing is newline-delimited byte records. DATA includes the
newline, and incomplete trailing records wait for a subsequent poll.
C<final=true> flushes a trailing record and emits EOF once for the current
size. A later append resumes reading; truncation detected between polls
resets the offset and emits RESET. Same-size rewrites and path replacement
are not detected: the listener follows its open descriptor. Polling is
synchronous; the caller supplies an event loop or scheduling policy.

  struct ASDLEventHandler handler = { on_event, context };
  /* int on_event(void *context, const struct ASDLEvent *event); */

An event has C<kind>, borrowed C<data>, C<length>, absolute byte C<offset>,
and C<depth> (used by the S-expression reader). Handlers run in registration
order. Return C<ASDL_EVENT_CONTINUE> (0), C<ASDL_EVENT_PAUSE> (1) or
C<ASDL_EVENT_ERROR> (-1). Poll returns the same status. PAUSE saves the next
handler, so resuming does not redeliver events to earlier handlers. ERROR is
sticky; inspect C<listener.error> (errno-style). Do not add/remove handlers
inside a callback or while paused. Close releases mappings, handlers and fd.

Mapped event bytes must be copied if needed after the next poll or close.
The producer must not truncate or modify mapped bytes during dispatch or a
paused event; coordinate such changes between completed polls. This transport
does not imply an ASDL binary serialization format: applications can feed
records to their chosen decoder or to the supplied S-expression reader.

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

This implementation supplies the paper's description language and a C11
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

The tests generate, compile and execute C11 programs, including multiple
translation units, visitor passes, runtime operations, listeners, streaming
readers, parser combinators, invalid inputs and allocator failure.
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

my $VERSION = '2.0';
my %SCALAR = (int => 'intmax_t', uint => 'uintmax_t', bool => 'bool',
    real => 'long double', size => 'ssize_t', usize => 'size_t');
my %BUILTIN = (list => 'ASDLVector', array => 'ASDLFixedArray', map => 'ASDLHashMap',
    port => 'ASDLPort', path => 'ASDLPath', buffer => 'ASDLByteBuffer',
    sexpr => 'ASDLSexpr', regexp => 'ASDLRegexp', parsec => 'ASDLParsec',
    event => 'ASDLEventHandler');
my %PRIMITIVE = map { $_ => 1 } (keys %SCALAR, qw(string identifier), keys %BUILTIN);
my %C_KEYWORD = map { $_ => 1 } qw(auto break case char const continue default
    do double else enum extern float for goto if inline int long register
    restrict return short signed sizeof static struct switch typedef union
    unsigned void volatile while _Bool _Complex _Imaginary _Alignas _Alignof
    _Atomic _Generic _Noreturn _Static_assert _Thread_local);
my %C_FIELD_MACRO = map { $_ => 1 } qw(NULL EXIT_SUCCESS EXIT_FAILURE RAND_MAX
    MB_CUR_MAX SIZE_MAX PTRDIFF_MIN PTRDIFF_MAX SIG_ATOMIC_MIN SIG_ATOMIC_MAX
    WCHAR_MIN WCHAR_MAX WINT_MIN WINT_MAX bool true false alignas alignof
    EOF stdin stdout stderr errno);
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
    } elsif ($text =~ /\G([0-9]+)/gc) {
        ($kind, $value) = ('number', $1);
    } elsif ($text =~ /\G([{}()\[\]=|,+*?;])/gc) {
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
    my %bool = map { $_ => 1 } qw(enums line-directives header-only builtins);
    my %string = map { $_ => 1 } qw(prefix guard header source visitor listener include schema builtins-header omit-builtin);
    if ($name eq 'no-builtin') { $name = 'omit-builtin'; }
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
        if ($name eq 'omit-builtin') {
            $options->{$name} = join(',', grep { defined && length } $options->{$name}, $value);
        } else { $options->{$name} = $value; }
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

sub parse_type {
    my ($p) = @_;
    my $token = expect_id($p, 'lower', 'a lowercase field type');
    my $name = $token->{value};
    return $token unless $name =~ /\A(?:list|array|map)\z/;
    expect($p, '[');
    my @args;
    if ($name ne 'array' || peek($p)->{kind} ne 'number') {
        push @args, parse_type($p)->{value};
        expect($p, ',') if $name ne 'list';
    }
    if ($name eq 'map') { push @args, parse_type($p)->{value}; }
    if ($name eq 'array') {
        my $n = take($p);
        fail($n, 'array size must be a positive decimal integer')
            unless $n->{kind} eq 'number' && $n->{value} =~ /[1-9]/;
        $n->{value} =~ s/^0+//;
        fail($n, 'array size is too large') if length($n->{value}) > 18;
        push @args, $n->{value};
    }
    expect($p, ']');
    my $type = $name . '[' . join(',', @args) . ']';
    $p->{generics}{$type} = { kind => $name, args => \@args, location => $token };
    return { %$token, value => $type };
}

sub parse_fields {
    my ($p) = @_;
    expect($p, '(');
    my @fields;
    while (1) {
        my $type = parse_type($p);
        my $qualifier = '';
        $qualifier = take($p)->{value} if peek($p)->{value} =~ /\A[?*+]\z/;
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
    my $p = { tokens => $g->{tokens}, index => 0, generics => {} };
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
    $g->{generics} = $p->{generics};
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
            my $name = type_stem($f->{type}) . $i;
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
                for my $type (type_dependencies($g, $f->{type})) {
                    fail($f->{location}, "undefined type '$type'") unless $PRIMITIVE{$type} || $types{$type};
                    $g->{used_builtins}{$type} = 1 if $PRIMITIVE{$type};
                }
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
                    for my $type (type_dependencies($g, $f->{type})) {
                        next if $PRIMITIVE{$type};
                        fail($f->{location}, 'recursive product type: ' . join(' -> ', @$path, $type))
                            if $type eq $root->{name};
                        $visit->($types{$type}, [@$path, $type]);
                    }
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

sub type_stem { my $s = $_[0]; $s =~ s/[^A-Za-z0-9_]+/_/g; $s =~ s/_$//; return $s; }
sub type_dependencies {
    my ($g, $type) = @_;
    my $generic = $g->{generics}{$type};
    return ($type) unless $generic;
    return ($generic->{kind}, map { type_dependencies($g, $_) }
        grep { !/^\d+$/ } @{$generic->{args}});
}
sub c_type { return $_[0]{prefix} . type_stem($_[1]) . '_t'; }
sub field_type {
    my ($g, $f) = @_;
    return $g->{prefix} . type_stem($f->{type}) . ($f->{qualifier} =~ /^[*+]$/ ? '_seq'
        : $f->{qualifier} eq '?' ? '_opt' : '_t');
}
sub tag_name {
    my ($g, $d, $c) = @_;
    return $g->{prefix} . $c->{name} . ($d->{enum} ? '' : '_kind');
}
sub primitive_types {
    my ($g) = @_;
    return (qw(int string identifier), sort grep { !/^(?:int|string|identifier|list|array|map)$/ }
        keys %{$g->{used_builtins} // {}});
}
sub all_types { return (primitive_types($_[0]), sort keys %{$_[0]{generics}}, map { $_->{name} } @{$_[0]{definitions}}); }

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
    return '1' if $SCALAR{$type};
    if (my $generic = $g->{generics}{$type}) {
        my $kind = $generic->{kind};
        if ($kind eq 'map') {
            return "(($v).key_size == sizeof(" . c_type($g, $generic->{args}[0])
                . ") && ($v).value_size == sizeof(" . c_type($g, $generic->{args}[1])
                . ") && ($v).hash && ($v).length <= ($v).capacity"
                . " && ($v).capacity <= SIZE_MAX / sizeof(*($v).entries) && (!($v).capacity || ($v).entries))";
        }
        my $t = @{$generic->{args}} == 1 && $kind eq 'array' ? 'unsigned char' : c_type($g, $generic->{args}[0]);
        my $test = "(($v).element_size == sizeof($t) && ($v).length <= SIZE_MAX / sizeof($t) && (!($v).length || ($v).data)";
        $test .= $kind eq 'array' ? " && ($v).length == $generic->{args}[-1]" : " && ($v).length <= ($v).capacity";
        return "$test)";
    }
    return '1' if $BUILTIN{$type};
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
    register_symbol($g, $p . $_, 'runtime type') for qw(error visit_result block intern arena string_t identifier_t);
    my $aliases = '';
    for my $name (primitive_types($g)) {
        next if $name eq 'string' || $name eq 'identifier';
        my $type = $SCALAR{$name} // "struct $BUILTIN{$name}";
        register_symbol($g, c_type($g, $name), 'builtin type');
        $aliases .= 'typedef ' . $type . ' ' . c_type($g, $name) . ";\n";
    }
    register_symbol($g, $u . $_, 'runtime constant') for qw(OK OOM INVALID OVERFLOW WALK_CONTINUE WALK_SKIP WALK_STOP WALK_ERROR);
    register_symbol($g, $u . $_, 'configuration macro') for qw(MALLOC FREE VISITOR_NAME
        VISITOR_CONTEXT VISITOR_MAX_DEPTH PRIVATE_CAT_I PRIVATE_CAT PRIVATE_FN
        PRIVATE_DEFAULT_CONTEXT PRIVATE_DEFAULT_DEPTH);
    return $aliases . template($g, <<'C');
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
    max_align_t alignment;
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
    for my $name (sort keys %{$g->{generics}}) {
        my $type = c_type($g, $name);
        register_symbol($g, $type, "generic $name");
        $out .= "typedef struct $BUILTIN{$g->{generics}{$name}{kind}} $type;\n";
    }
    $out .= "\n/* Typed collections and explicit presence, including scalar options. */\n";
    for my $name (all_types($g)) {
        my $t = c_type($g, $name);
        my $stem = $p . type_stem($name);
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
        my $stem = $p . type_stem($name);
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
    my $leaf = $operation;
    $operation = sub { value_operation($g, $invalid, $leaf, @_) };
    my $q = $f->{qualifier};
    if ($q eq '?') {
        return "$indent" . "if (($expr).has_value != 0 && ($expr).has_value != 1) { $invalid }\n"
            . "$indent" . "if (($expr).has_value) {\n"
            . $operation->($f->{type}, "($expr).value", $indent . '    ')
            . "$indent}\n";
    }
    if ($q eq '*' || $q eq '+') {
        return ($q eq '+' ? "${indent}if (!($expr).length) { $invalid }\n" : '')
            . "$indent" . "if (($expr).length && !($expr).items) { $invalid }\n"
            . "$indent" . "if (($expr).length > SIZE_MAX / sizeof(*($expr).items)) { $invalid }\n"
            . "$indent\{\n$indent    size_t index;\n"
            . "$indent    for (index = 0; index < ($expr).length; ++index) {\n"
            . $operation->($f->{type}, "($expr).items[index]", $indent . '        ')
            . "$indent    }\n$indent}\n";
    }
    return $operation->($f->{type}, $expr, $indent);
}

sub value_operation {
    my ($g, $invalid, $operation, $type, $value, $indent) = @_;
    my $generic = $g->{generics}{$type};
    return $operation->($type, $value, $indent) unless $generic;
    my $test = shallow_test($g, $type, $value);
    my $out = "${indent}if (!($test)) { $invalid }\n${indent}\{\n";
    my $kind = $generic->{kind};
    my @args = @{$generic->{args}};
    if ($kind eq 'array' && @args == 1) { return $out . "$indent}\n"; }
    my $limit = $kind eq 'map' ? 'capacity' : 'length';
    $out .= "${indent}    size_t occupied = 0;\n" if $kind eq 'map';
    $out .= "${indent}    size_t element;\n${indent}    for (element = 0; element < ($value).$limit; ++element) {\n";
    if ($kind eq 'map') {
        $out .= "${indent}        if (($value).entries[element].state > 2) { $invalid }\n"
              . "${indent}        if (!(($value).entries[element].state == 1)) continue;\n"
              . "${indent}        ++occupied;\n";
    } else { @args = ($args[0]); }
    for my $i (0 .. $#args) {
        my $t = c_type($g, $args[$i]);
        my $item = 'item_' . length($indent);
        my $data = $kind eq 'map' ? "($value).entries[element]." . ($i ? 'value' : 'key')
            : "(const unsigned char *)($value).data + element * sizeof($t)";
        $out .= "${indent}        {\n${indent}            $t $item;\n"
              . ($kind eq 'map' ? "${indent}            if (!($data)) { $invalid }\n" : '')
              . "${indent}            memcpy(&$item, $data, sizeof($item));\n"
              . "${indent}            (void)$item;\n"
              . value_operation($g, $invalid, $operation, $args[$i], $item, $indent . '            ')
              . "${indent}        }\n";
    }
    $out .= "${indent}    }\n";
    $out .= "${indent}    if (occupied != ($value).length) { $invalid }\n" if $kind eq 'map';
    return $out . "$indent}\n";
}

sub add_generic_functions {
    my ($g) = @_;
    for my $name (sort keys %{$g->{generics}}) {
        my $spec = $g->{generics}{$name};
        my ($kind, $args) = @{$spec}{qw(kind args)};
        my $stem = $g->{prefix} . type_stem($name);
        my $t = c_type($g, $name);
        my $element = $kind eq 'array' && @$args == 1 ? 'unsigned char' : c_type($g, $args->[0]);
        if ($kind eq 'map') {
            my $v = c_type($g, $args->[1]);
            my $hash = type_stem($args->[0]) . '_hashfn';
            add_function($g, "size_t ${stem}_hash(const void *key)",
                "    $element value;\n    memcpy(&value, key, sizeof(value));\n    return (size_t)$hash(value);\n");
            add_function($g, "$t ${stem}_new(int (*equal)(const void *, const void *))",
                "    $t result;\n    ASDLHashMap_init(&result, sizeof($element), sizeof($v), ${stem}_hash, equal);\n    return result;\n");
            add_function($g, "bool ${stem}_set($t *map, $element key, $v value)",
                "    return map && map->key_size == sizeof(key) && map->value_size == sizeof(value)\n"
                . "        && ASDLHashMap_set(map, &key, &value);\n");
            add_function($g, "$v *${stem}_get(const $t *map, $element key)",
                "    return map && map->key_size == sizeof(key) && map->value_size == sizeof($v)\n"
                . "        ? ($v *)ASDLHashMap_get(map, &key) : NULL;\n");
            add_function($g, "bool ${stem}_remove($t *map, $element key)",
                "    return map && map->key_size == sizeof(key) && map->value_size == sizeof($v)\n"
                . "        && ASDLHashMap_remove(map, &key);\n");
        } else {
            my $runtime = $BUILTIN{$kind};
            my $init = $kind eq 'list' ? "$runtime\_init(&result, sizeof($element));"
                : "$runtime\_init(&result, sizeof($element), $args->[-1]);";
            add_function($g, "$t ${stem}_new(void)", "    $t result = {0};\n    $init\n    return result;\n");
            add_function($g, "$element *${stem}_at($t *collection, size_t index)",
                "    return collection && collection->element_size == sizeof($element)\n"
                . "        ? ($element *)${runtime}_at(collection, index) : NULL;\n");
            add_function($g, "bool ${stem}_set($t *collection, size_t index, $element value)",
                "    return collection && collection->element_size == sizeof(value)\n"
                . "        && ${runtime}_set(collection, index, &value);\n");
            add_function($g, "bool ${stem}_push($t *collection, $element value)",
                "    return collection && collection->element_size == sizeof(value)\n"
                . "        && ASDLVector_push(collection, &value);\n") if $kind eq 'list';
        }
    }
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
        die "--visitor/--listener requires --header or --include when the header is written to stdout\n"
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
    add_generic_functions($g);
    add_collection_functions($g);
    add_builders($g);
    add_validators($g);
    my $linkage = $o->{'header-only'} ? 'static inline ' : '';
    my $prototypes = join '', map { "$linkage$_->{signature};\n" } @{$g->{functions}};
    my $bodies = join "\n", map { "$linkage$_->{signature}\n{\n$_->{body}}\n" } @{$g->{functions}};
    my $builtins = emit_builtins($g);
    if (defined $o->{'builtins-header'}) {
        $g->{builtins_output} = $builtins;
        my $path = File::Spec->abs2rel(File::Spec->rel2abs($o->{'builtins-header'}),
            File::Spec->rel2abs(dirname($o->{header} // '.')));
        die "invalid quoted C include path '$path'\n" if $path =~ /["\\\r\n\0]/;
        $builtins = '#include ' . c_string($path) . "\n";
    }
    my $header = "/* Generated by asdl.pl $VERSION. C11; edit the ASDL input. */\n"
        . "#ifndef $g->{guard}\n#define $g->{guard}\n\n"
        . "#include <stddef.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdbool.h>\n#include <sys/types.h>\n"
        . injected_c($g, $g->{prologue}, $o->{header} // '<asdl-header>')
        . $builtins
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
        primitives => [sort keys %PRIMITIVE],
        generics => { map { $_ => { %{$g->{generics}{$_}}, c_type => c_type($g, $_) } } sort keys %{$g->{generics}} },
        types => \@types,
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
  --listener FILE        mmap event listener header
  --no-builtins          omit all ASDL runtime definitions
  --omit-builtin NAME    omit one builtin (repeatable, or comma-separated)
  --builtins-header FILE emit ASDL runtime in a separate included header
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
        'listener=s' => \$cli{listener}, 'builtins!' => \$cli{builtins},
        'builtins-header=s' => \$cli{'builtins-header'},
        'omit-builtin|no-builtin=s' => sub { $cli{'omit-builtin'} = join(',', grep { defined } $cli{'omit-builtin'}, $_[1]); },
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
    my %options = (enums => 1, 'line-directives' => 1, 'header-only' => 1, builtins => 1,
        %{$g->{options}}, map { $_ => $cli{$_} } grep { defined $cli{$_} } keys %cli);
    for my $key (qw(header source visitor listener include prefix guard schema builtins-header omit-builtin)) {
        die "--$key requires a nonempty value\n" if defined $options{$key} && !length $options{$key};
    }
    if ($options{'header-only'}) { delete $options{source}; }
    else {
        die "split output requires --source FILE and --header FILE\n"
            unless defined $options{source} && defined $options{header}
                && $options{header} ne '-' && $options{source} ne '-';
    }
    die "--visitor requires a filename, not stdout\n" if defined $options{visitor} && $options{visitor} eq '-';
    for my $key (qw(listener builtins-header)) {
        die "--$key requires a filename, not stdout\n" if defined $options{$key} && $options{$key} eq '-';
    }
    my %omitted;
    for my $name (split /,/, $options{'omit-builtin'} // '') {
        my ($key) = grep { $_ eq $name || $BUILTIN{$_} eq $name } keys %BUILTIN;
        die "unknown builtin '$name'\n" unless defined $key;
        $omitted{$key} = 1;
    }
    $g->{omitted} = \%omitted;
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
        push @outputs, [$options{listener}, emit_listener($g, $options{listener})] if defined $options{listener};
        push @outputs, [$options{'builtins-header'}, $g->{builtins_output}] if defined $options{'builtins-header'};
    }
    push @outputs, [$options{schema}, emit_schema($g)] if defined $options{schema};
    publish_outputs($file, \@outputs);
}

# Runtime sections carry independent guards so separately generated ASTs can
# share the same ABI, and applications can replace individual components.
sub runtime_sections {
    state $sections;
    unless ($sections) {
        local $/;
        my $text = <DATA>;
        my @parts = split /^@@ ([a-z]+)\n/m, $text;
        shift @parts;
        $sections = { @parts };
    }
    return $sections;
}

sub emit_builtins {
    my ($g) = @_;
    return '' unless $g->{options}{builtins};
    my $sections = runtime_sections();
    my $out = $sections->{common};
    for my $name (qw(list array map buffer port path event sexpr regexp parsec)) {
        next if $g->{omitted}{$name};
        $out .= $sections->{$name};
    }
    return $out;
}

sub emit_listener {
    my ($g, $destination) = @_;
    return header_include($g, $destination) . template($g, runtime_sections()->{listener});
}

unless (caller) {
    my $ok = eval { main(@ARGV); 1 };
    if (!$ok) { print STDERR "asdl.pl: $@"; exit 1; }
}

__DATA__
@@ common
#ifndef ASDL_RUNTIME_COMMON_H
#define ASDL_RUNTIME_COMMON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef ASDL_MALLOC
#define ASDL_MALLOC(n) malloc(n)
#endif
#ifndef ASDL_FREE
#define ASDL_FREE(p) free(p)
#endif
static inline char *ASDL_copy_text(const char *text, size_t length) {
    if ((!text && length) || length == SIZE_MAX) return NULL;
    char *copy = (char *)ASDL_MALLOC(length + 1);
    if (copy) {
        if (length) memcpy(copy, text, length);
        copy[length] = 0;
    }
    return copy;
}
#endif
@@ list
#ifndef ASDL_VECTOR_H
#define ASDL_VECTOR_H
struct ASDLVector { void *data; size_t length, capacity, element_size; };
static inline void ASDLVector_init(struct ASDLVector *v, size_t element_size) {
    if (v) *v = (struct ASDLVector){NULL, 0, 0, element_size};
}
static inline void ASDLVector_destroy(struct ASDLVector *v) {
    if (v) { ASDL_FREE(v->data); ASDLVector_init(v, v->element_size); }
}
static inline bool ASDLVector_reserve(struct ASDLVector *v, size_t capacity) {
    if (!v || !v->element_size || capacity > SIZE_MAX / v->element_size) return false;
    if (capacity <= v->capacity) return true;
    void *data = ASDL_MALLOC(capacity * v->element_size);
    if (!data) return false;
    if (v->length) memcpy(data, v->data, v->length * v->element_size);
    ASDL_FREE(v->data);
    v->data = data;
    v->capacity = capacity;
    return true;
}
static inline void *ASDLVector_at(const struct ASDLVector *v, size_t index) {
    return v && index < v->length ? (unsigned char *)v->data + index * v->element_size : NULL;
}
static inline bool ASDLVector_set(struct ASDLVector *v, size_t index, const void *value) {
    void *slot = ASDLVector_at(v, index);
    if (!slot || !value) return false;
    memmove(slot, value, v->element_size);
    return true;
}
static inline bool ASDLVector_resize(struct ASDLVector *v, size_t length) {
    if (!ASDLVector_reserve(v, length)) return false;
    if (length > v->length) memset((unsigned char *)v->data + v->length * v->element_size,
        0, (length - v->length) * v->element_size);
    v->length = length;
    return true;
}
static inline bool ASDLVector_insert(struct ASDLVector *v, size_t index, const void *value) {
    if (!v || !value || !v->element_size || index > v->length || v->length == SIZE_MAX) return false;
    /* Snapshot first: value may point into storage moved by reserve/memmove. */
    void *copy = ASDL_MALLOC(v->element_size);
    if (!copy) return false;
    memcpy(copy, value, v->element_size);
    size_t capacity = v->capacity;
    if (capacity <= v->length) capacity = capacity && capacity <= SIZE_MAX / 2 ? capacity * 2 : v->length + 1;
    if (!ASDLVector_reserve(v, capacity)) { ASDL_FREE(copy); return false; }
    unsigned char *slot = (unsigned char *)v->data + index * v->element_size;
    memmove(slot + v->element_size, slot, (v->length - index) * v->element_size);
    memcpy(slot, copy, v->element_size);
    ASDL_FREE(copy);
    ++v->length;
    return true;
}
static inline bool ASDLVector_push(struct ASDLVector *v, const void *value) {
    return v && ASDLVector_insert(v, v->length, value);
}
static inline bool ASDLVector_erase(struct ASDLVector *v, size_t index, size_t count) {
    if (!v || index > v->length || count > v->length - index) return false;
    if (count) {
        unsigned char *slot = (unsigned char *)v->data + index * v->element_size;
        memmove(slot, slot + count * v->element_size, (v->length - index - count) * v->element_size);
        v->length -= count;
    }
    return true;
}
static inline bool ASDLVector_pop(struct ASDLVector *v, void *value) {
    if (!v || !v->length) return false;
    if (value) memmove(value, ASDLVector_at(v, v->length - 1), v->element_size);
    --v->length;
    return true;
}
static inline void ASDLVector_clear(struct ASDLVector *v) { if (v) v->length = 0; }
static inline void ASDLVector_sort(struct ASDLVector *v, int (*compare)(const void *, const void *)) {
    if (v && v->length > 1 && compare) qsort(v->data, v->length, v->element_size, compare);
}
static inline void ASDLVector_reverse(struct ASDLVector *v) {
    if (!v) return;
    for (size_t i = 0; i < v->length / 2; ++i) {
        unsigned char *a = (unsigned char *)ASDLVector_at(v, i);
        unsigned char *b = (unsigned char *)ASDLVector_at(v, v->length - i - 1);
        for (size_t j = 0; j < v->element_size; ++j) { unsigned char c = a[j]; a[j] = b[j]; b[j] = c; }
    }
}
#endif
@@ array
#ifndef ASDL_FIXED_ARRAY_H
#define ASDL_FIXED_ARRAY_H
struct ASDLFixedArray { void *data; size_t length, element_size; };
static inline bool ASDLFixedArray_init(struct ASDLFixedArray *a, size_t element_size, size_t length) {
    if (!a) return false;
    *a = (struct ASDLFixedArray){NULL, 0, element_size};
    if (!element_size || !length || length > SIZE_MAX / element_size) return false;
    a->data = ASDL_MALLOC(length * element_size);
    if (!a->data) return false;
    memset(a->data, 0, length * element_size);
    a->length = length;
    return true;
}
static inline void ASDLFixedArray_destroy(struct ASDLFixedArray *a) {
    if (a) { ASDL_FREE(a->data); *a = (struct ASDLFixedArray){0}; }
}
static inline void *ASDLFixedArray_at(const struct ASDLFixedArray *a, size_t index) {
    return a && index < a->length ? (unsigned char *)a->data + index * a->element_size : NULL;
}
static inline bool ASDLFixedArray_set(struct ASDLFixedArray *a, size_t index, const void *value) {
    void *slot = ASDLFixedArray_at(a, index);
    if (!slot || !value) return false;
    memmove(slot, value, a->element_size);
    return true;
}
static inline bool ASDLFixedArray_fill(struct ASDLFixedArray *a, const void *value) {
    if (!a || !a->data || !value) return false;
    /* First slot snapshots a possibly aliased element. */
    memmove(a->data, value, a->element_size);
    for (size_t i = 1; i < a->length; ++i) memcpy(ASDLFixedArray_at(a, i), a->data, a->element_size);
    return true;
}
static inline bool ASDLFixedArray_copy(struct ASDLFixedArray *to, const struct ASDLFixedArray *from) {
    if (!to || !from || to->length != from->length || to->element_size != from->element_size) return false;
    if (to->length) memmove(to->data, from->data, to->length * to->element_size);
    return true;
}
static inline void ASDLFixedArray_sort(struct ASDLFixedArray *a, int (*compare)(const void *, const void *)) {
    if (a && a->length > 1 && compare) qsort(a->data, a->length, a->element_size, compare);
}
#endif
@@ map
#ifndef ASDL_HASH_MAP_H
#define ASDL_HASH_MAP_H
struct ASDLHashEntry { void *key, *value; size_t hash; unsigned char state; };
struct ASDLHashMap {
    struct ASDLHashEntry *entries;
    size_t length, capacity, key_size, value_size;
    size_t (*hash)(const void *);
    int (*equal)(const void *, const void *);
};
static inline void ASDLHashMap_init(struct ASDLHashMap *m, size_t key_size, size_t value_size,
        size_t (*hash)(const void *), int (*equal)(const void *, const void *)) {
    if (m) *m = (struct ASDLHashMap){NULL, 0, 0, key_size, value_size, hash, equal};
}
static inline void ASDLHashMap_clear(struct ASDLHashMap *m) {
    if (!m) return;
    for (size_t i = 0; i < m->capacity; ++i) {
        if (m->entries[i].state == 1) { ASDL_FREE(m->entries[i].key); ASDL_FREE(m->entries[i].value); }
        m->entries[i] = (struct ASDLHashEntry){0};
    }
    m->length = 0;
}
static inline void ASDLHashMap_destroy(struct ASDLHashMap *m) {
    if (!m) return;
    ASDLHashMap_clear(m);
    ASDL_FREE(m->entries);
    m->entries = NULL;
    m->capacity = 0;
}
static inline bool ASDLHashMap_reserve(struct ASDLHashMap *m, size_t capacity) {
    if (!m || !m->key_size || !m->value_size || !m->hash) return false;
    if (capacity <= m->capacity) return true;
    if (capacity > SIZE_MAX / sizeof(*m->entries)) return false;
    struct ASDLHashEntry *entries = (struct ASDLHashEntry *)ASDL_MALLOC(capacity * sizeof(*entries));
    if (!entries) return false;
    for (size_t i = 0; i < capacity; ++i) entries[i] = (struct ASDLHashEntry){0};
    for (size_t i = 0; i < m->capacity; ++i) if (m->entries[i].state == 1) {
        size_t j = m->entries[i].hash % capacity;
        while (entries[j].state) j = j + 1 == capacity ? 0 : j + 1;
        entries[j] = m->entries[i];
    }
    ASDL_FREE(m->entries);
    m->entries = entries;
    m->capacity = capacity;
    return true;
}
static inline size_t ASDLHashMap_find(const struct ASDLHashMap *m, const void *key, size_t hash) {
    if (!m || !m->capacity || !key) return SIZE_MAX;
    size_t index = hash % m->capacity;
    for (size_t n = 0; n < m->capacity; ++n) {
        const struct ASDLHashEntry *e = &m->entries[index];
        if (!e->state) return SIZE_MAX;
        if (e->state == 1 && e->hash == hash && (m->equal ? m->equal(e->key, key)
                : memcmp(e->key, key, m->key_size) == 0)) return index;
        index = index + 1 == m->capacity ? 0 : index + 1;
    }
    return SIZE_MAX;
}
static inline void *ASDLHashMap_get(const struct ASDLHashMap *m, const void *key) {
    if (!m || !m->hash || !key) return NULL;
    size_t index = ASDLHashMap_find(m, key, m->hash(key));
    return index == SIZE_MAX ? NULL : m->entries[index].value;
}
static inline bool ASDLHashMap_contains(const struct ASDLHashMap *m, const void *key) {
    return ASDLHashMap_get(m, key) != NULL;
}
static inline bool ASDLHashMap_set(struct ASDLHashMap *m, const void *key, const void *value) {
    if (!m || !m->hash || !key || !value || !m->key_size || !m->value_size) return false;
    size_t hash = m->hash(key), index = ASDLHashMap_find(m, key, hash);
    if (index != SIZE_MAX) { memmove(m->entries[index].value, value, m->value_size); return true; }
    void *k = ASDL_MALLOC(m->key_size), *v = ASDL_MALLOC(m->value_size);
    if (!k || !v) { ASDL_FREE(k); ASDL_FREE(v); return false; }
    memcpy(k, key, m->key_size);
    memcpy(v, value, m->value_size);
    if (m->length >= m->capacity / 2) {
        size_t capacity = m->capacity ? m->capacity * 2 : 16;
        if (capacity < m->capacity || !ASDLHashMap_reserve(m, capacity)) {
            ASDL_FREE(k); ASDL_FREE(v); return false;
        }
    }
    index = hash % m->capacity;
    while (m->entries[index].state == 1) index = index + 1 == m->capacity ? 0 : index + 1;
    m->entries[index] = (struct ASDLHashEntry){k, v, hash, 1};
    ++m->length;
    return true;
}
static inline bool ASDLHashMap_remove(struct ASDLHashMap *m, const void *key) {
    if (!m || !m->hash || !key) return false;
    size_t index = ASDLHashMap_find(m, key, m->hash(key));
    if (index == SIZE_MAX) return false;
    ASDL_FREE(m->entries[index].key);
    ASDL_FREE(m->entries[index].value);
    m->entries[index] = (struct ASDLHashEntry){NULL, NULL, 0, 2};
    --m->length;
    return true;
}
/* Iteration: initialize cursor to zero; returned pointers borrow map storage. */
static inline bool ASDLHashMap_next(const struct ASDLHashMap *m, size_t *cursor, const void **key, void **value) {
    if (!m || !cursor) return false;
    while (*cursor < m->capacity) {
        const struct ASDLHashEntry *e = &m->entries[(*cursor)++];
        if (e->state == 1) { if (key) *key = e->key; if (value) *value = e->value; return true; }
    }
    return false;
}
#endif
@@ buffer
#ifndef ASDL_BYTE_BUFFER_H
#define ASDL_BYTE_BUFFER_H
struct ASDLByteBuffer { unsigned char *data; size_t length, capacity, position; };
static inline void ASDLByteBuffer_init(struct ASDLByteBuffer *b) { if (b) *b = (struct ASDLByteBuffer){0}; }
static inline void ASDLByteBuffer_destroy(struct ASDLByteBuffer *b) {
    if (b) { ASDL_FREE(b->data); ASDLByteBuffer_init(b); }
}
static inline bool ASDLByteBuffer_reserve(struct ASDLByteBuffer *b, size_t capacity) {
    if (!b) return false;
    if (capacity <= b->capacity) return true;
    unsigned char *data = (unsigned char *)ASDL_MALLOC(capacity);
    if (!data) return false;
    if (b->length) memcpy(data, b->data, b->length);
    ASDL_FREE(b->data);
    b->data = data; b->capacity = capacity;
    return true;
}
static inline bool ASDLByteBuffer_append(struct ASDLByteBuffer *b, const void *data, size_t length) {
    if (!b || (length && !data) || length > SIZE_MAX - b->length) return false;
    if (!length) return true;
    /* Snapshot permits self-append without comparing unrelated pointers. */
    unsigned char *copy = (unsigned char *)ASDL_MALLOC(length);
    if (!copy) return false;
    memcpy(copy, data, length);
    size_t need = b->length + length, capacity = b->capacity;
    if (capacity < need) capacity = capacity <= SIZE_MAX / 2 && capacity * 2 > need ? capacity * 2 : need;
    if (!ASDLByteBuffer_reserve(b, capacity)) { ASDL_FREE(copy); return false; }
    memcpy(b->data + b->length, copy, length);
    ASDL_FREE(copy);
    b->length = need;
    return true;
}
static inline bool ASDLByteBuffer_put(struct ASDLByteBuffer *b, unsigned char byte) {
    return ASDLByteBuffer_append(b, &byte, 1);
}
static inline size_t ASDLByteBuffer_read(struct ASDLByteBuffer *b, void *data, size_t length) {
    if (!b || !data || b->position > b->length) return 0;
    if (length > b->length - b->position) length = b->length - b->position;
    if (length) memmove(data, b->data + b->position, length);
    b->position += length;
    return length;
}
static inline bool ASDLByteBuffer_seek(struct ASDLByteBuffer *b, size_t position) {
    if (!b || position > b->length) return false;
    b->position = position;
    return true;
}
static inline bool ASDLByteBuffer_truncate(struct ASDLByteBuffer *b, size_t length) {
    if (!b || length > b->length) return false;
    b->length = length;
    if (b->position > length) b->position = length;
    return true;
}
static inline void ASDLByteBuffer_clear(struct ASDLByteBuffer *b) { if (b) b->length = b->position = 0; }
static inline const char *ASDLByteBuffer_cstr(struct ASDLByteBuffer *b) {
    if (!b || b->length == SIZE_MAX || !ASDLByteBuffer_reserve(b, b->length + 1)) return NULL;
    b->data[b->length] = 0;
    return (const char *)b->data;
}
#endif
@@ port
#ifndef ASDL_PORT_H
#define ASDL_PORT_H
struct ASDLPort {
    FILE *file;
    const unsigned char *input;
    size_t length, position;
    struct ASDLByteBuffer output;
    int kind, error;
    bool owned;
};
static inline void ASDLPort_init(struct ASDLPort *p) { if (p) *p = (struct ASDLPort){0}; }
static inline bool ASDLPort_open(struct ASDLPort *p, const char *path, const char *mode) {
    if (!p || !path || !mode) return false;
    ASDLPort_init(p);
    p->file = fopen(path, mode);
    p->kind = 1; p->owned = true;
    if (!p->file) p->error = errno;
    return p->file != NULL;
}
static inline void ASDLPort_file(struct ASDLPort *p, FILE *file, bool owned) {
    if (p) { ASDLPort_init(p); p->kind = 1; p->file = file; p->owned = owned; }
}
static inline bool ASDLPort_input(struct ASDLPort *p, const void *data, size_t length) {
    if (!p || (length && !data)) return false;
    ASDLPort_init(p); p->kind = 2; p->input = (const unsigned char *)data; p->length = length;
    return true;
}
static inline void ASDLPort_output(struct ASDLPort *p) { if (p) { ASDLPort_init(p); p->kind = 3; } }
static inline int ASDLPort_close(struct ASDLPort *p) {
    if (!p) return EOF;
    int status = p->file && p->owned ? fclose(p->file) : 0;
    ASDLByteBuffer_destroy(&p->output); ASDLPort_init(p);
    return status;
}
static inline size_t ASDLPort_read(struct ASDLPort *p, void *data, size_t length) {
    if (!p || !data) return 0;
    if (p->kind == 1 && p->file) {
        size_t count = fread(data, 1, length, p->file);
        if (ferror(p->file)) p->error = EIO;
        return count;
    }
    if (p->kind != 2) { p->error = EBADF; return 0; }
    if (length > p->length - p->position) length = p->length - p->position;
    if (length) memcpy(data, p->input + p->position, length);
    p->position += length;
    return length;
}
static inline size_t ASDLPort_write(struct ASDLPort *p, const void *data, size_t length) {
    if (!p || (length && !data)) return 0;
    if (!length) return 0;
    if (p->kind == 1 && p->file) {
        size_t count = fwrite(data, 1, length, p->file);
        if (count != length) p->error = EIO;
        return count;
    }
    if (p->kind != 3) { p->error = EBADF; return 0; }
    if (!ASDLByteBuffer_append(&p->output, data, length)) { p->error = ENOMEM; return 0; }
    return length;
}
static inline int ASDLPort_get(struct ASDLPort *p) { unsigned char c; return ASDLPort_read(p, &c, 1) ? c : EOF; }
static inline bool ASDLPort_put(struct ASDLPort *p, unsigned char c) { return ASDLPort_write(p, &c, 1) == 1; }
static inline bool ASDLPort_flush(struct ASDLPort *p) {
    if (!p || !p->kind) return false;
    if (p->kind != 1) return true;
    if (!p->file || fflush(p->file)) { p->error = EIO; return false; }
    return true;
}
static inline bool ASDLPort_seek(struct ASDLPort *p, long offset, int whence) {
    if (!p) return false;
    if (p->kind == 1 && p->file) return fseek(p->file, offset, whence) == 0;
    if (p->kind != 2) return false;
    size_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? p->position : p->length;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return false;
    if (offset < 0) {
        uintmax_t magnitude = (uintmax_t)(-(offset + 1)) + 1;
        if (magnitude > base) return false;
        p->position = base - (size_t)magnitude;
    } else {
        if ((uintmax_t)offset > p->length - base) return false;
        p->position = base + (size_t)offset;
    }
    return true;
}
static inline bool ASDLPort_eof(const struct ASDLPort *p) {
    return !p || (p->kind == 1 ? !p->file || feof(p->file) : p->kind == 2 && p->position == p->length);
}
static inline bool ASDLPort_read_line(struct ASDLPort *p, struct ASDLByteBuffer *line) {
    if (!line) return false;
    ASDLByteBuffer_clear(line);
    int c;
    while ((c = ASDLPort_get(p)) != EOF) {
        if (!ASDLByteBuffer_put(line, (unsigned char)c)) return false;
        if (c == '\n') break;
    }
    return line->length != 0;
}
#endif
@@ path
#ifndef ASDL_PATH_H
#define ASDL_PATH_H
struct ASDLPath { char *text; };
static inline bool ASDLPath_init(struct ASDLPath *p, const char *text) {
    if (!p) return false;
    p->text = text ? ASDL_copy_text(text, strlen(text)) : NULL;
    return p->text != NULL;
}
static inline void ASDLPath_destroy(struct ASDLPath *p) { if (p) { ASDL_FREE(p->text); p->text = NULL; } }
static inline bool ASDLPath_is_absolute(const struct ASDLPath *p) { return p && p->text && p->text[0] == '/'; }
static inline bool ASDLPath_join(struct ASDLPath *out, const struct ASDLPath *base, const char *part) {
    if (!out || !base || !base->text || !part) return false;
    if (*part == '/') { char *s = ASDL_copy_text(part, strlen(part)); if (!s) return false; ASDLPath_destroy(out); out->text = s; return true; }
    size_t a = strlen(base->text), b = strlen(part);
    if (a > SIZE_MAX - 2 || b > SIZE_MAX - a - 2) return false;
    char *s = (char *)ASDL_MALLOC(a + b + 2);
    if (!s) return false;
    memcpy(s, base->text, a);
    if (a && s[a - 1] != '/') s[a++] = '/';
    memcpy(s + a, part, b + 1);
    ASDLPath_destroy(out); out->text = s;
    return true;
}
static inline const char *ASDLPath_name(const struct ASDLPath *p) {
    if (!p || !p->text) return NULL;
    const char *slash = strrchr(p->text, '/');
    return slash ? slash + 1 : p->text;
}
static inline const char *ASDLPath_suffix(const struct ASDLPath *p) {
    const char *name = ASDLPath_name(p), *dot = name ? strrchr(name, '.') : NULL;
    return dot && dot != name ? dot : "";
}
static inline bool ASDLPath_parent(struct ASDLPath *out, const struct ASDLPath *p) {
    if (!out || !p || !p->text) return false;
    size_t n = strlen(p->text);
    while (n > 1 && p->text[n - 1] == '/') --n;
    while (n && p->text[n - 1] != '/') --n;
    while (n > 1 && p->text[n - 1] == '/') --n;
    char *s = n ? ASDL_copy_text(p->text, n) : ASDL_copy_text(".", 1);
    if (!s) return false;
    ASDLPath_destroy(out); out->text = s;
    return true;
}
static inline bool ASDLPath_exists(const struct ASDLPath *p) { struct stat st; return p && p->text && stat(p->text, &st) == 0; }
static inline bool ASDLPath_is_file(const struct ASDLPath *p) { struct stat st; return p && p->text && stat(p->text, &st) == 0 && S_ISREG(st.st_mode); }
static inline bool ASDLPath_is_dir(const struct ASDLPath *p) { struct stat st; return p && p->text && stat(p->text, &st) == 0 && S_ISDIR(st.st_mode); }
static inline bool ASDLPath_mkdir(const struct ASDLPath *p, mode_t mode) { return p && p->text && mkdir(p->text, mode) == 0; }
static inline bool ASDLPath_remove(const struct ASDLPath *p) { return p && p->text && remove(p->text) == 0; }
static inline bool ASDLPath_rename(const struct ASDLPath *p, const struct ASDLPath *to) { return p && p->text && to && to->text && rename(p->text, to->text) == 0; }
static inline bool ASDLPath_read(const struct ASDLPath *p, struct ASDLByteBuffer *out) {
    if (!p || !p->text || !out) return false;
    FILE *f = fopen(p->text, "rb");
    if (!f) return false;
    struct ASDLByteBuffer result = {0};
    unsigned char bytes[4096]; size_t n; bool ok = true;
    while ((n = fread(bytes, 1, sizeof(bytes), f)) != 0) if (!ASDLByteBuffer_append(&result, bytes, n)) { ok = false; break; }
    if (ferror(f)) ok = false;
    if (fclose(f)) ok = false;
    if (ok) { ASDLByteBuffer_destroy(out); *out = result; } else ASDLByteBuffer_destroy(&result);
    return ok;
}
static inline bool ASDLPath_write(const struct ASDLPath *p, const void *data, size_t length) {
    if (!p || !p->text || (length && !data)) return false;
    FILE *f = fopen(p->text, "wb");
    if (!f) return false;
    bool ok = !length || fwrite(data, 1, length, f) == length;
    if (fclose(f)) ok = false;
    return ok;
}
#endif
@@ event
#ifndef ASDL_EVENT_HANDLER_H
#define ASDL_EVENT_HANDLER_H
enum ASDLEventKind { ASDL_EVENT_DATA, ASDL_EVENT_EOF, ASDL_EVENT_RESET,
    ASDL_EVENT_OPEN, ASDL_EVENT_CLOSE, ASDL_EVENT_ATOM, ASDL_EVENT_STRING, ASDL_EVENT_QUOTE };
enum ASDLEventResult { ASDL_EVENT_ERROR = -1, ASDL_EVENT_CONTINUE = 0, ASDL_EVENT_PAUSE = 1 };
struct ASDLEvent { enum ASDLEventKind kind; const unsigned char *data; size_t length, offset, depth; };
struct ASDLEventHandler { int (*callback)(void *, const struct ASDLEvent *); void *context; };
static inline int ASDLEventHandler_call(const struct ASDLEventHandler *handler, const struct ASDLEvent *event) {
    if (!handler || !handler->callback) return ASDL_EVENT_CONTINUE;
    int result = handler->callback(handler->context, event);
    return result == ASDL_EVENT_CONTINUE || result == ASDL_EVENT_PAUSE ? result : ASDL_EVENT_ERROR;
}
#endif
@@ sexpr
#ifndef ASDL_SEXPR_H
#define ASDL_SEXPR_H
struct ASDLSexpr {
    struct ASDLByteBuffer token;
    struct ASDLEventHandler handler;
    size_t offset, start, depth;
    int state; /* 0 layout, 1 atom, 2 string, 3 escape, 4 comment */
    bool failed, finished, quoted;
};
static inline void ASDLSexpr_init(struct ASDLSexpr *s, struct ASDLEventHandler handler) {
    if (s) { *s = (struct ASDLSexpr){0}; s->handler = handler; }
}
static inline void ASDLSexpr_destroy(struct ASDLSexpr *s) {
    if (s) { ASDLByteBuffer_destroy(&s->token); *s = (struct ASDLSexpr){0}; }
}
static inline void ASDLSexpr_reset(struct ASDLSexpr *s) {
    if (!s) return;
    struct ASDLEventHandler handler = s->handler;
    ASDLSexpr_destroy(s); ASDLSexpr_init(s, handler);
}
static inline int ASDLSexpr_emit(struct ASDLSexpr *s, enum ASDLEventKind kind) {
    struct ASDLEvent event = {kind, s->token.data, s->token.length, s->start, s->depth};
    int result = ASDLEventHandler_call(&s->handler, &event);
    ASDLByteBuffer_clear(&s->token);
    if (result < 0) s->failed = true;
    return result;
}
/* A PAUSE saves the continuation in s. Resume with the unconsumed suffix.
 * Token bytes are borrowed only during the callback. final may be repeated
 * with an empty suffix after PAUSE to finish the EOF continuation. */
static inline int ASDLSexpr_feed(struct ASDLSexpr *s, const void *data, size_t length, bool final, size_t *consumed) {
    size_t i = 0;
    const unsigned char *bytes = (const unsigned char *)data;
    if (consumed) *consumed = 0;
    if (!s || s->failed || (length && !data) || (s->finished && length)) return ASDL_EVENT_ERROR;
    if (s->finished) return ASDL_EVENT_CONTINUE;
    while (i < length) {
        unsigned char c = bytes[i];
        int result = ASDL_EVENT_CONTINUE;
        if (s->state == 1 && (isspace(c) || c == '(' || c == ')' || c == ';' || c == '"' || c == '\'')) {
            s->state = 0;
            result = ASDLSexpr_emit(s, ASDL_EVENT_ATOM);
            if (result) { if (consumed) *consumed = i; return result; }
            continue;
        }
        if (s->offset == SIZE_MAX) { s->failed = true; break; }
        ++i; ++s->offset;
        if (s->state == 4) { if (c == '\n') s->state = 0; }
        else if (s->state == 3) {
            if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t';
            else if (c != '\\' && c != '"') { s->failed = true; break; }
            s->state = 2;
            if (!ASDLByteBuffer_put(&s->token, c)) { s->failed = true; break; }
        } else if (s->state == 2) {
            if (c == '\\') s->state = 3;
            else if (c == '"') { s->state = 0; result = ASDLSexpr_emit(s, ASDL_EVENT_STRING); }
            else if (!ASDLByteBuffer_put(&s->token, c)) { s->failed = true; break; }
        } else if (s->state == 1) {
            if (!c || !ASDLByteBuffer_put(&s->token, c)) { s->failed = true; break; }
        } else if (!isspace(c)) {
            s->start = s->offset - 1;
            if (c == ';') s->state = 4;
            else if (c == '"') { s->state = 2; s->quoted = false; }
            else if (c == '(') {
                if (s->depth == SIZE_MAX) { s->failed = true; break; }
                s->quoted = false; ++s->depth; result = ASDLSexpr_emit(s, ASDL_EVENT_OPEN);
            } else if (c == ')') {
                if (!s->depth || s->quoted) { s->failed = true; break; }
                --s->depth; result = ASDLSexpr_emit(s, ASDL_EVENT_CLOSE);
            } else if (c == '\'') { s->quoted = true; result = ASDLSexpr_emit(s, ASDL_EVENT_QUOTE); }
            else {
                s->state = 1; s->quoted = false;
                if (!c || !ASDLByteBuffer_put(&s->token, c)) { s->failed = true; break; }
            }
        }
        if (result) { if (consumed) *consumed = i; return result; }
    }
    if (consumed) *consumed = i;
    if (s->failed) return ASDL_EVENT_ERROR;
    if (final) {
        if (s->state == 1) {
            s->state = 0;
            int result = ASDLSexpr_emit(s, ASDL_EVENT_ATOM);
            if (result) return result;
        }
        if (s->state == 2 || s->state == 3 || s->depth || s->quoted) { s->failed = true; return ASDL_EVENT_ERROR; }
        s->finished = true; s->start = s->offset;
        return ASDLSexpr_emit(s, ASDL_EVENT_EOF);
    }
    return ASDL_EVENT_CONTINUE;
}
/* Writer is also continuation-based: deliver each event to this callback.
 * Output is canonical whitespace-separated S-expression text, not an AST
 * serialization format. Strings preserve arbitrary bytes, including NUL. */
static inline int ASDLSexpr_write(void *context, const struct ASDLEvent *event) {
    struct ASDLPort *port = (struct ASDLPort *)context;
    if (!port || !event || (event->length && !event->data)) return ASDL_EVENT_ERROR;
    if (event->kind == ASDL_EVENT_EOF) return ASDLPort_flush(port) ? 0 : -1;
    if (event->kind == ASDL_EVENT_OPEN) return ASDLPort_put(port, '(') ? 0 : -1;
    if (event->kind == ASDL_EVENT_CLOSE) return ASDLPort_write(port, ") ", 2) == 2 ? 0 : -1;
    if (event->kind == ASDL_EVENT_QUOTE) return ASDLPort_put(port, '\'') ? 0 : -1;
    if (event->kind != ASDL_EVENT_ATOM && event->kind != ASDL_EVENT_STRING) return -1;
    if (event->kind == ASDL_EVENT_ATOM) {
        if (!event->length) return -1;
        for (size_t i = 0; i < event->length; ++i) {
            unsigned char c = event->data[i];
            if (!c || isspace(c) || strchr("();\"'", c)) return -1;
        }
        if (ASDLPort_write(port, event->data, event->length) != event->length) return -1;
    } else {
        if (!ASDLPort_put(port, '"')) return -1;
        for (size_t i = 0; i < event->length; ++i) {
            unsigned char c = event->data[i];
            if (c == '\n' || c == '\r' || c == '\t' || c == '\\' || c == '"') {
                if (!ASDLPort_put(port, '\\')) return -1;
                if (c == '\n') c = 'n'; else if (c == '\r') c = 'r'; else if (c == '\t') c = 't';
            }
            if (!ASDLPort_put(port, c)) return -1;
        }
        if (!ASDLPort_put(port, '"')) return -1;
    }
    return ASDLPort_put(port, ' ') ? 0 : -1;
}
#endif
@@ regexp
#ifndef ASDL_REGEXP_H
#define ASDL_REGEXP_H
#include <regex.h>
struct ASDLRegexp { regex_t *regex; bool compiled; int error; };
static inline void ASDLRegexp_init(struct ASDLRegexp *r) { if (r) *r = (struct ASDLRegexp){0}; }
static inline void ASDLRegexp_destroy(struct ASDLRegexp *r) {
    if (r) { if (r->compiled) regfree(r->regex); ASDL_FREE(r->regex); ASDLRegexp_init(r); }
}
static inline bool ASDLRegexp_compile(struct ASDLRegexp *r, const char *pattern, int flags) {
    if (!r || !pattern) return false;
    ASDLRegexp_destroy(r);
    r->regex = (regex_t *)ASDL_MALLOC(sizeof(*r->regex));
    if (!r->regex) { r->error = REG_ESPACE; return false; }
    r->error = regcomp(r->regex, pattern, flags);
    r->compiled = r->error == 0;
    if (!r->compiled) { ASDL_FREE(r->regex); r->regex = NULL; }
    return r->compiled;
}
static inline int ASDLRegexp_exec(struct ASDLRegexp *r, const char *text, size_t count, regmatch_t *matches, int flags) {
    if (!r || !r->compiled || !text || (count && !matches)) return REG_BADPAT;
    r->error = regexec(r->regex, text, count, matches, flags);
    return r->error;
}
static inline bool ASDLRegexp_test(struct ASDLRegexp *r, const char *text) { return ASDLRegexp_exec(r, text, 0, NULL, 0) == 0; }
static inline size_t ASDLRegexp_error(const struct ASDLRegexp *r, char *buffer, size_t size) {
    return regerror(r ? r->error : REG_BADPAT, r && r->compiled ? r->regex : NULL, buffer, size);
}
/* Literal replacement of the first match; captures are exposed by exec. */
static inline bool ASDLRegexp_replace(struct ASDLRegexp *r, const char *text, const char *replacement, struct ASDLByteBuffer *out) {
    if (!r || !text || !replacement || !out) return false;
    regmatch_t match = {-1, -1};
    if (ASDLRegexp_exec(r, text, 1, &match, 0) || match.rm_so < 0 || match.rm_eo < match.rm_so) return false;
    struct ASDLByteBuffer result = {0};
    bool ok = ASDLByteBuffer_append(&result, text, (size_t)match.rm_so)
        && ASDLByteBuffer_append(&result, replacement, strlen(replacement))
        && ASDLByteBuffer_append(&result, text + match.rm_eo, strlen(text + match.rm_eo));
    if (ok) { ASDLByteBuffer_destroy(out); *out = result; } else ASDLByteBuffer_destroy(&result);
    return ok;
}
#endif
@@ parsec
#ifndef ASDL_PARSEC_H
#define ASDL_PARSEC_H
enum ASDLParsecKind { ASDL_PARSEC_LITERAL, ASDL_PARSEC_RANGE, ASDL_PARSEC_ANY,
    ASDL_PARSEC_END, ASDL_PARSEC_SEQUENCE, ASDL_PARSEC_CHOICE, ASDL_PARSEC_REPEAT, ASDL_PARSEC_REF };
struct ASDLParsecNode {
    enum ASDLParsecKind kind;
    struct ASDLParsecNode *left, *right, *next;
    char *text;
    size_t length, minimum, maximum;
    unsigned char low, high;
};
struct ASDLParsecRule { char *name; struct ASDLParsecNode *node; struct ASDLParsecRule *next; };
struct ASDLParsec {
    struct ASDLParsecNode *nodes, *root;
    struct ASDLParsecRule *rules;
    size_t max_depth, max_steps, error_offset;
    const char *error;
};
static inline void ASDLParsec_init(struct ASDLParsec *p) {
    if (p) { *p = (struct ASDLParsec){0}; p->max_depth = 256; p->max_steps = 1000000; }
}
static inline void ASDLParsec_destroy(struct ASDLParsec *p) {
    if (!p) return;
    while (p->nodes) { struct ASDLParsecNode *n = p->nodes; p->nodes = n->next; ASDL_FREE(n->text); ASDL_FREE(n); }
    while (p->rules) { struct ASDLParsecRule *r = p->rules; p->rules = r->next; ASDL_FREE(r->name); ASDL_FREE(r); }
    ASDLParsec_init(p);
}
static inline struct ASDLParsecNode *ASDLParsec_node(struct ASDLParsec *p, enum ASDLParsecKind kind) {
    if (!p || p->error) return NULL;
    struct ASDLParsecNode *n = (struct ASDLParsecNode *)ASDL_MALLOC(sizeof(*n));
    if (!n) { p->error = "out of memory"; return NULL; }
    *n = (struct ASDLParsecNode){0}; n->kind = kind; n->next = p->nodes; p->nodes = n;
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_literal(struct ASDLParsec *p, const void *text, size_t length) {
    if (length && !text) { if (p) p->error = "invalid literal"; return NULL; }
    struct ASDLParsecNode *n = ASDLParsec_node(p, ASDL_PARSEC_LITERAL);
    if (n) { n->text = ASDL_copy_text((const char *)text, length); n->length = length; if (!n->text) { p->error = "out of memory"; return NULL; } }
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_range(struct ASDLParsec *p, unsigned char low, unsigned char high) {
    if (low > high) { if (p) p->error = "invalid range"; return NULL; }
    struct ASDLParsecNode *n = ASDLParsec_node(p, ASDL_PARSEC_RANGE);
    if (n) { n->low = low; n->high = high; }
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_any(struct ASDLParsec *p) { return ASDLParsec_node(p, ASDL_PARSEC_ANY); }
static inline struct ASDLParsecNode *ASDLParsec_end(struct ASDLParsec *p) { return ASDLParsec_node(p, ASDL_PARSEC_END); }
static inline struct ASDLParsecNode *ASDLParsec_pair(struct ASDLParsec *p, enum ASDLParsecKind kind, struct ASDLParsecNode *a, struct ASDLParsecNode *b) {
    if (!a || !b) { if (p) p->error = "missing operand"; return NULL; }
    struct ASDLParsecNode *n = ASDLParsec_node(p, kind);
    if (n) { n->left = a; n->right = b; }
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_sequence(struct ASDLParsec *p, struct ASDLParsecNode *a, struct ASDLParsecNode *b) {
    return ASDLParsec_pair(p, ASDL_PARSEC_SEQUENCE, a, b);
}
static inline struct ASDLParsecNode *ASDLParsec_choice(struct ASDLParsec *p, struct ASDLParsecNode *a, struct ASDLParsecNode *b) {
    return ASDLParsec_pair(p, ASDL_PARSEC_CHOICE, a, b);
}
static inline struct ASDLParsecNode *ASDLParsec_repeat(struct ASDLParsec *p, struct ASDLParsecNode *child, size_t minimum, size_t maximum) {
    if (!child || minimum > maximum) { if (p) p->error = "invalid repetition"; return NULL; }
    struct ASDLParsecNode *n = ASDLParsec_node(p, ASDL_PARSEC_REPEAT);
    if (n) { n->left = child; n->minimum = minimum; n->maximum = maximum; }
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_ref(struct ASDLParsec *p, const char *name) {
    if (!name) return NULL;
    struct ASDLParsecNode *n = ASDLParsec_node(p, ASDL_PARSEC_REF);
    if (n) { n->text = ASDL_copy_text(name, strlen(name)); if (!n->text) { p->error = "out of memory"; return NULL; } }
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_lookup(const struct ASDLParsec *p, const char *name) {
    if (p && name) for (struct ASDLParsecRule *r = p->rules; r; r = r->next) if (!strcmp(name, r->name)) return r->node;
    return NULL;
}
static inline bool ASDLParsec_rule(struct ASDLParsec *p, const char *name, struct ASDLParsecNode *node) {
    if (!p || !name || !node || p->error) return false;
    if (ASDLParsec_lookup(p, name)) { p->error = "duplicate rule"; return false; }
    struct ASDLParsecRule *r = (struct ASDLParsecRule *)ASDL_MALLOC(sizeof(*r));
    if (!r) { p->error = "out of memory"; return false; }
    r->name = ASDL_copy_text(name, strlen(name));
    if (!r->name) { ASDL_FREE(r); p->error = "out of memory"; return false; }
    r->node = node; r->next = p->rules; p->rules = r;
    if (!p->root) p->root = node;
    return true;
}
/* Ordered choice and greedy, possessive repetition (PEG semantics). A bounded
 * step budget and depth bound also terminate left-recursive/cyclic grammars. */
static inline int ASDLParsec_run(struct ASDLParsec *p, const struct ASDLParsecNode *n,
        const unsigned char *input, size_t length, size_t *position, size_t depth, size_t *steps) {
    if (!n) { p->error = "undefined rule"; return -1; }
    if (!depth || !*steps) { p->error = "parser resource limit"; return -1; }
    --*steps;
    size_t start = *position;
    int result = 0;
    switch (n->kind) {
    case ASDL_PARSEC_LITERAL:
        if (n->length <= length - start && (!n->length || !memcmp(input + start, n->text, n->length))) { *position += n->length; return 1; }
        break;
    case ASDL_PARSEC_RANGE:
        if (start < length && input[start] >= n->low && input[start] <= n->high) { ++*position; return 1; }
        break;
    case ASDL_PARSEC_ANY: if (start < length) { ++*position; return 1; } break;
    case ASDL_PARSEC_END: if (start == length) return 1; break;
    case ASDL_PARSEC_REF:
        return ASDLParsec_run(p, ASDLParsec_lookup(p, n->text), input, length, position, depth - 1, steps);
    case ASDL_PARSEC_SEQUENCE:
        result = ASDLParsec_run(p, n->left, input, length, position, depth - 1, steps);
        if (result == 1) result = ASDLParsec_run(p, n->right, input, length, position, depth - 1, steps);
        if (result == 1) return 1;
        *position = start;
        return result;
    case ASDL_PARSEC_CHOICE:
        result = ASDLParsec_run(p, n->left, input, length, position, depth - 1, steps);
        if (result != 0) return result;
        *position = start;
        return ASDLParsec_run(p, n->right, input, length, position, depth - 1, steps);
    case ASDL_PARSEC_REPEAT: {
        size_t count = 0;
        while (count < n->maximum) {
            size_t before = *position;
            result = ASDLParsec_run(p, n->left, input, length, position, depth - 1, steps);
            if (result < 0) { *position = start; return -1; }
            if (!result) { *position = before; break; }
            ++count;
            if (*position == before) {
                if (n->maximum != SIZE_MAX) return 1;
                p->error = "repetition matched empty input"; *position = start; return -1;
            }
        }
        if (count >= n->minimum) return 1;
        *position = start;
        break;
    }
    default: p->error = "invalid parser node"; return -1;
    }
    if (start > p->error_offset) p->error_offset = start;
    return 0;
}
static inline int ASDLParsec_match(struct ASDLParsec *p, const void *input, size_t length, bool complete, size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!p || !p->root || (length && !input)) return -1;
    p->error = NULL; p->error_offset = 0;
    size_t position = 0, steps = p->max_steps;
    int result = ASDLParsec_run(p, p->root, (const unsigned char *)input, length, &position, p->max_depth, &steps);
    if (result == 1 && complete && position != length) { p->error_offset = position; result = 0; }
    if (result == 1 && consumed) *consumed = position;
    if (!result) p->error = "input does not match";
    return result;
}
struct ASDLParsecGrammar { const char *text; size_t position; struct ASDLParsec *parser; };
static inline void ASDLParsec_space(struct ASDLParsecGrammar *g) {
    while (isspace((unsigned char)g->text[g->position])) ++g->position;
}
static inline char *ASDLParsec_name(struct ASDLParsecGrammar *g) {
    ASDLParsec_space(g);
    size_t start = g->position;
    if (!isalpha((unsigned char)g->text[start]) && g->text[start] != '_') return NULL;
    do { ++g->position; } while (isalnum((unsigned char)g->text[g->position]) || g->text[g->position] == '_');
    char *name = ASDL_copy_text(g->text + start, g->position - start);
    if (!name) g->parser->error = "out of memory";
    return name;
}
static inline struct ASDLParsecNode *ASDLParsec_expression(struct ASDLParsecGrammar *g, size_t depth);
static inline struct ASDLParsecNode *ASDLParsec_term(struct ASDLParsecGrammar *g, size_t depth) {
    ASDLParsec_space(g);
    struct ASDLParsec *p = g->parser;
    if (!depth) { p->error = "grammar nesting limit"; return NULL; }
    char c = g->text[g->position];
    struct ASDLParsecNode *n = NULL;
    if (c == '\'' || c == '"') {
        ++g->position;
        struct ASDLByteBuffer text = {0};
        while (g->text[g->position] && g->text[g->position] != c) {
            unsigned char ch = (unsigned char)g->text[g->position++];
            if (ch == '\\') {
                ch = (unsigned char)g->text[g->position];
                if (!ch) break;
                ++g->position;
                if (ch == 'n') ch = '\n'; else if (ch == 'r') ch = '\r'; else if (ch == 't') ch = '\t';
            }
            if (!ASDLByteBuffer_put(&text, ch)) { p->error = "out of memory"; break; }
        }
        if (g->text[g->position] != c) p->error = "unterminated literal";
        else { ++g->position; n = ASDLParsec_literal(p, text.data, text.length); }
        ASDLByteBuffer_destroy(&text);
    } else if (c == '(' || c == '[' || c == '{') {
        ++g->position;
        n = ASDLParsec_expression(g, depth - 1);
        ASDLParsec_space(g);
        char end = c == '(' ? ')' : c == '[' ? ']' : '}';
        if (g->text[g->position] != end) { p->error = "unclosed group"; return NULL; }
        ++g->position;
        if (c != '(') n = ASDLParsec_repeat(p, n, 0, c == '[' ? 1 : SIZE_MAX);
    } else if (c == '.') { ++g->position; n = ASDLParsec_any(p); }
    else {
        char *name = ASDLParsec_name(g);
        if (!name) { p->error = "expected expression"; return NULL; }
        n = ASDLParsec_ref(p, name); ASDL_FREE(name);
    }
    ASDLParsec_space(g);
    c = g->text[g->position];
    if (c == '*' || c == '+' || c == '?') {
        ++g->position;
        n = ASDLParsec_repeat(p, n, c == '+' ? 1 : 0, c == '?' ? 1 : SIZE_MAX);
    }
    return n;
}
static inline bool ASDLParsec_starts_term(char c) {
    return isalpha((unsigned char)c) || c == '_' || c == '\'' || c == '"' || c == '(' || c == '[' || c == '{' || c == '.';
}
static inline struct ASDLParsecNode *ASDLParsec_expression(struct ASDLParsecGrammar *g, size_t depth) {
    struct ASDLParsecNode *choice = NULL, *sequence = ASDLParsec_term(g, depth);
    while (sequence && !g->parser->error) {
        ASDLParsec_space(g);
        char c = g->text[g->position];
        if (c == ',' || ASDLParsec_starts_term(c)) {
            if (c == ',') ++g->position;
            struct ASDLParsecNode *right = ASDLParsec_term(g, depth);
            sequence = ASDLParsec_sequence(g->parser, sequence, right);
        } else if (c == '|') {
            ++g->position;
            choice = choice ? ASDLParsec_choice(g->parser, choice, sequence) : sequence;
            sequence = ASDLParsec_term(g, depth);
        } else break;
    }
    return choice ? ASDLParsec_choice(g->parser, choice, sequence) : sequence;
}
static inline bool ASDLParsec_compile(struct ASDLParsec *p, const char *grammar, const char *start_rule) {
    if (!p || !grammar) return false;
    struct ASDLParsec result; ASDLParsec_init(&result);
    result.max_depth = p->max_depth; result.max_steps = p->max_steps;
    struct ASDLParsecGrammar g = {grammar, 0, &result};
    ASDLParsec_space(&g);
    while (grammar[g.position] && !result.error) {
        char *name = ASDLParsec_name(&g);
        ASDLParsec_space(&g);
        if (!name || grammar[g.position] != '=') { ASDL_FREE(name); result.error = "expected rule = expression;"; break; }
        ++g.position;
        struct ASDLParsecNode *node = ASDLParsec_expression(&g, result.max_depth);
        ASDLParsec_space(&g);
        if (!node || grammar[g.position] != ';') { ASDL_FREE(name); if (!result.error) result.error = "expected semicolon"; break; }
        ++g.position;
        ASDLParsec_rule(&result, name, node); ASDL_FREE(name);
        ASDLParsec_space(&g);
    }
    if (!result.root && !result.error) result.error = "empty grammar";
    for (struct ASDLParsecNode *n = result.nodes; n && !result.error; n = n->next)
        if (n->kind == ASDL_PARSEC_REF && !ASDLParsec_lookup(&result, n->text)) result.error = "undefined rule";
    if (start_rule && !result.error) {
        result.root = ASDLParsec_lookup(&result, start_rule);
        if (!result.root) result.error = "undefined start rule";
    }
    if (result.error) {
        p->error = result.error; p->error_offset = g.position;
        ASDLParsec_destroy(&result); return false;
    }
    ASDLParsec_destroy(p); *p = result;
    return true;
}
#endif
@@ listener
#ifndef @U@LISTENER_H
#define @U@LISTENER_H
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
struct @P@listener {
    int fd, error;
    unsigned char *data;
    size_t length, position;
    struct ASDLEventHandler *handlers;
    size_t count, capacity, next_handler;
    struct ASDLEvent pending;
    bool has_pending, eof;
};
static inline void @P@listener_init(struct @P@listener *l) {
    if (l) { *l = (struct @P@listener){0}; l->fd = -1; }
}
static inline void @P@listener_close(struct @P@listener *l) {
    if (!l) return;
    if (l->data) munmap(l->data, l->length);
    if (l->fd >= 0) close(l->fd);
    ASDL_FREE(l->handlers); @P@listener_init(l);
}
static inline bool @P@listener_open(struct @P@listener *l, const char *path) {
    if (!l || !path) return false;
    @P@listener_init(l);
    l->fd = open(path, O_RDONLY);
    if (l->fd < 0) { l->error = errno; return false; }
    struct stat st;
    if (fstat(l->fd, &st) || !S_ISREG(st.st_mode)) {
        l->error = EINVAL; close(l->fd); l->fd = -1; return false;
    }
    return true;
}
/* Handlers may be changed between events, not from inside a callback or while
 * paused. All handlers see each event once, in registration order. */
static inline bool @P@listener_add(struct @P@listener *l, struct ASDLEventHandler handler) {
    if (!l || l->has_pending || !handler.callback) return false;
    if (l->count == l->capacity) {
        size_t capacity = l->capacity ? l->capacity * 2 : 4;
        if (capacity < l->capacity || capacity > SIZE_MAX / sizeof(handler)) return false;
        struct ASDLEventHandler *handlers = (struct ASDLEventHandler *)ASDL_MALLOC(capacity * sizeof(handler));
        if (!handlers) return false;
        if (l->count) memcpy(handlers, l->handlers, l->count * sizeof(handler));
        ASDL_FREE(l->handlers); l->handlers = handlers; l->capacity = capacity;
    }
    l->handlers[l->count++] = handler;
    return true;
}
static inline bool @P@listener_remove(struct @P@listener *l, size_t index) {
    if (!l || l->has_pending || index >= l->count) return false;
    memmove(l->handlers + index, l->handlers + index + 1, (l->count - index - 1) * sizeof(*l->handlers));
    --l->count;
    return true;
}
static inline int @P@listener_dispatch(struct @P@listener *l) {
    while (l->next_handler < l->count) {
        int action = ASDLEventHandler_call(&l->handlers[l->next_handler++], &l->pending);
        if (action) { if (action < 0) l->error = ECANCELED; return action; }
    }
    l->has_pending = false; l->next_handler = 0;
    return ASDL_EVENT_CONTINUE;
}
static inline int @P@listener_event(struct @P@listener *l, enum ASDLEventKind kind, size_t offset, size_t length) {
    l->pending = (struct ASDLEvent){kind, length ? l->data + offset : NULL, length, offset, 0};
    l->has_pending = true;
    return @P@listener_dispatch(l);
}
/* Poll an append-only file. DATA events are newline-terminated byte records
 * (newline included). final flushes a trailing record and sends EOF once per
 * file size. Truncation between polls resets the cursor and sends RESET.
 * The producer must not truncate or mutate mapped bytes during callbacks. */
static inline int @P@listener_poll(struct @P@listener *l, bool final) {
    if (!l || l->fd < 0 || l->error) return ASDL_EVENT_ERROR;
    if (l->has_pending) { int action = @P@listener_dispatch(l); if (action) return action; }
    struct stat st;
    if (fstat(l->fd, &st)) { l->error = errno; return ASDL_EVENT_ERROR; }
    if (st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX) { l->error = EOVERFLOW; return ASDL_EVENT_ERROR; }
    size_t length = (size_t)st.st_size;
    if (length != l->length) {
        bool reset = length < l->length;
        unsigned char *data = NULL;
        if (length) {
            void *mapping = mmap(NULL, length, PROT_READ, MAP_SHARED, l->fd, 0);
            if (mapping == MAP_FAILED) { l->error = errno; return ASDL_EVENT_ERROR; }
            data = (unsigned char *)mapping;
        }
        if (l->data) munmap(l->data, l->length);
        l->data = data; l->length = length; l->eof = false;
        if (reset) { l->position = 0; int action = @P@listener_event(l, ASDL_EVENT_RESET, 0, 0); if (action) return action; }
    }
    while (l->position < l->length) {
        size_t start = l->position;
        const unsigned char *newline = (const unsigned char *)memchr(l->data + start, '\n', l->length - start);
        if (!newline && !final) break;
        size_t end = newline ? (size_t)(newline - l->data) + 1 : l->length;
        l->position = end;
        int action = @P@listener_event(l, ASDL_EVENT_DATA, start, end - start);
        if (action) return action;
    }
    if (final && !l->eof) { l->eof = true; return @P@listener_event(l, ASDL_EVENT_EOF, l->position, 0); }
    return ASDL_EVENT_CONTINUE;
}
#endif
