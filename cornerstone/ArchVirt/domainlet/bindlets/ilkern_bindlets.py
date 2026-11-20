"""Bindlet wrappers for ILKERN composable intermediate-language domainlets.

ILKERN kernels are intentionally small: they operate on a uniform JSON-shaped
module (functions, blocks, instructions, regions, dialects) and expose one or
more composable operations.  The bindlet layer keeps that model visible to
``Domfix`` by registering one bindlet class per ILKERN kernel through the
standard :class:`domainlet.bindlet_base.AbstractDomainlet` lifecycle.
"""

from __future__ import annotations

import json
from importlib import import_module
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
)


ILKERN_MODULES: dict[str, str] = {
    "core_il_block": "domainlet.ilkern.core.il_block",
    "core_il_dialect": "domainlet.ilkern.core.il_dialect",
    "core_il_function": "domainlet.ilkern.core.il_function",
    "core_il_instruction": "domainlet.ilkern.core.il_instruction",
    "core_il_module": "domainlet.ilkern.core.il_module",
    "core_il_region": "domainlet.ilkern.core.il_region",
    "core_il_type": "domainlet.ilkern.core.il_type",
    "core_il_unit": "domainlet.ilkern.core.il_unit",
    "core_il_value": "domainlet.ilkern.core.il_value",
    "lowering_ast_importer": "domainlet.ilkern.lowering.ast_importer",
    "lowering_backend_bridge": "domainlet.ilkern.lowering.backend_bridge",
    "lowering_call_abi_builder": "domainlet.ilkern.lowering.call_abi_builder",
    "lowering_cfg_builder": "domainlet.ilkern.lowering.cfg_builder",
    "lowering_closure_lowering": "domainlet.ilkern.lowering.closure_lowering",
    "lowering_codegen_bridge": "domainlet.ilkern.lowering.codegen_bridge",
    "lowering_exception_builder": "domainlet.ilkern.lowering.exception_builder",
    "lowering_hir_builder": "domainlet.ilkern.lowering.hir_builder",
    "lowering_isel_bridge": "domainlet.ilkern.lowering.isel_bridge",
    "lowering_lir_builder": "domainlet.ilkern.lowering.lir_builder",
    "lowering_memory_model": "domainlet.ilkern.lowering.memory_model",
    "lowering_mir_builder": "domainlet.ilkern.lowering.mir_builder",
    "lowering_phi_builder": "domainlet.ilkern.lowering.phi_builder",
    "lowering_regalloc_bridge": "domainlet.ilkern.lowering.regalloc_bridge",
    "lowering_semantic_bridge": "domainlet.ilkern.lowering.semantic_bridge",
    "lowering_ssa_builder": "domainlet.ilkern.lowering.ssa_builder",
    "profile_hil": "domainlet.ilkern.profiles.hil_profile",
    "profile_compute_backend": "domainlet.ilkern.profiles.compute_backend_profile",
    "profile_lir": "domainlet.ilkern.profiles.lir_profile",
    "profile_llvm": "domainlet.ilkern.profiles.llvm_profile",
    "profile_mir": "domainlet.ilkern.profiles.mir_profile",
    "profile_mlir": "domainlet.ilkern.profiles.mlir_profile",
    "profile_oop": "domainlet.ilkern.profiles.oop_profile",
    "profile_qbe": "domainlet.ilkern.profiles.qbe_profile",
    "profile_vtable": "domainlet.ilkern.profiles.vtable_profile",
    "compose_pipeline": "domainlet.ilkern.compose.pipeline",
    "compute_artifact_bridge": "domainlet.ilkern.compute.artifact_bridge",
    "compute_backend_relay": "domainlet.ilkern.compute.backend_relay",
    "compute_compute_ir": "domainlet.ilkern.compute.compute_ir",
    "compute_launch_geometry": "domainlet.ilkern.compute.launch_geometry",
    "compute_memory_model": "domainlet.ilkern.compute.memory_model",
    "compute_resource_bindings": "domainlet.ilkern.compute.resource_bindings",
    "memory_allocator_ffi": "domainlet.ilkern.memory.allocator_ffi",
    "memory_bump_allocator": "domainlet.ilkern.memory.bump_allocator",
    "memory_card_table": "domainlet.ilkern.memory.card_table",
    "memory_freelist_allocator": "domainlet.ilkern.memory.freelist_allocator",
    "memory_gc_scheduler": "domainlet.ilkern.memory.gc_scheduler",
    "memory_generational_plan": "domainlet.ilkern.memory.generational_plan",
    "memory_immix_space": "domainlet.ilkern.memory.immix_space",
    "memory_large_object_space": "domainlet.ilkern.memory.large_object_space",
    "memory_mark_sweep_plan": "domainlet.ilkern.memory.mark_sweep_plan",
    "memory_satb_barrier": "domainlet.ilkern.memory.satb_barrier",
    "memory_semispace_plan": "domainlet.ilkern.memory.semispace_plan",
    "memory_write_barrier": "domainlet.ilkern.memory.write_barrier",
    "frontend_ast_schema_import": "domainlet.ilkern.frontend.ast_schema_import",
    "frontend_directive_emit": "domainlet.ilkern.frontend.directive_emit",
    "frontend_directive_import": "domainlet.ilkern.frontend.directive_import",
    "frontend_lexical_import": "domainlet.ilkern.frontend.lexical_import",
    "frontend_rewrite_import": "domainlet.ilkern.frontend.rewrite_import",
    "frontend_semantic_import": "domainlet.ilkern.frontend.semantic_import",
    "frontend_validation_import": "domainlet.ilkern.frontend.validation_import",
    "meta_source_map": "domainlet.ilkern.meta.source_map",
    "metaprog_lua_bridge": "domainlet.ilkern.metaprog.lua_bridge",
    "graph_sea_of_nodes": "domainlet.ilkern.graphs.sea_of_nodes",
    "form_anf_ir": "domainlet.ilkern.forms.anf_ir",
    "form_cps_ir": "domainlet.ilkern.forms.cps_ir",
    "typed_ir": "domainlet.ilkern.typed.typed_ir",
    "ssa_advanced": "domainlet.ilkern.ssa.advanced_ssa",
    "backend_arch_bridge": "domainlet.ilkern.backend.arch_bridge",
    "backend_isel_suite": "domainlet.ilkern.backend.isel_suite",
    "backend_perf_profile": "domainlet.ilkern.backend.perf_profile",
    "backend_layout_peephole": "domainlet.ilkern.backend.layout_peephole",
    "backend_schedule_suite": "domainlet.ilkern.backend.schedule_suite",
    "analysis_alias_points_to": "domainlet.ilkern.analysis.alias_points_to",
    "analysis_dataflow_facts": "domainlet.ilkern.analysis.dataflow_facts",
    "analysis_escape_nullability": "domainlet.ilkern.analysis.escape_nullability",
    "analysis_taint_flow": "domainlet.ilkern.analysis.taint_flow",
    "dialects_dialect_composition": "domainlet.ilkern.dialects.dialect_composition",
    "dialects_dialect_conversion": "domainlet.ilkern.dialects.dialect_conversion",
    "dialects_dialect_evolution": "domainlet.ilkern.dialects.dialect_evolution",
    "dialects_dialect_pattern_rewriter": "domainlet.ilkern.dialects.dialect_pattern_rewriter",
    "dialects_dialect_verifier": "domainlet.ilkern.dialects.dialect_verifier",
    "graphs_dominance": "domainlet.ilkern.graphs.dominance",
    "graphs_loop_nest": "domainlet.ilkern.graphs.loop_nest",
    "opt_constant_sccp": "domainlet.ilkern.opt.constant_sccp",
    "opt_dead_code": "domainlet.ilkern.opt.dead_code",
    "opt_value_numbering": "domainlet.ilkern.opt.value_numbering",
    "rewrite_egraph_saturation": "domainlet.ilkern.rewrite.egraph_saturation",
    "transform_call_thread": "domainlet.ilkern.transform.call_thread",
    "transform_loop_hoist": "domainlet.ilkern.transform.loop_hoist",
    "verify_ir_verifier": "domainlet.ilkern.verify.ir_verifier",
    "polyhedral": "domainlet.ilkern.polyhedral",
    "concurrency": "domainlet.ilkern.concurrency",
    "interp": "domainlet.ilkern.interp",
    "ffi": "domainlet.ilkern.ffi",
    "intrinsics": "domainlet.ilkern.intrinsics",
    "runtime": "domainlet.ilkern.runtime",
}


