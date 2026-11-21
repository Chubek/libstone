# Generated from Cephyr QML fragments; edit .qpass and Orchestrator.qml instead.
# === Desugar.qpass ===
# Cephyr QML / desugar — normalize surface scalar constructs.
# These rules apply to pure, checked integer/Boolean expression MIR.
rewrite desugar inc_to_add : inc(?x) => add(?x, 1);
rewrite desugar dec_to_sub : dec(?x) => sub(?x, 1);
rewrite desugar square_to_mul : square(?x) => mul(?x, ?x);
rewrite desugar twice_to_add : twice(?x) => add(?x, ?x);
rewrite desugar greater_equal : ge(?a, ?b) => not(lt(?a, ?b));
rewrite desugar less_equal : le(?a, ?b) => not(gt(?a, ?b));
rewrite desugar not_equal : ne(?a, ?b) => not(eq(?a, ?b));
rewrite desugar implication : implies(?a, ?b) => or(not(?a), ?b);
rewrite desugar boolean_xor : xor(?a, ?b) => or(and(?a, not(?b)), and(not(?a), ?b));

# === Sema.qpass ===
# Cephyr QML / sema — declarative hard errors.
# General type checking, arity checking and unbound variables are built into QML.
reject sema poison_ir : poison();
reject sema opaque_pointer_access : unsafe_ptr(_);
reject sema unlowered_inline_asm : inline_asm(_);
reject sema unlowered_intrinsic : intrin(_, _);

# === Canonicalize.qpass ===
# Cephyr QML / canonicalize — shape normalization.
rewrite canonicalize remove_paren : paren(?x) => ?x;
rewrite canonicalize simplify_sub_zero : sub(?x, 0) => ?x;
rewrite canonicalize simplify_div_one : div(?x, 1) => ?x;
rewrite canonicalize double_boolean_negation : not(not(?x)) => ?x;
rewrite canonicalize redundant_selection : if(?cond, ?same, ?same) => ?same;
rewrite canonicalize if_not : if(not(?cond), ?yes, ?no) => if(?cond, ?no, ?yes);

# === Optimize.qpass ===
# Cephyr QML / optimize — local strength reduction on pure scalar MIR.
# Reassociation and duplication need explicit effect/overflow proofs for general C IR.
rewrite optimize multiply_by_two : mul(?x, 2) => add(?x, ?x);
rewrite optimize multiply_by_four : mul(?x, 4) => add(add(?x, ?x), add(?x, ?x));
rewrite optimize redundant_select : if(true, ?x, ?y) => ?x;
rewrite optimize inverted_select : if(false, ?x, ?y) => ?y;

# === Lower.qpass ===
# Cephyr QML / lower — legalize host-independent scalar intrinsics.
rewrite lower select_to_if : select(?c, ?yes, ?no) => if(?c, ?yes, ?no);
rewrite lower maximum_to_if : max(?a, ?b) => if(gt(?a, ?b), ?a, ?b);
rewrite lower minimum_to_if : min(?a, ?b) => if(lt(?a, ?b), ?a, ?b);
rewrite lower absolute_to_if : abs(?a) => if(lt(?a, 0), neg(?a), ?a);
rewrite lower nonzero_test : nonzero(?a) => not(eq(?a, 0));
rewrite lower bool_to_int : boolint(?b) => if(?b, 1, 0);

# === AbsInterp.qpass ===
# Cephyr QML / absinterp — first bounded interval-domain simplifications.
# This is NOT whole-program abstract interpretation. It only collapses
# structurally obvious facts before the full semantic validation pass.
rewrite absinterp singleton_interval_membership : contains(interval(?x, ?x), ?x) => true;
rewrite absinterp reflexive_interval_equivalence : interval_eq(interval(?lo, ?hi), interval(?lo, ?hi)) => true;
rewrite absinterp singleton_nonempty : interval_nonempty(interval(?x, ?x)) => true;

# === SymExec.qpass ===
# Cephyr QML / symexec — path-constraint simplifications.
# This is NOT a general SMT-backed symbolic executor.
rewrite symexec proven_assumption : assume(true, ?body) => ?body;
rewrite symexec true_path : path(true, ?body) => ?body;
rewrite symexec false_path : path(false, ?body) => unreachable();
rewrite symexec identical_choices : choose(?cond, ?a, ?a) => ?a;
rewrite symexec true_choice : choose(true, ?a, ?b) => ?a;
rewrite symexec false_choice : choose(false, ?a, ?b) => ?b;

# === Format.qpass ===
# Cephyr QML / format — remove formatting wrappers from expression MIR.
# Text formatting happens in the host adapter, not through IR substitution.
rewrite format flatten_group : group(?value) => ?value;
rewrite format normalize_format : format(?value) => ?value;

# === PrettyPrint.qpass ===
# Cephyr QML / prettyprint — explicit print-boundary wrappers.
# qml/runtime.py renders the final typed MIR as QML or JSON.
rewrite prettyprint unwrap_pretty : pretty(?value) => ?value;
rewrite prettyprint unwrap_render : render(?value) => ?value;

# Cephyr QML scaffold orchestration — combine with bundle.py.
# All .qpass definitions are prepended in deterministic order.
# The sample begins with surface constructs and ends in normalized, checked MIR.
pass desugar;
pass absinterp;
pass symexec;
pass lower;
pass canonicalize;
pass optimize;
pass format;
pass prettyprint;
pass sema;

run pretty(format(assume(true,
    select(
        ge(add(20, 22), 42),
        add(max(20, 21), min(21, 21)),
        0
    )
)));
