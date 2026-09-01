#!/usr/bin/env bash
# Build the browser bundle locally and publish it to the R2 bucket behind
# https://client.ots.ovh - the local equivalent of
# .github/workflows/deploy-webclient.yml (same bucket layout, same pointer).
#
#   tools/deploy-webclient.sh                  # build + upload
#   tools/deploy-webclient.sh --promote        # build + upload + go live
#   tools/deploy-webclient.sh --skip-build     # upload the existing build-emscripten-web/
#   tools/deploy-webclient.sh --promote-only <build-id>   # just flip the pointer (rollback too)
#
# Credentials: an rclone remote named "r2" (override with --remote or
# RCLONE_REMOTE) pointing at the Cloudflare R2 account; if none exists, the
# script falls back to R2_S3_ACCESS_KEY / R2_S3_SECRET_KEY env vars (the same
# secrets the workflow uses).
set -euo pipefail

R2_ENDPOINT="https://7790c004a881eb63eaf2691bd90d0cf0.eu.r2.cloudflarestorage.com"
BUCKET_PREFIX="ots/webclient"
OUTPUT_DIR="build-emscripten-web"

REMOTE="${RCLONE_REMOTE:-r2}"
PROMOTE=0
SKIP_BUILD=0
PROMOTE_ONLY=""
BUILD_ID=""

usage() {
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --promote) PROMOTE=1 ;;
        --skip-build) SKIP_BUILD=1 ;;
        --promote-only) PROMOTE_ONLY="${2:?--promote-only needs a build id}"; shift ;;
        --build-id) BUILD_ID="${2:?--build-id needs a value}"; shift ;;
        --remote) REMOTE="${2:?--remote needs a name}"; shift ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1" >&2; usage 1 ;;
    esac
    shift
done

cd "$(dirname "$0")/.."

command -v rclone >/dev/null || { echo "error: rclone is not installed" >&2; exit 1; }

# Pick the rclone remote: an existing named remote, or an ad-hoc one from the
# workflow's env vars
if ! rclone listremotes 2>/dev/null | grep -qx "${REMOTE}:"; then
    if [[ -n "${R2_S3_ACCESS_KEY:-}" && -n "${R2_S3_SECRET_KEY:-}" ]]; then
        export RCLONE_CONFIG_OTSWEBR2_TYPE=s3
        export RCLONE_CONFIG_OTSWEBR2_PROVIDER=Cloudflare
        export RCLONE_CONFIG_OTSWEBR2_ACCESS_KEY_ID="${R2_S3_ACCESS_KEY}"
        export RCLONE_CONFIG_OTSWEBR2_SECRET_ACCESS_KEY="${R2_S3_SECRET_KEY}"
        export RCLONE_CONFIG_OTSWEBR2_REGION=auto
        export RCLONE_CONFIG_OTSWEBR2_ENDPOINT="${R2_ENDPOINT}"
        REMOTE=otswebr2
    else
        cat >&2 <<EOF
error: no rclone remote "${REMOTE}:" and no R2_S3_ACCESS_KEY/R2_S3_SECRET_KEY set.
Either configure one once:
    rclone config create r2 s3 provider=Cloudflare region=auto \\
        endpoint=${R2_ENDPOINT} \\
        access_key_id=... secret_access_key=...
or export R2_S3_ACCESS_KEY and R2_S3_SECRET_KEY.
EOF
        exit 1
    fi
fi

promote() {
    local build="$1"
    # refuse to point the live site at a build that was never uploaded
    rclone lsf "${REMOTE}:${BUCKET_PREFIX}/builds/${build}/otclient.html" | grep -q otclient.html \
        || { echo "error: build '${build}' has no otclient.html in the bucket" >&2; exit 1; }
    printf '{"build":"%s"}' "${build}" | rclone rcat "${REMOTE}:${BUCKET_PREFIX}/current.json"
    echo "client.ots.ovh now serves build ${build} (the worker picks it up within ~60s)"
}

if [[ -n "${PROMOTE_ONLY}" ]]; then
    promote "${PROMOTE_ONLY}"
    exit 0
fi

if [[ ${SKIP_BUILD} -eq 0 ]]; then
    bash Dockerfile.browser.sh
fi

for f in otclient.html otclient.js otclient.wasm otclient.data; do
    [[ -f "${OUTPUT_DIR}/${f}" ]] || { echo "error: ${OUTPUT_DIR}/${f} is missing - build first" >&2; exit 1; }
done

if [[ -z "${BUILD_ID}" ]]; then
    BUILD_ID="$(git rev-parse HEAD)"
    # a dirty tree gets a unique id: /v/{build}/ is cached as immutable, so a
    # build id must never be re-uploaded with different content
    if ! git diff-index --quiet HEAD -- 2>/dev/null; then
        BUILD_ID="${BUILD_ID}.dirty.$(date -u +%Y%m%d%H%M%S)"
        echo "note: working tree is dirty, using build id ${BUILD_ID}"
    fi
fi

echo "uploading ${OUTPUT_DIR}/ -> ${REMOTE}:${BUCKET_PREFIX}/builds/${BUILD_ID}/"
rclone copy --exclude 'docker-browser-build.log' "${OUTPUT_DIR}/" "${REMOTE}:${BUCKET_PREFIX}/builds/${BUILD_ID}/"
echo "uploaded build ${BUILD_ID}"

if [[ ${PROMOTE} -eq 1 ]]; then
    promote "${BUILD_ID}"
else
    echo "not live yet - promote with: tools/deploy-webclient.sh --promote-only ${BUILD_ID}"
fi
