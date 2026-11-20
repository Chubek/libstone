"""Unix-domain control API."""

from __future__ import annotations

import json
import socket
from dataclasses import dataclass


@dataclass(slots=True)
class ControlApi:
    """A minimal line-oriented JSON control socket."""

    path: str

    def serve(self, handler) -> None:
        """Serve requests until the socket closes."""

        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind(self.path)
            server.listen(1)
            conn, _ = server.accept()
            with conn:
                for raw in conn.makefile("r"):
                    request = json.loads(raw)
                    response = handler(request)
                    conn.sendall((json.dumps(response) + "\n").encode("utf-8"))
