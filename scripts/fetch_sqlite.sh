#!/usr/bin/env bash
set -euo pipefail

# Fetch the SQLite amalgamation (sqlite3.c/.h) into third_party/sqlite/
# Usage: scripts/fetch_sqlite.sh <YEAR> <AMALGAMATION_VERSION> [--force]
# Example (as of Nov 2025):
#   scripts/fetch_sqlite.sh 2025 3500400
# This downloads:
#   https://sqlite.org/<YEAR>/sqlite-amalgamation-<AMALGAMATION_VERSION>.zip
# and places sqlite3.c and sqlite3.h at third_party/sqlite/.

# Defaults can be provided via env or arguments
DEFAULT_YEAR="${SQLITE_YEAR:-2025}"
DEFAULT_VER="${SQLITE_VER:-3500400}"

YEAR=${1:-${DEFAULT_YEAR}}
VER=${2:-${DEFAULT_VER}}
FORCE=${3:-}

if [[ -z "${YEAR}" || -z "${VER}" ]]; then
  echo "Usage: $0 [YEAR] [AMALGAMATION_VERSION] [--force]" >&2
  echo "Example: $0 2025 3500400" >&2
  exit 2
fi

dest_dir="third_party/sqlite"
mkdir -p "${dest_dir}"

zip_name="sqlite-amalgamation-${VER}.zip"
url="https://sqlite.org/${YEAR}/${zip_name}"
tmp_zip="${TMPDIR:-/tmp}/${zip_name}"

if [[ -f "${dest_dir}/sqlite3.c" && -f "${dest_dir}/sqlite3.h" && "${FORCE:-}" != "--force" ]]; then
  echo "sqlite3.c and sqlite3.h already present in ${dest_dir} (use --force to overwrite)" >&2
  exit 0
fi

echo "Downloading ${url}" >&2
if command -v curl >/dev/null 2>&1; then
  curl -fL "${url}" -o "${tmp_zip}"
elif command -v wget >/dev/null 2>&1; then
  wget -O "${tmp_zip}" "${url}"
else
  echo "Error: curl or wget is required to download ${url}" >&2
  exit 1
fi

work_dir="${TMPDIR:-/tmp}/sqlite-amalgamation-${VER}$$"
rm -rf "${work_dir}" && mkdir -p "${work_dir}"

echo "Extracting ${zip_name}" >&2
if command -v unzip >/dev/null 2>&1; then
  unzip -q "${tmp_zip}" -d "${work_dir}"
else
  echo "Error: unzip is required to extract ${zip_name}. Install with: sudo apt install -y unzip" >&2
  exit 1
fi

src_dir=$(find "${work_dir}" -maxdepth 1 -type d -name "sqlite-amalgamation-*" | head -n 1)
if [[ -z "${src_dir}" ]]; then
  echo "Error: could not locate extracted sqlite-amalgamation directory" >&2
  exit 1
fi

cp -f "${src_dir}/sqlite3.c" "${dest_dir}/sqlite3.c"
cp -f "${src_dir}/sqlite3.h" "${dest_dir}/sqlite3.h"

echo "Wrote: ${dest_dir}/sqlite3.c" >&2
echo "Wrote: ${dest_dir}/sqlite3.h" >&2
echo "Done." >&2
