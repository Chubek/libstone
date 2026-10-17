#!/usr/bin/env python3
"""
Architecture Dataset Manager

A Textual terminal UI for maintaining a SQLite dataset of microprocessor,
GPU, and virtual architectures, including BPF/eBPF.

Install:
    python -m pip install textual pyyaml

Run:
    python scripts/archdb.py
    python scripts/archdb.py --db architectures.sqlite3
    python scripts/archdb.py --export architectures.csv
    python scripts/archdb.py --cache /tmp/archdb-cache.json

Keyboard shortcuts:
    N       New architecture
    E       Edit selected architecture
    D       Delete selected architecture
    V       View selected architecture
    S       Search/filter
    C       Clear filters
    X       Export CSV
    T       Save Temp
    L       Load Temp
    R       Refresh
    Q       Quit
"""

from __future__ import annotations

import argparse
import csv
import json
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

try:
    import yaml
except ImportError:  # pragma: no cover - exercised only when PyYAML is absent
    yaml = None


CATEGORIES = ("Microprocessor", "GPU", "Virtual")
ENDIANNESSES = ("Big", "Little", "Bi")
STATUSES = ("Draft", "Reviewed", "Final")

JSON_LIST_FIELDS = {"mnemonics", "opcodes", "semantic_rules", "registers"}
JSON_MAP_FIELDS = {
    "micro_architecture",
    "abi",
    "extra_attributes",
}

FIELDNAMES = [
    "id",
    "name",
    "category",
    "vendor",
    "bit_width",
    "endianness",
    "instruction_set",
    "mnemonics",
    "opcodes",
    "semantic_rules",
    "micro_architecture",
    "registers",
    "abi",
    "extra_attributes",
    "description",
    "status",
    "created_at",
    "updated_at",
]


class ValidationError(ValueError):
    """Raised when an architecture record is invalid."""


class CacheError(Exception):
    """Raised when a temp cache cannot be read or parsed."""


def _normalize_json(value: Any, *, expect_type: type) -> Any:
    """Return a canonical JSON-compatible value from user input."""
    if value in (None, ""):
        return [] if expect_type is list else {}

    if isinstance(value, expect_type):
        return value

    if not isinstance(value, str):
        raise ValidationError(
            "JSON fields must be valid JSON text, list, or dictionary."
        )

    payload = value.strip()
    if not payload:
        return [] if expect_type is list else {}

    try:
        parsed = json.loads(payload)
    except json.JSONDecodeError as exc:
        raise ValidationError("Invalid JSON payload.") from exc

    if not isinstance(parsed, expect_type):
        raise ValidationError(
            f"Expected JSON {expect_type.__name__}, got {type(parsed).__name__}."
        )

    return parsed


def _dump_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True)


def _format_json_field(value: Any) -> str:
    return _dump_json(value)


def _dump_yaml(value: Any) -> str:
    """Return a YAML representation for a structured field."""
    if yaml is not None:
        return yaml.safe_dump(
            value,
            allow_unicode=True,
            default_flow_style=False,
            sort_keys=True,
        ).strip()

    if isinstance(value, list):
        lines = []
        for item in value:
            if isinstance(item, str):
                lines.append(f"- {item}")
            else:
                lines.append(f"- {_dump_json(item)}")
        return "\n".join(lines)

    return _dump_json(value)


def _normalize_yaml(value: Any, *, expect_type: type) -> Any:
    """Return a canonical structured value from user-supplied YAML text."""
    if value in (None, ""):
        return [] if expect_type is list else {}

    if isinstance(value, expect_type):
        return value

    if not isinstance(value, str):
        raise ValidationError(
            "Structured fields must be valid YAML/JSON text, list, or dictionary."
        )

    payload = value.strip()
    if not payload:
        return [] if expect_type is list else {}

    if yaml is not None:
        try:
            parsed = _yaml_to_json(yaml.safe_load(payload))
        except yaml.YAMLError as exc:
            raise ValidationError("Invalid YAML payload.") from exc
    else:
        if payload.startswith("["):
            parsed = _normalize_json(payload, expect_type=expect_type)
        else:
            parsed = _yaml_list_fallback(payload)

    if not isinstance(parsed, expect_type):
        raise ValidationError(
            f"Expected YAML {expect_type.__name__}, got {type(parsed).__name__}."
        )

    return parsed


