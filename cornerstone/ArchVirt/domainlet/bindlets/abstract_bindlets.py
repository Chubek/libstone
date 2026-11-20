"""
abstract_bindlets — Bindlet wrappers for abstract.* domainlets.

Exposes abstract interpretation via the executor, transfer functions,
abstract state machines, abstract state, and abstract VM.
"""

from __future__ import annotations

import tempfile
import os
from typing import Any

from domainlet.bindlet_base import (
    AbstractDomainlet,
    AnalysisContext,
    AnalysisIssue,
    Confidence,
    DomainletCapability,
    DomainletInfo,
    InputKind,
    Severity,
    SourceSpan,
)


def _parse_context(context: AnalysisContext) -> Any:
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".dump", delete=False
    ) as f:
        f.write(context.data)
        dump_path = f.name
    try:
        from domainlet.domgate_feed import parsedump
        return parsedump(dump_path)
    finally:
        os.unlink(dump_path)


# ══════════════════════════════════════════════════════════════════════
#  Abstract Executor Bindlet
# ══════════════════════════════════════════════════════════════════════

class AbstractExecBindlet(AbstractDomainlet):
    """Bindlet for the abstract executor — execute programs over abstract domains."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="abstract.executor",
            version="0.6.0",
            description="Abstract executor — run programs with abstract values, track abstract state changes",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="abstract-execute",
                    description="Execute a function using abstract interpretation",
                    input_kind=InputKind.AST_JSON,
                    tags=["abstract", "execution", "soundness"],
                ),
                DomainletCapability(
                    name="state-summary",
                    description="Collect abstract state after each statement",
                    input_kind=InputKind.AST_JSON,
                    tags=["abstract", "state", "summary"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "abstract-execute")
        self._function_name: str | None = config.get("function") or None
        self._domain_name = config.get("domain", "interval")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.abstract.executor import AbstractExecutor

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            func_cfgs = build_all_cfgs(configuration)
            for func_name, cfg in func_cfgs.items():
                if self._function_name and func_name != self._function_name:
                    continue

                try:
                    executor = AbstractExecutor(cfg, domain=self._domain_name)
                    result = executor.run()

                    summary = getattr(result, "summary", str(result))
                    issues.append(
                        AnalysisIssue(
                            rule_id="abstract.result",
                            message=f"Abstract execution of '{func_name}' ({self._domain_name}): {summary}",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

                    if self._capability == "state-summary":
                        states = getattr(result, "states", [])
                        for i, state in enumerate(states[:20]):
                            issues.append(
                                AnalysisIssue(
                                    rule_id="abstract.state",
                                    message=f"State {i} at '{func_name}': {state}",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.MEDIUM,
                                )
                            )

                except Exception as exc:
                    issues.append(
                        AnalysisIssue(
                            rule_id="abstract.error",
                            message=f"Abstract execution failed for '{func_name}': {exc}",
                            severity=Severity.ERROR,
                            confidence=Confidence.LOW,
                        )
                    )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Abstract Interpreter Bindlet
# ══════════════════════════════════════════════════════════════════════

class AbstractInterpBindlet(AbstractDomainlet):
    """Bindlet for C transfer functions and expression evaluation over abstract domains."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="abstract.interp",
            version="0.6.0",
            description="C Transfer Functions — evaluate C expressions in the abstract, track abstract state",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="evaluate-expression",
                    description="Evaluate a C expression over an abstract domain",
                    input_kind=InputKind.AST_JSON,
                    tags=["abstract", "expression", "interval"],
                ),
                DomainletCapability(
                    name="transfer-statement",
                    description="Apply a transfer function for a C statement",
                    input_kind=InputKind.AST_JSON,
                    tags=["abstract", "transfer", "statement"],
                ),
                DomainletCapability(
                    name="analyze-function",
                    description="Run abstract interpretation on a whole function",
                    input_kind=InputKind.AST_JSON,
                    tags=["abstract", "function", "soundness"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "analyze-function")
        self._domain_name = config.get("domain", "interval")
        self._function_name: str | None = config.get("function") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.abstract.interp import (
            CTransferFunction,
            CExpressionEvaluator,
            IntervalDomain,
        )

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        domain = IntervalDomain()

        for configuration in data.configurations:
            func_cfgs = build_all_cfgs(configuration)
            for func_name, cfg in func_cfgs.items():
                if self._function_name and func_name != self._function_name:
                    continue

                try:
                    if self._capability == "analyze-function":
                        transfer = CTransferFunction(cfg, domain)
                        result = transfer.analyze()

                        issues.append(
                            AnalysisIssue(
                                rule_id="interp.result",
                                message=f"Function '{func_name}' ({self._domain_name}): "
                                        f"abstract interpretation complete, "
                                        f"{len(getattr(result, 'issues', []))} potential issue(s)",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                        for finding in getattr(result, "issues", []):
                            issues.append(
                                AnalysisIssue(
                                    rule_id=f"interp.{getattr(finding, 'kind', 'finding')}",
                                    message=str(finding),
                                    severity=Severity.WARNING,
                                    confidence=Confidence.MEDIUM,
                                )
                            )

                    elif self._capability == "evaluate-expression":
                        evaluator = CExpressionEvaluator(cfg, domain)
                        exprs = evaluator.collect_expressions()
                        for i, expr in enumerate(exprs[:20]):
                            val = evaluator.evaluate(expr)
                            issues.append(
                                AnalysisIssue(
                                    rule_id="interp.expr",
                                    message=f"Expression {i}: {expr} → {val}",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.MEDIUM,
                                )
                            )

                except Exception as exc:
                    issues.append(
                        AnalysisIssue(
                            rule_id="interp.error",
                            message=f"C transfer function failed for '{func_name}': {exc}",
                            severity=Severity.ERROR,
                            confidence=Confidence.LOW,
                        )
                    )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Abstract State Machine Bindlet
# ══════════════════════════════════════════════════════════════════════

class AbstractSMBindlet(AbstractDomainlet):
    """Bindlet for abstract state machines — rule-based abstract interpretation steps."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="abstract.sm",
            version="0.6.0",
            description="Abstract State Machine — rule-based operational semantics in the abstract",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="step",
                    description="Perform a single abstract state-machine step",
                    input_kind=InputKind.AST_JSON,
                    tags=["asm", "step", "operational"],
                ),
                DomainletCapability(
                    name="run",
                    description="Run the abstract state machine until fixpoint or bound",
                    input_kind=InputKind.AST_JSON,
                    tags=["asm", "run", "fixpoint"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "run")
        self._max_steps = int(config.get("max_steps", "100"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.abstract.sm import step, run as sm_run, State, Rule

        issues: list[AnalysisIssue] = []

        try:
            if self._capability == "run":
                initial_state = State()
                final_state = sm_run(initial_state, max_steps=self._max_steps)
                steps_taken = getattr(
                    final_state, "step_count", self._max_steps)
                issues.append(
                    AnalysisIssue(
                        rule_id="asm.run",
                        message=f"ASM completed: {steps_taken} step(s), final state: {final_state}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

            elif self._capability == "step":
                s = State()
                for i in range(min(self._max_steps, 50)):
                    s = step(s)
                    issues.append(
                        AnalysisIssue(
                            rule_id="asm.step",
                            message=f"Step {i}: {s}",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="asm.error",
                    message=f"ASM execution failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Abstract State Bindlet
# ══════════════════════════════════════════════════════════════════════

class AbstractStateBindlet(AbstractDomainlet):
    """Bindlet for abstract state — abstract store, memory regions, abstract heaps."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="abstract.state",
            version="0.6.0",
            description="Abstract State — abstract store, memory regions, heap abstraction",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build-abstract-store",
                    description="Build an abstract store from variables and their abstract values",
                    input_kind=InputKind.AST_JSON,
                    tags=["abstract", "store", "memory"],
                ),
                DomainletCapability(
                    name="resolve-pointer",
                    description="Resolve a pointer through abstract memory regions",
                    input_kind=InputKind.AST_JSON,
                    tags=["abstract", "pointer", "alias"],
                ),
                DomainletCapability(
                    name="memory-safety-check",
                    description="Check for memory safety issues in the abstract state",
                    input_kind=InputKind.AST_JSON,
                    tags=["abstract", "memory", "safety"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "memory-safety-check")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.abstract.state import (
            AbstractState as AbsState,
            AbstractStore,
            MemoryRegion,
        )

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                if self._capability == "memory-safety-check":
                    store = AbstractStore()
                    for var in getattr(configuration, "variables", []):
                        store.register(var)

                    regions = store.memory_regions()
                    for region in regions:
                        overlap = store.find_overlaps(region)
                        if overlap:
                            issues.append(
                                AnalysisIssue(
                                    rule_id="abstract.memory-overlap",
                                    message=f"Memory region overlap: {region} overlaps with {overlap}",
                                    severity=Severity.WARNING,
                                    confidence=Confidence.MEDIUM,
                                )
                            )

                    leaks = store.find_leaks()
                    for leak in leaks:
                        issues.append(
                            AnalysisIssue(
                                rule_id="abstract.memory-leak",
                                message=f"Potential memory leak: {leak}",
                                severity=Severity.WARNING,
                                confidence=Confidence.MEDIUM,
                                cwe_ids=[401],
                            )
                        )

                    if not issues:
                        issues.append(
                            AnalysisIssue(
                                rule_id="abstract.memory-ok",
                                message="No memory safety issues detected in abstract state",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.MEDIUM,
                            )
                        )

                elif self._capability == "build-abstract-store":
                    store = AbstractStore()
                    for var in getattr(configuration, "variables", []):
                        store.register(var)
                    issues.append(
                        AnalysisIssue(
                            rule_id="abstract.store",
                            message=f"Abstract store: {store.size()} variable(s) tracked",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

                elif self._capability == "resolve-pointer":
                    store = AbstractStore()
                    for var in getattr(configuration, "variables", []):
                        store.register(var)

                    pointers = store.collect_pointers()
                    for ptr_expr, targets in pointers:
                        issues.append(
                            AnalysisIssue(
                                rule_id="abstract.pointer",
                                message=f"Pointer '{ptr_expr}' may point to: {targets}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.MEDIUM,
                            )
                        )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="abstract.state-error",
                        message=f"Abstract state analysis failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Abstract VM Bindlet
# ══════════════════════════════════════════════════════════════════════

class AbstractVMBindlet(AbstractDomainlet):
    """Bindlet for the abstract VM — compile and run programs in an abstract virtual machine."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="abstract.vm",
            version="0.6.0",
            description="Abstract Virtual Machine — compile C to abstract bytecode and execute symbolically",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compile",
                    description="Compile a C program to abstract bytecode",
                    input_kind=InputKind.AST_JSON,
                    tags=["abstract", "vm", "bytecode"],
                ),
                DomainletCapability(
                    name="execute",
                    description="Execute abstract bytecode in the abstract VM",
                    input_kind=InputKind.AST_JSON,
                    tags=["abstract", "vm", "execution"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "execute")
        self._function_name: str | None = config.get("function") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.abstract.vm import AbstractProgram, AbstractCompiler, CodeBlock

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                if self._capability == "compile":
                    compiler = AbstractCompiler(configuration)
                    program = compiler.compile()

                    issues.append(
                        AnalysisIssue(
                            rule_id="vm.compiled",
                            message=f"Compiled program: {len(program.blocks)} code block(s), "
                                    f"{program.instruction_count} instruction(s)",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

                elif self._capability == "execute":
                    compiler = AbstractCompiler(configuration)
                    program = compiler.compile()

                    for func_name, block in program.blocks.items():
                        if self._function_name and func_name != self._function_name:
                            continue

                        vm = AbstractProgram(program)
                        result = vm.execute(block)

                        issues.append(
                            AnalysisIssue(
                                rule_id="vm.executed",
                                message=f"Function '{func_name}': executed {result.steps} step(s), "
                                        f"final state: {result.final_state}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                        for warning in getattr(result, "warnings", []):
                            issues.append(
                                AnalysisIssue(
                                    rule_id="vm.warning",
                                    message=str(warning),
                                    severity=Severity.WARNING,
                                    confidence=Confidence.MEDIUM,
                                )
                            )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="vm.error",
                        message=f"Abstract VM operation failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues
