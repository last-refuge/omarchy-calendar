#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
service_binary="${1:-$project_dir/build-service/omarchy-calendar-service}"

tests=(
  test-release-metadata.sh
  test-service-import.sh
  test-fresh-start.sh
  test-google-database.sh
  test-dbus-contract.sh
  test-google-auth-contract.sh
  test-google-sync-retry.sh
  test-google-mutation-upload.sh
  test-google-event-move.sh
  test-google-rsvp.sh
  test-mutation-recovery.sh
  test-secret-store-failure.sh
  test-reminder-scheduler.sh
  test-theme-accessibility.sh
  test-performance-smoke.sh
  test-google-series-update.sh
  test-quick-entry-parser.sh
  test-google-all-day-upload.sh
  test-google-delete-undo.sh
  test-event-adjustment.sh
  test-all-day-multiday.sh
  test-timezone-contract.sh
  test-recurrence-contract.sh
)

for test_name in "${tests[@]}"; do
  printf '\n==> %s\n' "$test_name"
  case "$test_name" in
    test-service-import.sh|test-fresh-start.sh|test-dbus-contract.sh|test-google-auth-contract.sh)
      "$project_dir/tests/$test_name" "$service_binary"
      ;;
    *)
      "$project_dir/tests/$test_name"
      ;;
  esac
done
