#!/bin/bash
# clang-format check and format script
# Usage:
#   ./format.sh --check    Check if all .c/.h files are properly formatted
#   ./format.sh --format   Auto-format all .c/.h files in place

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

CLANG_FORMAT="clang-format-14"
if ! command -v "$CLANG_FORMAT" &>/dev/null; then
  CLANG_FORMAT="clang-format"
fi

cd "$REPO_ROOT"

files=$(find . -name "*.c" -o -name "*.h" | sort)

if [ -z "$files" ]; then
  echo "No .c or .h files found."
  exit 0
fi

case "${1:-}" in
  --check)
    echo "Checking formatting with $CLANG_FORMAT..."
    fail=0
    for f in $files; do
      if ! "$CLANG_FORMAT" -style=file --dry-run --Werror "$f"; then
        echo "  FAIL: $f"
        fail=1
      fi
    done

    if [ $fail -ne 0 ]; then
      echo ""
      echo "Some files are not formatted correctly."
      echo "Run './tools/format.sh --format' to fix."
      exit 1
    fi
    echo "All files are properly formatted."
    ;;

  --format)
    echo "Formatting all .c/.h files with $CLANG_FORMAT..."

    # Run twice: rule AlignConsecutiveMacros need a second
    # pass to converge.
    echo "$files" | xargs "$CLANG_FORMAT" -style=file -i
    echo "$files" | xargs "$CLANG_FORMAT" -style=file -i

    echo "Done."
    ;;

  *)
    echo "Usage: $0 [--check | --format]"
    echo "  --check    Check if files are properly formatted (exit 1 if not)"
    echo "  --format   Auto-format all .c/.h files in place"
    exit 1
    ;;
esac
