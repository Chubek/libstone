#!/bin/sh
# Optional, explicitly trusted example hook: reply using the request ID.
IFS=' ' read -r tag version kind correlation payload || exit 1
[ "$tag" = '(libdsl-script' ] &&
[ "$version" = '1' ] &&
[ "$kind" = 'request' ] || exit 1
case "$correlation" in
  ''|*[!0-9]*) exit 1 ;;
esac
printf '(libdsl-script 1 ok %s "hello from a trusted hook")\n' "$correlation"
