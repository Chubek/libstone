import ctypes as c
import sys

lib = c.CDLL(sys.argv[1])
class Config(c.Structure):
    _fields_ = [("struct_size", c.c_size_t)] + [
        (name, c.c_char_p) for name in ("machine", "backend", "archml_path", "kernel_path",
                                      "image_path", "firmware_path", "cmdline")
    ] + [("memory_mb", c.c_uint64), ("instruction_limit", c.c_uint64), ("flags", c.c_uint32)]

Serial = c.CFUNCTYPE(c.c_uint8, c.c_void_p, c.c_int, c.c_uint8)
lib.archvirt_create.restype = c.c_void_p
lib.archvirt_initialize.argtypes = [c.c_void_p, c.POINTER(Config)]
lib.archvirt_set_serial.argtypes = [c.c_void_p, Serial, c.c_void_p]
lib.archvirt_last_error.argtypes = [c.c_void_p]
lib.archvirt_last_error.restype = c.c_char_p
for name in ("archvirt_start", "archvirt_wait", "archvirt_stop", "archvirt_destroy"):
    getattr(lib, name).argtypes = [c.c_void_p]
for machine in (b"x86_64", b"aarch64", b"riscv64"):
    vm = lib.archvirt_create()
    assert vm
    output = bytearray()
    commands = iter(b"uname\nexit\n")
    @Serial
    def serial(user, write, value):
        if write:
            output.append(value)
            return 0
        return next(commands, 0)
    try:
        assert lib.archvirt_set_serial(vm, serial, None) == 0
        cfg = Config()
        cfg.struct_size = c.sizeof(cfg)
        cfg.machine, cfg.backend = machine, b"software"
        cfg.memory_mb, cfg.instruction_limit, cfg.flags = 40, 3000000, 3
        assert lib.archvirt_initialize(vm, c.byref(cfg)) == 0, lib.archvirt_last_error(vm)
        assert lib.archvirt_start(vm) == 0, lib.archvirt_last_error(vm)
        assert lib.archvirt_wait(vm) == 0, lib.archvirt_last_error(vm)
        assert b"Domix " + machine + b"\n" in output, output
        assert b"Domix shutdown" in output, output
    finally:
        lib.archvirt_destroy(vm)
print("C API boots all three machines and reports execution results")