def _yaml_to_json(value: Any) -> Any:
    """Convert YAML values to JSON-compatible Python values."""
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, list):
        return [_yaml_to_json(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _yaml_to_json(item) for key, item in value.items()}
    return str(value)


def _yaml_list_fallback(payload: str) -> list[Any]:
    """Parse the simple YAML list form used by the row editor fallback."""
    items: list[Any] = []

    for line in payload.splitlines():
        item_text = line.strip()
        if not item_text or item_text.startswith("#"):
            continue
        if item_text.startswith("- "):
            item_text = item_text[2:].strip()
        elif item_text == "-":
            items.append("")
            continue

        if not item_text:
            items.append("")
            continue

        try:
            items.append(json.loads(item_text))
        except json.JSONDecodeError:
            if (
                len(item_text) >= 2
                and item_text[0] == item_text[-1]
                and item_text[0] in ("'", '"')
            ):
                items.append(item_text[1:-1])
            else:
                items.append(item_text)

    return items


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def validate_record(values: dict[str, Any]) -> dict[str, Any]:
    """Validate and normalize an architecture record."""

    name = str(values.get("name", "")).strip()
    if not name:
        raise ValidationError("Name is required.")
    if len(name) > 200:
        raise ValidationError("Name must be 200 characters or fewer.")

    category = str(values.get("category", "")).strip()
    if category not in CATEGORIES:
        raise ValidationError(
            f"Category must be one of: {', '.join(CATEGORIES)}."
        )

    vendor = str(values.get("vendor", "")).strip()
    if len(vendor) > 200:
        raise ValidationError("Vendor must be 200 characters or fewer.")

    raw_bit_width = values.get("bit_width")
    bit_width: Optional[int]

    if raw_bit_width in (None, ""):
        bit_width = None
    else:
        try:
            bit_width = int(str(raw_bit_width).strip())
        except ValueError as exc:
            raise ValidationError("Bit width must be a positive integer.") from exc

        if bit_width <= 0:
            raise ValidationError("Bit width must be a positive integer.")

    endianness = str(values.get("endianness", "")).strip()
    if endianness and endianness not in ENDIANNESSES:
        raise ValidationError(
            f"Endianness must be one of: {', '.join(ENDIANNESSES)}."
        )

    instruction_set = str(values.get("instruction_set", "")).strip()
    if len(instruction_set) > 500:
        raise ValidationError("Instruction set must be 500 characters or fewer.")

    description = str(values.get("description", "")).strip()
    if len(description) > 5000:
        raise ValidationError("Description must be 5000 characters or fewer.")

    mnemonics = _normalize_json(values.get("mnemonics"), expect_type=list)
    opcodes = _normalize_json(values.get("opcodes"), expect_type=list)
    semantic_rules = _normalize_json(
        values.get("semantic_rules"), expect_type=list
    )
    registers = _normalize_json(values.get("registers"), expect_type=list)
    micro_architecture = _normalize_json(
        values.get("micro_architecture"), expect_type=dict
    )
    abi = _normalize_json(values.get("abi"), expect_type=dict)
    extra_attributes = _normalize_json(
        values.get("extra_attributes"), expect_type=dict
    )

    status = str(values.get("status", "")).strip()
    if status and status not in STATUSES:
        raise ValidationError(
            f"Status must be one of: {', '.join(STATUSES)}."
        )

    return {
        "name": name,
        "category": category,
        "vendor": vendor,
        "bit_width": bit_width,
        "endianness": endianness or None,
        "instruction_set": instruction_set,
        "mnemonics": mnemonics,
        "opcodes": opcodes,
        "semantic_rules": semantic_rules,
        "micro_architecture": micro_architecture,
        "registers": registers,
        "abi": abi,
        "extra_attributes": extra_attributes,
        "description": description,
        "status": status or "Draft",
    }


class ArchitectureDB:
    """SQLite persistence layer for architecture records."""

    def __init__(self, path: str | Path):
        self.path = str(path)
        self.connection = sqlite3.connect(self.path)
        self.connection.row_factory = sqlite3.Row
        self.connection.execute("PRAGMA foreign_keys = ON")
        self._create_schema()

    def _table_columns(self) -> set[str]:
        cursor = self.connection.execute("PRAGMA table_info(architectures)")
        return {row[1] for row in cursor.fetchall()}

    def _create_schema(self) -> None:
        self.connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS architectures (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL COLLATE NOCASE UNIQUE,
                category TEXT NOT NULL
                    CHECK(category IN ('Microprocessor', 'GPU', 'Virtual')),
                vendor TEXT NOT NULL DEFAULT '',
                bit_width INTEGER,
                endianness TEXT
                    CHECK(endianness IS NULL OR endianness IN
                          ('Big', 'Little', 'Bi')),
                instruction_set TEXT NOT NULL DEFAULT '',
                mnemonics TEXT NOT NULL DEFAULT '[]',
                opcodes TEXT NOT NULL DEFAULT '[]',
                semantic_rules TEXT NOT NULL DEFAULT '[]',
                micro_architecture TEXT NOT NULL DEFAULT '{}',
                registers TEXT NOT NULL DEFAULT '[]',
                abi TEXT NOT NULL DEFAULT '{}',
                extra_attributes TEXT NOT NULL DEFAULT '{}',
                description TEXT NOT NULL DEFAULT '',
                status TEXT NOT NULL DEFAULT 'Draft'
                    CHECK(status IN ('Draft', 'Reviewed', 'Final')),
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            """
        )

        existing_columns = self._table_columns()
        missing_columns = {
            "mnemonics": "TEXT NOT NULL DEFAULT '[]'",
            "opcodes": "TEXT NOT NULL DEFAULT '[]'",
            "semantic_rules": "TEXT NOT NULL DEFAULT '[]'",
            "micro_architecture": "TEXT NOT NULL DEFAULT '{}'",
            "registers": "TEXT NOT NULL DEFAULT '[]'",
            "abi": "TEXT NOT NULL DEFAULT '{}'",
            "extra_attributes": "TEXT NOT NULL DEFAULT '{}'",
        }

        for name, definition in missing_columns.items():
            if name not in existing_columns:
                self.connection.execute(
                    f"ALTER TABLE architectures ADD COLUMN {name} {definition}"
                )

        self.connection.commit()

    @staticmethod
    def _row_to_dict(row: sqlite3.Row | None) -> Optional[dict[str, Any]]:
        if row is None:
            return None

        record = dict(row)
        for field in JSON_LIST_FIELDS:
            value = record.get(field, "[]")
            try:
                record[field] = _normalize_json(value, expect_type=list)
            except ValidationError:
                record[field] = []

        for field in JSON_MAP_FIELDS:
            value = record.get(field, "{}")
            try:
                record[field] = _normalize_json(value, expect_type=dict)
            except ValidationError:
                record[field] = {}

        return record

    def add(self, values: dict[str, Any]) -> dict[str, Any]:
        record = validate_record(values)
        timestamp = utc_now()

        try:
            cursor = self.connection.execute(
                """
                INSERT INTO architectures (
                    name, category, vendor, bit_width, endianness,
                    instruction_set, mnemonics, opcodes, semantic_rules,
                    micro_architecture, registers, abi, extra_attributes,
                    description, status, created_at, updated_at
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record["name"],
                    record["category"],
                    record["vendor"],
                    record["bit_width"],
                    record["endianness"],
                    record["instruction_set"],
                    _dump_json(record["mnemonics"]),
                    _dump_json(record["opcodes"]),
                    _dump_json(record["semantic_rules"]),
                    _dump_json(record["micro_architecture"]),
                    _dump_json(record["registers"]),
                    _dump_json(record["abi"]),
                    _dump_json(record["extra_attributes"]),
                    record["description"],
                    record["status"],
                    timestamp,
                    timestamp,
                ),
            )
            self.connection.commit()
        except sqlite3.IntegrityError as exc:
            if "UNIQUE" in str(exc).upper():
                raise ValidationError(
                    "An architecture with that name already exists."
                ) from exc
            raise

        result = self.get(cursor.lastrowid)
        assert result is not None
        return result

    def get(self, record_id: int) -> Optional[dict[str, Any]]:
        row = self.connection.execute(
            "SELECT * FROM architectures WHERE id = ?",
            (record_id,),
        ).fetchone()
        return self._row_to_dict(row)

    def update(self, record_id: int, values: dict[str, Any]) -> dict[str, Any]:
        record = validate_record(values)
        timestamp = utc_now()

        try:
            cursor = self.connection.execute(
                """
                UPDATE architectures
                SET name = ?,
                    category = ?,
                    vendor = ?,
                    bit_width = ?,
                    endianness = ?,
                    instruction_set = ?,
                    mnemonics = ?,
                    opcodes = ?,
                    semantic_rules = ?,
                    micro_architecture = ?,
                    registers = ?,
                    abi = ?,
                    extra_attributes = ?,
                    description = ?,
                    status = ?,
                    updated_at = ?
                WHERE id = ?
                """,
                (
                    record["name"],
                    record["category"],
                    record["vendor"],
                    record["bit_width"],
                    record["endianness"],
                    record["instruction_set"],
                    _dump_json(record["mnemonics"]),
                    _dump_json(record["opcodes"]),
                    _dump_json(record["semantic_rules"]),
                    _dump_json(record["micro_architecture"]),
                    _dump_json(record["registers"]),
                    _dump_json(record["abi"]),
                    _dump_json(record["extra_attributes"]),
                    record["description"],
                    record["status"],
                    timestamp,
                    record_id,
                ),
            )
            self.connection.commit()
        except sqlite3.IntegrityError as exc:
            if "UNIQUE" in str(exc).upper():
                raise ValidationError(
                    "An architecture with that name already exists."
                ) from exc
            raise

        if cursor.rowcount == 0:
            raise KeyError(f"Architecture {record_id} does not exist.")

        result = self.get(record_id)
        assert result is not None
        return result

    def delete(self, record_id: int) -> bool:
        cursor = self.connection.execute(
            "DELETE FROM architectures WHERE id = ?",
            (record_id,),
        )
        self.connection.commit()
        return cursor.rowcount > 0

    def search(
        self,
        text: str = "",
        category: str = "",
        status: str = "",
    ) -> list[dict[str, Any]]:
        clauses = ["1 = 1"]
        parameters: list[Any] = []

        text = text.strip()
        if text:
            pattern = f"%{text}%"
            clauses.append(
                """
                (
                    name LIKE ?
                    OR vendor LIKE ?
                    OR instruction_set LIKE ?
                    OR description LIKE ?
                    OR mnemonics LIKE ?
                    OR opcodes LIKE ?
                    OR micro_architecture LIKE ?
                    OR registers LIKE ?
                )
                """
            )
            parameters.extend(
                [
                    pattern,
                    pattern,
                    pattern,
                    pattern,
                    pattern,
                    pattern,
                    pattern,
                    pattern,
                ]
            )

        if category and category in CATEGORIES:
            clauses.append("category = ?")
            parameters.append(category)

        if status and status in STATUSES:
            clauses.append("status = ?")
            parameters.append(status)

        query = f"""
            SELECT *
            FROM architectures
            WHERE {' AND '.join(clauses)}
            ORDER BY category, name COLLATE NOCASE
        """

        rows = self.connection.execute(query, parameters).fetchall()
        return [self._row_to_dict(row) for row in rows]

    def export_csv(
        self,
        path: str | Path,
        rows: Optional[list[dict[str, Any]]] = None,
    ) -> int:
        output_path = Path(path)
        output_path.parent.mkdir(parents=True, exist_ok=True)

        if rows is None:
            rows = self.search()

        with output_path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=FIELDNAMES)
            writer.writeheader()

            for row in rows:
                row_values = {}
                for field in FIELDNAMES:
                    value = row.get(field, "")
                    if field in JSON_LIST_FIELDS or field in JSON_MAP_FIELDS:
                        row_values[field] = _dump_json(value)
                    else:
                        row_values[field] = value
                writer.writerow(row_values)

        return len(rows)

    def close(self) -> None:
        self.connection.close()


