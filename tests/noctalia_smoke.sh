#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(realpath "${1:-.}")
for command in sway noctalia socat grim jq timeout cc pkg-config wayland-scanner; do
  command -v "$command" >/dev/null || { printf 'Missing test dependency: %s\n' "$command" >&2; exit 1; }
done
fixture_dir="$repo_dir/tests/fixtures/noctalia"
test_dir=$(mktemp -d /tmp/luxaxis-noctalia.XXXXXX)
export XDG_RUNTIME_DIR="$test_dir/runtime"
export NOCTALIA_CONFIG_HOME="$test_dir/config" NOCTALIA_DATA_HOME="$test_dir/data" NOCTALIA_STATE_HOME="$test_dir/state"
export XDG_CACHE_HOME="$test_dir/cache"
export HYPRLAND_INSTANCE_SIGNATURE=luxaxis-smoke
export LUXAXIS_TEST_DISPATCH_LOG="$test_dir/dispatch.log"
export PATH="$fixture_dir:$PATH"
mkdir -p "$XDG_RUNTIME_DIR" "$NOCTALIA_CONFIG_HOME/noctalia" "$NOCTALIA_DATA_HOME/noctalia/plugins" "$NOCTALIA_STATE_HOME" "$XDG_CACHE_HOME"
chmod 700 "$XDG_RUNTIME_DIR"
cp "$fixture_dir/config.toml" "$NOCTALIA_CONFIG_HOME/noctalia/config.toml"
ln -s "$repo_dir" "$NOCTALIA_DATA_HOME/noctalia/plugins/luxaxis"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 LIBGL_ALWAYS_SOFTWARE=1
sway -c /dev/null >"$test_dir/compositor.log" 2>&1 &
compositor_pid=$!
shell_pid=
pointer_pid=
cleanup() {
  if [[ -n "$pointer_pid" ]]; then kill "$pointer_pid" 2>/dev/null || true; fi
  if [[ -n "$shell_pid" ]]; then kill "$shell_pid" 2>/dev/null || true; fi
  kill "$compositor_pid" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT
fail() {
  printf 'Noctalia smoke test failed: %s\nArtifacts: %s\n' "$1" "$test_dir" >&2
  if [[ -f "$test_dir/noctalia.log" ]]; then tail -60 "$test_dir/noctalia.log" >&2; fi
  exit 1
}
check_ipc() {
  timeout 3 noctalia msg plugins list >"$test_dir/plugins.txt" 2>"$test_dir/ipc.log" || fail "Noctalia IPC stopped responding"
  [[ -s "$test_dir/plugins.txt" ]] || fail "Noctalia returned an empty IPC reply"
  kill -0 "$shell_pid" 2>/dev/null || fail "Noctalia exited unexpectedly"
}
click() {
  printf '%s %s %s\n' "$1" "$2" "${3:-272}" >&"$pointer_input"
  read -r -t 3 -u "$pointer_output" result || fail "virtual pointer stopped responding"
  [[ "$result" == clicked ]] || fail "virtual pointer failed to click"
  sleep 0.2
}
for _ in {1..50}; do
  sockets=("$XDG_RUNTIME_DIR"/wayland-*)
  if [[ -S "${sockets[0]}" ]]; then break; fi
  kill -0 "$compositor_pid" 2>/dev/null || { cat "$test_dir/compositor.log"; exit 1; }
  sleep 0.1
done
[[ -S "${sockets[0]}" ]]
export WAYLAND_DISPLAY="${sockets[0]##*/}"
wayland-scanner client-header "$fixture_dir/wlr-virtual-pointer-unstable-v1.xml" "$test_dir/virtual-pointer.h"
wayland-scanner private-code "$fixture_dir/wlr-virtual-pointer-unstable-v1.xml" "$test_dir/virtual-pointer.c"
read -r -a wayland_flags <<< "$(pkg-config --cflags --libs wayland-client)"
cc -Wall -Wextra -Werror -I"$test_dir" "$repo_dir/tests/wayland_pointer.c" "$test_dir/virtual-pointer.c" "${wayland_flags[@]}" -o "$test_dir/pointer"
coproc POINTER { exec "$test_dir/pointer"; }
pointer_pid=$POINTER_PID
pointer_input=${POINTER[1]}
pointer_output=${POINTER[0]}
read -r -t 3 -u "$pointer_output" result
[[ "$result" == ready ]] || fail "virtual pointer could not start"
noctalia >"$test_dir/noctalia.log" 2>&1 &
shell_pid=$!
ready=false
for _ in {1..50}; do
  if noctalia msg plugins list >"$test_dir/plugins.txt" 2>"$test_dir/ipc.log" && [[ -s "$test_dir/plugins.txt" ]]; then ready=true; break; fi
  kill -0 "$shell_pid" 2>/dev/null || { cat "$test_dir/noctalia.log"; exit 1; }
  sleep 0.1
done
[[ "$ready" == true ]]
grep -q 'yyc94/luxaxis .* enabled' "$test_dir/plugins.txt"
grep -q "started service 'yyc94/luxaxis:workspace-state'" "$test_dir/noctalia.log"
sleep 1
grim "$test_dir/widget.png"
click 152 17
[[ -s "$LUXAXIS_TEST_DISPATCH_LOG" ]] || fail "workspace click did not dispatch"
grep -Fxq 'workspace' "$LUXAXIS_TEST_DISPATCH_LOG" || fail "wrong workspace dispatcher"
grep -Fxq '2' "$LUXAXIS_TEST_DISPATCH_LOG" || fail "wrong workspace selected"
noctalia msg panel-open yyc94/luxaxis:workspace-styles
sleep 1
grim "$test_dir/panel.png"
click 838 346 273
grim "$test_dir/menu.png"
click 838 404
click 936 681
styles_path="$NOCTALIA_STATE_HOME/noctalia/plugins/data/yyc94/luxaxis/styles.json"
[[ -f "$styles_path" ]] || fail "Apply did not save workspace styles"
jq -e '.base.labelSource == "name"' "$styles_path" >/dev/null || fail "native menu did not change the saved label source"
click 838 346
grim "$test_dir/choices.png"
click 838 388
click 936 681
jq -e '.base.labelSource == "id"' "$styles_path" >/dev/null || fail "left-click choices did not save the selected label source"
noctalia msg panel-close yyc94/luxaxis:workspace-styles
for cycle in {1..3}; do
  if (( cycle > 1 )); then
    noctalia msg panel-open yyc94/luxaxis:workspace-styles
    sleep 0.3
    if (( cycle == 3 )); then
      noctalia msg config-reload
      sleep 0.2
      check_ipc
      noctalia msg panel-open yyc94/luxaxis:workspace-styles
      sleep 0.3
      noctalia msg panel-close yyc94/luxaxis:workspace-styles
    fi
  fi
  timeout 3 noctalia msg plugins disable yyc94/luxaxis || fail "plugin disable failed"
  sleep 0.2
  check_ipc
  grep -q 'yyc94/luxaxis .* disabled' "$test_dir/plugins.txt" || fail "plugin was not disabled"
  timeout 3 noctalia msg plugins enable yyc94/luxaxis || fail "plugin enable failed"
  sleep 0.2
  check_ipc
  grep -q 'yyc94/luxaxis .* enabled' "$test_dir/plugins.txt" || fail "plugin was not enabled"
  service_starts=$(grep -c "started service 'yyc94/luxaxis:workspace-state'" "$test_dir/noctalia.log")
  [[ "$service_starts" -eq "$((cycle + 1))" ]] || fail "workspace service did not restart"
done
kill "$shell_pid"
shell_status=0
wait "$shell_pid" || shell_status=$?
shell_pid=
[[ "$shell_status" -eq 0 || "$shell_status" -eq 143 ]] || fail "Noctalia exited with status $shell_status"
if grep -E 'targets plugin API|attempt to|script.*(error|failed)|getColor|pure virtual|terminate called|Segmentation fault' "$test_dir/noctalia.log"; then
  fail "Noctalia reported an API, script, or native crash"
fi
printf 'Noctalia load, workspace click, style choices/save, reload, and disable/enable passed\nArtifacts: %s\n' "$test_dir"
