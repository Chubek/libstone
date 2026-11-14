#!/bin/sh
# From repository root (configure and build first):
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build"}
"$BUILD/exotract" inspect --name Fixture --header "$ROOT/tests/fixture.h"
"$BUILD/exotract" emit-source --name Fixture --header "$ROOT/tests/fixture.h" \
  --impl-define EXOTRACT_IMPL_FIXTURE --out "$BUILD/fixture-impl.c"
cc -std=c11 -Wall -Wextra -Werror -c "$BUILD/fixture-impl.c" -o "$BUILD/fixture-impl.o"
# If LibStone ExoML is built with discovery, a full declaration inventory:
if [ -n "${EXOML_BIND:-}" ]; then
  "$BUILD/exotract" inspect --name Fixture --header "$ROOT/tests/fixture.h" \
    --exoml-bind "$EXOML_BIND" --resource-dir "$(clang -print-resource-dir)"
fi
