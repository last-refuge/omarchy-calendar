# Changelog

All notable changes to Omarchy Calendar are documented here. The project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.0.0] - 2026-10-04

### Added

- Production-approved Google OAuth for the first stable release.
- Native calendar views, offline Google Calendar synchronization and editing,
  recurrence, invitations, reminders, search, and Omarchy theme integration.
- Official Omarchy repository submission bundle with a stable GitHub release watch
  and native x86_64/aarch64 package builds in CI.
- Reproducible release source archives with desktop OAuth configuration and
  checksummed AUR submission files, plus fresh-container package validation in CI.

### Fixed

- Calendar range queries reuse the compiled meeting-link pattern instead of
  recompiling it for every event.
- Disconnect clears the exported calendar cache and prevents stale native feeds
  from restoring a removed account's events.
- First launch now starts the service without requiring an existing calendar feed.
- Build requirements now specify Qt 6.9, matching the OAuth APIs in use.
- Production install checks now validate the configured URLs and current version.
- All-day/multi-day tests now cover display dates in four explicit time zones.
- Package and project links now use the canonical last-refuge GitHub organization.
- CI runs makepkg checks as an unprivileged user, and RSVP tests keep invitation
  fixtures in the future so they do not expire between releases.
- Arch packages install the MIT license in the standard license directory and
  document the Secret Service provider needed for Google sign-in.

## [0.9.0] - 2026-09-21

### Added

- Native day, week, month, agenda, and search experiences.
- Offline SQLite storage with full and incremental Google Calendar sync.
- Create, edit, move, resize, delete, undo, recurrence, and calendar moves.
- Invitations, RSVP, guests, reminders, availability, privacy, and meeting links.
- Durable offline mutation queue with conflict handling and recovery controls.
- Omarchy theme integration, onboarding, accessibility, diagnostics, and large-calendar performance coverage.

[Unreleased]: https://github.com/last-refuge/omarchy-calendar/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/last-refuge/omarchy-calendar/releases/tag/v1.0.0
[0.9.0]: https://github.com/last-refuge/omarchy-calendar/releases/tag/v0.9.0
