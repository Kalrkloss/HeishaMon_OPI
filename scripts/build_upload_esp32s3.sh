#!/usr/bin/env bash

set -euo pipefail

BOARD=""
PORT=""
UPLOAD="false"
VERBOSE_BUILD="false"

usage() {
  cat <<'EOF'
Usage: ./scripts/build_upload_esp32s3.sh --board <devkitc-opi|zero-qspi> [options]

Options:
  --board <name>      Target board preset (required)
  --port <tty>        Upload port (required when --upload is set)
  --upload            Upload after successful compile
  --verbose           Enable verbose build output
  -h, --help          Show this help

Board presets:
  devkitc-opi   ESP32-S3-DevKit-C (16MB QSPI Flash, 8MB OPI PSRAM)
  zero-qspi     ESP32-S3 Zero (4MB QSPI Flash, 2MB QSPI PSRAM)

Examples:
  ./scripts/build_upload_esp32s3.sh --board devkitc-opi
  ./scripts/build_upload_esp32s3.sh --board devkitc-opi --upload --port /dev/ttyACM0
  ./scripts/build_upload_esp32s3.sh --board zero-qspi --upload --port /dev/ttyUSB0
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --board)
      BOARD="${2:-}"
      shift 2
      ;;
    --port)
      PORT="${2:-}"
      shift 2
      ;;
    --upload)
      UPLOAD="true"
      shift
      ;;
    --verbose)
      VERBOSE_BUILD="true"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$BOARD" ]]; then
  echo "Error: --board is required" >&2
  usage
  exit 1
fi

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "Error: arduino-cli not found in PATH" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SKETCH_DIR="$REPO_DIR/HeishaMon"
SKETCH_FILE="$SKETCH_DIR/HeishaMon.ino"
PARTITIONS_CSV="$SKETCH_DIR/partitions.csv"

if [[ ! -f "$SKETCH_FILE" ]]; then
  echo "Error: sketch not found: $SKETCH_FILE" >&2
  exit 1
fi

case "$BOARD" in
  devkitc-opi)
    BOARD_NAME="ESP32-S3-DevKit-C (16MB QSPI Flash, 8MB OPI PSRAM)"
    FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=custom"
    PARTITION_SOURCE="$SKETCH_DIR/partitions_16m_opi.csv"
    ;;
  zero-qspi)
    BOARD_NAME="ESP32-S3 Zero (4MB QSPI Flash, 2MB QSPI PSRAM)"
    FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=4M,PSRAM=enabled,PartitionScheme=custom"
    PARTITION_SOURCE="$SKETCH_DIR/partitions_4m_qspi.csv"
    ;;
  *)
    echo "Error: unsupported board preset: $BOARD" >&2
    usage
    exit 1
    ;;
esac

if [[ ! -f "$PARTITION_SOURCE" ]]; then
  echo "Error: partition file not found: $PARTITION_SOURCE" >&2
  exit 1
fi

if [[ "$UPLOAD" == "true" && -z "$PORT" ]]; then
  echo "Error: --port is required when --upload is set" >&2
  exit 1
fi

echo "Target: $BOARD_NAME"
echo "FQBN:   $FQBN"

cleanup() {
  rm -f "$PARTITIONS_CSV"
}
trap cleanup EXIT

cp "$PARTITION_SOURCE" "$PARTITIONS_CSV"

pushd "$SKETCH_DIR" >/dev/null

COMPILE_ARGS=(
  compile
  --output-dir .
  --warnings=none
  -b "$FQBN"
  HeishaMon.ino
)

if [[ "$VERBOSE_BUILD" == "true" ]]; then
  COMPILE_ARGS+=(--verbose)
fi

arduino-cli "${COMPILE_ARGS[@]}"

if [[ "$UPLOAD" == "true" ]]; then
  arduino-cli upload -b "$FQBN" -p "$PORT" --input-dir .
fi

popd >/dev/null

if [[ "$UPLOAD" == "true" ]]; then
  echo "Done: build + upload successful for $BOARD on $PORT"
else
  echo "Done: build successful for $BOARD"
fi
