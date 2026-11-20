"""Bindlet wrappers for the backend-neutral :mod:`domainlet.fuzz` modules.

The fuzz domainlets exchange structured campaign data rather than domgate
AST dumps.  Their bindlets therefore accept a JSON object (or an already
decoded mapping) through ``AnalysisContext.data`` and return JSON-encoded
results in ``AnalysisIssue.extra["result"]``.
"""

from __future__ import annotations

import dataclasses
import enum
import json
from abc import abstractmethod
from collections.abc import Mapping, Sequence
from datetime import date, datetime
from pathlib import Path
from typing import Any, ClassVar, TypeVar, cast

from domainlet.bindlet_base import (
    AbstractDomainlet,
    AnalysisContext,
    AnalysisIssue,
    BindletError,
    BindletErrorCode,
    Confidence,
    DomainletCapability,
    DomainletInfo,
    InputKind,
    Severity,
)


def _jsonable(value: Any) -> Any:
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        return {
            field.name: _jsonable(getattr(value, field.name)) for field in dataclasses.fields(value)
        }
    if isinstance(value, enum.Enum):
        return value.value
    if isinstance(value, bytes):
        return {"encoding": "hex", "data": value.hex()}
    if isinstance(value, (date, datetime)):
        return value.isoformat()
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, Mapping):
        return {str(key): _jsonable(item) for key, item in value.items()}
    if isinstance(value, (set, frozenset)):
        return sorted((_jsonable(item) for item in value), key=repr)
    if isinstance(value, (list, tuple)):
        return [_jsonable(item) for item in value]
    return value


def _decode_bytes(value: Any, *, field: str = "data") -> bytes:
    if isinstance(value, bytes):
        return value
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, Mapping):
        encoding = str(value.get("encoding", "hex"))
        data = value.get("data", "")
        if encoding == "hex":
            try:
                return bytes.fromhex(str(data))
            except ValueError as exc:
                raise ValueError(
                    f"{field} contains invalid hexadecimal data") from exc
        if encoding in {"utf8", "utf-8", "text"}:
            return str(data).encode("utf-8")
    if isinstance(value, Sequence):
        try:
            return bytes(value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"{field} is not a valid byte sequence") from exc
    raise ValueError(
        f"{field} must be text, bytes, a byte array, or an encoded object")


_EnumT = TypeVar("_EnumT", bound=enum.Enum)


def _enum(enum_type: type[_EnumT], value: Any, *, field: str) -> _EnumT:
    try:
        return enum_type(value)
    except (TypeError, ValueError) as exc:
        choices = ", ".join(str(item.value) for item in enum_type)
        raise ValueError(f"{field} must be one of: {choices}") from exc


def _require_mapping(value: Any, *, field: str) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        raise ValueError(f"{field} must be a JSON object")
    return dict(value)


class _FuzzBindlet(AbstractDomainlet):
    """Shared JSON, configuration, and result handling for fuzz bindlets."""

    bindlet_name: ClassVar[str]
    description: ClassVar[str]
    capability_descriptions: ClassVar[dict[str, str]]
    default_capability: ClassVar[str]
    config_keys: ClassVar[frozenset[str]] = frozenset()
    version: ClassVar[str] = "1.0.0"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name=self.bindlet_name,
            version=self.version,
            description=self.description,
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name=name,
                    description=description,
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["fuzz", self.bindlet_name.removeprefix(
                        "fuzz."), "json"],
                )
                for name, description in self.capability_descriptions.items()
            ],
            config_schema={
                "capability": {
                    "type": "string",
                    "default": self.default_capability,
                    "enum": list(self.capability_descriptions),
                },
                "input": {"type": "object", "encoding": "json"},
            },
        )

    def configure(self, config: dict[str, str]) -> None:
        unknown = set(config) - self.config_keys - {"capability"}
        if unknown:
            self._config_error(
                f"unknown configuration keys: {', '.join(sorted(unknown))}")
        capability = config.get("capability", self.default_capability)
        if capability not in self.capability_descriptions:
            self._config_error(
                f"unknown capability '{capability}'; choose from "
                f"{sorted(self.capability_descriptions)}"
            )
        self._capability = capability
        self._config = dict(config)
        try:
            self._configure(config)
        except BindletError:
            raise
        except (TypeError, ValueError, KeyError) as exc:
            self._config_error(str(exc))

    def _configure(self, config: dict[str, str]) -> None:
        pass

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        capability = getattr(self, "_capability", self.default_capability)
        try:
            payload = self._payload(context)
            result = self._execute(capability, payload, context)
        except BindletError:
            raise
        except (json.JSONDecodeError, TypeError, ValueError, KeyError) as exc:
            raise BindletError(
                BindletErrorCode.INPUT_MALFORMED,
                str(exc),
                source=type(self).__name__,
            ) from exc
        except Exception as exc:
            raise BindletError(
                BindletErrorCode.EXECUTION_FAILED,
                str(exc),
                source=type(self).__name__,
            ) from exc
        if isinstance(result, list) and all(isinstance(item, AnalysisIssue) for item in result):
            return result
        return [self._issue(capability, result)]

    @abstractmethod
    def _execute(
        self,
        capability: str,
        payload: dict[str, Any],
        context: AnalysisContext,
    ) -> Any:
        raise NotImplementedError

    def _payload(self, context: AnalysisContext) -> dict[str, Any]:
        value = context.data
        if isinstance(value, Mapping):
            return dict(value)
        if isinstance(value, bytes):
            value = value.decode("utf-8")
        if not isinstance(value, str):
            raise ValueError("fuzz bindlet input must be a JSON object")
        if not value.strip():
            return {}
        return _require_mapping(json.loads(value), field="input")

    def _issue(
        self,
        rule_id: str,
        result: Any,
        *,
        message: str | None = None,
        severity: Severity = Severity.INFORMATION,
        confidence: Confidence = Confidence.HIGH,
    ) -> AnalysisIssue:
        encoded = json.dumps(
            _jsonable(result),
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        )
        return AnalysisIssue(
            rule_id=f"{self.bindlet_name}.{rule_id}",
            message=message or f"{self.bindlet_name} completed '{rule_id}'",
            severity=severity,
            confidence=confidence,
            extra={"result": encoded, "capability": rule_id},
        )

    def _config_error(self, message: str) -> None:
        raise BindletError(
            BindletErrorCode.CONFIG_INVALID,
            message,
            source=type(self).__name__,
        )


# ``AbstractDomainlet.__init_subclass__`` runs before ``ABCMeta`` has finished
# populating ``__abstractmethods__``, so remove this shared abstract helper from
# the public registry explicitly.  Concrete subclasses below auto-register.
AbstractDomainlet._registry.pop(f"{__name__}._FuzzBindlet", None)


def _target_from_dict(data: Any) -> Any:
    from domainlet.fuzz.target import FuzzTarget

    return FuzzTarget.from_dict(_require_mapping(data, field="target"))


