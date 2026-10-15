module gateml.AbstractMachine;
import gateml.Bytecode;

/// Machine implementations must verify programs and enforce resource limits.
interface AbstractMachine {
    long run(const Program program);
    void reset();
}
/// Arguments are borrowed for the duration of the callback; copy them to retain
/// them. VM callbacks must not re-enter or reset their current VM instance.
alias Facility = long delegate(const(long)[] arguments);
