"""DomPyBind binding definitions for domainlet.blaskern kernels.

Exposes the BLAS domainlet kernels to the C++ Domfix engine through
the bindlet bridge.  A single ``BlasKernBindlet`` class dispatches
to any of the 40 BLAS kernels based on the ``operation`` config key.

Usage (C++ host side)::

    import bindlets.blaskern_bindlets
    bindlet = bindlets.blaskern_bindlets.BlasKernBindlet()
    bindlet.configure({"operation": "gemm"})
    results = bindlet.execute(context)
"""

from __future__ import annotations

import importlib
from typing import Any, ClassVar

from domainlet.bindlet_base import (
    AbstractDomainlet,
    AnalysisContext,
    AnalysisIssue,
    BindletError,
    BindletErrorCode,
    DomainletCapability,
    DomainletInfo,
    Severity,
)

# Maps each BLAS operation name to its kernel module path.
_OPERATION_MODULE: dict[str, str] = {
    # Level 1 — Vector operations
    "dot": "domainlet.blaskern.dot",
    "dotu": "domainlet.blaskern.dotu",
    "dotc": "domainlet.blaskern.dotc",
    "nrm2": "domainlet.blaskern.nrm2",
    "asum": "domainlet.blaskern.asum",
    "iamax": "domainlet.blaskern.iamax",
    "swap": "domainlet.blaskern.swap",
    "copy": "domainlet.blaskern.copy",
    "axpy": "domainlet.blaskern.axpy",
    "scal": "domainlet.blaskern.scal",
    "rot": "domainlet.blaskern.rot",
    "rotg": "domainlet.blaskern.rotg",
    "rotm": "domainlet.blaskern.rotm",
    "rotmg": "domainlet.blaskern.rotmg",
    # Level 2 — Matrix–vector operations
    "gemv": "domainlet.blaskern.gemv",
    "gbmv": "domainlet.blaskern.gbmv",
    "symv": "domainlet.blaskern.symv",
    "hemv": "domainlet.blaskern.hemv",
    "trmv": "domainlet.blaskern.trmv",
    "tbmv": "domainlet.blaskern.tbmv",
    "tpmv": "domainlet.blaskern.tpmv",
    "trsv": "domainlet.blaskern.trsv",
    "tbsv": "domainlet.blaskern.tbsv",
    "tpsv": "domainlet.blaskern.tpsv",
    "ger": "domainlet.blaskern.ger",
    "geru": "domainlet.blaskern.geru",
    "gerc": "domainlet.blaskern.gerc",
    "syr": "domainlet.blaskern.syr",
    "her": "domainlet.blaskern.her",
    "syr2": "domainlet.blaskern.syr2",
    "her2": "domainlet.blaskern.her2",
    # Level 3 — Matrix–matrix operations
    "gemm": "domainlet.blaskern.gemm",
    "symm": "domainlet.blaskern.symm",
    "hemm": "domainlet.blaskern.hemm",
    "syrk": "domainlet.blaskern.syrk",
    "herk": "domainlet.blaskern.herk",
    "syr2k": "domainlet.blaskern.syr2k",
    "her2k": "domainlet.blaskern.her2k",
    "trmm": "domainlet.blaskern.trmm",
    "trsm": "domainlet.blaskern.trsm",
}

_OPERATION_LEVEL: dict[str, int] = {
    "dot": 1, "dotu": 1, "dotc": 1, "nrm2": 1, "asum": 1, "iamax": 1,
    "swap": 1, "copy": 1, "axpy": 1, "scal": 1, "rot": 1, "rotg": 1,
    "rotm": 1, "rotmg": 1,
    "gemv": 2, "gbmv": 2, "symv": 2, "hemv": 2, "trmv": 2, "tbmv": 2,
    "tpmv": 2, "trsv": 2, "tbsv": 2, "tpsv": 2, "ger": 2, "geru": 2,
    "gerc": 2, "syr": 2, "her": 2, "syr2": 2, "her2": 2,
    "gemm": 3, "symm": 3, "hemm": 3, "syrk": 3, "herk": 3, "syr2k": 3,
    "her2k": 3, "trmm": 3, "trsm": 3,
}

