#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
cd "${repo_root}"

configuration="${IOS_CONFIGURATION:-Release}"
deployment_target="${IOS_DEPLOYMENT_TARGET:-15.0}"
bundle_id="${IOS_BUNDLE_ID:-io.github.aw514844.unleashedrecomp}"
artifact_root="${repo_root}/out/artifacts/ios"

case "${configuration}" in
  Debug) preset_suffix="debug" ;;
  Release) preset_suffix="release" ;;
  RelWithDebInfo) preset_suffix="relwithdebinfo" ;;
  *)
    echo "Unsupported IOS_CONFIGURATION: ${configuration}" >&2
    exit 1
    ;;
esac

preset="ios-xcode-${preset_suffix}"
build_dir="${repo_root}/out/build/${preset}"
archive_path="${artifact_root}/UnleashedRecomp.xcarchive"
app_zip="${artifact_root}/UnleashedRecomp-${configuration}.app.zip"
log_path="${artifact_root}/xcodebuild-${configuration}.log"

mkdir -p "${artifact_root}"

required_tools=(cmake xcodebuild xcrun)
for tool in "${required_tools[@]}"; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "Missing required tool: ${tool}" >&2
    exit 1
  fi
done

spirv_count="$(find "${repo_root}/UnleashedRecomp/gpu/shader/hlsl" -name '*.spirv.h' | wc -l | tr -d ' ')"
if [[ "${spirv_count}" -lt 23 ]]; then
  echo "Expected at least 23 generated .spirv.h files, found ${spirv_count}" >&2
  exit 1
fi

rm -rf "${archive_path}"

cmake . --preset "${preset}" \
  -DUNLEASHED_RECOMP_IOS_ENABLE_CODE_SIGNING=OFF \
  -DUNLEASHED_RECOMP_IOS_BUNDLE_ID="${bundle_id}" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${deployment_target}"

xcodebuild \
  -project "${build_dir}/UnleashedRecomp.xcodeproj" \
  -scheme "UnleashedRecomp" \
  -configuration "${configuration}" \
  -sdk iphoneos \
  -destination "generic/platform=iOS" \
  -archivePath "${archive_path}" \
  archive | tee "${log_path}"

app_path="$(find "${archive_path}/Products/Applications" -maxdepth 1 -name '*.app' -print -quit)"
if [[ -z "${app_path}" ]]; then
  echo "Archive completed but no .app bundle was found in ${archive_path}" >&2
  exit 1
fi

ditto -c -k --sequesterRsrc --keepParent "${app_path}" "${app_zip}"

echo "Archive: ${archive_path}"
echo "App zip: ${app_zip}"
echo "Log: ${log_path}"
