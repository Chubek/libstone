# Boost 1.92.0 header subset

These unmodified headers were copied from `/usr/include/boost` in Arch Linux's
`boost` package (`1.92.0-1`, `BOOST_VERSION=109200`). The corresponding upstream
release is [Boost 1.92.0](https://github.com/boostorg/boost/tree/boost-1.92.0).
They are distributed under the [Boost Software License 1.0](LICENSE_1_0.txt),
copied from that release.

The snapshot starts at `boost/multiprecision/cpp_int.hpp` and recursively includes
its literal `boost/...` includes, including conditional branches. Boost.Config's
default compiler, standard-library, and platform selections are included too.
Weighstone does not set `BOOST_MP_STANDALONE` or change the `cpp_int` public type.
Other Multiprecision backends and general Boost libraries are not provided.

When refreshing this snapshot, take all headers from the same Boost release,
recompute the include closure, and verify that build-tree and installed
Weighstone consumers do not fall back to system Boost headers. If an application
also uses an external Boost installation, use the same Boost version to avoid
mixing definitions from different releases.
