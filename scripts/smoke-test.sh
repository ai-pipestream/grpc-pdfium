#!/usr/bin/env bash
# Boot-proofs a grpc-pdfium image: a green build is not "done" until the
# artifact starts under the flags it ships with. Hermetic (no documents, no
# network beyond the docker socket), so it runs in CI and before any push.
#
#   1. closure: every shared library the binary links resolves inside the
#      image. The runtime base carries no ldd and may carry no shell, so the
#      dynamic loader answers directly: LD_TRACE_LOADED_OBJECTS=1 makes it
#      print the closure and exit, which is all ldd does.
#   2. boot: the front reaches its own "listening on" line under the
#      hardened run flags (read-only rootfs, tmpfs /tmp for the worker
#      sockets, no capabilities), runs as uid 65532, and its worker pool is
#      spawned in full: the text workers and the render workers, each with
#      its role on its command line.
set -euo pipefail

usage() {
  echo "Usage: $0 IMAGE" >&2
  exit 64
}
[[ $# -eq 1 ]] || usage
image=$1
binary=/usr/local/bin/grpc_pdfium
workers=2
render_workers=1
container="grpc-pdfium-smoke-$$"

cleanup() {
  docker rm -f "$container" >/dev/null 2>&1 || true
}

# Polls the container log for a line until it appears or the deadline passes.
wait_for_log() {
  local pattern=$1 deadline=$2
  for _ in $(seq 1 "$deadline"); do
    if docker logs "$container" 2>&1 | grep -q "$pattern"; then
      return 0
    fi
    if [[ "$(docker inspect -f '{{.State.Running}}' "$container" 2>/dev/null)" != "true" ]]; then
      break
    fi
    sleep 1
  done
  echo "container did not log '$pattern'; logs:" >&2
  docker logs "$container" >&2 || true
  return 1
}

echo "== smoke: library closure of the shipped binary"
trace=$(docker run --rm --entrypoint "$binary" -e LD_TRACE_LOADED_OBJECTS=1 "$image" 2>&1)
echo "$trace"
if grep -q "not found" <<<"$trace"; then
  echo "unresolved shared libraries or symbol versions in $image" >&2
  exit 1
fi

echo "== smoke: boot to listening under the hardened run flags"
trap cleanup EXIT
docker run -d --name "$container" \
  --read-only --tmpfs /tmp --cap-drop ALL --security-opt no-new-privileges:true \
  -e GRPC_PDFIUM_WORKERS="$workers" \
  -e GRPC_PDFIUM_RENDER_WORKERS="$render_workers" "$image" >/dev/null
wait_for_log "grpc-pdfium listening on" 60

processes=$(docker top "$container" -o uid,pid,args | tail -n +2)
echo "$processes"
foreign_uid=$(awk '$1 != 65532' <<<"$processes" || true)
if [[ -n "$foreign_uid" ]]; then
  echo "a process is not running as uid 65532" >&2
  exit 1
fi
spawned=$(grep -c -- '--worker' <<<"$processes" || true)
text=$(grep -c -- '--role text' <<<"$processes" || true)
render=$(grep -c -- '--role render' <<<"$processes" || true)
if [[ "$spawned" -ne $((workers + render_workers)) || "$text" -ne "$workers" ||
      "$render" -ne "$render_workers" ]]; then
  echo "expected $workers text and $render_workers render workers," \
       "found $text text and $render render of $spawned" >&2
  exit 1
fi

echo "smoke-test: OK ($image)"
