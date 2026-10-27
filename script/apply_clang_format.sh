#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Format the kernel sources with clang-format (override with CLANG_FORMAT=...).

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
REPO_ROOT=$( cd -- "$SCRIPT_DIR/.." &> /dev/null && pwd )

CLANG_FORMAT=${CLANG_FORMAT:-clang-format-14}
TARGET_DIRS=(
  src/libcompat
  src/mango_mgmt
)

if ! command -v "$CLANG_FORMAT" > /dev/null 2>&1; then
  echo "$CLANG_FORMAT not found; install it or set CLANG_FORMAT"
  exit 1
fi

for TARGET in "${TARGET_DIRS[@]}"; do
  [[ -d "$REPO_ROOT/$TARGET" ]] || continue
  echo "Formatting $TARGET..."
  find "$REPO_ROOT/$TARGET" \( -name "*.c" -o -name "*.h" \) \
    -exec "$CLANG_FORMAT" -i {} \;
done
