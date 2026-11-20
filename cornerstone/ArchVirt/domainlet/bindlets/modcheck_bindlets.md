# Modcheck Bindlets

`modcheck` provides JSON-serializable, stateless model-checking kernels. A
consumer binds a kernel by importing its module and invoking its exported
`KERNEL` with a payload and one of the advertised operation names:

```python
from domainlet.modcheck.reachability_checker import KERNEL

result = KERNEL.run(
    {
        "initial_state": {"x": 0},
        "relation": relation,
        "predicate": {"key": "x", "equals": 2},
    },
    operation="modcheck_reachability_checker_witness",
)["result"]
```

The result has a `status`, `value`, and diagnostic records. Invalid payloads
and unsupported operations are reported through diagnostics rather than by
leaking kernel-specific exceptions. Callers must treat returned values as
owned results; kernels do not retain mutable state between invocations.

## Kernel families

| Family | Kernels |
| --- | --- |
| Explicit state space | `state_store`, `transition_relation`, `bfs_explorer`, `dfs_explorer`, `reachability_checker`, `deadlock_detector`, `counterexample_trace` |
| Temporal logic | `ltl_parser`, `ltl_evaluator`, `ctl_parser`, `ctl_evaluator`, `buchi_automaton`, `product_construction`, `emptiness_checker`, `fixpoint_engine` |
| Abstraction and refinement | `abstract_domain`, `predicate_abstraction`, `cegar_state_store`, `cegar_loop`, `spuriousness_checker`, `refinement_strategy` |
| Bounded verification | `unroller`, `sat_solver_interface`, `bmc_property_encoder`, `bmc_incremental_solver`, `k_induction`, `interpolation_engine` |
| Symbolic verification | `bdd_manager`, `symbolic_state_set`, `symbolic_transition_relation`, `symbolic_reachability`, `symbolic_ctl_model_checker` |
| Orchestration | `kernel_orchestrator`, `verification_pipeline`, `verdict_reporter` |

## Discovery contract

Each module exports `KERNEL`, a `ModcheckKernel` descriptor. Its stable name
is `modcheck_<module>`, and `KERNEL.operations` is the authoritative list of
operation names. The kernel contract reports deterministic, pure operations,
JSON-serializable inputs and outputs, and diagnostics as the failure channel.

Bindlets should expose the full operation string rather than reconstructing
names from user input. This keeps calls compatible with runtime discovery and
with `modcheck/tests/test_kernels.py`, which imports every kernel and invokes
every advertised operation.

## Operation inventory

The current public surface has 93 operations. Query the runtime descriptor
instead of duplicating this table in code:

```python
from domainlet.modcheck.bmc_incremental_solver import KERNEL

print(KERNEL.name)
print(KERNEL.operations)
print(KERNEL.contract)
```

For a complete inventory across modules, enumerate `domainlet.modcheck` with
`pkgutil.iter_modules`, import each module, and read `KERNEL.operations` when
present. This is the same compatibility check exercised by the test suite.
