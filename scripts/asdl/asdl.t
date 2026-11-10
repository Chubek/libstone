use strict;
use warnings;
use 5.014;
use Test::More;
use File::Basename qw(dirname);
use File::Spec;
use File::Temp qw(tempdir);
use File::Path qw(make_path);
use IPC::Open3;
use JSON::PP;
use Text::ParseWords qw(shellwords);

my $generator = File::Spec->rel2abs(File::Spec->catfile(dirname(__FILE__), 'asdl.pl'));
my $tmp = -d '/tmp/opencode' ? '/tmp/opencode' : File::Spec->tmpdir;
my $dir = tempdir('asdl-test-XXXXXXXX', DIR => $tmp, CLEANUP => 1);
my $serial = 0;
my @cc = shellwords($ENV{CC} // 'cc');
my @cflags = shellwords($ENV{CFLAGS} // '');

sub write_file {
    my ($path, $text) = @_;
    open my $fh, '>', $path or die "open $path: $!";
    binmode $fh;
    print {$fh} $text or die "write $path: $!";
    close $fh or die "close $path: $!";
}

sub read_file {
    my ($path) = @_;
    open my $fh, '<', $path or die "open $path: $!";
    local $/;
    return <$fh> // '';
}

# Redirect both streams to files: large generated headers and compiler
# diagnostics cannot fill opposing open3 pipes and deadlock the test runner.
sub command {
    my ($input, @argv) = @_;
    my $base = File::Spec->catfile($dir, 'run' . ++$serial);
    open my $out, '>', "$base.out" or die $!;
    open my $err, '>', "$base.err" or die $!;
    my $pid = open3(my $in, '>&' . fileno($out), '>&' . fileno($err), @argv);
    print {$in} $input if defined $input;
    close $in;
    waitpid($pid, 0);
    my $status = $? & 127 ? 128 + ($? & 127) : $? >> 8;
    close $out;
    close $err;
    return ($status, read_file("$base.out"), read_file("$base.err"));
}

sub generate {
    my ($source, @options) = @_;
    my $path = File::Spec->catfile($dir, 'grammar' . ++$serial . '.asdl');
    write_file($path, $source);
    return (command(undef, $^X, $generator, @options, $path), $path);
}

sub program {
    my ($name, $sources, @extra) = @_;
    my @files;
    for my $file (sort keys %$sources) {
        my $path = File::Spec->catfile($dir, $file);
        write_file($path, $sources->{$file});
        push @files, $path if $file =~ /\.c\z/;
    }
    my $binary = File::Spec->catfile($dir, $name);
    my ($status, $out, $err) = command(undef, @cc, '-std=c99', '-Wall', '-Wextra',
        '-Werror', '-pedantic', @cflags, '-I', $dir, @files, @extra, '-o', $binary);
    is($status, 0, "$name compiles as strict C99") or diag($out . $err);
    return if $status;
    ($status, $out, $err) = command(undef, $binary);
    is($status, 0, "$name executes successfully") or diag($out . $err);
}

my $tree = <<'ASDL';
%prefix tree
%%
-- Product declarations may follow their uses.
expr = Number(int value)
     | Name(identifier text)
     | Binary(expr left, op operation, expr right)
     | Group(expr* children)
     | Maybe(expr? child)
     | Data(string bytes, string? note, int? code, op? mode)
     | Empty
     attributes (pos? location)
op = Plus | Minus
pos = (string? file, int line, int column)
root = (expr tree, identifier* names, int* numbers, op* modes,
        string* strings, pos* positions, pos? origin)
ASDL

subtest '1997 grammar, typed builders, ownership and validation' => sub {
    my ($status, $header, $err) = generate($tree);
    is($status, 0, 'generates sums, products, attributes, all primitives and qualifiers') or diag($err);
    is($err, '', 'successful generation has no diagnostics');
    return if $status;
    program('builders', { 'tree.h' => $header, 'builders.c' => <<'C' });
#include "tree.h"
#include "tree.h"
#include <assert.h>
#define CHECK_TYPE(name, ctype, shape) + 1
#define CHECK_CTOR(type, name, tag) + 1
enum { types = 0 TREE_FOR_EACH_TYPE(CHECK_TYPE) };
enum { constructors = 0 TREE_FOR_EACH_CONSTRUCTOR(CHECK_CTOR) };
int main(void) {
    tree_arena arena;
    tree_arena_init(&arena);
    assert(types == TREE_TYPE_COUNT && types == 4);
    assert(constructors == TREE_CONSTRUCTOR_COUNT && constructors == 9);
    assert(tree_Plus == 1 && tree_Minus == 2 && tree_Number_kind == 1);
    char bytes[] = {'a', '\0', 'b'};
    tree_string_t owned = tree_string_copy(&arena, bytes, sizeof(bytes));
    bytes[0] = 'x';
    assert(owned.length == 3 && owned.data[0] == 'a' && owned.data[1] == 0);
    assert(owned.data[3] == 0);
    tree_identifier_t id = tree_identifier(&arena, "name");
    assert(id && id == tree_identifier(&arena, "name"));
    assert(id != tree_identifier(&arena, "other"));
    tree_pos_t pos = tree_make_pos(&arena, tree_string_none(), 10, 2);
    tree_expr_t number = tree_Number(&arena, INT64_MIN, tree_pos_some(pos));
    tree_expr_t empty = tree_Empty(&arena, tree_pos_none());
    tree_expr_t name = tree_Name(&arena, id, tree_pos_none());
    assert(number && empty && name);
    assert(number->v.Number.value == INT64_MIN && number->location.value == pos);
    assert(strcmp(tree_expr_name(number), "Number") == 0);
    assert(strcmp(tree_op_name(tree_Minus), "Minus") == 0);
    assert(strcmp(tree_pos_name(pos), "pos") == 0);
    assert(!tree_pos_name(NULL) && !tree_expr_name(NULL) && !tree_op_name((tree_op_t)0));
    tree_expr_t children[] = {number, empty, name};
    tree_expr_seq sequence = tree_expr_seq_copy(&arena, children, 3);
    assert(sequence.length == 3 && sequence.items != children);
    assert(sequence.items[0] == number);
    tree_expr_t group = tree_Group(&arena, sequence, tree_pos_none());
    assert(group && tree_validate_expr(group, 3));
    assert(!tree_validate_expr(group, 2) && !tree_validate_expr(group, 0));
    assert(tree_validate_expr(tree_Maybe(&arena, tree_expr_none(), tree_pos_none()), 1));
    assert(tree_Maybe(&arena, tree_expr_some(group), tree_pos_none()));
    assert(tree_Binary(&arena, number, tree_Plus, group, tree_pos_none()));
    tree_expr_t data = tree_Data(&arena, owned, tree_string_some(tree_string_view(NULL, 0)),
                                tree_int_some(0), tree_op_some(tree_Plus), tree_pos_none());
    assert(data && data->v.Data.code.has_value && data->v.Data.code.value == 0);
    assert(data->v.Data.note.has_value && data->v.Data.note.value.length == 0);
    assert(tree_validate_expr(data, 2));
    assert(!tree_validate_expr(data, 1)); /* the optional enum consumes a level */
    tree_identifier_t ids[] = {id};
    tree_int_t ints[] = {0, INT64_MAX};
    tree_op_t ops[] = {tree_Plus, tree_Minus};
    tree_pos_t positions[] = {pos};
    tree_root_t root = tree_make_root(&arena, group,
        tree_identifier_seq_copy(&arena, ids, 1), tree_int_seq_copy(&arena, ints, 2),
        tree_op_seq_copy(&arena, ops, 2), tree_string_seq_copy(&arena, &owned, 1),
        tree_pos_seq_copy(&arena, positions, 1), tree_pos_none());
    assert(root && tree_validate_root(root, 4));
    root->origin = tree_pos_some(pos);
    assert(tree_validate_root(root, 4));
    root->modes.items[0] = (tree_op_t)999;
    assert(!tree_validate_root(root, 4));
    root->modes.items[0] = tree_Plus;
    group->v.Group.children.items[0] = group;
    assert(!tree_validate_expr(group, 8));
    group->v.Group.children.items[0] = number;
    assert(tree_validate_expr(group, 3));
    tree_expr_kind saved = number->kind;
    number->kind = (tree_expr_kind)999;
    assert(!tree_expr_name(number) && !tree_validate_expr(number, 3));
    number->kind = saved;
    size_t allocations = arena.allocations;
    assert(!tree_Binary(&arena, NULL, tree_Plus, number, tree_pos_none()));
    assert(arena.error == TREE_INVALID && arena.allocations == allocations);
    assert(!tree_Empty(&arena, tree_pos_none()));
    tree_arena_clear_error(&arena);
    assert(!tree_Maybe(&arena, tree_expr_some(NULL), tree_pos_none()));
    tree_arena_clear_error(&arena);
    tree_pos_opt bad = {2, pos};
    assert(!tree_Number(&arena, 3, bad));
    tree_arena_clear_error(&arena);
    tree_expr_seq broken = {1, NULL};
    assert(!tree_Group(&arena, broken, tree_pos_none()));
    tree_arena_clear_error(&arena);
    tree_expr_t missing[] = {NULL};
    assert(tree_expr_seq_copy(&arena, missing, 1).length == 0);
    assert(arena.error == TREE_INVALID);
    tree_arena_clear_error(&arena);
    tree_expr_seq blank = tree_expr_seq_new(&arena, 2);
    assert(blank.items && blank.items[0] == NULL && blank.items[1] == NULL);
    tree_op_seq blank_ops = tree_op_seq_new(&arena, 1);
    assert(blank_ops.items && blank_ops.items[0] == 0);
    tree_string_seq blank_strings = tree_string_seq_new(&arena, 1);
    assert(blank_strings.items && blank_strings.items[0].data == NULL);
    assert(tree_expr_seq_new(&arena, 0).length == 0);
    assert(!tree_Empty(NULL, tree_pos_none()));
    assert(arena.bytes > 0 && arena.allocations > 0 && arena.error == TREE_OK);
    tree_arena_destroy(&arena);
    assert(!arena.blocks && !arena.identifiers && arena.bytes == 0 && arena.allocations == 0);
    assert(tree_Empty(&arena, tree_pos_none()));
    tree_arena_destroy(&arena);
    tree_arena_destroy(&arena);
    tree_arena_destroy(NULL);
    return 0;
}
C
};

subtest 'paper examples, forward references and enum policy' => sub {
    my $paper = <<'ASDL';
stm = Compound(stm, stm) | Assign(identifier, exp) | Print(exp_list)
exp_list = ExpList(exp, exp_list) | Nil
exp = Id(identifier) | Num(int) | Op(exp, binop, exp)
binop = Plus | Minus | Times | Div
ASDL
    my ($status, $header, $err) = generate($paper);
    is($status, 0, 'Figure 3 with unnamed fields') or diag($err);
    program('paper', { 'paper.h' => $header, 'paper.c' => <<'C' }) unless $status;
#include "paper.h"
#include <assert.h>
int main(void) {
    asdl_arena arena;
    asdl_arena_init(&arena);
    asdl_exp_t one = asdl_Num(&arena, 1);
    asdl_exp_t two = asdl_Num(&arena, 2);
    asdl_exp_t sum = asdl_Op(&arena, one, asdl_Plus, two);
    assert(sum->v.Op.exp1 == one && sum->v.Op.exp3 == two);
    asdl_stm_t statement = asdl_Print(&arena, asdl_ExpList(&arena, sum, asdl_Nil(&arena)));
    assert(asdl_validate_stm(statement, 16));
    asdl_arena_destroy(&arena);
    return 0;
}
C
    ($status, $header, $err) = generate('module Box { op = Plus | Minus; unit = Unit }', '--no-enums');
    is($status, 0, 'module wrapper, semicolons and boxed nullary sums') or diag($err);
    program('boxed', { 'boxed.h' => $header, 'boxed.c' => <<'C' }) unless $status;
#include "boxed.h"
#include <assert.h>
int main(void) {
    box_arena arena;
    box_arena_init(&arena);
    box_op_t plus = box_Plus(&arena);
    box_unit_t unit = box_Unit(&arena);
    assert(plus && plus->kind == box_Plus_kind);
    assert(unit && box_validate_unit(unit, 1));
    assert(box_validate_op(plus, 1));
    box_arena_destroy(&arena);
    return 0;
}
C
    my $self_description = <<'ASDL';
asdl_ty = Sum(identifier, field*, constructor, constructor*)
        | Product(identifier, field, field*)
constructor = Con(identifier, field*)
field = Id(identifier, identifier?) | Option(identifier, identifier?)
      | Sequence(identifier, identifier?)
ASDL
    ($status, undef, $err) = generate($self_description, '--check');
    is($status, 0, 'Figure 15: ASDL describing ASDL') or diag($err);
};

subtest 'macro visitor ordering, pruning, cancellation and repeated passes' => sub {
    my $ast = File::Spec->catfile($dir, 'visit_ast.h');
    my $walk = File::Spec->catfile($dir, 'walk.h');
    my ($status, $out, $err) = generate(<<'ASDL', '--header', $ast, '--visitor', $walk);
%prefix visit
%%
node = Leaf(int value) | Branch(node* children) | Optional(node? child)
       attributes (int mark)
mode = On | Off
wrapper = (node tree, mode mode, string text, identifier id)
node_depth = Value(int n)
ASDL
    is($status, 0, 'writes AST and re-includable visitor') or diag($err);
    is($out, '', 'named outputs leave stdout empty');
    return if $status;
    program('visitor', { 'visitor.c' => <<'C' });
#include "visit_ast.h"
#include <assert.h>
struct log { char text[128]; size_t used; int mode; int strings; int ids; };
static int event(struct log *ctx, char ch) {
    ctx->text[ctx->used++] = ch;
    ctx->text[ctx->used] = 0;
    return VISIT_WALK_CONTINUE;
}
static int leaf(struct log *ctx, visit_node_t node) {
    assert(node->kind == visit_Leaf_kind);
    return event(ctx, 'L');
}
#define VISIT_VISITOR_NAME trace
#define VISIT_VISITOR_CONTEXT struct log
#define VISIT_ENTER_node(c, n) event(c, 'E')
#define VISIT_LEAVE_node(c, n) event(c, 'e')
#define VISIT_VISIT_Leaf leaf
#define VISIT_LEAVE_Leaf(c, n) event(c, 'l')
#define VISIT_VISIT_Branch(c, n) event(c, 'B')
#define VISIT_LEAVE_Branch(c, n) event(c, 'b')
#define VISIT_VALUE_int(c, n) event(c, (char)('0' + (n)))
#include "walk.h"
#undef VISIT_VISITOR_NAME
#define VISIT_VISITOR_NAME prune
#undef VISIT_VISIT_Branch
#define VISIT_VISIT_Branch(c, n) (event(c, 'B'), VISIT_WALK_SKIP)
#include "walk.h"
#undef VISIT_VISITOR_NAME
#define VISIT_VISITOR_NAME prune_type
#undef VISIT_ENTER_node
#define VISIT_ENTER_node(c, n) (event(c, 'E'), VISIT_WALK_SKIP)
#include "walk.h"
#undef VISIT_VISITOR_NAME
#undef VISIT_ENTER_node
#undef VISIT_VISIT_Branch
#define VISIT_VISITOR_NAME stop
#undef VISIT_VALUE_int
#define VISIT_VALUE_int(c, n) VISIT_WALK_STOP
#include "walk.h"
#undef VISIT_VISITOR_NAME
#define VISIT_VISITOR_NAME bad_callback
#undef VISIT_VALUE_int
#define VISIT_VALUE_int(c, n) 999
#include "walk.h"
#undef VISIT_VISITOR_NAME
#undef VISIT_VISITOR_CONTEXT
#undef VISIT_VALUE_int
#undef VISIT_LEAVE_node
#undef VISIT_VISIT_Leaf
#undef VISIT_LEAVE_Leaf
#undef VISIT_LEAVE_Branch

#define VISIT_VISITOR_NAME quiet
#define VISIT_VISITOR_MAX_DEPTH 8
#include "walk.h"
#undef VISIT_VISITOR_NAME
#undef VISIT_VISITOR_MAX_DEPTH
#ifdef VISIT_VISITOR_CONTEXT
#error "default context leaked"
#endif
#ifdef VISIT_PRIVATE_FN
#error "private helper macro leaked"
#endif

static int enum_visit(struct log *ctx, visit_mode_t mode) {
    assert(mode == visit_On);
    ++ctx->mode;
    return VISIT_WALK_CONTINUE;
}
static int string_visit(struct log *ctx, visit_string_t text) {
    assert(text.length == 3 && text.data[1] == '\0');
    ++ctx->strings;
    return VISIT_WALK_SKIP; /* primitive SKIP is equivalent to CONTINUE */
}
static int identifier_visit(struct log *ctx, visit_identifier_t id) {
    assert(strcmp(id, "symbol") == 0);
    ++ctx->ids;
    return VISIT_WALK_CONTINUE;
}
#define VISIT_VISITOR_NAME values
#define VISIT_VISITOR_CONTEXT struct log
#define VISIT_VISIT_On enum_visit
#define VISIT_VALUE_string string_visit
#define VISIT_VALUE_identifier identifier_visit
#include "walk.h"
#undef VISIT_VISITOR_NAME
#undef VISIT_VISITOR_CONTEXT
#undef VISIT_VISIT_On
#undef VISIT_VALUE_string
#undef VISIT_VALUE_identifier
#ifdef VISIT_VISITOR_MAX_DEPTH
#error "default depth leaked"
#endif

int main(void) {
    visit_arena arena;
    visit_arena_init(&arena);
    visit_node_t child = visit_Leaf(&arena, 2, 1);
    visit_node_t branch = visit_Branch(&arena, visit_node_seq_copy(&arena, &child, 1), 3);
    struct log log = {{0}, 0, 0, 0, 0};
    assert(trace_node(branch, &log) == VISIT_WALK_CONTINUE);
    assert(strcmp(log.text, "EB3EL12lebe") == 0);
    log.used = 0;
    assert(prune_node(branch, &log) == VISIT_WALK_CONTINUE);
    assert(strcmp(log.text, "EBbe") == 0);
    log.used = 0;
    assert(prune_type_node(branch, &log) == VISIT_WALK_CONTINUE);
    assert(strcmp(log.text, "Ee") == 0);
    log.used = 0;
    assert(stop_node(child, &log) == VISIT_WALK_STOP);
    assert(strcmp(log.text, "L") == 0); /* no LEAVEs on cancellation */
    assert(bad_callback_node(child, &log) == VISIT_WALK_ERROR);
    assert(quiet_node(branch, NULL) == VISIT_WALK_CONTINUE);
    assert(quiet_node(NULL, NULL) == VISIT_WALK_ERROR);
    visit_node_t optional = visit_Optional(&arena, visit_node_none(), 0);
    assert(quiet_node(optional, NULL) == VISIT_WALK_CONTINUE);
    optional->v.Optional.child = visit_node_some(child);
    assert(quiet_node(optional, NULL) == VISIT_WALK_CONTINUE);
    optional->v.Optional.child.has_value = 2;
    assert(quiet_node(optional, NULL) == VISIT_WALK_ERROR);
    optional->v.Optional.child.has_value = 1;
    optional->v.Optional.child.value = NULL;
    assert(quiet_node(optional, NULL) == VISIT_WALK_ERROR);
    branch->v.Branch.children.items[0] = branch;
    assert(quiet_node(branch, NULL) == VISIT_WALK_ERROR);
    assert(prune_node(branch, &log) == VISIT_WALK_CONTINUE);
    branch->v.Branch.children.items[0] = child;
    branch->kind = (visit_node_kind)999;
    assert(prune_node(branch, &log) == VISIT_WALK_ERROR);
    branch->kind = visit_Branch_kind;
    branch->v.Branch.children.items = NULL;
    assert(quiet_node(branch, NULL) == VISIT_WALK_ERROR);
    visit_wrapper_t wrapper = visit_make_wrapper(&arena, child, visit_On,
        visit_string_view("a\0b", 3), "symbol");
    assert(values_wrapper(wrapper, &log) == VISIT_WALK_CONTINUE);
    assert(log.mode == 1 && log.strings == 1 && log.ids == 1);
    assert(quiet_mode((visit_mode_t)0, NULL) == VISIT_WALK_ERROR);
    assert(quiet_node_depth(visit_Value(&arena, 5), NULL) == VISIT_WALK_CONTINUE);
    visit_arena_destroy(&arena);
    return 0;
}
C
};

subtest 'allocation failures, checked sizes and arena reuse' => sub {
    my ($status, $header, $err) = generate(<<'ASDL');
%{
static size_t budget = SIZE_MAX;
static size_t live = 0;
static inline void *tracked_alloc(size_t size) {
    if (!budget) return NULL;
    if (budget != SIZE_MAX) --budget;
    void *p = malloc(size);
    if (p) ++live;
    return p;
}
static inline void tracked_free(void *p) { if (p) --live; free(p); }
#define POOL_MALLOC(n) tracked_alloc(n)
#define POOL_FREE(p) tracked_free(p)
%}
%prefix pool
%%
expr = Value(int n) | Text(string s) | Name(identifier id)
ASDL
    is($status, 0, 'allocator overrides in prologue') or diag($err);
    return if $status;
    program('allocation', { 'pool.h' => $header, 'allocation.c' => <<'C' });
#include "pool.h"
#include <assert.h>
int main(void) {
    pool_arena arena;
    pool_arena_init(&arena);
    budget = 0;
    assert(!pool_Value(&arena, 1) && arena.error == POOL_OOM && live == 0);
    budget = SIZE_MAX;
    assert(!pool_Value(&arena, 2)); /* sticky failure */
    pool_arena_clear_error(&arena);
    assert(pool_Value(&arena, 3) && live == 1 && arena.allocations == 1);
    budget = 0;
    assert(!pool_string_copy(&arena, "test", 4).data && arena.error == POOL_OOM);
    pool_arena_clear_error(&arena);
    assert(!pool_identifier(&arena, "test") && arena.error == POOL_OOM);
    pool_arena_clear_error(&arena);
    assert(!pool_int_seq_new(&arena, 1).items && arena.error == POOL_OOM);
    pool_arena_clear_error(&arena);
    budget = SIZE_MAX;
    assert(!pool_arena_alloc(&arena, SIZE_MAX) && arena.error == POOL_OVERFLOW);
    pool_arena_clear_error(&arena);
    assert(!pool_string_copy(&arena, "x", SIZE_MAX).data && arena.error == POOL_OVERFLOW);
    pool_arena_clear_error(&arena);
    assert(!pool_int_seq_new(&arena, SIZE_MAX).items && arena.error == POOL_OVERFLOW);
    pool_arena_clear_error(&arena);
    pool_int_t n = 1;
    assert(!pool_int_seq_copy(&arena, &n, SIZE_MAX).items && arena.error == POOL_OVERFLOW);
    pool_arena_clear_error(&arena);
    assert(!pool_string_copy(&arena, NULL, 1).data && arena.error == POOL_INVALID);
    pool_arena_clear_error(&arena);
    assert(!pool_identifier(&arena, NULL) && arena.error == POOL_INVALID);
    pool_arena_clear_error(&arena);
    assert(!pool_Name(&arena, NULL) && arena.error == POOL_INVALID);
    pool_arena_clear_error(&arena);
    assert(!pool_Text(&arena, pool_string_view(NULL, 1)) && arena.error == POOL_INVALID);
    pool_arena_clear_error(&arena);
    assert(!pool_int_seq_copy(&arena, NULL, 1).items && arena.error == POOL_INVALID);
    pool_arena_clear_error(&arena);
    size_t bytes = arena.bytes;
    arena.bytes = SIZE_MAX;
    assert(!pool_arena_alloc(&arena, 1) && arena.error == POOL_OVERFLOW);
    arena.bytes = bytes;
    pool_arena_clear_error(&arena);
    size_t allocations = arena.allocations;
    arena.allocations = SIZE_MAX;
    assert(!pool_arena_alloc(&arena, 1) && arena.error == POOL_OVERFLOW);
    arena.allocations = allocations;
    pool_arena_clear_error(&arena);
    assert(pool_arena_alloc(&arena, 0));
    pool_identifier_t id = pool_identifier(&arena, "same");
    size_t count = arena.allocations;
    assert(id == pool_identifier(&arena, "same") && arena.allocations == count);
    assert(pool_string_copy(&arena, NULL, 0).length == 0 && arena.error == POOL_OK);
    assert(pool_Text(&arena, pool_string_view(NULL, 0)));
    assert(!pool_arena_alloc(NULL, 1));
    assert(!pool_string_copy(NULL, "a", 1).data && !pool_identifier(NULL, "a"));
    assert(!pool_int_seq_new(NULL, 1).items && !pool_int_seq_copy(NULL, &n, 1).items);
    assert(strcmp(pool_error_string(POOL_OOM), "out of memory") == 0);
    assert(strcmp(pool_error_string((pool_error)999), "unknown error") == 0);
    assert(live == arena.allocations);
    pool_arena_destroy(&arena);
    assert(live == 0 && arena.error == POOL_OK);
    assert(pool_Value(&arena, 4));
    pool_arena_destroy(&arena);
    assert(live == 0);
    return 0;
}
C
};

subtest 'split compilation, C injection and relative includes' => sub {
    my $base = File::Spec->catdir($dir, 'split');
    make_path("$base/include", "$base/src", "$base/visitors");
    my ($status, $out, $err) = generate(<<'ASDL', '--header', "$base/include/api.h", '--source', "$base/src/api.c", '--visitor', "$base/visitors/walk.h");
/* %% in a comment does not start the definitions. */
%{
/* %} ignored, as is %% */
// %} ignored here too
static inline const char *delimiters(void) { return "%} %%"; }
int injected_answer(void);
%}
%option prefix=split enums
%%
value = Value(int n)
%%
int injected_answer(void) { return 73; }
/* %% remains literal epilogue text */
ASDL
    is($status, 0, 'generates separate header, body and visitor') or diag($err);
    is($out, '', 'no unsolicited stdout');
    return if $status;
    my $source = read_file("$base/src/api.c");
    like($source, qr{#include "\.\./include/api\.h"}, 'source uses relative header include');
    like(read_file("$base/visitors/walk.h"), qr{#include "\.\./include/api\.h"}, 'visitor uses its own relative include');
    like($source, qr/\/\* %% remains literal epilogue text \*\//, 'secondary delimiter preserves all subsequent text');
    like($source, qr/#line \d+ ".*grammar\d+\.asdl"/, 'injected C has source line mapping');
    unlike(read_file("$base/include/api.h"), qr/int injected_answer\(void\) \{/, 'epilogue lives in implementation');
    program('split_linkage', {
        'split_main.c' => <<'C',
#include "split/include/api.h"
#include <assert.h>
split_value_t other(split_arena *arena);
#define SPLIT_VISITOR_NAME walk
#include "split/visitors/walk.h"
int main(void) {
    split_arena arena;
    split_arena_init(&arena);
    split_value_t node = other(&arena);
    assert(node && node->v.Value.n == injected_answer());
    assert(strcmp(delimiters(), "%} %%") == 0);
    assert(walk_value(node, NULL) == SPLIT_WALK_CONTINUE);
    split_arena_destroy(&arena);
    return 0;
}
C
        'split_other.c' => <<'C',
#include "split/include/api.h"
#include "split/include/api.h"
split_value_t other(split_arena *arena) { return split_Value(arena, injected_answer()); }
C
    }, "$base/src/api.c");
};

subtest 'header-only multi-TU linkage and independent AST namespaces' => sub {
    my ($status, $one, $err) = generate('module One { node = Node(int value) }');
    is($status, 0, 'first namespaced AST') or diag($err);
    my ($second, $two, $err2) = generate('module Two { node = Node(string value) }');
    is($second, 0, 'second namespaced AST') or diag($err2);
    return if $status || $second;
    program('multi_tu', {
        'one.h' => $one, 'two.h' => $two,
        'multi_main.c' => <<'C',
#include "one.h"
#include "two.h"
#include <assert.h>
one_node_t from_other(one_arena *arena);
int main(void) {
    one_arena a;
    two_arena b;
    one_arena_init(&a);
    two_arena_init(&b);
    one_node_t x = from_other(&a);
    two_node_t y = two_Node(&b, two_string_view("ok", 2));
    assert(x && x->v.Node.value == 7 && y && y->v.Node.value.length == 2);
    one_arena_destroy(&a); /* allocations made in another translation unit */
    two_arena_destroy(&b);
    return 0;
}
C
        'multi_other.c' => <<'C',
#include "one.h"
one_node_t from_other(one_arena *arena) { return one_Node(arena, 7); }
C
    });
};

subtest 'C keyword escaping, source mappings and deterministic JSON' => sub {
    my $json = File::Spec->catfile($dir, 'names.json');
    my ($status, $header, $err, $path) = generate(<<'ASDL', '--prefix', 'names', '--schema', $json);
module Original {
switch = (int int, int int_, int arena, int node, int int1, int)
expr = Case(int switch, int switch_) attributes (int kind, int v, int kind_)
arena = (switch value)
}
ASDL
    is($status, 0, 'names are mapped without losing field identity') or diag($err);
    return if $status;
    my $schema_text = read_file($json);
    my $schema = decode_json($schema_text);
    is($schema->{prefix}, 'names_', 'CLI prefix wins over module default');
    is($schema->{format_version}, 1, 'versioned schema');
    is_deeply([map { $_->{c_name} } @{$schema->{types}[0]{fields}}],
              [qw(int_ int__2 arena node int1 int6)], 'keyword and collision mapping');
    is_deeply([map { $_->{c_name} } @{$schema->{types}[1]{attributes}}],
              [qw(kind_ v_ kind__2)], 'attributes avoid generated members');
    ok($schema->{types}[0]{fields}[5]{unnamed}, 'implicit field names are identified');
    is($schema->{types}[0]{location}{line}, 2, 'definition location retained');
    is($schema->{types}[1]{constructors}[0]{tag}, 1, 'constructor tags exposed');
    my ($again, $header2, $error2) = command(undef, $^X, $generator, '--prefix', 'names', '--schema', $json, $path);
    is($again, 0, 'repeat generation succeeds') or diag($error2);
    is($header2, $header, 'C output is byte-for-byte deterministic');
    is(read_file($json), $schema_text, 'JSON is byte-for-byte deterministic');
    program('names', { 'names.h' => $header, 'names.c' => <<'C' });
#include "names.h"
#include <assert.h>
int main(void) {
    names_arena a;
    names_arena_init(&a);
    names_switch_t x = names_make_switch(&a, 1, 2, 3, 4, 5, 6);
    names_expr_t y = names_Case(&a, 10, 11, 12, 13, 14);
    assert(x->int_ == 1 && x->int__2 == 2 && x->int6 == 6);
    assert(y->v.Case.switch_ == 10 && y->v.Case.switch__2 == 11);
    assert(y->kind_ == 12 && y->v_ == 13 && y->kind__2 == 14);
    assert(names_make_arena(&a, x));
    names_arena_destroy(&a);
    return 0;
}
C
};

subtest 'minimal schemas, macro-safe identifiers and mutual recursion' => sub {
    my ($status, $product, $err) = generate('module = (int NULL, int SIZE_MAX, int EXIT_SUCCESS, int INT64_MAX)',
        '--prefix', 'record');
    is($status, 0, 'a product-only schema with standard C macro field names') or diag($err);
    my ($status2, $enums, $err2) = generate('choice = ERROR | CONTINUE | SKIP | STOP', '--prefix', 'constant',
        '--visitor', "$dir/constants-walk.h", '--include', 'constants.h');
    is($status2, 0, 'enum-only schema can use callback control words as constructors') or diag($err2);
    my ($status3, $mutual, $err3) = generate('a = A(b? next) b = B(a? previous) attributes = Metadata(int n)',
        '--prefix', 'mutual');
    is($status3, 0, 'mutually recursive sums and contextual attributes type name') or diag($err3);
    return if $status || $status2 || $status3;
    program('minimal', { 'record.h' => $product, 'constants.h' => $enums, 'mutual.h' => $mutual,
        'minimal.c' => <<'C' });
#include "record.h"
#include "constants.h"
#include "mutual.h"
#include <assert.h>
static int on_error(int *count, constant_choice_t choice) {
    assert(choice == constant_ERROR);
    ++*count;
    return CONSTANT_WALK_CONTINUE;
}
#define CONSTANT_VISITOR_NAME constants
#define CONSTANT_VISITOR_CONTEXT int
#define CONSTANT_VISIT_ERROR on_error
#define CONSTANT_LEAVE_choice(c, n) CONSTANT_WALK_SKIP
#include "constants-walk.h"
int main(void) {
    record_arena a;
    record_arena_init(&a);
    record_module_t x = record_make_module(&a, 1, 2, 3, 4);
    assert(x && x->NULL_ == 1 && x->SIZE_MAX_ == 2);
    assert(x->EXIT_SUCCESS_ == 3 && x->INT64_MAX_ == 4);
    assert(record_validate_module(x, 1) && RECORD_CONSTRUCTOR_COUNT == 0);
    assert(constant_validate_choice(constant_ERROR, 1));
    assert(!constant_validate_choice((constant_choice_t)999, 1));
    int count = 0;
    assert(constants_choice(constant_ERROR, &count) == CONSTANT_WALK_CONTINUE && count == 1);
    mutual_arena b;
    mutual_arena_init(&b);
    mutual_a_t leaf = mutual_A(&b, mutual_b_none());
    mutual_b_t middle = mutual_B(&b, mutual_a_some(leaf));
    mutual_a_t root = mutual_A(&b, mutual_b_some(middle));
    assert(mutual_validate_a(root, 3) && !mutual_validate_a(root, 2));
    assert(mutual_Metadata(&b, 5));
    record_arena_destroy(&a);
    mutual_arena_destroy(&b);
    return 0;
}
C
};

subtest 'preamble options, stdin, comments, code envelopes and command line' => sub {
    my $named = File::Spec->catfile($dir, 'with spaces.h');
    my ($status, $out, $err) = generate("%option prefix=from_file header=\"$named\" no-enums\n%%\nop = A | B\n",
        '--prefix', 'from_cli', '--enums', '--no-line-directives');
    is($status, 0, 'quoted directive values and CLI overrides') or diag($err);
    like(read_file($named), qr/from_cli_A = 1/, 'boolean CLI override selects enum');
    unlike(read_file($named), qr/^#line/m, 'line directives can be disabled');
    ($status, $out, $err) = command("# comment\nmodule Input { t = C(int) } // eof", $^X, $generator, '-');
    is($status, 0, 'stdin and EOF line comments') or diag($err);
    like($out, qr/input_C\(/, 'module namespace generated from stdin');
    ($status, $out, $err) = generate("%{ static inline int answer(void) { return 8; } %}\r\n%prefix crlf\r\n%%\r\nt = C(int)\r\n%%\r\n/* %% */\r\n");
    is($status, 0, 'inline prologue and CRLF separators') or diag($err);
    like($out, qr/answer\(void\)/, 'inline prologue retained');
    ($status, $out, $err) = generate("%{\n// continued comment \\\n%} still a comment\nstatic inline int x(void) { return 0; }\n%}\n%%\nt = C\n");
    is($status, 0, 'C line comments respect backslash-newline continuation') or diag($err);
    like($out, qr/static inline int x\(void\)/, 'continued comment does not close the prologue early');
    ($status, $out, $err) = generate("/*\n%%\n*/\n-- %% ignored\n%%\nt = C(int) /* %% */\n");
    is($status, 0, 'comment-contained separators ignored') or diag($err);
    ($status, $out, $err) = generate("%header \"$dir/overridden.h\"\n%source \"$dir/overridden.c\"\n%%\nt = C(int)\n", '--header-only');
    is($status, 0, 'CLI header-only overrides a source directive') or diag($err);
    ok(!-e "$dir/overridden.c", 'overridden implementation is not emitted');
    like(read_file("$dir/overridden.h"), qr/static inline.*asdl_C\(/, 'header contains implementations');
    ($status, $out, $err) = generate('t = C(int)', '--check', '--schema', '-');
    is($status, 0, 'check plus schema to stdout') or diag($err);
    is(decode_json($out)->{types}[0]{name}, 't', 'stdout contains only schema JSON');
    ($status, $out, $err) = generate('t = C(int)', '--check', '--header', "$dir/not_created.h");
    is($status, 0, 'check-only mode succeeds') or diag($err);
    is($out, '', 'check-only is quiet');
    ok(!-e "$dir/not_created.h", 'check-only does not create C outputs');
    ($status, $out, $err) = generate('t = C(int)', '--visitor', "$dir/install-walk.h", '--include', 'project/tree.h');
    is($status, 0, 'visitor can accompany a redirected header') or diag($err);
    like(read_file("$dir/install-walk.h"), qr/#include "project\/tree.h"/, 'explicit installed include path');
    for my $option ('--help', '--man', '--version') {
        ($status, $out, $err) = command(undef, $^X, $generator, $option);
        is($status, 0, "$option succeeds without input") or diag($err);
        ok(length($out) > 10, "$option has useful output");
    }
};

subtest 'malformed input is rejected before producing C' => sub {
    my @cases = (
        ['', qr/no ASDL definitions/],
        ['Expr = C', qr/lowercase type name/],
        ['expr = lower', qr/uppercase constructor/],
        ['_expr = C', qr/unexpected character/],
        ['expr = C(Missing value)', qr/lowercase field type/],
        ['expr = C(missing)', qr/undefined type 'missing'/],
        ['expr = C expr = D', qr/duplicate type/],
        ['a = C b = C', qr/duplicate constructor/],
        ['a = C | C', qr/duplicate constructor/],
        ['a = C(int x, string x)', qr/duplicate field/],
        ['a = C attributes (int x, int x)', qr/duplicate field/],
        ['int = C', qr/cannot redefine primitive/],
        ['string = C', qr/cannot redefine primitive/],
        ['identifier = C', qr/cannot redefine primitive/],
        ['a = C(int?* x)', qr/expected '\)'/],
        ['a = C(int,)', qr/lowercase field type/],
        ['a = ()', qr/lowercase field type/],
        ['a = C()', qr/lowercase field type/],
        ['a = C attributes ()', qr/lowercase field type/],
        ['a = (int x) attributes (int y)', qr/expected '='/],
        ['a = C(int, (int, int)*)', qr/lowercase field type/],
        ['a = C |', qr/uppercase constructor/],
        ['a = C(int', qr/expected '\)'/],
        ['a = (a self)', qr/recursive product type: a -> a/],
        ['a = (b next) b = (a prev)', qr/recursive product type/],
        ['a = (a? next)', qr/recursive product type/],
        ['a = (a* next)', qr/recursive product type/],
        ['a = (b next) b = Stop | More(a prev)', qr/recursive product type/],
        ['a = (b next) b = C attributes (a prev)', qr/recursive product type/],
        ['module A { a = C } trailing', qr/unexpected text after definitions/],
        ['module A { a = C', qr/expected '\}'/],
        ['a = C /* missing', qr/unterminated block comment/],
        ["%{\n/* missing\n", qr/unterminated C block comment/],
        ["%{\nconst char *s = \"missing\n", qr/unterminated C literal/],
        ["%{\nint x;\n", qr/unterminated %\{/],
        ["%prefix foo\na = C", qr/expected initial %%/],
        ["a = C\n%%\nint x;\n", qr/epilogue requires an initial %%/],
        ["%% a = C\n", qr/%% must occupy a line/],
        ["%%\na = C\n%prefix late\n", qr/unexpected character '%'/],
        ["%mystery value\n%%\na = C", qr/unknown option/],
        ["%enums perhaps\n%%\na = C", qr/invalid boolean/],
        ["%prefix\n%%\na = C", qr/requires a nonempty value/],
        ["%option\n%%\na = C", qr/requires at least one option/],
        ["%option no-enums=off\n%%\na = C", qr/does not take a value/],
        ["%prefix \"missing\n%%\na = C", qr/unterminated quoted option/],
        ["%prefix first second\n%%\na = C", qr/takes at most one value/],
    );
    for my $case (@cases) {
        my ($status, $out, $err) = generate($case->[0]);
        isnt($status, 0, 'rejects ' . ($case->[0] =~ s/\n/ /gr));
        is($out, '', 'no partial C output');
        like($err, $case->[1], 'specific diagnostic');
        like($err, qr/grammar\d+\.asdl:\d+:\d+:/, 'source line and column');
    }
    for my $options (
        ['--unknown'], ['--prefix', '_reserved'], ['--guard', 'bad-name'],
        ['--source', "$dir/only.c"], ['--visitor', "$dir/no-header.h"],
        ['--header-only', '--source', "$dir/conflict.c"],
        ['--schema', '-'], ['--guard', 'asdl_int_t'],
    ) {
        my ($status, $out, $err) = generate('t = C(int)', @$options);
        isnt($status, 0, "invalid CLI: @$options");
        is($out, '', 'no partial output for bad CLI');
        ok(length($err), 'CLI diagnostic');
    }
    my ($status, $out, $err) = generate('t = OK', '--prefix', 'ASDL');
    isnt($status, 0, 'rejects collision between enum constant and runtime constant');
    like($err, qr/generated C symbol.*conflicts/, 'target-name collision explained');
    is($out, '', 'no C for target-name collision');
};

subtest 'output aliases and failed publication preserve existing files' => sub {
    my $header = File::Spec->catfile($dir, 'preserved.h');
    write_file($header, 'sentinel');
    my ($status, $out, $err) = generate('broken = C(undefined)', '--header', $header);
    isnt($status, 0, 'invalid grammar cannot overwrite header');
    is(read_file($header), 'sentinel', 'existing header unchanged');
    ($status, $out, $err) = generate('t = C(int)', '--header', $header,
        '--visitor', "$dir/missing-directory/walk.h");
    isnt($status, 0, 'invalid output directory fails before publication');
    is(read_file($header), 'sentinel', 'header survives another destination failing');
    ($status, $out, $err) = generate('t = C(int)', '--header', $header, '--source', $header);
    isnt($status, 0, 'duplicate output paths rejected');
    like($err, qr/aliases/, 'alias diagnostic');
    is(read_file($header), 'sentinel', 'duplicate outputs do not truncate file');
    my $input = File::Spec->catfile($dir, 'protect.asdl');
    write_file($input, 't = C(int)');
    ($status, $out, $err) = command(undef, $^X, $generator, '--header', $input, $input);
    isnt($status, 0, 'input cannot also be an output');
    is(read_file($input), 't = C(int)', 'input retained');
    my $link = File::Spec->catfile($dir, 'protect-link.h');
    if (symlink($input, $link)) {
        ($status, $out, $err) = command(undef, $^X, $generator, '--header', $link, $input);
        isnt($status, 0, 'symlink to input cannot be overwritten');
        is(read_file($input), 't = C(int)', 'input behind symlink retained');
    }
    my $hardlink = File::Spec->catfile($dir, 'protect-hardlink.h');
    if (link($input, $hardlink)) {
        ($status, $out, $err) = command(undef, $^X, $generator, '--header', $hardlink, $input);
        isnt($status, 0, 'hardlink to input cannot be overwritten');
    }
};

done_testing();
