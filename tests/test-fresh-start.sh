#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export TEST_SERVICE_BINARY="${1:-$project_dir/build-service/omarchy-calendar-service}"
export TEST_FRESH_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_FRESH_DIR"' EXIT
export OMARCHY_CALENDAR_DB_PATH="$TEST_FRESH_DIR/data/calendar.db"
export OMARCHY_CALENDAR_FEED_PATH="$TEST_FRESH_DIR/state/feed.json"
export OMARCHY_CALENDAR_GOOGLE_CLIENT_ID=fresh-start.apps.googleusercontent.com
export OMARCHY_CALENDAR_GOOGLE_CLIENT_SECRET=fresh-start-test

# An explicit import still requires a real feed.
if "$TEST_SERVICE_BINARY" --import-only >/dev/null 2>&1; then
  echo 'Import unexpectedly accepted a missing feed' >&2
  exit 1
fi

dbus-run-session -- bash -c '
  set -euo pipefail
  "$TEST_SERVICE_BINARY" >"$TEST_FRESH_DIR/service.log" 2>&1 &
  service_pid=$!
  trap "kill $service_pid 2>/dev/null || true" EXIT
  ready=false
  for _ in {1..60}; do
    if gdbus introspect --session --dest org.omarchy.Calendar --object-path /org/omarchy/Calendar >/dev/null 2>&1; then
      ready=true
      break
    fi
    kill -0 "$service_pid" 2>/dev/null || { cat "$TEST_FRESH_DIR/service.log"; exit 1; }
    sleep 0.05
  done
  [[ "$ready" == true ]]
  status="$(gdbus call --session --dest org.omarchy.Calendar --object-path /org/omarchy/Calendar --method org.omarchy.Calendar1.GetStatus)"
  provider="$(gdbus call --session --dest org.omarchy.Calendar --object-path /org/omarchy/Calendar --method org.omarchy.Calendar1.GetProviderStatus)"
  [[ "$status" == *"\"eventCount\":0"* ]]
  [[ "$provider" == *"\"configured\":true"* ]]
  [[ "$provider" == *"\"state\":\"disconnected\""* ]]
'

echo 'fresh startup without a compatibility feed: ok'
