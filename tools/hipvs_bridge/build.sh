#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
# GPU targets of the bridge's own kernels (comma separated). The fixed hipVS build ships
# gfx1100 code only (Radeon PRO W7900 / W7900D).
HIP_ARCHS=${SEEKDB_HIP_ARCHS:-gfx1100}
OUTPUT=${1:-$SCRIPT_DIR/libseekdb_cuvs_bridge.so}

[[ -x "$HIPCC" ]] || { echo "hipcc is not executable: $HIPCC" >&2; exit 2; }
[[ -f "$HIPVS_ROOT/include/cuvs/core/c_api.h" ]] || {
  echo "hipVS headers are missing under $HIPVS_ROOT/include" >&2
  exit 2
}
[[ -f "$HIPVS_ROOT/lib/libcuvs_c.so" ]] || {
  echo "libcuvs_c.so is missing under $HIPVS_ROOT/lib" >&2
  exit 2
}

mkdir -p "$(dirname "$OUTPUT")"
OBJ_DIR=$(mktemp -d "${TMPDIR:-/tmp}/seekdb_cuvs_bridge.XXXXXX")
trap 'rm -rf "$OBJ_DIR"' EXIT

ARCH_FLAGS=()
for arch in ${HIP_ARCHS//,/ }; do
  ARCH_FLAGS+=("--offload-arch=$arch")
done

# GPU CAGRA graph construction (HIP kernels); its symbols stay internal to the bridge.
"$HIPCC" \
  -c \
  -fPIC \
  -O3 \
  -std=c++17 \
  -fvisibility=hidden \
  -x hip \
  "${ARCH_FLAGS[@]}" \
  -D__HIP_PLATFORM_AMD__ \
  -I"$HIPVS_ROOT/include" \
  -I"$ROCM_ROOT/include" \
  "$SCRIPT_DIR/seekdb_cuvs_graph.hip" \
  -o "$OBJ_DIR/seekdb_cuvs_graph.o"

# C API of the bridge (compiled on its own: hipcc applies "-x c" to every later input).
"$HIPCC" \
  -c \
  -fPIC \
  -O2 \
  -D__HIP_PLATFORM_AMD__ \
  -I"$HIPVS_ROOT/include" \
  -I"$ROCM_ROOT/include" \
  "$SCRIPT_DIR/seekdb_cuvs_bridge.c" \
  -o "$OBJ_DIR/seekdb_cuvs_bridge.o"

"$HIPCC" \
  -shared \
  -fPIC \
  "$OBJ_DIR/seekdb_cuvs_bridge.o" \
  "$OBJ_DIR/seekdb_cuvs_graph.o" \
  -L"$HIPVS_ROOT/lib" \
  -L"$ROCM_ROOT/lib" \
  -Wl,-soname,libseekdb_cuvs_bridge.so \
  -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" \
  -lcuvs_c \
  -lamdhip64 \
  -lpthread \
  -ldl \
  -o "$OUTPUT"

echo "built $OUTPUT"