class DatasetCache:
    """JSON snapshot of the architecture dataset for temporary recovery."""

    CACHE_VERSION = 1

    def __init__(self, path: str | Path):
        self.path = Path(path)

    def save(self, rows: list[dict[str, Any]]) -> int:
        """Atomically write rows to the cache file and return the row count."""
        self.path.parent.mkdir(parents=True, exist_ok=True)

        payload = {
            "version": self.CACHE_VERSION,
            "saved_at": utc_now(),
            "records": rows,
        }

        temporary_path = self.path.with_name(self.path.name + ".tmp")
        with temporary_path.open("w", encoding="utf-8") as handle:
            json.dump(
                payload,
                handle,
                ensure_ascii=False,
                indent=2,
                sort_keys=True,
            )
            handle.write("\n")

        temporary_path.replace(self.path)
        return len(rows)

    def load(self) -> list[dict[str, Any]]:
        """Read records from the cache file."""
        if not self.path.exists():
            raise CacheError(f"Cache file does not exist: {self.path}")

        try:
            with self.path.open("r", encoding="utf-8") as handle:
                payload = json.load(handle)
        except (OSError, json.JSONDecodeError) as exc:
            raise CacheError(f"Could not read cache file {self.path}: {exc}") from exc

        if not isinstance(payload, dict):
            raise CacheError(f"Invalid cache file format: {self.path}")

        records = payload.get("records")
        if not isinstance(records, list):
            raise CacheError(f"Cache file has no records list: {self.path}")

        return records


try:
    from textual.app import App, ComposeResult
    from textual.containers import Container, Horizontal, Vertical, VerticalScroll
    from textual.screen import ModalScreen, Screen
    from textual.widget import Widget
    from textual.widgets import (
        Button,
        DataTable,
        Footer,
        Header,
        Input,
        Label,
        Select,
        Static,
        TextArea,
    )

    TEXTUAL_AVAILABLE = True
except ImportError:
    TEXTUAL_AVAILABLE = False


