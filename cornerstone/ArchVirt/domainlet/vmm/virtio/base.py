"""Validated, transport-neutral descriptors for VirtIO device domainlets.

The descriptors intentionally model the stable parts of the VirtIO
specification: device IDs, feature bits, virtqueue roles, and device
configuration fields.  Transport-specific register offsets are not baked
into the device modules because PCI, MMIO, and CCW expose the common device
parts through different layouts.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, IntFlag
from typing import Any, Final, Iterable


class VirtioAccess(str, Enum):
    READ_ONLY = "ro"
    READ_WRITE = "rw"
    WRITE_ONLY = "wo"


class VirtioQueueDirection(str, Enum):
    DRIVER_TO_DEVICE = "driver-to-device"
    DEVICE_TO_DRIVER = "device-to-driver"
    BIDIRECTIONAL = "bidirectional"


class VirtioTransport(str, Enum):
    PCI = "pci"
    MMIO = "mmio"
    CCW = "ccw"


class VirtioDeviceStatus(IntFlag):
    ACKNOWLEDGE = 1
    DRIVER = 2
    DRIVER_OK = 4
    FEATURES_OK = 8
    SUSPEND = 16
    DEVICE_NEEDS_RESET = 64
    FAILED = 128


@dataclass(frozen=True, slots=True)
class VirtioFeature:
    """A feature bit defined by a VirtIO device or common mechanism."""

    bit: int
    name: str
    description: str = ""
    requires: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if self.bit < 0:
            raise ValueError("feature bit must be non-negative")
        if not self.name.strip():
            raise ValueError("feature name cannot be empty")
        object.__setattr__(self, "requires", tuple(self.requires))

    def to_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "bit": self.bit,
            "name": self.name,
            "description": self.description,
        }
        if self.requires:
            result["requires"] = list(self.requires)
        return result


@dataclass(frozen=True, slots=True)
class VirtioQueue:
    """A device virtqueue, including its role and conditional presence."""

    name: str
    direction: VirtioQueueDirection | str = VirtioQueueDirection.BIDIRECTIONAL
    purpose: str = ""
    minimum: int = 1
    maximum: int | None = 1
    index: int | None = None
    optional_feature: str | None = None

    def __post_init__(self) -> None:
        if not self.name.strip():
            raise ValueError("queue name cannot be empty")
        direction = (
            self.direction
            if isinstance(self.direction, VirtioQueueDirection)
            else VirtioQueueDirection(self.direction)
        )
        object.__setattr__(self, "direction", direction)
        if self.minimum < 0:
            raise ValueError("queue minimum cannot be negative")
        if self.maximum is not None and self.maximum < self.minimum:
            raise ValueError("queue maximum cannot be less than minimum")
        if self.index is not None and self.index < 0:
            raise ValueError("queue index cannot be negative")

    def to_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "name": self.name,
            "direction": self.direction.value,
            "purpose": self.purpose,
            "minimum": self.minimum,
            "maximum": self.maximum,
        }
        if self.index is not None:
            result["index"] = self.index
        if self.optional_feature is not None:
            result["optional_feature"] = self.optional_feature
        return result


@dataclass(frozen=True, slots=True)
class VirtioRegister:
    """A transport-common or device-configuration field.

    ``offset`` is optional because many device configuration layouts contain
    variable-length or feature-conditional fields.  ``region`` distinguishes
    common transport state from device-specific configuration.
    """

    name: str
    offset: int | None = None
    width: int = 32
    access: VirtioAccess | str = VirtioAccess.READ_WRITE
    description: str = ""
    condition: str | None = None
    region: str = "device-config"

    def __post_init__(self) -> None:
        if not self.name.strip():
            raise ValueError("register name cannot be empty")
        if self.offset is not None and self.offset < 0:
            raise ValueError("register offset cannot be negative")
        if self.width <= 0:
            raise ValueError("register width must be positive")
        access = self.access if isinstance(
            self.access, VirtioAccess) else VirtioAccess(self.access)
        object.__setattr__(self, "access", access)
        if not self.region.strip():
            raise ValueError("register region cannot be empty")

    def to_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "name": self.name,
            "offset": self.offset,
            "width": self.width,
            "access": self.access.value,
            "description": self.description,
            "region": self.region,
        }
        if self.condition is not None:
            result["condition"] = self.condition
        return result


@dataclass(frozen=True, slots=True)
class VirtioDeviceDescriptor:
    """The statically known contract of one VirtIO device type."""

    device_id: int
    name: str
    spec_ref: str
    summary: str = ""
    queues: tuple[VirtioQueue, ...] = ()
    features: tuple[VirtioFeature, ...] = ()
    config_registers: tuple[VirtioRegister, ...] = ()
    aliases: tuple[str, ...] = ()
    notes: tuple[str, ...] = ()
    transports: tuple[VirtioTransport, ...] = (
        VirtioTransport.PCI,
        VirtioTransport.MMIO,
        VirtioTransport.CCW,
    )

    def __post_init__(self) -> None:
        if self.device_id <= 0:
            raise ValueError("device ID must be positive")
        if not self.name.strip():
            raise ValueError("device name cannot be empty")
        if not self.spec_ref.strip():
            raise ValueError("spec reference cannot be empty")
        object.__setattr__(self, "queues", tuple(self.queues))
        object.__setattr__(self, "features", tuple(self.features))
        object.__setattr__(self, "config_registers",
                           tuple(self.config_registers))
        object.__setattr__(self, "aliases", tuple(
            alias.strip().lower() for alias in self.aliases))
        object.__setattr__(self, "notes", tuple(self.notes))
        normalized_transports = tuple(
            item if isinstance(
                item, VirtioTransport) else VirtioTransport(item)
            for item in self.transports
        )
        object.__setattr__(self, "transports", normalized_transports)
        errors = self.validation_errors()
        if errors:
            raise ValueError("; ".join(errors))

    @property
    def canonical_name(self) -> str:
        return self.name.strip().lower()

    @property
    def identifiers(self) -> frozenset[str]:
        return frozenset((self.canonical_name, *self.aliases, str(self.device_id)))

    def feature(self, key: int | str) -> VirtioFeature | None:
        if isinstance(key, int):
            return next((item for item in self.features if item.bit == key), None)
        normalized = key.strip().lower()
        return next(
            (item for item in self.features if item.name.lower() == normalized),
            None,
        )

    def queue(self, name: str) -> VirtioQueue | None:
        normalized = name.strip().lower()
        return next((item for item in self.queues if item.name.lower() == normalized), None)

    def config_field(self, name: str) -> VirtioRegister | None:
        normalized = name.strip().lower()
        return next(
            (item for item in self.config_registers if item.name.lower() == normalized),
            None,
        )

    def validation_errors(self) -> tuple[str, ...]:
        errors: list[str] = []
        feature_bits = [item.bit for item in self.features]
        if len(feature_bits) != len(set(feature_bits)):
            errors.append("feature bits must be unique")
        feature_names = [item.name.lower() for item in self.features]
        if len(feature_names) != len(set(feature_names)):
            errors.append("feature names must be unique")
        queue_names = [item.name.lower() for item in self.queues]
        if len(queue_names) != len(set(queue_names)):
            errors.append("queue names must be unique")
        config_names = [item.name.lower() for item in self.config_registers]
        if len(config_names) != len(set(config_names)):
            errors.append("configuration field names must be unique")
        known_features = set(feature_names)
        for feature in self.features:
            missing = [name for name in feature.requires if name.lower()
                       not in known_features]
            if missing:
                errors.append(
                    f"feature {feature.name} requires unknown feature(s): {', '.join(missing)}"
                )
        for queue in self.queues:
            if queue.optional_feature and queue.optional_feature.lower() not in known_features:
                errors.append(
                    f"queue {queue.name} refers to unknown feature {queue.optional_feature}"
                )
        return tuple(errors)

    def to_dict(self) -> dict[str, Any]:
        return {
            "device_id": self.device_id,
            "name": self.canonical_name,
            "summary": self.summary,
            "spec_ref": self.spec_ref,
            "transports": [item.value for item in self.transports],
            "queues": [item.to_dict() for item in self.queues],
            "features": [item.to_dict() for item in self.features],
            "config_registers": [item.to_dict() for item in self.config_registers],
            "aliases": list(self.aliases),
            "notes": list(self.notes),
        }


@dataclass(frozen=True, slots=True)
class VirtioKernelDescriptor:
    """Composition metadata used by the VMM bindlet."""

    device_id: int
    device_name: str
    module: str
    spec_ref: str
    description: str = ""
    registers: tuple[VirtioRegister, ...] = ()
    notes: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if self.device_id <= 0:
            raise ValueError("kernel device ID must be positive")
        if not self.device_name.strip():
            raise ValueError("kernel device name cannot be empty")
        if not self.module.strip():
            raise ValueError("kernel module cannot be empty")
        object.__setattr__(self, "registers", tuple(self.registers))
        object.__setattr__(self, "notes", tuple(self.notes))

    def to_dict(self) -> dict[str, Any]:
        return {
            "device_id": self.device_id,
            "device_name": self.device_name,
            "module": self.module,
            "spec_ref": self.spec_ref,
            "description": self.description,
            "registers": [item.to_dict() for item in self.registers],
            "notes": list(self.notes),
        }


COMMON_FEATURES: Final[tuple[VirtioFeature, ...]] = (
    VirtioFeature(28, "VIRTIO_F_INDIRECT_DESC",
                  "Indirect descriptor tables are supported."),
    VirtioFeature(29, "VIRTIO_F_EVENT_IDX",
                  "Event-index notification suppression is supported."),
    VirtioFeature(32, "VIRTIO_F_VERSION_1",
                  "VirtIO 1.0-compliant non-legacy interface."),
    VirtioFeature(33, "VIRTIO_F_ACCESS_PLATFORM",
                  "Device memory accesses may be translated or restricted."),
    VirtioFeature(34, "VIRTIO_F_RING_PACKED",
                  "Packed virtqueue layout is supported."),
    VirtioFeature(35, "VIRTIO_F_IN_ORDER",
                  "Buffers are used in the order they were made available."),
    VirtioFeature(36, "VIRTIO_F_ORDER_PLATFORM",
                  "Platform memory ordering rules are required."),
    VirtioFeature(37, "VIRTIO_F_SR_IOV",
                  "PCI Single Root I/O Virtualization is supported."),
    VirtioFeature(38, "VIRTIO_F_NOTIFICATION_DATA",
                  "Notifications carry additional queue data."),
    VirtioFeature(39, "VIRTIO_F_NOTIF_CONFIG_DATA",
                  "Device-provided notification data is supported."),
    VirtioFeature(40, "VIRTIO_F_RING_RESET",
                  "Individual virtqueues can be reset."),
)

COMMON_REGISTERS: Final[tuple[VirtioRegister, ...]] = (
    VirtioRegister(
        "device_status",
        width=8,
        access=VirtioAccess.READ_WRITE,
        description="Device initialization and error status bits.",
        region="transport-common",
    ),
    VirtioRegister(
        "device_features",
        width=64,
        access=VirtioAccess.READ_ONLY,
        description="Feature bits offered by the device.",
        region="transport-common",
    ),
    VirtioRegister(
        "driver_features",
        width=64,
        access=VirtioAccess.READ_WRITE,
        description="Feature bits accepted by the driver.",
        region="transport-common",
    ),
    VirtioRegister(
        "queue_select",
        width=16,
        access=VirtioAccess.READ_WRITE,
        description="Selected virtqueue index.",
        region="transport-common",
    ),
    VirtioRegister(
        "queue_size",
        width=16,
        access=VirtioAccess.READ_WRITE,
        description="Selected virtqueue size.",
        region="transport-common",
    ),
    VirtioRegister(
        "queue_notify",
        width=16,
        access=VirtioAccess.WRITE_ONLY,
        description="Available-buffer notification.",
        region="transport-notification",
    ),
    VirtioRegister(
        "interrupt_status",
        width=8,
        access=VirtioAccess.READ_ONLY,
        description="Pending configuration and used-buffer notifications.",
        region="transport-common",
    ),
    VirtioRegister(
        "interrupt_ack",
        width=8,
        access=VirtioAccess.WRITE_ONLY,
        description="Acknowledges pending notifications.",
        region="transport-common",
    ),
)


class VirtioRegistry:
    """Immutable-index-style registry for validated device descriptors."""

    def __init__(self, devices: Iterable[VirtioDeviceDescriptor] = ()) -> None:
        ordered = tuple(sorted(devices, key=lambda item: item.device_id))
        by_id = {item.device_id: item for item in ordered}
        if len(by_id) != len(ordered):
            raise ValueError("VirtIO device IDs must be unique")
        identifiers: dict[str, VirtioDeviceDescriptor] = {}
        for device in ordered:
            for identifier in device.identifiers:
                previous = identifiers.setdefault(identifier, device)
                if previous is not device:
                    raise ValueError(
                        f"duplicate VirtIO identifier: {identifier}")
        self._devices = ordered
        self._by_id = by_id
        self._by_identifier = identifiers

    @property
    def devices(self) -> tuple[VirtioDeviceDescriptor, ...]:
        return self._devices

    def get(self, key: int | str) -> VirtioDeviceDescriptor | None:
        if isinstance(key, int):
            return self._by_id.get(key)
        return self._by_identifier.get(key.strip().lower())

    def require(self, key: int | str) -> VirtioDeviceDescriptor:
        result = self.get(key)
        if result is None:
            raise KeyError(f"unknown VirtIO device: {key}")
        return result

    def to_dict(self) -> dict[str, Any]:
        return {
            "count": len(self._devices),
            "devices": [item.to_dict() for item in self._devices],
        }


def common_registers() -> tuple[VirtioRegister, ...]:
    return COMMON_REGISTERS


def common_queues() -> tuple[VirtioQueue, ...]:
    return ()


def make_device(
    device_id: int,
    name: str,
    *,
    spec_ref: str,
    summary: str,
    queues: tuple[VirtioQueue, ...] = (),
    features: tuple[VirtioFeature, ...] = (),
    config_registers: tuple[VirtioRegister, ...] = (),
    aliases: tuple[str, ...] = (),
    notes: tuple[str, ...] = (),
    transports: tuple[VirtioTransport, ...] = (
        VirtioTransport.PCI,
        VirtioTransport.MMIO,
        VirtioTransport.CCW,
    ),
) -> VirtioDeviceDescriptor:
    return VirtioDeviceDescriptor(
        device_id=device_id,
        name=name,
        spec_ref=spec_ref,
        summary=summary,
        queues=queues,
        features=features,
        config_registers=config_registers,
        aliases=aliases,
        notes=notes,
        transports=transports,
    )


def make_kernel(
    device_id: int,
    name: str,
    *,
    module: str,
    spec_ref: str,
    description: str,
    registers: tuple[VirtioRegister, ...] = COMMON_REGISTERS,
    notes: tuple[str, ...] = (),
) -> VirtioKernelDescriptor:
    return VirtioKernelDescriptor(
        device_id=device_id,
        device_name=name,
        module=module,
        spec_ref=spec_ref,
        description=description,
        registers=registers,
        notes=notes,
    )


def feature(
    bit: int,
    name: str,
    description: str = "",
    *,
    requires: tuple[str, ...] = (),
) -> VirtioFeature:
    return VirtioFeature(bit, name, description, requires)


def queue(
    name: str,
    direction: VirtioQueueDirection | str,
    purpose: str,
    *,
    index: int | None = None,
    minimum: int = 1,
    maximum: int | None = 1,
    optional_feature: str | None = None,
) -> VirtioQueue:
    return VirtioQueue(
        name=name,
        direction=direction,
        purpose=purpose,
        minimum=minimum,
        maximum=maximum,
        index=index,
        optional_feature=optional_feature,
    )


def field(
    name: str,
    width: int,
    description: str = "",
    *,
    access: VirtioAccess | str = VirtioAccess.READ_ONLY,
    offset: int | None = None,
    condition: str | None = None,
) -> VirtioRegister:
    return VirtioRegister(
        name=name,
        offset=offset,
        width=width,
        access=access,
        description=description,
        condition=condition,
    )


__all__ = [
    "COMMON_FEATURES",
    "COMMON_REGISTERS",
    "VirtioAccess",
    "VirtioDeviceDescriptor",
    "VirtioDeviceStatus",
    "VirtioFeature",
    "VirtioKernelDescriptor",
    "VirtioQueue",
    "VirtioQueueDirection",
    "VirtioRegister",
    "VirtioRegistry",
    "VirtioTransport",
    "common_queues",
    "common_registers",
    "feature",
    "field",
    "make_device",
    "make_kernel",
    "queue",
]
