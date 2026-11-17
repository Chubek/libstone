#!/usr/bin/env sh
# Full local verification for Moonstone QMR, mirroring the acceptance gates
# in moonstone/qmr/AGENTS.md:
#   - standalone build + full ctest, GCC and Clang, Debug and Release
#   - compiler/lexer tests rebuilt with AddressSanitizer/UndefinedBehaviorSanitizer
#   - opt-in root integration under moonstone (MOONSTONE_BUILD_QMR=ON)
#
# Usage: sh moonstone/qmr/scripts/verify.sh [workspace-root]
# The script creates build-qmr-* directories in the workspace root and
# overwrites them on re-run. It makes no other changes.

set -eu
root="${1:-$(cd "$(dirname "$0")/../../.." && pwd)}"
cd "$root"

failures=0
run_matrix() {
  compiler="$1"; shift
  for build_type in Debug Release; do
    dir="build-qmr-verify-${compiler}-${build_type}"
    echo "==> QMR $compiler $build_type"
    env "$@" cmake -S moonstone/qmr -B "$dir" -DCMAKE_BUILD_TYPE="$build_type" \
      -DQMR_BUILD_TESTS=ON -DQMR_BUILD_TOOLS=ON >/dev/null
    cmake --build "$dir" --parallel >/dev/null
    if ctest --test-dir "$dir" --output-on-failure >/tmp/qmr-verify-$compiler-$build_type.log 2>&1; then
      echo "    passed"
    else
      echo "    FAILED (see /tmp/qmr-verify-$compiler-$build_type.log)"
      failures=$((failures + 1))
    fi
  done
}

echo "==> QMR sanitized (GCC, Debug, ASan/UBSan)"
cmake -S moonstone/qmr -B build-qmr-verify-sanitized -DCMAKE_BUILD_TYPE=Debug \
  -DQMR_BUILD_TESTS=ON -DQMR_BUILD_TOOLS=ON -DQMR_SANITIZE=ON >/dev/null
cmake --build build-qmr-verify-sanitized --parallel >/dev/null
if ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-qmr-verify-sanitized \
     --output-on-failure >/tmp/qmr-verify-sanitized.log 2>&1; then
  echo "    passed"
else
  echo "    FAILED (see /tmp/qmr-verify-sanitized.log)"
  failures=$((failures + 1))
fi

run_matrix gcc CC=gcc CXX=g++
if command -v clang >/dev/null 2>&1; then
  run_matrix clang CC=clang CXX=clang++
else
  echo "==> clang not found; skipping (gate requires GCC and Clang)"
fi

echo "==> QMR root integration (moonstone, MOONSTONE_BUILD_QMR=ON)"
if [ ! -f build-moonstone/CMakeCache.txt ]; then
  cmake -S moonstone -B build-moonstone -DMOONSTONE_BUILD_QMR=ON \
    -DMOONSTONE_BUILD_TESTS=ON >/dev/null
fi
cmake --build build-moonstone --target qmr qmr_compiler_tests qmr_runtime_tests \
  qmr_abi_tests qmr_lexer_fuzz --parallel >/dev/null
if ctest --test-dir build-moonstone -R '\.qmr_' --output-on-failure \
     >/tmp/qmr-verify-root.log 2>&1; then
  echo "    passed"
else
  echo "    FAILED (see /tmp/qmr-verify-root.log)"
  failures=$((failures + 1))
fi

if [ "$failures" -ne 0 ]; then
  echo "QMR verification: $failures stage(s) failed"
  exit 1
fi
echo "QMR verification: all stages passed"
