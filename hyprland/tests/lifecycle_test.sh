#!/usr/bin/env bash
set -euo pipefail

test_dir=$(mktemp -d /tmp/luxaxis-lifecycle.XXXXXX)
fixture_dir=$(dirname "${BASH_SOURCE[0]}")/fixtures
mkdir -p "$test_dir/runtime" "$test_dir/config/hypr" "$test_dir/cache"
chmod 700 "$test_dir/runtime"
cp "$fixture_dir/luxaxis.toml" "$test_dir/config/hypr/luxaxis.toml"
args=(--config "$fixture_dir/hyprland.conf")
if (( EUID == 0 )); then args+=(--i-am-really-stupid); fi
export XDG_RUNTIME_DIR="$test_dir/runtime" XDG_CONFIG_HOME="$test_dir/config" XDG_CACHE_HOME="$test_dir/cache"
if ! env LD_PRELOAD="$2" LUXAXIS_TEST_PLUGIN="$3" "$1" "${args[@]}" >"$test_dir/host.log" 2>&1; then
  tail -40 "$test_dir/host.log"
  printf 'Artifacts: %s\n' "$test_dir"
  exit 1
fi
grep 'load/unload' "$test_dir/host.log"
