#!/usr/bin/env bash
#
# Bump the pinned third_party/fujinet-firmware commit to a newer upstream ref
# (default: origin's master) and update FUJINET_COMMIT in
# cmake/Dependencies.cmake to match.
#
# This only moves the pin -- it does not commit anything, and it does not
# rebuild. build-fujinet-desktop.sh's patch() calls are anchored to exact
# upstream text, so before committing a bump:
#   1. Rebuild: cmake --build build --target fujinet-runtime
#      (or FN_REFRESH=1 tools/fujinet/build-fujinet-desktop.sh for a from-
#      scratch re-stage). If the bump moved text a patch anchors to, this is
#      where it fails -- loudly, at build time, rather than silently.
#   2. Run ctest.
#
# Usage: tools/fujinet/update-fujinet-firmware.sh [ref]
#   ref   a branch, tag or commit on FUJINET_URL's remote (default: master)

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
ROOT_DIR=$(cd -- "${SCRIPT_DIR}/../.." &>/dev/null && pwd)
SUBMODULE_PATH="third_party/fujinet-firmware"
SUBMODULE_DIR="${ROOT_DIR}/${SUBMODULE_PATH}"
DEPS_CMAKE="${ROOT_DIR}/cmake/Dependencies.cmake"
REF="${1:-master}"

cd "${ROOT_DIR}"

if [[ ! -e "${SUBMODULE_DIR}/.git" ]]; then
    echo "Initialising ${SUBMODULE_PATH}..."
    git submodule update --init "${SUBMODULE_PATH}"
fi

if [[ -n "$(git -C "${SUBMODULE_DIR}" status --porcelain)" ]]; then
    echo "error: ${SUBMODULE_PATH} has uncommitted changes;" \
         "refusing to move its pin out from under them." >&2
    exit 1
fi

FUJINET_URL=$(sed -n 's/^set(FUJINET_URL "\(.*\)")/\1/p' "${DEPS_CMAKE}")
OLD_COMMIT=$(sed -n 's/^set(FUJINET_COMMIT "\(.*\)")/\1/p' "${DEPS_CMAKE}")
if [[ -z "${FUJINET_URL}" || -z "${OLD_COMMIT}" ]]; then
    echo "error: could not find FUJINET_URL/FUJINET_COMMIT in ${DEPS_CMAKE}" >&2
    exit 1
fi

echo "Fetching ${REF} from ${FUJINET_URL}..."
git -C "${SUBMODULE_DIR}" fetch --quiet origin "${REF}"
NEW_COMMIT=$(git -C "${SUBMODULE_DIR}" rev-parse FETCH_HEAD)

if [[ "${NEW_COMMIT}" == "${OLD_COMMIT}" ]]; then
    echo "Already pinned to ${REF} (${OLD_COMMIT}); nothing to do."
    exit 0
fi

echo
echo "Commits between the current pin and ${REF}:"
git -C "${SUBMODULE_DIR}" log --oneline "${OLD_COMMIT}..${NEW_COMMIT}"
echo

git -C "${SUBMODULE_DIR}" -c advice.detachedHead=false checkout --quiet \
    "${NEW_COMMIT}"

sed -i.bak \
    "s/^set(FUJINET_COMMIT \"${OLD_COMMIT}\")/set(FUJINET_COMMIT \"${NEW_COMMIT}\")/" \
    "${DEPS_CMAKE}"
rm -f "${DEPS_CMAKE}.bak"

git add "${SUBMODULE_PATH}" "${DEPS_CMAKE}"

cat <<EOF
Pinned ${SUBMODULE_PATH} to ${NEW_COMMIT} (was ${OLD_COMMIT}) and staged it
with cmake/Dependencies.cmake. Nothing has been committed.

Before committing:
  1. Rebuild: cmake --build build --target fujinet-runtime
     (or FN_REFRESH=1 tools/fujinet/build-fujinet-desktop.sh to force a
     from-scratch re-stage). A patch anchor broken by this bump fails the
     build here, loudly, instead of misbehaving silently at runtime.
  2. Run ctest.
EOF
