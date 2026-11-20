from dataclasses import dataclass

@dataclass(frozen=True, slots=True)
class IoeventFdRegistration:
    addr: int
    length: int
    fd: int
    datamatch: int = ...
    pio: bool = ...

def register_ioeventfd(vm_fd: int, registration: IoeventFdRegistration) -> None: ...
