# Source and validation provenance

The source tree included in this archive is an overlay-only output, not a Git
commit and not a full LibStone checkout. It was produced by extracting the
Moonstone archive from the preceding session, preserving those files, and
adding/reworking Plaid and CMake integration in the current session.

The linked GitHub connector was used to verify `Chubek/libstone` on its
`master` branch and inspect the existing Firestone CPS, concurrency, compute,
vector and related subsystem paths. At inspection time, LibStone did not yet
contain a tracked `moonstone/` directory. The host environment could not run
`git clone` directly because external DNS resolution was unavailable. Thus a
full LibStone root build and vendored dependency link test were not run.

Verification performed on the contained standalone Moonstone build: GCC
C++20 build, Clang C++20 build, 16/16 CTest tests on both, GCC AddressSanitizer
and UndefinedBehaviorSanitizer build/test, and a syntax test of installed
headers using a downstream C++20 source file. Full TSan stress and platform
portability are pending.
