# Cephyr implementation boundaries

1. ucpp preprocesses each translation unit independently. Cephyr retains its
   line map when passing the preprocessed source to cparser-iso11; parser
   diagnostics and frontend errors go through Syren.
2. Cephyr lowers the parsed AST with target C types. Wetstone supplies the
   integer constant domain and CFG reachability. Unreachable code still gets
   checked; unreachable blocks are omitted from native MachineIR. C data
   pointers, code pointers, and integers remain distinct in that interface.
3. Archon generates native backend packages at build time. The driver loads
   the matching packaged module; its verified runtime lowers MachineIR to target assembly.
   A host or cross C compiler does not compile the user's C translation unit.
4. Cephyr drives tools with argv, not a shell. The target assembler produces
   objects; the linker receives those objects in CLI order. AMD64, AArch64, and RV64IM
   Linux static links can include Cephyr's own startup and libc. The compiler for
   that packaged runtime is independently configurable.
5. Except when writing to stdout, a stage publishes an artifact only after
   it succeeds. Configurations, target definitions and tool binaries are
   trusted build inputs. Unsupported syntax, types, ABI choices or runtime
   combinations must be rejected rather than delegated to a different
   compiler or translated approximately.

Regression coverage includes independent frontend and driver stages, static
links on all three targets, AMD64 and AArch64 interoperability with independent C code,
and malformed input and configuration. Running Linux binaries requires a
suitable host or emulator.