if TEXTUAL_AVAILABLE:

    class MessageScreen(ModalScreen[None]):
        CSS = """
        MessageScreen {
            align: center middle;
            background: $background 70%;
        }

        #message-dialog {
            width: 70%;
            max-width: 90;
            height: auto;
            border: round $accent;
            background: $surface;
            padding: 1 2;
        }

        #message-text {
            height: auto;
            margin-bottom: 1;
        }
        """

        def __init__(self, title: str, message: str):
            super().__init__()
            self.message_title = title
            self.message = message

        def compose(self) -> ComposeResult:
            with Container(id="message-dialog"):
                yield Label(self.message_title)
                yield Static(self.message, id="message-text", markup=False)
                yield Button("OK", id="ok", variant="primary")

        def on_button_pressed(self, event: Button.Pressed) -> None:
            if event.button.id == "ok":
                self.dismiss(None)


    class ConfirmScreen(ModalScreen[bool]):
        CSS = """
        ConfirmScreen {
            align: center middle;
            background: $background 70%;
        }

        #confirm-dialog {
            width: 70%;
            max-width: 80;
            height: auto;
            border: round $warning;
            background: $surface;
            padding: 1 2;
        }

        #confirm-text {
            height: auto;
            margin-bottom: 1;
        }

        .dialog-buttons {
            height: auto;
            align: right middle;
        }
        """

        def __init__(self, message: str):
            super().__init__()
            self.message = message

        def compose(self) -> ComposeResult:
            with Container(id="confirm-dialog"):
                yield Static(self.message, id="confirm-text", markup=False)
                with Horizontal(classes="dialog-buttons"):
                    yield Button("Cancel", id="cancel")
                    yield Button("Delete", id="delete", variant="error")

        def on_button_pressed(self, event: Button.Pressed) -> None:
            if event.button.id == "delete":
                self.dismiss(True)
            elif event.button.id == "cancel":
                self.dismiss(False)


    class StructuredListEditor(Widget):
        """Row-first editor for list-valued fields with JSON/YAML raw modes."""

        def __init__(
            self,
            field: str,
            label: str,
            value: Any,
            placeholder: str = "",
        ):
            super().__init__(id=f"{field}-editor")
            self.field = field
            self.label = label
            self.placeholder = placeholder
            self.values = self._coerce(value)
            self.mode = "rows"

        @staticmethod
        def _coerce(value: Any) -> list[Any]:
            if isinstance(value, list):
                return value
            if value in (None, ""):
                return []
            if isinstance(value, str):
                try:
                    parsed = json.loads(value)
                except json.JSONDecodeError:
                    return [value]
                return parsed if isinstance(parsed, list) else [parsed]
            return [value]

        @staticmethod
        def _row_text(value: Any) -> str:
            return value if isinstance(value, str) else _dump_json(value)

        @staticmethod
        def _parse_row_text(value: str) -> Any:
            text = value.strip()
            if not text:
                return ""
            try:
                return json.loads(text)
            except json.JSONDecodeError:
                return text

        def _raw_text(self) -> str:
            if self.mode == "yaml":
                return _dump_yaml(self.values)
            return _dump_json(self.values)

        def compose(self) -> ComposeResult:
            with Container(id=f"{self.field}-box"):
                yield Label(self.label, classes="field-label")

                with Horizontal(classes="editor-toolbar"):
                    yield Button(
                        "Rows",
                        id=f"{self.field}-mode-rows",
                        variant="primary",
                    )
                    yield Button("JSON", id=f"{self.field}-mode-json")
                    yield Button("YAML", id=f"{self.field}-mode-yaml")
                    yield Button("Add Row", id=f"{self.field}-add")

                yield Vertical(
                    id=f"{self.field}-rows",
                    classes="structured-rows",
                )
                yield TextArea(
                    self._raw_text(),
                    id=f"{self.field}-raw",
                    classes="structured-raw",
                )

        def on_mount(self) -> None:
            self._refresh_editor()

        def _refresh_editor(self) -> None:
            rows = self.query_one(f"#{self.field}-rows", Vertical)
            raw = self.query_one(f"#{self.field}-raw", TextArea)

            rows.display = self.mode == "rows"
            raw.display = self.mode != "rows"

            if self.mode == "rows":
                self._rebuild_rows()
            else:
                raw.text = self._raw_text()

            self._refresh_toolbar()

        def _refresh_toolbar(self) -> None:
            for mode in ("rows", "json", "yaml"):
                button = self.query_one(
                    f"#{self.field}-mode-{mode}",
                    Button,
                )
                button.variant = "primary" if self.mode == mode else "default"

        def _rebuild_rows(self) -> None:
            container = self.query_one(f"#{self.field}-rows", Vertical)
            container.remove_children()

            display_values = [*self.values, ""]
            for index, value in enumerate(display_values):
                container.mount(
                    Horizontal(
                        Input(
                            value=self._row_text(value),
                            placeholder=self.placeholder,
                            id=f"{self.field}-row-{index}",
                        ),
                        Button(
                            "Remove",
                            id=f"{self.field}-remove-{index}",
                            variant="warning",
                        ),
                        classes="hard-row",
                    )
                )

        def _read_rows(self) -> list[Any]:
            container = self.query_one(f"#{self.field}-rows", Vertical)
            values = []
            for input_widget in container.query(Input):
                parsed = self._parse_row_text(input_widget.value)
                if parsed != "":
                    values.append(parsed)
            return values

        def _sync_values_from_editor(self) -> None:
            if self.mode == "rows":
                self.values = self._read_rows()
            elif self.mode == "json":
                self.values = _normalize_json(
                    self.query_one(f"#{self.field}-raw", TextArea).text,
                    expect_type=list,
                )
            else:
                self.values = _normalize_yaml(
                    self.query_one(f"#{self.field}-raw", TextArea).text,
                    expect_type=list,
                )

        def _set_mode(self, mode: str) -> None:
            if mode not in {"rows", "json", "yaml"}:
                return
            self._sync_values_from_editor()
            self.mode = mode
            self._refresh_editor()

        def _add_row(self) -> None:
            self._sync_values_from_editor()
            self.values.append("")
            self.mode = "rows"
            self._refresh_editor()

        def _remove_row(self, index: int) -> None:
            self._sync_values_from_editor()
            if 0 <= index < len(self.values):
                self.values.pop(index)
            self.mode = "rows"
            self._refresh_editor()

        def value(self) -> list[Any]:
            self._sync_values_from_editor()
            return self.values

        def on_button_pressed(self, event: Button.Pressed) -> None:
            button_id = event.button.id or ""

            if button_id == f"{self.field}-mode-rows":
                self._set_mode("rows")
            elif button_id == f"{self.field}-mode-json":
                self._set_mode("json")
            elif button_id == f"{self.field}-mode-yaml":
                self._set_mode("yaml")
            elif button_id == f"{self.field}-add":
                self._add_row()
            elif button_id.startswith(f"{self.field}-remove-"):
                try:
                    index = int(button_id.rsplit("-", 1)[1])
                except ValueError:
                    return
                self._remove_row(index)
            else:
                return

            event.stop()


    class StructuredMapEditor(Widget):
        """Row-first editor for JSON/YAML object-valued fields."""

        def __init__(
            self,
            field: str,
            label: str,
            value: Any,
            key_placeholder: str = "Key",
            value_placeholder: str = "Value",
        ):
            super().__init__(id=f"{field}-editor")
            self.field = field
            self.label = label
            self.key_placeholder = key_placeholder
            self.value_placeholder = value_placeholder
            self.values = self._coerce(value)
            self.mode = "rows"

        @staticmethod
        def _coerce(value: Any) -> dict[str, Any]:
            if isinstance(value, dict):
                return value
            if value in (None, ""):
                return {}
            if isinstance(value, str):
                try:
                    parsed = json.loads(value)
                except json.JSONDecodeError:
                    return {value: ""}
                return parsed if isinstance(parsed, dict) else {str(parsed): ""}
            return {str(value): ""}

        @staticmethod
        def _value_text(value: Any) -> str:
            return value if isinstance(value, str) else _dump_json(value)

        @staticmethod
        def _parse_value_text(value: str) -> Any:
            text = value.strip()
            if not text:
                return ""
            try:
                return json.loads(text)
            except json.JSONDecodeError:
                return text

        def _raw_text(self) -> str:
            if self.mode == "yaml":
                return _dump_yaml(self.values)
            return _dump_json(self.values)

        def compose(self) -> ComposeResult:
            with Container(id=f"{self.field}-box"):
                yield Label(self.label, classes="field-label")

                with Horizontal(classes="editor-toolbar"):
                    yield Button(
                        "Rows",
                        id=f"{self.field}-mode-rows",
                        variant="primary",
                    )
                    yield Button("JSON", id=f"{self.field}-mode-json")
                    yield Button("YAML", id=f"{self.field}-mode-yaml")
                    yield Button("Add Row", id=f"{self.field}-add")

                yield Vertical(
                    id=f"{self.field}-rows",
                    classes="structured-rows",
                )
                yield TextArea(
                    self._raw_text(),
                    id=f"{self.field}-raw",
                    classes="structured-raw",
                )

        def on_mount(self) -> None:
            self._refresh_editor()

        def _refresh_editor(self) -> None:
            rows = self.query_one(f"#{self.field}-rows", Vertical)
            raw = self.query_one(f"#{self.field}-raw", TextArea)

            rows.display = self.mode == "rows"
            raw.display = self.mode != "rows"

            if self.mode == "rows":
                self._rebuild_rows()
            else:
                raw.text = self._raw_text()

            self._refresh_toolbar()

        def _refresh_toolbar(self) -> None:
            for mode in ("rows", "json", "yaml"):
                button = self.query_one(
                    f"#{self.field}-mode-{mode}",
                    Button,
                )
                button.variant = "primary" if self.mode == mode else "default"

        def _rebuild_rows(self) -> None:
            container = self.query_one(f"#{self.field}-rows", Vertical)
            container.remove_children()

            display_items = [*self.values.items(), ("", "")]
            for index, (key, value) in enumerate(display_items):
                container.mount(
                    Horizontal(
                        Input(
                            value=str(key),
                            placeholder=self.key_placeholder,
                            id=f"{self.field}-key-{index}",
                        ),
                        Input(
                            value=self._value_text(value),
                            placeholder=self.value_placeholder,
                            id=f"{self.field}-value-{index}",
                        ),
                        Button(
                            "Remove",
                            id=f"{self.field}-remove-{index}",
                            variant="warning",
                        ),
                        classes="hard-row hard-map-row",
                    )
                )

        def _read_rows(self) -> dict[str, Any]:
            container = self.query_one(f"#{self.field}-rows", Vertical)
            values: dict[str, Any] = {}

            for row in container.query(Horizontal):
                inputs = list(row.query(Input))
                if len(inputs) < 2:
                    continue
                key = inputs[0].value.strip()
                if not key:
                    continue
                values[key] = self._parse_value_text(inputs[1].value)

            return values

        def _sync_values_from_editor(self) -> None:
            if self.mode == "rows":
                self.values = self._read_rows()
            elif self.mode == "json":
                self.values = _normalize_json(
                    self.query_one(f"#{self.field}-raw", TextArea).text,
                    expect_type=dict,
                )
            else:
                self.values = _normalize_yaml(
                    self.query_one(f"#{self.field}-raw", TextArea).text,
                    expect_type=dict,
                )

        def _set_mode(self, mode: str) -> None:
            if mode not in {"rows", "json", "yaml"}:
                return
            self._sync_values_from_editor()
            self.mode = mode
            self._refresh_editor()

        def _add_row(self) -> None:
            self._sync_values_from_editor()
            self.values[""] = ""
            self.mode = "rows"
            self._refresh_editor()

        def _remove_row(self, index: int) -> None:
            self._sync_values_from_editor()
            items = list(self.values.items())
            if 0 <= index < len(items):
                self.values.pop(items[index][0], None)
            self.mode = "rows"
            self._refresh_editor()

        def value(self) -> dict[str, Any]:
            self._sync_values_from_editor()
            return self.values

        def on_button_pressed(self, event: Button.Pressed) -> None:
            button_id = event.button.id or ""

            if button_id == f"{self.field}-mode-rows":
                self._set_mode("rows")
            elif button_id == f"{self.field}-mode-json":
                self._set_mode("json")
            elif button_id == f"{self.field}-mode-yaml":
                self._set_mode("yaml")
            elif button_id == f"{self.field}-add":
                self._add_row()
            elif button_id.startswith(f"{self.field}-remove-"):
                try:
                    index = int(button_id.rsplit("-", 1)[1])
                except ValueError:
                    return
                self._remove_row(index)
            else:
                return

            event.stop()


    class InstructionSetEditor(StructuredListEditor):
        """Structured editor for the comma-separated instruction_set field."""

        @staticmethod
        def _coerce(value: Any) -> list[Any]:
            if isinstance(value, list):
                return value

            text = "" if value in (None, "") else str(value)
            return [part.strip() for part in text.split(",") if part.strip()]

        def joined_value(self) -> str:
            values = self.value()
            return ", ".join(
                str(value).strip()
                for value in values
                if str(value).strip()
            )


    class FormScreen(ModalScreen[bool]):
        CSS = """
        FormScreen {
            align: center middle;
            background: $background 70%;
        }

        #form-dialog {
            width: 90%;
            max-width: 110;
            height: 90%;
            border: round $accent;
            background: $surface;
            padding: 1 2;
        }

        #form-scroll {
            height: 1fr;
        }

        .field-label {
            margin-top: 1;
        }

        .dialog-buttons {
            height: auto;
            margin-top: 1;
            align: right middle;
        }

        #form-error {
            color: $error;
            height: auto;
            min-height: 1;
        }

        .editor-toolbar {
            height: auto;
            margin-top: 0;
        }

        .editor-toolbar Button {
            margin: 0 1 0 0;
            min-width: 6;
        }

        .hard-row {
            height: auto;
            margin-top: 1;
        }

        .hard-row Input {
            width: 1fr;
        }

        .hard-row Button {
            min-width: 9;
            margin-left: 1;
        }

        .structured-raw {
            margin-top: 1;
        }
        """

        def __init__(
            self,
            database: ArchitectureDB,
            record: Optional[dict[str, Any]] = None,
        ):
            super().__init__()
            self.database = database
            self.record = record or {}

        def compose(self) -> ComposeResult:
            editing = bool(self.record)

            with Container(id="form-dialog"):
                yield Label(
                    "Edit Architecture" if editing else "New Architecture"
                )

                with VerticalScroll(id="form-scroll"):
                    yield Label("Name *", classes="field-label")
                    yield Input(
                        value=str(self.record.get("name", "")),
                        placeholder="For example: BPF, x86-64, CUDA SM",
                        id="name",
                    )

                    yield Label("Category *", classes="field-label")
                    yield Select(
                        [(item, item) for item in CATEGORIES],
                        value=self.record.get("category", CATEGORIES[0]),
                        id="category",
                    )

                    yield Label("Vendor", classes="field-label")
                    yield Input(
                        value=str(self.record.get("vendor", "")),
                        placeholder="For example: Intel, NVIDIA, Linux",
                        id="vendor",
                    )

                    yield Label("Bit width", classes="field-label")
                    yield Input(
                        value=(
                            ""
                            if self.record.get("bit_width") is None
                            else str(self.record.get("bit_width"))
                        ),
                        placeholder="For example: 32 or 64",
                        id="bit_width",
                    )

                    yield Label("Endianness", classes="field-label")
                    yield Select(
                        [("Not specified", ""), *[
                            (item, item) for item in ENDIANNESSES
                        ]],
                        value=self.record.get("endianness") or "",
                        id="endianness",
                    )

                    yield InstructionSetEditor(
                        "instruction_set",
                        "Instruction set / ISA",
                        self.record.get("instruction_set", ""),
                        placeholder="For example: RV64GC, SM_90, eBPF ISA",
                    )

                    yield Label("Status", classes="field-label")
                    yield Select(
                        [(item, item) for item in STATUSES],
                        value=self.record.get("status", "Draft"),
                        id="status",
                    )

                    yield Label("Description", classes="field-label")
                    yield TextArea(
                        text=str(self.record.get("description", "")),
                        id="description",
                    )

                    yield StructuredListEditor(
                        "mnemonics",
                        "Mnemonics",
                        self.record.get("mnemonics", []),
                        placeholder="For example: MOV, ADD, CALL",
                    )

                    yield StructuredListEditor(
                        "opcodes",
                        "Opcodes",
                        self.record.get("opcodes", []),
                        placeholder="For example: 0x90, 0x0F, 0x05",
                    )

                    yield StructuredListEditor(
                        "semantic_rules",
                        "Semantics",
                        self.record.get("semantic_rules", []),
                        placeholder="Semantic rules or JSON objects",
                    )

                    yield StructuredMapEditor(
                        "micro_architecture",
                        "Micro-architecture",
                        self.record.get("micro_architecture", {}),
                        key_placeholder="Attribute",
                        value_placeholder="Value",
                    )

                    yield StructuredListEditor(
                        "registers",
                        "Registers",
                        self.record.get("registers", []),
                        placeholder="For example: RAX, X0, R1",
                    )

                    yield StructuredMapEditor(
                        "abi",
                        "ABI",
                        self.record.get("abi", {}),
                        key_placeholder="Convention",
                        value_placeholder="Value",
                    )

                    yield StructuredMapEditor(
                        "extra_attributes",
                        "Extra attributes",
                        self.record.get("extra_attributes", {}),
                        key_placeholder="Attribute",
                        value_placeholder="Value",
                    )

                yield Static("", id="form-error", markup=False)

                with Horizontal(classes="dialog-buttons"):
                    yield Button("Cancel", id="cancel")
                    yield Button("Save", id="save", variant="primary")

        @staticmethod
        def _select_value(widget: Select) -> str:
            value = widget.value
            return value if isinstance(value, str) else ""

        def _read_values(self) -> dict[str, Any]:
            return {
                "name": self.query_one("#name", Input).value,
                "category": self._select_value(
                    self.query_one("#category", Select)
                ),
                "vendor": self.query_one("#vendor", Input).value,
                "bit_width": self.query_one("#bit_width", Input).value,
                "endianness": self._select_value(
                    self.query_one("#endianness", Select)
                ),
                "instruction_set": self.query_one(
                    "#instruction_set-editor", InstructionSetEditor
                ).joined_value(),
                "status": self._select_value(
                    self.query_one("#status", Select)
                ),
                "description": self.query_one("#description", TextArea).text,
                "mnemonics": self.query_one(
                    "#mnemonics-editor", StructuredListEditor
                ).value(),
                "opcodes": self.query_one(
                    "#opcodes-editor", StructuredListEditor
                ).value(),
                "semantic_rules": self.query_one(
                    "#semantic_rules-editor", StructuredListEditor
                ).value(),
                "micro_architecture": self.query_one(
                    "#micro_architecture-editor", StructuredMapEditor
                ).value(),
                "registers": self.query_one(
                    "#registers-editor", StructuredListEditor
                ).value(),
                "abi": self.query_one(
                    "#abi-editor", StructuredMapEditor
                ).value(),
                "extra_attributes": self.query_one(
                    "#extra_attributes-editor", StructuredMapEditor
                ).value(),
            }

        def on_button_pressed(self, event: Button.Pressed) -> None:
            if event.button.id == "cancel":
                self.dismiss(False)
                return

            if event.button.id != "save":
                return

            try:
                record = validate_record(self._read_values())

                if self.record:
                    self.database.update(int(self.record["id"]), record)
                else:
                    self.database.add(record)

            except (ValidationError, KeyError, sqlite3.Error) as exc:
                self.query_one("#form-error", Static).update(str(exc))
                return

            self.dismiss(True)


    class SearchScreen(ModalScreen[Optional[dict[str, str]]]):
        CSS = """
        SearchScreen {
            align: center middle;
            background: $background 70%;
        }

        #search-dialog {
            width: 80%;
            max-width: 90;
            height: auto;
            border: round $accent;
            background: $surface;
            padding: 1 2;
        }

        .field-label {
            margin-top: 1;
        }

        .dialog-buttons {
            height: auto;
            margin-top: 1;
            align: right middle;
        }
        """

        def __init__(self, current: dict[str, str]):
            super().__init__()
            self.current = current

        def compose(self) -> ComposeResult:
            category_options = [
                ("All categories", "all"),
                *[(item, item) for item in CATEGORIES],
            ]
            status_options = [
                ("All statuses", "all"),
                *[(item, item) for item in STATUSES],
            ]

            with Container(id="search-dialog"):
                yield Label("Search and Filter")

                yield Label("Text", classes="field-label")
                yield Input(
                    value=self.current.get("text", ""),
                    placeholder="Name, vendor, ISA, description, or JSON attributes",
                    id="search-text",
                )

                yield Label("Category", classes="field-label")
                yield Select(
                    category_options,
                    value=self.current.get("category", "all"),
                    id="search-category",
                )

                yield Label("Status", classes="field-label")
                yield Select(
                    status_options,
                    value=self.current.get("status", "all"),
                    id="search-status",
                )

                with Horizontal(classes="dialog-buttons"):
                    yield Button("Cancel", id="cancel")
                    yield Button("Apply", id="apply", variant="primary")

        @staticmethod
        def _select_value(widget: Select) -> str:
            value = widget.value
            return value if isinstance(value, str) else "all"

        def on_button_pressed(self, event: Button.Pressed) -> None:
            if event.button.id == "cancel":
                self.dismiss(None)
                return

            if event.button.id == "apply":
                category = self._select_value(
                    self.query_one("#search-category", Select)
                )
                status = self._select_value(
                    self.query_one("#search-status", Select)
                )

                self.dismiss(
                    {
                        "text": self.query_one("#search-text", Input).value,
                        "category": "" if category == "all" else category,
                        "status": "" if status == "all" else status,
                    }
                )


    class ExportScreen(ModalScreen[Optional[str]]):
        CSS = """
        ExportScreen {
            align: center middle;
            background: $background 70%;
        }

        #export-dialog {
            width: 80%;
            max-width: 90;
            height: auto;
            border: round $accent;
            background: $surface;
            padding: 1 2;
        }

        .dialog-buttons {
            height: auto;
            margin-top: 1;
            align: right middle;
        }
        """

        def __init__(self, default_path: str = "architectures.csv"):
            super().__init__()
            self.default_path = default_path

        def compose(self) -> ComposeResult:
            with Container(id="export-dialog"):
                yield Label("Export CSV")
                yield Input(
                    value=self.default_path,
                    id="export-path",
                )

                with Horizontal(classes="dialog-buttons"):
                    yield Button("Cancel", id="cancel")
                    yield Button("Export", id="export", variant="primary")

        def on_button_pressed(self, event: Button.Pressed) -> None:
            if event.button.id == "cancel":
                self.dismiss(None)
            elif event.button.id == "export":
                path = self.query_one("#export-path", Input).value.strip()
                self.dismiss(path or None)


    class MainScreen(Screen):
        CSS = """
        #toolbar {
            height: auto;
            padding: 0 1;
            background: $surface;
        }

        #toolbar Button {
            margin: 0 1 0 0;
        }

        #status {
            height: 1;
            padding: 0 1;
            color: $text-muted;
        }

        #boot-hints {
            height: auto;
            padding: 0 1;
            color: $text-muted;
        }

        #boot-hints Static {
            height: 1;
        }

        #architecture-table {
            height: 1fr;
        }
        """

        def __init__(self):
            super().__init__()
            self.row_ids: list[int] = []

        def compose(self) -> ComposeResult:
            yield Header()

            with Horizontal(id="toolbar"):
                yield Button("New", id="new")
                yield Button("Edit", id="edit")
                yield Button("Delete", id="delete", variant="error")
                yield Button("View", id="view")
                yield Button("Search", id="search")
                yield Button("Clear", id="clear")
                yield Button("Export", id="export")
                yield Button("Save Temp", id="save_temp")
                yield Button("Load Temp", id="load_temp")
                yield Button("Refresh", id="refresh")

            yield Static("Loading...", id="status")

            with Vertical(id="boot-hints"):
                yield Static("Press N to Begin", id="boot-new-hint")
                yield Static("Press L to load up saved work", id="boot-load-hint")

            yield DataTable(
                id="architecture-table",
                cursor_type="row",
                zebra_stripes=True,
            )

            yield Footer()

        def on_mount(self) -> None:
            table = self.query_one("#architecture-table", DataTable)
            table.add_columns(
                "ID",
                "Name",
                "Category",
                "Vendor",
                "Bits",
                "Endianness",
                "ISA",
                "Status",
            )

        def set_rows(self, rows: list[dict[str, Any]]) -> None:
            table = self.query_one("#architecture-table", DataTable)
            table.clear()
            self.row_ids = []
            for row in rows:
                self.row_ids.append(int(row["id"]))
                table.add_row(
                    str(row["id"]),
                    row["name"],
                    row["category"],
                    row["vendor"],
                    "" if row["bit_width"] is None else str(row["bit_width"]),
                    row["endianness"] or "",
                    row["instruction_set"],
                    row["status"],
                )

            hints = self.query_one("#boot-hints", Vertical)
            hints.display = not rows

        def selected_id(self) -> Optional[int]:
            if not self.row_ids:
                return None

            table = self.query_one("#architecture-table", DataTable)
            row_index = table.cursor_row

            if row_index is None or row_index < 0:
                return None

            if row_index >= len(self.row_ids):
                return None

            return self.row_ids[row_index]


    class DatasetApp(App[None]):
        TITLE = "Architecture Dataset Manager"

        CSS = """
        Screen {
            background: $background;
        }

        Button {
            min-width: 10;
        }

        TextArea {
            height: 8;
        }

        .dialog-buttons Button {
            margin-left: 1;
        }
        """

        BINDINGS = [
            ("n", "new", "New"),
            ("e", "edit", "Edit"),
            ("d", "delete", "Delete"),
            ("v", "view", "View"),
            ("s", "search", "Search"),
            ("c", "clear_filter", "Clear"),
            ("x", "export", "Export"),
            ("t", "save_temp", "Save Temp"),
            ("l", "load_temp", "Load Temp"),
            ("r", "refresh", "Refresh"),
            ("q", "quit", "Quit"),
        ]

        def __init__(self, database: ArchitectureDB, cache_path: str | Path):
            super().__init__()
            self.database = database
            self.cache_path = str(cache_path)
            self.cache = DatasetCache(cache_path)
            self.filters = {
                "text": "",
                "category": "",
                "status": "",
            }
            self.pending_delete_id: Optional[int] = None

        def compose(self) -> ComposeResult:
            yield MainScreen()

        def on_mount(self) -> None:
            self.refresh_rows()
            self.set_interval(
                300,
                self.action_save_temp,
                name="autosave-cache",
            )

        def on_unmount(self) -> None:
            self.database.close()

        def notify_error(self, message: str) -> None:
            self.notify(message, severity="error", timeout=5)

        def selected_record(self) -> Optional[dict[str, Any]]:
            main_screen = self.query_one(MainScreen)
            record_id = main_screen.selected_id()

            if record_id is None:
                self.notify_error("Select an architecture first.")
                return None

            record = self.database.get(record_id)
            if record is None:
                self.notify_error("The selected record no longer exists.")
                self.refresh_rows()
                return None

            return record

        def refresh_rows(self) -> None:
            rows = self.database.search(
                text=self.filters["text"],
                category=self.filters["category"],
                status=self.filters["status"],
            )

            main_screen = self.query_one(MainScreen)
            main_screen.set_rows(rows)

            active_filters = []
            if self.filters["text"]:
                active_filters.append(f"text={self.filters['text']!r}")
            if self.filters["category"]:
                active_filters.append(f"category={self.filters['category']}")
            if self.filters["status"]:
                active_filters.append(f"status={self.filters['status']}")

            suffix = f" | Filters: {', '.join(active_filters)}" if active_filters else ""
            self.query_one("#status", Static).update(
                f"{len(rows)} architecture(s){suffix}"
            )

        def action_new(self) -> None:
            self.push_screen(
                FormScreen(self.database),
                self._form_finished,
            )

        def action_edit(self) -> None:
            record = self.selected_record()
            if record is None:
                return

            self.push_screen(
                FormScreen(self.database, record),
                self._form_finished,
            )

        def _form_finished(self, saved: Optional[bool]) -> None:
            if saved:
                self.refresh_rows()
                self.notify("Architecture saved.")

        def action_delete(self) -> None:
            record = self.selected_record()
            if record is None:
                return

            self.pending_delete_id = int(record["id"])
            self.push_screen(
                ConfirmScreen(
                    f"Delete '{record['name']}'?\n\n"
                    "This operation cannot be undone."
                ),
                self._delete_confirmed,
            )

        def _delete_confirmed(self, confirmed: Optional[bool]) -> None:
            if not confirmed or self.pending_delete_id is None:
                return

            try:
                deleted = self.database.delete(self.pending_delete_id)
            except sqlite3.Error as exc:
                self.notify_error(str(exc))
                return
            finally:
                self.pending_delete_id = None

            if deleted:
                self.refresh_rows()
                self.notify("Architecture deleted.")
            else:
                self.notify_error("Architecture was not found.")

        def action_view(self) -> None:
            record = self.selected_record()
            if record is None:
                return

            details = "\n".join(
                [
                    f"ID: {record['id']}",
                    f"Name: {record['name']}",
                    f"Category: {record['category']}",
                    f"Vendor: {record['vendor'] or '(not specified)'}",
                    f"Bit width: {record['bit_width'] or '(not specified)'}",
                    f"Endianness: {record['endianness'] or '(not specified)'}",
                    f"Instruction set: "
                    f"{record['instruction_set'] or '(not specified)'}",
                    f"Mnemonics: {_format_json_field(record.get('mnemonics', []))}",
                    f"Opcodes: {_format_json_field(record.get('opcodes', []))}",
                    "Semantics: {0}".format(
                        _format_json_field(record.get("semantic_rules", []))
                    ),
                    f"Micro-architecture: {_format_json_field(record.get('micro_architecture', {}))}",
                    f"Registers: {_format_json_field(record.get('registers', []))}",
                    f"ABI: {_format_json_field(record.get('abi', {}))}",
                    "Extra attributes: {0}".format(
                        _format_json_field(record.get("extra_attributes", {}))
                    ),
                    f"Status: {record['status']}",
                    f"Created: {record['created_at']}",
                    f"Updated: {record['updated_at']}",
                    "",
                    "Description:",
                    record["description"] or "(no description)",
                ]
            )

            self.push_screen(MessageScreen("Architecture Details", details))

        def action_search(self) -> None:
            self.push_screen(
                SearchScreen(self.filters),
                self._search_finished,
            )

        def _search_finished(
            self,
            result: Optional[dict[str, str]],
        ) -> None:
            if result is not None:
                self.filters = result
                self.refresh_rows()

        def action_clear_filter(self) -> None:
            self.filters = {
                "text": "",
                "category": "",
                "status": "",
            }
            self.refresh_rows()

        def action_export(self) -> None:
            self.push_screen(
                ExportScreen(),
                self._export_finished,
            )

        def _export_finished(self, path: Optional[str]) -> None:
            if not path:
                return

            try:
                rows = self.database.search(
                    text=self.filters["text"],
                    category=self.filters["category"],
                    status=self.filters["status"],
                )
                count = self.database.export_csv(path, rows)
            except (OSError, sqlite3.Error) as exc:
                self.notify_error(str(exc))
                return

            self.notify(f"Exported {count} record(s) to {path}")

        def action_save_temp(self) -> None:
            try:
                rows = self.database.search()
                count = self.cache.save(rows)
            except (OSError, sqlite3.Error) as exc:
                self.notify_error(f"Failed to save temp cache: {exc}")
                return

            self.notify(
                f"Saved {count} record(s) to temp cache: {self.cache_path}"
            )

        def action_load_temp(self) -> None:
            try:
                rows = self.cache.load()
            except (CacheError, OSError) as exc:
                self.notify_error(str(exc))
                return

            if not rows:
                self.notify(
                    "Temp cache is empty.",
                    severity="warning",
                    timeout=5,
                )
                return

            try:
                added, updated = self._merge_cached_rows(rows)
            except (ValidationError, KeyError, sqlite3.Error) as exc:
                self.notify_error(f"Failed to load temp cache: {exc}")
                return

            self.refresh_rows()
            self.notify(
                "Loaded temp cache: "
                f"{added} added, {updated} updated from {self.cache_path}"
            )

        def _merge_cached_rows(
            self,
            rows: list[dict[str, Any]],
        ) -> tuple[int, int]:
            records: list[dict[str, Any]] = []
            seen_names: set[str] = set()

            for row in rows:
                if not isinstance(row, dict):
                    raise ValidationError(
                        "Temp cache records must be dictionaries."
                    )
                record = validate_record(row)
                name_key = record["name"].casefold()
                if name_key in seen_names:
                    continue
                seen_names.add(name_key)
                records.append(record)

            current_by_name = {
                record["name"].casefold(): int(record["id"])
                for record in self.database.search()
            }

            added = 0
            updated = 0

            for record in records:
                record_id = current_by_name.get(record["name"].casefold())
                if record_id is None:
                    self.database.add(record)
                    added += 1
                else:
                    self.database.update(record_id, record)
                    updated += 1

            return added, updated

        def action_refresh(self) -> None:
            self.refresh_rows()
            self.notify("Dataset refreshed.")

        def on_button_pressed(self, event: Button.Pressed) -> None:
            actions = {
                "new": self.action_new,
                "edit": self.action_edit,
                "delete": self.action_delete,
                "view": self.action_view,
                "search": self.action_search,
                "clear": self.action_clear_filter,
                "export": self.action_export,
                "save_temp": self.action_save_temp,
                "load_temp": self.action_load_temp,
                "refresh": self.action_refresh,
            }

            action = actions.get(event.button.id)
            if action is not None:
                action()


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Textual architecture dataset manager"
    )
    parser.add_argument(
        "--db",
        default="architectures.sqlite3",
        help="SQLite database path.",
    )
    parser.add_argument(
        "--export",
        metavar="CSV_PATH",
        help="Export the current dataset to CSV and exit.",
    )
    parser.add_argument(
        "--cache",
        metavar="CACHE_PATH",
        help="Temp cache file path. Defaults to <DB_PATH>.cache.json.",
    )

    args = parser.parse_args(argv)
    cache_path = args.cache or f"{args.db}.cache.json"
    if Path(cache_path).resolve() == Path(args.db).resolve():
        parser.error("--cache must not be the same file as --db")

    database = ArchitectureDB(args.db)

    if args.export:
        try:
            count = database.export_csv(args.export)
        finally:
            database.close()

        print(f"Exported {count} record(s) to {args.export}")
        return 0

    if not TEXTUAL_AVAILABLE:
        database.close()
        print(
            "Textual is not installed. Install it with:\n"
            "  python -m pip install textual",
            file=sys.stderr,
        )
        return 2

    try:
        DatasetApp(database, cache_path).run()
    except KeyboardInterrupt:
        database.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
