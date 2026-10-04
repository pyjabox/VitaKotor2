#!/usr/bin/env bash
set -euo pipefail

binary=${1:?usage: check_vitagl_build.sh <elf> [nm]}
nm=${2:-arm-vita-eabi-nm}
symbols=$($nm -C "$binary")

cache_state=off
if [[ $symbols == *"vgl_shader_cache_path"* ]]; then
  if [[ ${KOTOR2_ALLOW_UNSAFE_SHADER_CACHE:-0} == 1 ]]; then
    cache_state=on
    echo "WARN  build explicitly enables the validated experimental custom shader cache" >&2
  else
    echo "FAIL  vitaGL custom shader cache is enabled without KOTOR2_ALLOW_UNSAFE_SHADER_CACHE=1" >&2
    exit 1
  fi
fi

if [[ $symbols == *"invoke_splashscreen"* ]]; then
  echo "FAIL  vitaGL splashscreen is enabled; build with NO_SPLASHSCREEN=1" >&2
  exit 1
fi

echo "vitaGL build flags OK: custom shader cache $cache_state, splashscreen off"
