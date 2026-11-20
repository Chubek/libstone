"""
domainlet.bindlets — Canonical Bindlet Implementations
======================================================

This package provides one ``AbstractDomainlet`` subclass per domainlet
module.  Importing this package causes all bindlet classes to be
registered with the ``AbstractDomainlet._registry``, making them
discoverable by ``AbstractDomainlet.discover()`` and loadable by
the C bindlet engine (``Domfix``) via dompybind.

Bindlet categories
------------------

* ``analysis_bindlets``   — ctrlflow, dataflow, distrib, interproc, path, taint, typing
* ``graph_bindlets``      — cfg, callgraph, codeprop, dependency, progdep, sysdep
* ``reporting_bindlets``  — checkers, plus_reporter
* ``symbolic_bindlets``   — symexec, constraints, rweqsat (e-graph), transition
* ``abstract_bindlets``   — executor, interp (C transfer), sm (state machine), state, vm
* ``lattice_bindlets``    — interval, domains (sign/parity/congruence), fixpoint, cost, memory, widening
* ``repair_bindlets``     — fault_pattern, naive_rules, constraint_repair, egraph_rewriter,
                           equality_saturation, genetic_prog, invariant_collect, llm_query,
                           localize_fault, synthesis_grammar
* ``frontend_bindlets``   — domgate_feed (parsedump, reporting, token extraction)
* ``fuzz_bindlets``       — target discovery, harnessing, corpus management,
                            feedback, guidance, reinforcement learning, solving,
                            minimization, and triage
* ``binary_bindlets``     — ELF, PE, Mach-O parsing and machine-code disassembly
* ``visualize_bindlets``  — visualize.ascii, visualize.export (terminal
                            rendering and structured format export for all
                            graph types)
* ``vmm_core``            — KVM bindings, guest memory, exits, and vCPU
* ``vmm_boot``            — kernel loader, boot params, command line, and platform tables
* ``vmm_interrupts``      — irqchip, irqfd, MSI routing, and timers
* ``vmm_io``              — buses, event loop, PCI, and I/O backends
* ``vmm_control``         — builder, lifecycle, snapshot, API, and sandbox
* ``vmm_virtio_adapter``  — VirtIO adapter layer for the VMM seams
* ``vmm_virtio``          — VirtIO device catalog and descriptor validation

Discovery
---------

::

    from domainlet.bindlet_base import AbstractDomainlet

    # Trigger registration by importing all bindlets
    import domainlet.bindlets

    # Enumerate available domainlets
    for info in AbstractDomainlet.discover():
        print(f"{info.name} v{info.version} — {info.description}")
"""

from domainlet.bindlets.analysis_bindlets import (
    CtrlFlowBindlet,
    DataflowBindlet,
    DistribBindlet,
    InterProcBindlet,
    PathBindlet,
    TaintBindlet,
    TypingBindlet,
)

from domainlet.bindlets.graph_bindlets import (
    CFGBindlet,
    CallGraphBindlet,
    CodePropBindlet,
    DependencyBindlet,
    ProgDepBindlet,
    SysDepBindlet,
)

from domainlet.bindlets.reporting_bindlets import (
    CheckersBindlet,
    PlusReporterBindlet,
)

from domainlet.bindlets.symbolic_bindlets import (
    SymbolicExecBindlet,
    ConstraintsBindlet,
    EqSatBindlet,
    TransitionBindlet,
)

from domainlet.bindlets.abstract_bindlets import (
    AbstractExecBindlet,
    AbstractInterpBindlet,
    AbstractSMBindlet,
    AbstractStateBindlet,
    AbstractVMBindlet,
)

from domainlet.bindlets.lattice_bindlets import (
    IntervalBindlet,
    AbstractDomainsBindlet,
    FixpointBindlet,
    CostAlgebraBindlet,
    MemoryModelBindlet,
    WideningBindlet,
)

from domainlet.bindlets.repair_bindlets import (
    FaultPatternBindlet,
    NaiveRulesBindlet,
    ConstraintRepairBindlet,
    EGraphRewriterBindlet,
    EqualitySaturationBindlet,
    GeneticProgBindlet,
    InvariantCollectBindlet,
    LLMQueryBindlet,
    LocalizeFaultBindlet,
    SynthesisGrammarBindlet,
)

from domainlet.bindlets.frontend_bindlets import (
    FrontendFeedBindlet,
)

from domainlet.bindlets.fuzz_bindlets import (
    FuzzAFLBindlet,
    FuzzAutoReproBindlet,
    FuzzConstraintsBindlet,
    FuzzCorpusBindlet,
    FuzzCoverageBindlet,
    FuzzCrashBindlet,
    FuzzDiffBindlet,
    FuzzDiscoveryBindlet,
    FuzzDistanceBindlet,
    FuzzFeedbackBindlet,
    FuzzFrontierBindlet,
    FuzzGuidanceBindlet,
    FuzzHarnessBindlet,
    FuzzInstrumentBindlet,
    FuzzMinimizeBindlet,
    FuzzMutateBindlet,
    FuzzObjectiveBindlet,
    FuzzOracleBindlet,
    FuzzQueueBindlet,
    FuzzRIBindlet,
    FuzzReachabilityBindlet,
    FuzzScheduleBindlet,
    FuzzSeedgenBindlet,
    FuzzSliceBindlet,
    FuzzSolveBindlet,
    FuzzTaintBindlet,
    FuzzTargetBindlet,
)

from domainlet.bindlets.binary_bindlets import (
    BinaryDisassemblyBindlet,
)

from domainlet.bindlets.blaskern_bindlets import (
    BlasKernBindlet,
)