def _score_from_dict(data: Mapping[str, Any]) -> Any:
    from domainlet.fuzz.discovery import DiscoveryScore

    return DiscoveryScore(
        complexity=float(data.get("complexity", 0.0)),
        exposure=float(data.get("exposure", 0.0)),
        sink_proximity=float(data.get("sink_proximity", 0.0)),
        input_shape=float(data.get("input_shape", 0.0)),
        taint=float(data.get("taint", 0.0)),
        penalty=float(data.get("penalty", 0.0)),
    )


def _seed_from_value(value: Any, *, target_id: str | None = None) -> Any:
    from domainlet.fuzz.corpus import SeedInput, SeedOrigin

    if isinstance(value, Mapping) and "data" in value:
        origin = _enum(
            SeedOrigin,
            value.get("origin", SeedOrigin.IMPORTED.value),
            field="seed.origin",
        )
        return SeedInput(
            _decode_bytes(value["data"], field="seed.data"),
            origin,
            value.get("name"),
            value.get("target_id", target_id),
        )
    return SeedInput(_decode_bytes(value, field="seed"), target_id=target_id)


def _constraint_set(payload: Mapping[str, Any]) -> Any:
    from domainlet.fuzz.constraints import (
        ConstraintOperator,
        ConstraintSet,
        InputVariable,
        PathConstraint,
    )

    constraints = tuple(
        PathConstraint(
            str(item["variable"]),
            _enum(ConstraintOperator,
                  item["operator"], field="constraint.operator"),
            item.get("value"),
            item.get("branch_id"),
            bool(item.get("desired", True)),
        )
        for item in payload.get("constraints", ())
    )
    variables = tuple(
        InputVariable(
            str(item["name"]),
            item.get("offset"),
            int(item.get("width", 1)),
            tuple(item.get("domain", ())),
        )
        for item in payload.get("variables", ())
    )
    return ConstraintSet(constraints, variables, dict(payload.get("metadata", {})))


def _coverage_delta(data: Mapping[str, Any]) -> Any:
    from domainlet.fuzz.coverage import CoverageDelta

    return CoverageDelta(
        frozenset(data.get("added", ())),
        frozenset(data.get("removed", ())),
        dict(data.get("count_increases", {})),
    )


def _ri_distance(value: Any) -> Any:
    from domainlet.fuzz.distance import (
        BranchDistance,
        DistanceMetric,
        ObjectiveDistance,
    )

    if value is None or isinstance(value, (int, float)):
        return value
    data = _require_mapping(value, field="outcome.distance")
    if "branch_id" in data:
        return BranchDistance(
            str(data["branch_id"]),
            float(data.get("value", 0.0)),
            _enum(
                DistanceMetric,
                data.get("metric", DistanceMetric.ABSOLUTE.value),
                field="outcome.distance.metric",
            ),
            bool(data.get("satisfied", False)),
        )
    if "objective_id" in data:
        components = tuple(_ri_distance(item)
                           for item in data.get("components", ()))
        if not all(isinstance(item, BranchDistance) for item in components):
            raise ValueError(
                "outcome.distance.components must contain branch distances")
        return ObjectiveDistance(
            str(data["objective_id"]),
            float(data.get("value", 0.0)),
            components,
            bool(data.get("reachable", True)),
        )
    if "value" in data:
        return float(data["value"])
    raise ValueError(
        "outcome.distance must be numeric or identify a branch/objective distance")


def _ri_outcome(value: Any, *, field: str = "outcome") -> Any:
    from domainlet.fuzz.ri import FuzzingOutcome

    data = _require_mapping(value, field=field)
    metrics = _require_mapping(
        data.get("metrics", {}), field=f"{field}.metrics")
    info = _require_mapping(data.get("info", {}), field=f"{field}.info")
    return FuzzingOutcome(
        seed_id=data.get("seed_id"),
        distance=_ri_distance(data.get("distance")),
        path_length=int(data.get("path_length", 0)),
        coverage_delta=int(data.get("coverage_delta", 0)),
        target_reached=bool(data.get("target_reached", False)),
        crashed=bool(data.get("crashed", False)),
        metrics={str(name): float(metric) for name, metric in metrics.items()},
        info=info,
    )


def _ri_weights(value: Any) -> Any:
    from domainlet.fuzz.ri import RewardWeights

    data = _require_mapping(value, field="weights")
    defaults = RewardWeights()
    return RewardWeights(
        distance_progress=float(
            data.get("distance_progress", defaults.distance_progress)),
        coverage=float(data.get("coverage", defaults.coverage)),
        target=float(data.get("target", defaults.target)),
        crash=float(data.get("crash", defaults.crash)),
        path_length=float(data.get("path_length", defaults.path_length)),
        step=float(data.get("step", defaults.step)),
    )


def _ri_seed_cases(payload: Mapping[str, Any]) -> tuple[tuple[Any, ...], dict[str, Any]]:
    seeds = []
    outcomes: dict[str, Any] = {}
    for index, value in enumerate(payload.get("seeds", ())):
        item = _require_mapping(value, field=f"seeds[{index}]")
        if "outcome" not in item:
            raise ValueError(f"seeds[{index}].outcome is required")
        seed = _seed_from_value(item.get("seed", item))
        if seed.id in outcomes:
            raise ValueError("seed data must be unique")
        seeds.append(seed)
        outcomes[seed.id] = _ri_outcome(
            item["outcome"],
            field=f"seeds[{index}].outcome",
        )
    if not seeds:
        raise ValueError("seeds must not be empty")
    return tuple(seeds), outcomes


class FuzzAFLBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.afl"
    description = "Describe and validate pure-Python AFL runner configurations"
    capability_descriptions: ClassVar[dict[str, str]] = {
        "describe-engine": "Report the installed python-afl binding and engine capabilities",
        "validate-runner": "Validate and normalize an AFL runner configuration",
    }
    default_capability = "describe-engine"
    config_keys = frozenset({"max_input_size", "max_iterations", "persistent"})

    @staticmethod
    def _optional_int(value: Any, *, field: str, positive: bool) -> int | None:
        if value is None:
            return None
        if isinstance(value, str) and value.strip().lower() in {"", "none", "null"}:
            return None
        if isinstance(value, bool):
            raise TypeError(f"{field} must be an integer or null")
        if not isinstance(value, (int, str)):
            raise TypeError(f"{field} must be an integer or null")
        result = int(value)
        if positive and result <= 0:
            raise ValueError(f"{field} must be positive")
        if not positive and result < 0:
            raise ValueError(f"{field} must be non-negative")
        return result


class FuzzLibFuzzerBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.libfuzzer"
    description = "Describe and validate libFuzzer runner configurations"
    capability_descriptions: ClassVar[dict[str, str]] = {
        "describe-engine": "Report the installed libfuzzer binding and engine capabilities",
        "validate-runner": "Validate and normalize a libFuzzer runner configuration",
    }
    default_capability = "describe-engine"
    config_keys = frozenset({"counters_size", "argv"})

    @staticmethod
    def _optional_int(value: Any, *, field: str, positive: bool) -> int | None:
        if value is None:
            return None
        if isinstance(value, str) and value.strip().lower() in {"", "none", "null"}:
            return None
        if isinstance(value, bool):
            raise TypeError(f"{field} must be an integer or null")
        if not isinstance(value, (int, str)):
            raise TypeError(f"{field} must be an integer or null")
        result = int(value)
        if positive and result <= 0:
            raise ValueError(f"{field} must be positive")
        if not positive and result < 0:
            raise ValueError(f"{field} must be non-negative")
        return result

    def _configure(self, config: dict[str, str]) -> None:
        self._counters_size = self._optional_int(
            config.get("counters_size", "4096"),
            field="counters_size",
            positive=True,
        )
        argv_raw = config.get("argv")
        self._argv = tuple(argv_raw.split()) if isinstance(
            argv_raw, str) else ()

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.libfuzzer import BINDING_VERSION, LIBFUZZER_CAPABILITY, LibFuzzerConfig

        config_data = _require_mapping(
            payload.get("config", payload), field="config")
        argv_raw = config_data.get("argv", getattr(self, "_argv", ()))
        if isinstance(argv_raw, str):
            argv = tuple(argv_raw.split())
        elif isinstance(argv_raw, (list, tuple)):
            argv = tuple(str(arg) for arg in argv_raw)
        else:
            argv = ()

        config = LibFuzzerConfig(
            counters_size=self._optional_int(
                config_data.get("counters_size", getattr(
                    self, "_counters_size", 4096)),
                field="counters_size",
                positive=True,
            ) or 4096,
            argv=argv,
        )
        result: dict[str, Any] = {
            "binding": "libfuzzer",
            "binding_version": BINDING_VERSION,
            "configuration": config,
        }
        if capability == "describe-engine":
            result["engine"] = LIBFUZZER_CAPABILITY
        return result

    @staticmethod
    def _bool(value: Any, *, field: str) -> bool:
        if isinstance(value, bool):
            return value
        if isinstance(value, str):
            normalized = value.strip().lower()
            if normalized in {"1", "true", "yes", "on"}:
                return True
            if normalized in {"0", "false", "no", "off"}:
                return False
        raise ValueError(f"{field} must be true or false")

    def _configure(self, config: dict[str, str]) -> None:
        self._persistent = self._bool(config.get(
            "persistent", "false"), field="persistent")
        self._max_iterations = self._optional_int(
            config.get("max_iterations", "1000"),
            field="max_iterations",
            positive=True,
        )
        self._max_input_size = self._optional_int(
            config.get("max_input_size"),
            field="max_input_size",
            positive=False,
        )

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.afl import AFL_CAPABILITY, BINDING_VERSION, AFLConfig

        config_data = _require_mapping(
            payload.get("config", payload), field="config")
        config = AFLConfig(
            persistent=self._bool(
                config_data.get("persistent", getattr(
                    self, "_persistent", False)),
                field="persistent",
            ),
            max_iterations=self._optional_int(
                config_data.get(
                    "max_iterations",
                    getattr(self, "_max_iterations", 1_000),
                ),
                field="max_iterations",
                positive=True,
            ),
            max_input_size=self._optional_int(
                config_data.get(
                    "max_input_size",
                    getattr(self, "_max_input_size", None),
                ),
                field="max_input_size",
                positive=False,
            ),
        )
        result: dict[str, Any] = {
            "binding": "python-afl",
            "binding_version": BINDING_VERSION,
            "configuration": config,
        }
        if capability == "describe-engine":
            result["engine"] = AFL_CAPABILITY
        return result


class FuzzTargetBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.target"
    description = "Validate and normalize engine-independent fuzz target descriptions"
    capability_descriptions = {
        "describe-target": "Validate a target and return its canonical description and stable ID",
    }
    default_capability = "describe-target"

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        target = _target_from_dict(payload.get("target", payload))
        return {"id": target.id, "target": target.to_dict()}


class FuzzDiscoveryBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.discovery"
    description = "Rank fuzz targets using supplied static-analysis discovery scores"
    capability_descriptions = {
        "rank-targets": "Rank target candidates by combined discovery score",
    }
    default_capability = "rank-targets"

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.discovery import TargetCandidate, rank_candidates

        candidates = [
            TargetCandidate(
                _target_from_dict(item.get("target", item)),
                _score_from_dict(_require_mapping(
                    item.get("score", {}), field="score")),
            )
            for item in payload.get("candidates", payload.get("targets", ()))
        ]
        return [
            {"rank": index, "target_id": item.target.id, "score": item.score.total}
            for index, item in enumerate(rank_candidates(candidates), 1)
        ]


class FuzzHarnessBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.harness"
    description = "Synthesize and validate fuzz harness specifications"
    capability_descriptions = {
        "synthesize-harness": "Create bindings and optionally render a harness source template",
        "validate-harness": "Validate synthesized target argument bindings",
    }
    default_capability = "synthesize-harness"

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.harness import HarnessTemplate, synthesize_harness

        target = _target_from_dict(payload["target"])
        template_data = payload.get("template")
        template = None
        if template_data is not None:
            template_map = _require_mapping(template_data, field="template")
            template = HarnessTemplate(
                str(template_map["source"]),
                str(template_map.get("language", "c++")),
                str(template_map.get("name", "default")),
            )
        spec = synthesize_harness(target, template)
        diagnostics = spec.validate()
        return {
            "valid": spec.valid,
            "source": spec.source,
            "bindings": spec.bindings,
            "diagnostics": diagnostics,
        }


class FuzzInstrumentBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.instrument"
    description = "Validate instrumentation requirements against fuzz engine capabilities"
    capability_descriptions = {
        "validate-engine": "Report unsupported tracing, sanitizer, and execution features",
        "compatible-engines": "List engines compatible with an instrumentation specification",
    }
    default_capability = "validate-engine"

    @staticmethod
    def _spec(data: Mapping[str, Any]) -> Any:
        from domainlet.fuzz.instrument import (
            InstrumentationSpec,
            Sanitizer,
            SanitizerConfig,
            TraceMode,
        )

        return InstrumentationSpec(
            frozenset(
                _enum(TraceMode, item, field="trace_modes")
                for item in data.get("trace_modes", ("edge",))
            ),
            SanitizerConfig(
                frozenset(
                    _enum(Sanitizer, item, field="sanitizers")
                    for item in data.get("sanitizers", ())
                ),
                bool(data.get("recover", False)),
                bool(data.get("symbolize", True)),
                dict(data.get("sanitizer_options", {})),
            ),
            bool(data.get("persistent_mode", False)),
            bool(data.get("custom_feedback", False)),
            tuple(data.get("compiler_flags", ())),
            tuple(data.get("linker_flags", ())),
            dict(data.get("environment", {})),
        )

    @staticmethod
    def _engine(data: Mapping[str, Any]) -> Any:
        from domainlet.fuzz.instrument import EngineCapability, Sanitizer, TraceMode

        return EngineCapability(
            str(data["engine"]),
            frozenset(data.get("languages", ())),
            frozenset(
                _enum(TraceMode, item, field="trace_modes") for item in data.get("trace_modes", ())
            ),
            frozenset(
                _enum(Sanitizer, item, field="sanitizers") for item in data.get("sanitizers", ())
            ),
            bool(data.get("persistent_mode", False)),
            bool(data.get("fork_server", False)),
            bool(data.get("structured_inputs", False)),
            bool(data.get("custom_feedback", False)),
        )

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.instrument import InstrumentationRegistry

        spec = self._spec(_require_mapping(
            payload.get("spec", {}), field="spec"))
        engines = [
            self._engine(_require_mapping(item, field="engine"))
            for item in payload.get("engines", ())
        ]
        if capability == "validate-engine":
            if "engine" in payload:
                engine = self._engine(_require_mapping(
                    payload["engine"], field="engine"))
            elif len(engines) == 1:
                engine = engines[0]
            else:
                raise ValueError("validate-engine requires an 'engine' object")
            diagnostics = spec.validate(engine)
            return {
                "engine": engine.engine,
                "supported": not diagnostics,
                "diagnostics": diagnostics,
            }
        compatible = InstrumentationRegistry(engines).compatible(spec)
        return {"engines": [engine.engine for engine in compatible]}


class FuzzCorpusBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.corpus"
    description = "Build, deduplicate, query, and shrink fuzz seed corpora"
    capability_descriptions = {
        "summarize-corpus": "Deduplicate seeds and report corpus size and byte totals",
        "shrink-corpus": "Keep the smallest configured number of unique seeds",
        "query-corpus": "Look up seeds by metadata label or tag",
    }
    default_capability = "summarize-corpus"
    config_keys = frozenset({"max_entries", "label", "tag"})

    def _configure(self, config: dict[str, str]) -> None:
        self._max_entries = int(config.get("max_entries", "64"))
        if self._max_entries < 0:
            raise ValueError("max_entries must be non-negative")
        self._label = config.get("label")
        self._tag = config.get("tag")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.corpus import InputMetadata, SeedCorpus

        seeds = []
        for value in payload.get("seeds", ()):
            seed = _seed_from_value(value, target_id=payload.get("target_id"))
            if isinstance(value, Mapping):
                seed.metadata = InputMetadata(
                    labels=set(value.get("labels", ())),
                    tags=set(value.get("tags", ())),
                    annotations=dict(value.get("annotations", {})),
                )
            seeds.append(seed)
        corpus = SeedCorpus(seeds, name=str(payload.get("name", "corpus")))
        removed = corpus.shrink(
            self._max_entries) if capability == "shrink-corpus" else []
        selected = (
            corpus.index.lookup(label=self._label, tag=self._tag)
            if capability == "query-corpus"
            else list(corpus)
        )
        return {
            "name": corpus.name,
            "entries": len(corpus),
            "total_bytes": corpus.total_bytes,
            "seeds": [{"id": seed.id, "size": seed.size, "data": seed.data} for seed in selected],
            "removed": [seed.id for seed in removed],
        }


class FuzzSeedgenBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.seedgen"
    description = "Generate deterministic initial fuzz seeds from literals and boundary strategies"
    capability_descriptions = {
        "generate-seeds": "Generate a bounded, deduplicated seed corpus",
    }
    default_capability = "generate-seeds"
    config_keys = frozenset({"max_seeds", "max_size", "random_seed"})

    def _configure(self, config: dict[str, str]) -> None:
        self._max_seeds = int(config.get("max_seeds", "64"))
        self._max_size = int(config.get("max_size", str(1 << 20)))
        self._random_seed = int(
            config["random_seed"]) if "random_seed" in config else None
        if self._max_seeds < 0 or self._max_size < 0:
            raise ValueError("seed limits must be non-negative")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.seedgen import SeedFactory, SeedGenerationRequest, SeedStrategy

        request = SeedGenerationRequest(
            payload.get("target_id"),
            tuple(
                _enum(SeedStrategy, item, field="strategies")
                for item in payload.get("strategies", ("empty", "literal"))
            ),
            tuple(_decode_bytes(item, field="literals")
                  for item in payload.get("literals", ())),
            tuple(
                _decode_bytes(item, field="dictionary") for item in payload.get("dictionary", ())
            ),
            self._max_seeds,
            self._max_size,
            dict(payload.get("metadata", {})),
        )
        corpus = SeedFactory(random_seed=self._random_seed).generate(request)
        return {
            "name": corpus.name,
            "entries": len(corpus),
            "seeds": [{"id": seed.id, "data": seed.data, "origin": seed.origin} for seed in corpus],
        }


class FuzzMutateBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.mutate"
    description = "Apply deterministic byte and dictionary mutations to fuzz inputs"
    capability_descriptions = {
        "mutate-input": "Produce bounded mutations for one seed input",
    }
    default_capability = "mutate-input"
    config_keys = frozenset({"attempts", "max_output_size", "random_seed"})

    def _configure(self, config: dict[str, str]) -> None:
        self._attempts = int(config.get("attempts", "1"))
        self._max_output_size = int(
            config.get("max_output_size", str(1 << 20)))
        self._random_seed = int(
            config["random_seed"]) if "random_seed" in config else None
        if self._attempts < 0 or self._max_output_size < 0:
            raise ValueError("mutation limits must be non-negative")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.mutate import (
            MutationBudget,
            MutationSite,
            Mutator,
            builtin_operators,
        )

        seed = _seed_from_value(payload.get("seed", payload.get("data", b"")))
        dictionary = tuple(
            _decode_bytes(item, field="dictionary") for item in payload.get("dictionary", ())
        )
        sites = [
            MutationSite(
                int(item["start"]),
                int(item["end"]),
                float(item.get("influence", 0.0)),
                str(item.get("reason", "")),
            )
            for item in payload.get("sites", ())
        ]
        results = Mutator(
            builtin_operators(dictionary),
            random_seed=self._random_seed,
        ).mutate(
            seed,
            MutationBudget(self._attempts, self._max_output_size),
            sites=sites,
        )
        return [
            {
                "id": item.seed.id,
                "data": item.seed.data,
                "strategy": item.strategy,
                "site": item.site,
                "cost": item.cost,
                "description": item.description,
            }
            for item in results
        ]


class FuzzMinimizeBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.minimize"
    description = "Minimize fuzz inputs while preserving a configured byte predicate"
    capability_descriptions = {
        "minimize-input": "Delta-debug an input while preserving a substring, prefix, or suffix",
    }
    default_capability = "minimize-input"
    config_keys = frozenset(
        {"strategy", "min_size", "max_iterations", "preserve"})

    def _configure(self, config: dict[str, str]) -> None:
        self._strategy = config.get("strategy", "delete-chunks")
        self._min_size = int(config.get("min_size", "0"))
        self._max_iterations = int(config.get("max_iterations", "1000"))
        self._preserve = config.get("preserve", "contains")
        if self._min_size < 0 or self._max_iterations < 0:
            raise ValueError("minimization limits must be non-negative")
        if self._preserve not in {"contains", "prefix", "suffix", "nonempty", "always"}:
            raise ValueError(
                "preserve must be contains, prefix, suffix, nonempty, or always")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.minimize import (
            MinimizationTask,
            Minimizer,
            ReductionStrategy,
        )

        seed = _seed_from_value(payload.get("seed", payload.get("data", b"")))
        marker = _decode_bytes(payload.get("marker", b""), field="marker")
        predicates = {
            "contains": lambda data: marker in data,
            "prefix": lambda data: data.startswith(marker),
            "suffix": lambda data: data.endswith(marker),
            "nonempty": lambda data: bool(data),
            "always": lambda data: True,
        }
        task = MinimizationTask(
            seed,
            predicates[self._preserve],
            _enum(ReductionStrategy, self._strategy, field="strategy"),
            self._max_iterations,
            self._min_size,
        )
        result = Minimizer().minimize(task)
        return {
            "input": result.input.data,
            "id": result.input.id,
            "reduction_ratio": result.reduction_ratio,
            "steps": result.steps,
            "checks": len(result.checks),
        }


class FuzzCoverageBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.coverage"
    description = "Aggregate code coverage and calculate novelty against a baseline"
    capability_descriptions = {
        "summarize-coverage": "Record coverage events and report the resulting summary",
        "coverage-delta": "Compare observed coverage against baseline counts",
    }
    default_capability = "summarize-coverage"
    config_keys = frozenset({"kind", "total_points"})

    def _configure(self, config: dict[str, str]) -> None:
        self._kind = config.get("kind", "edge")
        self._total_points = int(
            config["total_points"]) if "total_points" in config else None
        if self._total_points is not None and self._total_points < 0:
            raise ValueError("total_points must be non-negative")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.coverage import CoverageEvent, CoverageKind, CoverageMap

        kind = _enum(CoverageKind, self._kind, field="kind")
        current = CoverageMap(kind, dict(
            payload.get("counts", {})), self._total_points)
        events = [
            CoverageEvent(
                item["point"],
                _enum(CoverageKind, item.get(
                    "kind", kind.value), field="event.kind"),
                int(item.get("count", 1)),
                item.get("target_id"),
            )
            if isinstance(item, Mapping)
            else item
            for item in payload.get("events", ())
        ]
        delta = current.update(events)
        if capability == "coverage-delta":
            baseline = CoverageMap(kind, dict(
                payload.get("baseline", {})), self._total_points)
            delta = current.delta(baseline)
        return {"summary": current.summary(), "delta": delta}


class FuzzFeedbackBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.feedback"
    description = "Aggregate per-input runtime feedback into campaign summaries"
    capability_descriptions = {
        "summarize-feedback": "Summarize executions, interesting inputs, crashes, and coverage",
        "rank-inputs": "Rank inputs by novelty, objectives, and crashes",
    }
    default_capability = "summarize-feedback"
    config_keys = frozenset({"limit"})

    def _configure(self, config: dict[str, str]) -> None:
        self._limit = int(config["limit"]) if "limit" in config else None
        if self._limit is not None and self._limit < 0:
            raise ValueError("limit must be non-negative")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.feedback import CampaignFeedback, InputFeedback
        from domainlet.fuzz.objective import ObjectiveScore

        campaign = CampaignFeedback()
        for item in payload.get("inputs", ()):
            coverage = _coverage_delta(
                item["coverage"]) if item.get("coverage") else None
            objectives = tuple(
                ObjectiveScore(
                    str(score["objective_id"]),
                    float(score.get("value", 0.0)),
                    bool(score.get("satisfied", False)),
                    tuple(score.get("evidence", ())),
                )
                for score in item.get("objectives", ())
            )
            campaign.record(
                InputFeedback(
                    str(item["input_id"]),
                    item.get("target_id"),
                    coverage,
                    objectives=objectives,
                    crashes=tuple(item.get("crashes", ())),
                    duration_ms=item.get("duration_ms"),
                )
            )
        if capability == "rank-inputs":
            return [
                {"input_id": item.input_id, "score": item.score,
                    "interesting": item.interesting}
                for item in campaign.best_inputs(self._limit)
            ]
        return campaign.summary()


class FuzzFrontierBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.frontier"
    description = "Rank and cluster uncovered fuzzing frontier branches"
    capability_descriptions = {
        "rank-frontier": "Rank uncovered branches by distance and blocker cost",
        "cluster-frontier": "Cluster branches with shared blocker kinds and dependencies",
    }
    default_capability = "rank-frontier"

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.frontier import (
            BlockerKind,
            CoverageBlocker,
            Frontier,
            FrontierBranch,
        )

        branches = []
        for item in payload.get("branches", ()):
            blocker_data = item.get("blocker")
            blocker = None
            if blocker_data:
                blocker = CoverageBlocker(
                    str(blocker_data.get("id", f"blocker-{item['id']}")),
                    _enum(
                        BlockerKind,
                        blocker_data.get("kind", "unknown"),
                        field="blocker.kind",
                    ),
                    blocker_data.get("constraint_id"),
                    frozenset(blocker_data.get("dependencies", ())),
                    str(blocker_data.get("description", "")),
                    float(blocker_data.get("estimated_cost", 1.0)),
                )
            branches.append(
                FrontierBranch(
                    str(item["id"]),
                    item.get("source"),
                    item.get("destination"),
                    item.get("predicate"),
                    blocker,
                    int(item.get("observations", 0)),
                    item.get("distance"),
                )
            )
        frontier = Frontier()
        frontier.update(branches, payload.get("execution"))
        return frontier.clusters() if capability == "cluster-frontier" else frontier.rank()


class FuzzGuidanceBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.guidance"
    description = "Choose the next fuzz campaign action from feedback and frontier state"
    capability_descriptions = {
        "choose-action": "Select mutate, solve, reseed, or minimize from campaign signals",
    }
    default_capability = "choose-action"

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.feedback import CampaignFeedback, InputFeedback
        from domainlet.fuzz.frontier import (
            BlockerKind,
            CoverageBlocker,
            Frontier,
            FrontierBranch,
        )
        from domainlet.fuzz.guidance import GuidanceEngine

        feedback = CampaignFeedback()
        for item in payload.get("inputs", ()):
            feedback.record(
                InputFeedback(
                    str(item["input_id"]),
                    crashes=tuple(item.get("crashes", ())),
                    coverage=_coverage_delta(item["coverage"]) if item.get(
                        "coverage") else None,
                )
            )
        frontier = Frontier()
        branches = []
        for item in payload.get("branches", ()):
            blocker_data = item.get("blocker")
            blocker = None
            if blocker_data:
                blocker = CoverageBlocker(
                    str(blocker_data.get("id", f"blocker-{item['id']}")),
                    _enum(
                        BlockerKind,
                        blocker_data.get("kind", "unknown"),
                        field="blocker.kind",
                    ),
                    blocker_data.get("constraint_id"),
                    frozenset(blocker_data.get("dependencies", ())),
                    str(blocker_data.get("description", "")),
                    float(blocker_data.get("estimated_cost", 1.0)),
                )
            branches.append(
                FrontierBranch(
                    str(item["id"]),
                    item.get("source"),
                    item.get("destination"),
                    blocker=blocker,
                    distance=item.get("distance"),
                )
            )
        frontier.update(branches)
        return GuidanceEngine().decide(feedback, frontier)


class FuzzConstraintsBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.constraints"
    description = "Evaluate and transform lightweight fuzz path constraints"
    capability_descriptions = {
        "evaluate-constraints": "Evaluate a constraint set against an assignment",
        "negate-last": "Negate the desired result of the final path constraint",
    }
    default_capability = "evaluate-constraints"
    config_keys = frozenset({"partial"})

    def _configure(self, config: dict[str, str]) -> None:
        self._partial = config.get("partial", "false").lower() in {
            "1", "true", "yes", "on"}

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        constraints = _constraint_set(payload)
        if capability == "negate-last":
            return constraints.negate_last()
        assignment = _require_mapping(payload.get(
            "assignment", {}), field="assignment")
        return {
            "satisfied": constraints.evaluate(assignment, partial=self._partial),
            "free_variables": constraints.free_variables,
            "results": [constraint.evaluate(assignment) for constraint in constraints.constraints],
        }


class FuzzDistanceBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.distance"
    description = "Calculate branch distance for an observed fuzz input value"
    capability_descriptions = {
        "branch-distance": "Measure how far an observation is from satisfying a branch",
    }
    default_capability = "branch-distance"

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.constraints import ConstraintOperator, PathConstraint
        from domainlet.fuzz.distance import branch_distance

        item = _require_mapping(payload.get(
            "constraint", payload), field="constraint")
        constraint = PathConstraint(
            str(item["variable"]),
            _enum(ConstraintOperator,
                  item["operator"], field="constraint.operator"),
            item.get("value"),
            item.get("branch_id"),
            bool(item.get("desired", True)),
        )
        return branch_distance(constraint, payload.get("actual"))


class FuzzRIBindlet(_FuzzBindlet):
    """JSON bridge for directed-fuzzing reward and seed-selection learning.

    Seed-ranking and training inputs pair each serialized seed with a
    precomputed :class:`domainlet.fuzz.ri.FuzzingOutcome`.  This keeps the
    bindlet deterministic and C-ABI friendly while the Python RI environment
    retains callback-based execution for in-process fuzzers.
    """

    bindlet_name = "fuzz.ri"
    description = "Score, rank, and train single-agent RL seed selection for fuzzing"
    capability_descriptions: ClassVar[dict[str, str]] = {
        "score-transition": "Score progress between two fuzzing outcomes",
        "rank-seeds": "Evaluate supplied seed outcomes and select the best seed",
        "train-seed-selector": "Train a bounded Stable-Baselines3 DQN seed selector",
    }
    default_capability = "score-transition"
    config_keys = frozenset(
        {
            "deterministic",
            "max_steps",
            "seed",
            "total_timesteps",
        }
    )

    @staticmethod
    def _bool(value: Any, *, field: str) -> bool:
        if isinstance(value, bool):
            return value
        if isinstance(value, str):
            normalized = value.strip().lower()
            if normalized in {"1", "true", "yes", "on"}:
                return True
            if normalized in {"0", "false", "no", "off"}:
                return False
        raise ValueError(f"{field} must be true or false")

    def _configure(self, config: dict[str, str]) -> None:
        self._max_steps = int(config.get("max_steps", "1"))
        self._total_timesteps = int(config.get("total_timesteps", "256"))
        self._seed = int(config["seed"]) if "seed" in config else None
        self._deterministic = self._bool(
            config.get("deterministic", "true"),
            field="deterministic",
        )
        if self._max_steps <= 0:
            raise ValueError("max_steps must be positive")
        if not 1 <= self._total_timesteps <= 1_000_000:
            raise ValueError("total_timesteps must be between 1 and 1000000")

    def _reward(self, payload: Mapping[str, Any]) -> Any:
        from domainlet.fuzz.ri import directed_fuzzing_reward

        weights_data = payload.get("weights")
        if weights_data is None:
            return directed_fuzzing_reward
        weights = _ri_weights(weights_data)
        return lambda previous, current: directed_fuzzing_reward(
            previous,
            current,
            weights,
        )

    def _environment(self, payload: Mapping[str, Any]) -> Any:
        from domainlet.fuzz.ri import FuzzingOutcome, SeedSelectionEnv

        seeds, outcomes = _ri_seed_cases(payload)
        initial_data = payload.get("initial_outcome")
        initial = (
            _ri_outcome(initial_data, field="initial_outcome")
            if initial_data is not None
            else FuzzingOutcome(distance=float("inf"))
        )
        max_steps = int(payload.get(
            "max_steps", getattr(self, "_max_steps", 1)))
        if max_steps <= 0:
            raise ValueError("max_steps must be positive")
        return SeedSelectionEnv(
            seeds,
            lambda seed: outcomes[seed.id],
            max_steps=max_steps,
            reward=self._reward(payload),
            initial_outcome=initial,
        )

    def _execute(
        self,
        capability: str,
        payload: dict[str, Any],
        context: AnalysisContext,
    ) -> Any:
        from domainlet.fuzz.ri import StableBaselinesAgent, directed_fuzzing_reward

        if capability == "score-transition":
            previous = _ri_outcome(payload.get(
                "previous", {}), field="previous")
            current = _ri_outcome(payload.get("current", {}), field="current")
            weights = _ri_weights(
                payload["weights"]) if "weights" in payload else None
            return {
                "reward": directed_fuzzing_reward(previous, current, weights),
                "previous_normalized_distance": previous.normalized_distance,
                "current_normalized_distance": current.normalized_distance,
                "target_reached": current.target_reached,
            }

        environment = self._environment(payload)
        if capability == "rank-seeds":
            evaluations = []
            for index in range(len(environment.seeds)):
                environment.reset(seed=getattr(self, "_seed", None))
                _, reward, terminated, truncated, info = environment.step(
                    index)
                evaluations.append(
                    {
                        "seed_index": index,
                        "seed_id": info["seed_id"],
                        "reward": reward,
                        "terminated": terminated,
                        "truncated": truncated,
                        "outcome": environment.last_outcome,
                    }
                )
            best = environment.best_evaluation
            return {
                "best_seed_id": best.seed.id if best is not None else None,
                "best_seed_index": (
                    next(
                        index
                        for index, seed in enumerate(environment.seeds)
                        if best is not None and seed.id == best.seed.id
                    )
                    if best is not None
                    else None
                ),
                "evaluations": evaluations,
            }

        from stable_baselines3 import DQN

        timesteps = int(
            payload.get(
                "total_timesteps",
                getattr(self, "_total_timesteps", 256),
            )
        )
        if not 1 <= timesteps <= 1_000_000:
            raise ValueError("total_timesteps must be between 1 and 1000000")
        seed = payload.get("seed", getattr(self, "_seed", None))
        seed = int(seed) if seed is not None else None
        deterministic = self._bool(
            payload.get(
                "deterministic",
                getattr(self, "_deterministic", True),
            ),
            field="deterministic",
        )
        agent = StableBaselinesAgent.build(
            DQN,
            "MlpPolicy",
            environment,
            learning_starts=0,
            buffer_size=max(100, timesteps),
            batch_size=min(32, timesteps),
            train_freq=1,
            gradient_steps=1,
            seed=seed,
            verbose=0,
        )
        agent.learn(timesteps)
        observation, _ = environment.reset(seed=seed)
        action = int(agent.predict(observation, deterministic=deterministic))
        best = environment.best_evaluation
        return {
            "algorithm": "DQN",
            "selected_seed_index": action,
            "selected_seed_id": environment.seeds[action].id,
            "best_observed_seed_id": best.seed.id if best is not None else None,
            "training_evaluations": len(environment.history),
            "total_timesteps": timesteps,
        }


class FuzzSolveBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.solve"
    description = "Solve finite-domain fuzz path constraints with the deterministic fallback solver"
    capability_descriptions = {
        "solve-constraints": "Find satisfying assignments for finite input domains",
    }
    default_capability = "solve-constraints"
    config_keys = frozenset({"timeout_ms", "max_models", "seed"})

    def _configure(self, config: dict[str, str]) -> None:
        self._timeout_ms = int(config.get("timeout_ms", "1000"))
        self._max_models = int(config.get("max_models", "1"))
        self._seed = int(config["seed"]) if "seed" in config else None
        if self._timeout_ms <= 0 or self._max_models <= 0:
            raise ValueError("solver limits must be positive")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.solve import SolverConfig, solve_constraints

        return solve_constraints(
            _constraint_set(payload),
            config=SolverConfig(
                self._timeout_ms, self._max_models, self._seed),
        )


class FuzzTaintBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.taint"
    description = "Query input-region influence over fuzz target sinks"
    capability_descriptions = {
        "influential-regions": "List input regions meeting a confidence threshold",
        "regions-for-sink": "List input regions influencing a named sink",
        "sinks-for-region": "List sinks influenced by an overlapping input region",
    }
    default_capability = "influential-regions"
    config_keys = frozenset({"minimum_confidence", "sink"})

    def _configure(self, config: dict[str, str]) -> None:
        self._minimum_confidence = float(config.get("minimum_confidence", "0"))
        self._sink = config.get("sink")
        if not 0.0 <= self._minimum_confidence <= 1.0:
            raise ValueError("minimum_confidence must be between 0 and 1")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.taint import InputRegion, TaintKind, TaintMap, TaintObservation

        taint_map = TaintMap(
            [
                TaintObservation(
                    InputRegion(int(item["start"]), int(item["end"])),
                    item["sink"],
                    _enum(TaintKind, item.get(
                        "kind", "data"), field="taint.kind"),
                    float(item.get("confidence", 1.0)),
                    tuple(item.get("path", ())),
                )
                for item in payload.get("observations", ())
            ]
        )
        if capability == "regions-for-sink":
            sink = payload.get("sink", self._sink)
            if sink is None:
                raise ValueError("regions-for-sink requires a sink")
            return taint_map.regions_for(sink)
        if capability == "sinks-for-region":
            region = _require_mapping(
                payload.get("region", {}), field="region")
            return taint_map.sinks_for(InputRegion(int(region["start"]), int(region["end"])))
        return taint_map.influential_regions(minimum_confidence=self._minimum_confidence)


class FuzzSliceBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.slice"
    description = "Compute forward, backward, and source-to-sink fuzzing slices"
    capability_descriptions = {
        "backward-slice": "Compute dependencies needed by the criteria",
        "forward-slice": "Compute nodes affected by the criteria",
        "chop": "Intersect forward source and backward sink slices",
    }
    default_capability = "backward-slice"

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.slice import SliceAnalyzer

        analyzer = SliceAnalyzer(
            cast(
                Mapping[Any, Sequence[Any]],
                _require_mapping(payload.get(
                    "dependencies", {}), field="dependencies"),
            )
        )
        if capability == "forward-slice":
            return analyzer.forward(payload.get("criteria", ()))
        if capability == "chop":
            return analyzer.chop(payload.get("sources", ()), payload.get("sinks", ()))
        return analyzer.backward(payload.get("criteria", ()))


class FuzzReachabilityBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.reachability"
    description = "Compute reachability and shortest paths between fuzz target graph nodes"
    capability_descriptions = {
        "reachable-nodes": "List graph nodes reachable from a source",
        "shortest-path": "Find the shortest source-to-sink path",
        "distance": "Return the shortest source-to-sink distance",
    }
    default_capability = "reachable-nodes"
    config_keys = frozenset({"max_depth"})

    def _configure(self, config: dict[str, str]) -> None:
        self._max_depth = int(
            config["max_depth"]) if "max_depth" in config else None
        if self._max_depth is not None and self._max_depth < 0:
            raise ValueError("max_depth must be non-negative")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.reachability import ReachabilityAnalyzer

        analyzer = ReachabilityAnalyzer(
            cast(
                Mapping[Any, Sequence[Any]],
                _require_mapping(payload.get("graph", {}), field="graph"),
            )
        )
        source = payload["source"]
        if capability == "shortest-path":
            return analyzer.shortest_path(source, payload["sink"])
        if capability == "distance":
            return {"distance": analyzer.distance(source, payload["sink"])}
        return analyzer.analyze(
            source,
            blocked=payload.get("blocked", ()),
            max_depth=self._max_depth,
        )


class FuzzObjectiveBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.objective"
    description = "Aggregate fuzz objective scores using campaign policies"
    capability_descriptions = {
        "aggregate-scores": "Aggregate precomputed objective scores",
    }
    default_capability = "aggregate-scores"
    config_keys = frozenset({"policy"})

    def _configure(self, config: dict[str, str]) -> None:
        self._policy = config.get("policy", "weighted-sum")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.objective import ObjectivePolicy, ObjectiveScore, ObjectiveSet

        scores = tuple(
            ObjectiveScore(
                str(item["objective_id"]),
                float(item.get("value", 0.0)),
                bool(item.get("satisfied", False)),
                tuple(item.get("evidence", ())),
            )
            for item in payload.get("scores", ())
        )
        objectives = ObjectiveSet(policy=_enum(
            ObjectivePolicy, self._policy, field="policy"))
        return {"score": objectives.aggregate(scores), "scores": scores}


class FuzzDiffBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.diff"
    description = "Compare execution observations from multiple implementations"
    capability_descriptions = {
        "compare-observations": "Classify output, status, exception, state, and timing differences",
    }
    default_capability = "compare-observations"
    config_keys = frozenset({"compare_timing", "timing_tolerance_ms"})

    def _configure(self, config: dict[str, str]) -> None:
        self._compare_timing = config.get("compare_timing", "false").lower() in {
            "1",
            "true",
            "yes",
            "on",
        }
        self._timing_tolerance = float(config.get("timing_tolerance_ms", "0"))
        if self._timing_tolerance < 0:
            raise ValueError("timing_tolerance_ms must be non-negative")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.diff import DifferentialComparator, ExecutionObservation

        observations = [
            ExecutionObservation(
                str(item["implementation"]),
                item.get("output"),
                item.get("exit_code"),
                item.get("exception"),
                tuple(item.get("diagnostics", ())),
                dict(item.get("state", {})),
                item.get("duration_ms"),
            )
            for item in payload.get("observations", ())
        ]
        return DifferentialComparator(
            compare_timing=self._compare_timing,
            timing_tolerance_ms=self._timing_tolerance,
        ).compare(observations)


class FuzzOracleBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.oracle"
    description = "Evaluate differential fuzzing mismatches as semantic oracle results"
    capability_descriptions = {
        "differential-oracle": "Fail when supplied differential observations contain mismatches",
    }
    default_capability = "differential-oracle"
    config_keys = frozenset({"name"})

    def _configure(self, config: dict[str, str]) -> None:
        self._name = config.get("name", "differential")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.diff import (
            DifferenceKind,
            DifferentialObservation,
            ExecutionObservation,
        )
        from domainlet.fuzz.oracle import differential_oracle

        differences = []
        for item in payload.get("differences", ()):
            baseline = ExecutionObservation(
                str(item.get("baseline", "baseline")))
            candidate = ExecutionObservation(
                str(item.get("candidate", "candidate")))
            differences.append(
                DifferentialObservation(
                    baseline,
                    candidate,
                    _enum(DifferenceKind, item["kind"],
                          field="difference.kind"),
                    item.get("expected"),
                    item.get("actual"),
                    str(item.get("detail", "")),
                )
            )
        return differential_oracle(self._name).evaluate(differences)


class FuzzCrashBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.crash"
    description = "Normalize, fingerprint, bucket, and triage fuzz crashes"
    capability_descriptions = {
        "fingerprint-crash": "Build a stable normalized crash fingerprint",
        "triage-crashes": "Bucket crashes and select compact representatives",
    }
    default_capability = "triage-crashes"

    @staticmethod
    def _record(item: Mapping[str, Any]) -> Any:
        from domainlet.fuzz.crash import CrashKind, CrashRecord, CrashSeverity

        return CrashRecord(
            _enum(CrashKind, item.get("kind", "unknown"), field="crash.kind"),
            str(item.get("target_id", "")),
            _seed_from_value(item.get("input", b"")),
            str(item.get("message", "")),
            tuple(item.get("stack", ())),
            item.get("fault_site"),
            CrashSeverity(int(item.get("severity", int(CrashSeverity.HIGH)))),
            item.get("signal"),
            item.get("exit_code"),
            item.get("sanitizer"),
            dict(item.get("metadata", {})),
        )

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.crash import triage

        if capability == "fingerprint-crash":
            return self._record(payload.get("crash", payload)).fingerprint
        return triage(
            self._record(_require_mapping(item, field="crash"))
            for item in payload.get("crashes", ())
        )


class FuzzAutoReproBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.autorepro"
    description = "Construct deterministic reproducer commands without launching external processes"
    capability_descriptions = {
        "build-command": "Build the command line for a crash reproducer",
    }
    default_capability = "build-command"
    config_keys = frozenset({"input_flag"})

    def _configure(self, config: dict[str, str]) -> None:
        self._input_flag = config.get("input_flag")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.autorepro import command_for
        from domainlet.fuzz.crash import Reproducer

        reproducer = Reproducer(
            _seed_from_value(payload.get("input", b"")),
            tuple(payload.get("arguments", ())),
            dict(payload.get("environment", {})),
        )
        return {
            "command": command_for(
                str(payload["executable"]),
                reproducer,
                input_flag=self._input_flag,
            ),
            "environment": reproducer.environment,
            "input_id": reproducer.input.id,
        }


class FuzzQueueBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.queue"
    description = "Prioritize and deduplicate fuzz campaign work items"
    capability_descriptions = {
        "prioritize-work": "Enqueue work items and return them in priority order",
        "queue-summary": "Report pending work grouped by action",
    }
    default_capability = "prioritize-work"

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.queue import (
            CandidateInputWorkQueue,
            CandidateWorkItem,
            WorkAction,
            WorkPriority,
            WorkRationale,
        )

        queue = CandidateInputWorkQueue()
        for item in payload.get("items", ()):
            priority = _require_mapping(
                item.get("priority", {}), field="priority")
            rationale_data = item.get("rationale")
            rationale = None
            if rationale_data:
                rationale = WorkRationale(
                    str(rationale_data.get("reason", "")),
                    tuple(rationale_data.get("evidence", ())),
                    rationale_data.get("strategy"),
                )
            queue.enqueue(
                CandidateWorkItem(
                    str(item["input_id"]),
                    _enum(WorkAction, item.get(
                        "action", "mutate"), field="action"),
                    WorkPriority(
                        float(priority.get("score", 0.0)),
                        float(priority.get("urgency", 0.0)),
                        float(priority.get("age", 0.0)),
                    ),
                    rationale,
                    item.get("target_id"),
                    item.get("dedup_key"),
                    max_attempts=int(item.get("max_attempts", 3)),
                )
            )
        if capability == "queue-summary":
            return queue.snapshot()
        ordered = []
        while queue:
            item = queue.pop()
            if item is not None:
                ordered.append(item)
        return ordered


class FuzzScheduleBindlet(_FuzzBindlet):
    bindlet_name = "fuzz.schedule"
    description = "Allocate fuzzing actions to ranked target candidates"
    capability_descriptions = {
        "schedule-targets": "Create an ordered mutation schedule from target scores",
    }
    default_capability = "schedule-targets"
    config_keys = frozenset(
        {"exploration_weight", "exploitation_weight", "max_pending"})

    def _configure(self, config: dict[str, str]) -> None:
        self._exploration = float(config.get("exploration_weight", "1"))
        self._exploitation = float(config.get("exploitation_weight", "1"))
        self._max_pending = int(config.get("max_pending", "1000"))
        if self._max_pending < 0:
            raise ValueError("max_pending must be non-negative")

    def _execute(self, capability: str, payload: dict[str, Any], context: AnalysisContext) -> Any:
        from domainlet.fuzz.discovery import TargetCandidate
        from domainlet.fuzz.schedule import SchedulePolicy, Scheduler

        candidates = [
            TargetCandidate(
                _target_from_dict(item.get("target", item)),
                _score_from_dict(_require_mapping(
                    item.get("score", {}), field="score")),
            )
            for item in payload.get("candidates", ())
        ]
        policy = SchedulePolicy(
            self._exploration,
            self._exploitation,
            max_pending=self._max_pending,
        )
        return Scheduler(policy).schedule_targets(candidates).ordered()


__all__ = [
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
    "FuzzLibFuzzerBindlet",
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
]