_OPERATION_DESCRIPTIONS: dict[str, str] = {
    "dot":     "Real dot product: x^T y",
    "dotu":    "Unconjugated complex dot product: x^T y",
    "dotc":    "Conjugated complex dot product: x^H y",
    "nrm2":    "Euclidean vector norm with stable scaled accumulation",
    "asum":    "Sum of absolute values; for complex, |Re| + |Im|",
    "iamax":   "Index of element with maximum BLAS absolute value",
    "swap":    "Exchange the elements of two vectors",
    "copy":    "Copy one vector into another",
    "axpy":    "Scaled vector addition: y := alpha*x + y",
    "scal":    "Scale a vector: x := alpha*x",
    "rot":     "Apply a plane rotation to paired vector elements",
    "rotg":    "Construct the parameters of a Givens plane rotation",
    "rotm":    "Apply a modified Givens transformation",
    "rotmg":   "Construct the parameters of a modified Givens transformation",
    "gemv":    "General matrix-vector multiplication",
    "gbmv":    "General band-matrix-vector multiplication",
    "symv":    "Symmetric matrix-vector multiplication",
    "hemv":    "Hermitian matrix-vector multiplication",
    "trmv":    "Triangular matrix-vector multiplication",
    "tbmv":    "Triangular band-matrix-vector multiplication",
    "tpmv":    "Triangular matrix-vector multiplication (packed storage)",
    "trsv":    "Solve a triangular system",
    "tbsv":    "Solve a triangular band system",
    "tpsv":    "Solve a triangular system (packed storage)",
    "ger":     "Real general rank-1 update",
    "geru":    "Unconjugated complex general rank-1 update",
    "gerc":    "Conjugated complex general rank-1 update",
    "syr":     "Symmetric rank-1 update",
    "her":     "Hermitian rank-1 update",
    "syr2":    "Symmetric rank-2 update",
    "her2":    "Hermitian rank-2 update",
    "gemm":    "General matrix multiplication",
    "symm":    "Symmetric matrix multiply",
    "hemm":    "Hermitian matrix multiply",
    "syrk":    "Symmetric rank-k update",
    "herk":    "Hermitian rank-k update",
    "syr2k":   "Symmetric rank-2k update",
    "her2k":   "Hermitian rank-2k update",
    "trmm":    "Triangular matrix-matrix multiplication",
    "trsm":    "Solve triangular systems with multiple right-hand sides",
}


class BlasKernBindlet(AbstractDomainlet):
    """Bindlet dispatching to any of the 40 BLAS domainlet kernels.

    Configure with ``operation`` set to the BLAS operation name
    (e.g. ``"gemm"``, ``"dot"``, ``"trsm"``).  The ``execute``
    method deserialises the context data and delegates to the
    appropriate kernel module.
    """

    def probe(self) -> DomainletInfo:
        capabilities = [
            DomainletCapability(
                name=f"blas.{op}",
                description=_OPERATION_DESCRIPTIONS.get(op, op),
            )
            for op in sorted(_OPERATION_MODULE)
        ]
        return DomainletInfo(
            name="blaskern",
            version="1.0.0",
            description="BLAS Level 1, 2, and 3 domainlet kernels (40 operations)",
            capabilities=capabilities,
        )

    def configure(self, config: dict[str, str]) -> None:
        self._check_required_keys(config, ("operation",), source="blaskern")
        op = config["operation"]
        if op not in _OPERATION_MODULE:
            raise BindletError(
                code=BindletErrorCode.CONFIG_INVALID,
                message=f"unknown BLAS operation {op!r}; available: {sorted(_OPERATION_MODULE)}",
                source="blaskern",
            )
        self._operation = op
        self._kernel_module = importlib.import_module(_OPERATION_MODULE[op])
        self._kernel_fn = getattr(self._kernel_module, op)

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        import json
        import numpy as np

        params: dict[str, Any] = {}
        try:
            params = json.loads(context.data) if context.data else {}
        except json.JSONDecodeError:
            raise BindletError(
                code=BindletErrorCode.INPUT_MALFORMED,
                message="context data must be valid JSON",
                source="blaskern",
            ) from None

        try:

            args = params.get("args", [])
            kwargs = params.get("kwargs", {})

            result = self._kernel_fn(*args, **kwargs)
            return []
        except Exception as exc:
            raise BindletError(
                code=BindletErrorCode.EXECUTION_FAILED,
                message=f"{self._operation}: {exc}",
                source="blaskern",
            ) from exc


_BINDLET_CLASSES: ClassVar[list[type[AbstractDomainlet]]] = [
    BlasKernBindlet,
]


def discover() -> list[DomainletInfo]:
    """Return metadata for the BLAS domainlet bindlet."""
    return [cls.probe_static() for cls in _BINDLET_CLASSES]


def get_bindlet(name: str) -> type[AbstractDomainlet] | None:
    """Look up a BLAS bindlet class by name."""
    for cls in _BINDLET_CLASSES:
        try:
            if cls.probe_static().name == name:
                return cls
        except Exception:
            continue
    return None


__all__ = [
    "BlasKernBindlet",
    "discover",
    "get_bindlet",
]
