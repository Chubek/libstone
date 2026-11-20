"""Source locations and errors shared by Archon's frontend and runtime."""

from dataclasses import dataclass


@dataclass(frozen=True)
class Location:
    filename: str
    line: int
    column: int

    def __str__(self) -> str:
        return f"{self.filename}:{self.line}:{self.column}"


class ArchonError(Exception):
    """Invalid architecture input or use of the compiled target."""

    def __init__(self, message: str, location: Location | None = None):
        self.message = message
        self.location = location
        super().__init__(message)

    def __str__(self) -> str:
        if self.location is None:
            return self.message
        return f"{self.location}: {self.message}"
