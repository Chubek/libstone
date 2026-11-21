# Cephyr QML scaffold orchestration — combine with bundle.py.
# All .qpass definitions are prepended in deterministic order.
# The sample begins with surface constructs and ends in normalized, checked MIR.
pass desugar;
pass absinterp;
pass symexec;
pass lower;
pass canonicalize;
pass optimize;
pass format;
pass prettyprint;
pass sema;

run pretty(format(assume(true,
    select(
        ge(add(20, 22), 42),
        add(max(20, 21), min(21, 21)),
        0
    )
)));
