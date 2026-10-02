# Changelog

All notable changes to Omarchy Calendar are documented here. The project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Release packaging and final v1.0 validation work.
- Reproducible release source archives with desktop OAuth configuration and
  checksummed AUR submission files, plus clean-chroot package validation in CI.

### Fixed

- Production install checks now validate the configured URLs and current version.
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

[Unreleased]: https://github.com/last-refuge/omarchy-calendar/compare/v0.9.0...HEAD
[0.9.0]: https://github.com/last-refuge/omarchy-calendar/releases/tag/v0.9.0
