#!/bin/sh
# plugin-import-test.sh "<cppscheme2 exe>"
#
# Regression guard for the native .dll-via-import path (FRAME_ENSURE_LOADED): the
# 11.9k-case battery only exercises pure .sld libraries, so this is the one test
# that the loader actually loads a colocated <lib>.dll and runs its primitives.
#
# It builds a throwaway library  demo/thing.{dll,sld}  -- thing.dll is a copy of
# the example_plugin.dll that sits beside the exe (the `example_plugin` CMake
# target) -- then imports (demo thing) and asserts the plugin's native-answer
# primitive returns 42.  cpp-only and build-artifact-dependent, so it lives here
# (with gc_test) rather than in the cross-port / in-interp suites; it is wired
# into scheme-tests/test-suites.scm as the cpp-only `plugin-import` suite.
#
# Needs a POSIX shell (git-bash): mktemp / cygpath / cp.
set -u

EXE="${1:?usage: plugin-import-test.sh <cppscheme2 exe>}"
DIR="$(dirname "$EXE")"
PLUGIN="$DIR/example_plugin.dll"

if [ ! -f "$PLUGIN" ]; then
  echo "FAIL: example_plugin.dll not found at $PLUGIN"
  echo "      build it:  cmake --build build --config Release --target example_plugin"
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/demo"
cp "$PLUGIN" "$TMP/demo/thing.dll"
# The native primitives are registered into the global env by thing.dll's
# cppscheme2_plugin_init on import; the .sld is just the library shell.
printf '(define-library (demo thing) (import (scheme base)) (begin))\n' \
  > "$TMP/demo/thing.sld"

# cppscheme2 resolves -L paths in native (Windows) form.
WINTMP="$(cygpath -m "$TMP" 2>/dev/null || printf '%s' "$TMP")"
OUT="$("$EXE" -L "$WINTMP" -e '(begin (import (demo thing)) (native-answer))' 2>&1)"

case "$OUT" in
  *"==> 42"*)
    echo "PASS: (import (demo thing)) loaded thing.dll; (native-answer) => 42"
    exit 0 ;;
  *)
    echo "FAIL: expected '==> 42' from the native plugin, got:"
    echo "$OUT"
    exit 1 ;;
esac
