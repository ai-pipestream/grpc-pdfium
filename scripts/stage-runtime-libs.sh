#!/usr/bin/env bash
# Stages the shared-library closure of one or more binaries into a directory
# so a glibc-only runtime image can load them from LD_LIBRARY_PATH alone: no
# package manager, no ldconfig, nothing installed in the runtime stage.
#
#   stage-runtime-libs.sh STAGE_DIR BINARY...
#
# Runs in the build stage. Every library ldd resolves for the binaries (the
# transitive closure, libraries already placed in STAGE_DIR included) is
# copied into STAGE_DIR, except glibc's own: the loader and its libraries are
# inseparable, so the runtime base owns them. LD_LIBRARY_PATH set by the
# caller is honoured for the first pass, so a vendored library tree under a
# prefix such as /opt/poppler/lib is found without being installed.
#
# The second pass is the gate: with STAGE_DIR alone on the library path every
# non-glibc library must resolve from STAGE_DIR, or the build fails here
# instead of at the first docker run against a base that cannot run ldd.
set -euo pipefail

usage() {
  echo "Usage: $0 STAGE_DIR BINARY..." >&2
  exit 64
}
[[ $# -ge 2 ]] || usage
stage=$1
shift

# glibc's own libraries, by file name; the runtime base provides these.
glibc_owned='/(ld-linux[^/]*|libc|libm|libmvec|libdl|libpthread|librt|libresolv|libnsl|libutil|libanl|libBrokenLocale)\.so'

# Prints the resolved path of every non-glibc library in the closure of the
# arguments, one per line; fails on an unresolved one.
resolved_libraries() {
  local out
  out=$(ldd "$@")
  if grep -q "not found" <<<"$out"; then
    echo "unresolved shared libraries:" >&2
    grep "not found" <<<"$out" >&2
    return 1
  fi
  awk '/=> \// {print $3}' <<<"$out" | sort -u | grep -v -E "$glibc_owned" || true
}

mkdir -p "$stage"
echo "== staging the library closure of: $*"
LD_LIBRARY_PATH="$stage${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" resolved_libraries "$@" \
  | while read -r lib; do
      case "$lib" in
        "$stage"/*) ;;
        *) cp -L "$lib" "$stage/" ;;
      esac
    done

echo "== gate: the closure resolves from $stage alone"
stray=$(LD_LIBRARY_PATH="$stage" resolved_libraries "$@" | grep -v "^$stage/" || true)
if [[ -n "$stray" ]]; then
  echo "libraries still resolved outside $stage:" >&2
  echo "$stray" >&2
  exit 1
fi
ls -1 "$stage"
