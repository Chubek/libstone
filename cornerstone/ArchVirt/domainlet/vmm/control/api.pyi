from dataclasses import dataclass

@dataclass
class ControlApi:
    path: str
    def serve(self, handler) -> None: ...