class BaseIlkernBindlet(AbstractDomainlet):
    """Shared lifecycle for all ILKERN kernel bindlets."""

    _kernel_module: str = ""
    _capability: str = ""

    def _get_kernel(self):
        return import_module(self._kernel_module).KERNEL

    def _run_operation(self, operation: str, payload: dict[str, Any]) -> Any:
        kernel = self._get_kernel()
        result = kernel.run(payload, operation=operation)
        nested = result["result"]
        if nested["status"] != "ok":
            diagnostics = nested.get("diagnostics", [])
            message = "; ".join(
                diagnostic.get("message", "unknown")
                for diagnostic in diagnostics
            )
            raise RuntimeError(f"{kernel.name}: {message or nested['status']}")
        return nested["value"]

    def probe(self) -> DomainletInfo:
        kernel = self._get_kernel()
        return DomainletInfo(
            name=kernel.name,
            version="1.0.0",
            description=kernel.description,
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name=operation,
                    description=operation,
                    input_kind=InputKind.AST_JSON,
                    tags=["ilkern", kernel.category, kernel.stage],
                )
                for operation in kernel.operations
            ],
            config_schema={
                "kernel": "|".join(sorted(ILKERN_MODULES)),
                "capability": "|".join(kernel.operations),
            },
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", self._get_kernel().operations[0])

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        kernel = self._get_kernel()
        capability = self._capability or kernel.operations[0]
        value = self._run_operation(
            capability,
            {"data": context.data} if context.data else {},
        )
        return [
            AnalysisIssue(
                rule_id=f"{kernel.name}.{capability}",
                message=json.dumps(value, sort_keys=True, default=str),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
                extra={
                    "kernel": kernel.name,
                    "capability": capability,
                    "category": kernel.category,
                    "stage": kernel.stage,
                },
            )
        ]


