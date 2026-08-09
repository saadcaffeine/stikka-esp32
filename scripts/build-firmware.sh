#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
ESP32_DIR="$REPO_ROOT"
FRONTEND_REPO="$REPO_ROOT/../stikka-mqtt"
OUT_ROOT="$FRONTEND_REPO/frontend/public/firmware"

# PlatformIO installs tool Python packages into an internal --target path.
# If PIP_USER=1 leaks into the environment (common in Nix shells), pip fails
# with: "Can not combine '--user' and '--target'".
unset PIP_USER
unset PIP_TARGET
unset PYTHONUSERBASE
export PIP_CONFIG_FILE=/dev/null

if ! command -v pio >/dev/null 2>&1; then
  echo "error: platformio (pio) is not available in PATH"
  echo "hint: enter nix dev shell first: nix develop"
  exit 1
fi

if [[ ! -f "$ESP32_DIR/platformio.ini" ]]; then
  echo "error: missing $ESP32_DIR/platformio.ini"
  exit 1
fi

if [[ ! -d "$FRONTEND_REPO" ]]; then
  echo "error: expected sibling repo at $FRONTEND_REPO (clone stikka-mqtt next to this repo)"
  exit 1
fi

ENVS=()
while IFS= read -r env_line; do
  ENVS+=("$env_line")
done < <(grep -E '^\[env:[^]]+\]' "$ESP32_DIR/platformio.ini" | sed -E 's/^\[env:([^]]+)\]$/\1/')

if [[ ${#ENVS[@]} -eq 0 ]]; then
  echo "error: no [env:<name>] entries found in $ESP32_DIR/platformio.ini"
  exit 1
fi

mkdir -p "$OUT_ROOT"
rm -rf "$OUT_ROOT"/*

echo "Building ${#ENVS[@]} firmware environment(s)..."

chip_family_for_env() {
  local env_name="$1"
  case "$env_name" in
    *esp32-c3*|*c3*) echo "ESP32-C3" ;;
    *esp32-s3*|*s3*) echo "ESP32-S3" ;;
    *esp32-s2*|*s2*) echo "ESP32-S2" ;;
    *) echo "ESP32" ;;
  esac
}

for env_name in "${ENVS[@]}"; do
  echo "\n==> Building $env_name"
  pio run -d "$ESP32_DIR" -e "$env_name"

  build_dir="$ESP32_DIR/.pio/build/$env_name"
  out_dir="$OUT_ROOT/$env_name"
  mkdir -p "$out_dir"

  if [[ ! -f "$build_dir/firmware.bin" ]]; then
    echo "error: $env_name did not produce firmware.bin"
    exit 1
  fi

  cp "$build_dir/firmware.bin" "$out_dir/"

  if [[ -f "$build_dir/bootloader.bin" ]]; then
    cp "$build_dir/bootloader.bin" "$out_dir/"
  fi
  if [[ -f "$build_dir/partitions.bin" ]]; then
    cp "$build_dir/partitions.bin" "$out_dir/"
  fi
  if [[ -f "$build_dir/boot_app0.bin" ]]; then
    cp "$build_dir/boot_app0.bin" "$out_dir/"
  fi

  chip_family="$(chip_family_for_env "$env_name")"

  parts_json=''
  if [[ -f "$out_dir/bootloader.bin" ]]; then
    parts_json+='      { "path": "bootloader.bin", "offset": 4096 },'
    parts_json+=$'\n'
  fi
  if [[ -f "$out_dir/partitions.bin" ]]; then
    parts_json+='      { "path": "partitions.bin", "offset": 32768 },'
    parts_json+=$'\n'
  fi
  if [[ -f "$out_dir/boot_app0.bin" ]]; then
    parts_json+='      { "path": "boot_app0.bin", "offset": 57344 },'
    parts_json+=$'\n'
  fi
  parts_json+='      { "path": "firmware.bin", "offset": 65536 }'

  cat > "$out_dir/manifest.json" <<EOF
{
  "name": "Stikka Firmware ($env_name)",
  "version": "$(date -u +"%Y-%m-%d")",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "$chip_family",
      "parts": [
$parts_json
      ]
    }
  ]
}
EOF

  cat > "$out_dir/flash.json" <<EOF
{
  "env": "$env_name",
  "chipFamily": "$chip_family",
  "parts": [
    { "path": "bootloader.bin", "offset": "0x1000", "optional": true },
    { "path": "partitions.bin", "offset": "0x8000", "optional": true },
    { "path": "boot_app0.bin", "offset": "0xE000", "optional": true },
    { "path": "firmware.bin", "offset": "0x10000", "optional": false }
  ]
}
EOF

done

index_file="$OUT_ROOT/index.json"

{
  echo '{'
  echo '  "generatedAt": "'"$(date -u +"%Y-%m-%dT%H:%M:%SZ")"'",'
  echo '  "source": "stikka-firmware",'
  echo '  "environments": ['
  for i in "${!ENVS[@]}"; do
    env_name="${ENVS[$i]}"
    comma=','
    if [[ "$i" -eq "$((${#ENVS[@]} - 1))" ]]; then
      comma=''
    fi
    cat <<EOF
    {
      "env": "$env_name",
      "basePath": "./$env_name",
      "chipFamily": "$(chip_family_for_env "$env_name")",
      "files": {
        "firmware": "firmware.bin",
        "bootloader": "bootloader.bin",
        "partitions": "partitions.bin",
        "boot_app0": "boot_app0.bin"
      },
      "flashPlan": "flash.json",
      "manifest": "manifest.json"
    }$comma
EOF
  done
  echo '  ]'
  echo '}'
} > "$index_file"

echo "\nFirmware artifacts prepared in: $OUT_ROOT"
echo "Index: $index_file"
