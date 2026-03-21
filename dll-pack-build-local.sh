set -euo pipefail

gh_tag="${DLL_PACK_GH_TAG:-${GITHUB_REF_NAME:-${GITHUB_REF:-}}}"
gh_tag="${gh_tag#refs/tags/}"
if [ -z "${gh_tag}" ]; then
    gh_tag="${GITHUB_SHA:-local}"
fi

sh ./build.sh

mkdir -p ./artifacts/

LD_LIBRARY_PATH=$(pwd)/build/build/_deps/llvm_project-src/llvm/lib/ \
    dll-pack-builder local foro-clang-format \
    $(dll-pack-builder find ${BUILD_OUT_DIR}) \
    ./artifacts/ ${DLL_PACK_TARGET} ${GITHUB_REPOSITORY} ${gh_tag} \
    --include "$(pwd)/build/**" \
    --macho-rpath $(pwd)/build/build/_deps/llvm_project-src/llvm/lib/