class IlkernBindlet(BaseIlkernBindlet):
    """Generic configurable ILKERN bindlet.

    ``kernel`` may be any key from :data:`ILKERN_MODULES`.  This class is
    useful for callers that do not know the concrete ILKERN kernel until
    configuration time.
    """

    _kernel_module = "domainlet.ilkern.compose.pipeline"

    def configure(self, config: dict[str, str]) -> None:
        selected = config.get("kernel", self._kernel_module)
        if selected in ILKERN_MODULES:
            self._kernel_module = ILKERN_MODULES[selected]
        elif selected and "." in selected:
            self._kernel_module = selected
        self._capability = config.get("capability", self._get_kernel().operations[0])


def _bindlet_class_for(name: str, module: str) -> type[BaseIlkernBindlet]:
    class_name = "".join(part.title() for part in name.split("_")) + "Bindlet"
    return type(class_name, (BaseIlkernBindlet,), {"_kernel_module": module})


ILKERN_BINDLETS: dict[str, type[BaseIlkernBindlet]] = {
    name: _bindlet_class_for(name, module)
    for name, module in ILKERN_MODULES.items()
}


def get_ilkern_bindlet(name: str) -> BaseIlkernBindlet:
    """Return a configured instance for a named ILKERN kernel.

    ``name`` may be a registry key (``"verify_ir_verifier"``), a manifest
    kernel id (``"ilkern.verify.ir_verifier"``), or a dotted module path
    (``"domainlet.ilkern.verify.ir_verifier"``); unknown names raise
    ``ValueError`` rather than silently substituting another kernel.
    """
    cls = ILKERN_BINDLETS.get(name)
    if cls is None:
        if name in ILKERN_MODULES:
            bindlet = IlkernBindlet()
            bindlet.configure({"kernel": ILKERN_MODULES[name]})
            return bindlet
        if name in ILKERN_MODULES.values():
            bindlet = IlkernBindlet()
            bindlet.configure({"kernel": name})
            return bindlet
        if name.startswith("ilkern."):
            module = "domainlet." + name
            if module in ILKERN_MODULES.values():
                bindlet = IlkernBindlet()
                bindlet.configure({"kernel": module})
                return bindlet
        raise ValueError(
            f"Unknown ILKERN bindlet: {name!r}. Available: {sorted(ILKERN_MODULES)}"
        )
    return cls()