from domainlet.bindlets.visualize_bindlets import (
    VisualizeAsciiBindlet,
    VisualizeExportBindlet,
)

from domainlet.bindlets.vmm_core_bindlets import (
    VmmCoreBindlet,
)

from domainlet.bindlets.vmm_boot_bindlets import (
    VmmBootBindlet,
)

from domainlet.bindlets.vmm_interrupts_bindlets import (
    VmmInterruptsBindlet,
)

from domainlet.bindlets.vmm_io_bindlets import (
    VmmIoBindlet,
)

from domainlet.bindlets.vmm_control_bindlets import (
    VmmControlBindlet,
)

from domainlet.bindlets.vmm_virtio_adapter_bindlets import (
    VmmVirtioAdapterBindlet,
)

from domainlet.bindlets.vmm_virtio import (
    VirtioBindlet,
)
from domainlet.bindlets.esimkern_bindlets import (
    BaseEsimKernelBindlet,
    ESIMKERN_BINDLETS,
    EsimPackageBindlet,
    get_esimkern_bindlet,
)
try:
    from domainlet.bindlets.ccweave_bindlets import CCWeaveBindlet
except ModuleNotFoundError:
    CCWeaveBindlet = None  # type: ignore[assignment]

try:
    from domainlet.bindlets.vmweave_bindlets import VMWeaveCoreDomainlet, VMWeaveFFIDomainlet
except ModuleNotFoundError:
    VMWeaveCoreDomainlet = None  # type: ignore[assignment]
    VMWeaveFFIDomainlet = None  # type: ignore[assignment]
from domainlet.bindlets.unittest_bindlets import UnittestBindlet, UnittestDomainlet
from domainlet.bindlets.buildsys_bindlets import BuildSysBindlet
from domainlet.bindlets.ilkern_bindlets import IlkernBindlet, get_ilkern_bindlet

__all__ = [
    # Analysis
    "CtrlFlowBindlet",
    "DataflowBindlet",
    "DistribBindlet",
    "InterProcBindlet",
    "PathBindlet",
    "TaintBindlet",
    "TypingBindlet",
    # Graphs
    "CFGBindlet",
    "CallGraphBindlet",
    "CodePropBindlet",
    "DependencyBindlet",
    "ProgDepBindlet",
    "SysDepBindlet",
    # Reporting
    "CheckersBindlet",
    "PlusReporterBindlet",
    # Symbolic
    "SymbolicExecBindlet",
    "ConstraintsBindlet",
    "EqSatBindlet",
    "TransitionBindlet",
    # Abstract
    "AbstractExecBindlet",
    "AbstractInterpBindlet",
    "AbstractSMBindlet",
    "AbstractStateBindlet",
    "AbstractVMBindlet",
    # Lattice
    "IntervalBindlet",
    "AbstractDomainsBindlet",
    "FixpointBindlet",
    "CostAlgebraBindlet",
    "MemoryModelBindlet",
    "WideningBindlet",
    # Repair
    "FaultPatternBindlet",
    "NaiveRulesBindlet",
    "ConstraintRepairBindlet",
    "EGraphRewriterBindlet",
    "EqualitySaturationBindlet",
    "GeneticProgBindlet",
    "InvariantCollectBindlet",
    "LLMQueryBindlet",
    "LocalizeFaultBindlet",
    "SynthesisGrammarBindlet",
    # Frontend
    "FrontendFeedBindlet",
    # Fuzz
    "FuzzAFLBindlet",
    "FuzzAutoReproBindlet",
    "FuzzConstraintsBindlet",
    "FuzzCorpusBindlet",
    "FuzzCoverageBindlet",
    "FuzzCrashBindlet",
    "FuzzDiffBindlet",
    "FuzzDiscoveryBindlet",
    "FuzzDistanceBindlet",
    "FuzzFeedbackBindlet",
    "FuzzFrontierBindlet",
    "FuzzGuidanceBindlet",
    "FuzzHarnessBindlet",
    "FuzzInstrumentBindlet",
    "FuzzMinimizeBindlet",
    "FuzzMutateBindlet",
    "FuzzObjectiveBindlet",
    "FuzzOracleBindlet",
    "FuzzQueueBindlet",
    "FuzzRIBindlet",
    "FuzzReachabilityBindlet",
    "FuzzScheduleBindlet",
    "FuzzSeedgenBindlet",
    "FuzzSliceBindlet",
    "FuzzSolveBindlet",
    "FuzzTaintBindlet",
    "FuzzTargetBindlet",
    # Binary
    "BinaryDisassemblyBindlet",
    # BLAS
    "BlasKernBindlet",
    # Visualize
    "VisualizeAsciiBindlet",
    "VisualizeExportBindlet",
    # VMM categories
    "VmmCoreBindlet",
    "VmmBootBindlet",
    "VmmInterruptsBindlet",
    "VmmIoBindlet",
    "VmmControlBindlet",
    "VmmVirtioAdapterBindlet",
    # VMM VirtIO
    "VirtioBindlet",
    # ESIM
    "EsimPackageBindlet",
    "BaseEsimKernelBindlet",
    "ESIMKERN_BINDLETS",
    "get_esimkern_bindlet",
    *( ["CCWeaveBindlet"] if CCWeaveBindlet is not None else [] ),
    *( ["VMWeaveCoreDomainlet", "VMWeaveFFIDomainlet"]
       if VMWeaveCoreDomainlet is not None and VMWeaveFFIDomainlet is not None else [] ),
    "UnittestBindlet",
    "UnittestDomainlet",
    "BuildSysBindlet",
    "IlkernBindlet",
    "get_ilkern_bindlet",
]
