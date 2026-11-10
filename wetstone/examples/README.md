# Example programs

With Wetstone in `libstone/wetstone`:

```sh
c++ -std=c++20 -Iwetstone wetstone/examples/interval.cpp -o /tmp/ws-interval
c++ -std=c++20 -Iwetstone wetstone/examples/pointer.cpp -o /tmp/ws-pointer
c++ -std=c++20 -Iwetstone wetstone/examples/smt.cpp -o /tmp/ws-smt
```

Add `-Ithird_party/satie/include` to enable the Satie-backed checks. The SMT-LIB2 example works even when that vendor is absent. The numerical example uses representable signed-integer arithmetic; for Flintstone's modular arithmetic, prefer a bitvector analysis.
