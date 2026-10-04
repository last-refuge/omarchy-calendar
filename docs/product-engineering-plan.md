# Omarchy Calendar — Product and Engineering Plan

## Implementation status

As of September 20, 2026, the first Phase 0 prototype is running:

- Qt/QML application shell with polished day, week, month, and agenda views, navigation, sidebar, event cards, and inspector.
- Live palette and font-token loading from the active Omarchy theme.
- Read-only loading and file watching for `calendar-events.json` version 1.
- SQLite schema version 1 with WAL mode, transactional compatibility imports, integrity checks, and multi-day event keys.
- A persistent `org.omarchy.Calendar` user service with the versioned `org.omarchy.Calendar1` DBus interface.
- The desktop app now reads calendars, ranges, next-event data, and change notifications from DBus, with JSON fallback.
- Real day/week/month navigation, current-day/current-time indicators, calendar discovery, next-event summary, all-day event treatment, and event inspection.
- Functional 45-day agenda and SQLite-backed search across titles, locations, and calendar names.
- Calendar visibility toggles shared by every view and persisted across launches.
- Collision-aware side-by-side layout for overlapping timed events.
- Keyboard focus cursors for day, week, month, agenda, and search, including arrow keys, `J`/`K`, Enter activation, and automatic scrolling to selected events.
- Schema version 2 with provider accounts, multi-account event identity, synchronization cursors, expanded provider metadata, and lossless migration from the compatibility database.
- Google desktop OAuth foundation using PKCE, a loopback callback, DBus authorization signals, staged calendar permissions, and refresh-token storage in Secret Service.
- An account Settings surface that reflects configuration, authorization, connected, and error states.
- Google Calendar List and Events synchronization with pagination, selected-calendar filtering, transactional full and incremental imports, cancellation handling, multi-day expansion, expired-token recovery, manual refresh, and a five-minute background interval.
- Live synchronization progress and diagnostics, bounded retry backoff, immediate retry after network restoration, automatic access-token rotation, and guarded account removal with Secret Service cleanup.
- Narrow read-only Calendar List plus event-management OAuth scopes, with a persisted editing capability and atomic compatibility-feed export for the Omarchy top bar.
- Persistent per-calendar sync selection with a polished Settings control, immediate app and top-bar updates, cached paused calendars, and automatic incremental sync when resumed.
- Deterministic Google failure drills covering transient server errors, rate limiting, expired sync tokens, revoked authorization, bounded retry state, cursor safety, and cached calendar retention.
- Phase 2 foundation: schema version 3, durable pending mutations, atomic optimistic event creation, and a native keyboard-accessible event composer.
- Scope-aware editing upgrade: schema version 4 records granted capabilities and Settings offers an explicit Google consent upgrade before queued mutations can upload.
- Google create uploader with exact event payload conversion, durable retry/blocked states, atomic local-to-provider identity reconciliation, compatibility-feed refresh, and deterministic localhost API coverage.
- Phase 2 editing slice with a prefilled native composer, optimistic transactional updates, create-operation coalescing, ETag-protected Google PATCH uploads, and response reconciliation.
- Safe edit-conflict recovery with current-event lookup, field-level three-way merging, automatic retry for non-overlapping changes, and an explicit conflict state for overlapping changes.
- Optimistic event deletion with a six-second undo window, restart-safe mutation finalization, ETag-protected Google DELETE, local-create cancellation, remote-absence handling, and restoration after permanent provider rejection.
- Direct event manipulation in day, week, and month views with 15-minute snapping, lower-edge resizing, keyboard equivalents, optimistic persistence, and debounced mutation coalescing.
- CMake project plus a qmake bootstrap for the current development machine.
- Real Wayland visual review with a scoped opaque-window rule for calendar legibility.

The real Google authorization, incremental synchronization, service restart, compatibility fallback, Omarchy bar feed, calendar-selection lifecycle, and key Google failure paths are now proven end to end. The Phase 1 exit behavior is in place for the development build; work can now proceed into event creation and editing.

## Product goal

Build a beautiful, keyboard-friendly calendar that feels native to Omarchy while providing the depth expected from Apple Calendar or Outlook Calendar. The first provider is Google Calendar. The architecture must support CalDAV and additional providers later without changing the interface or local data model.

The product has two surfaces:

1. **A standalone desktop application** for day, week, month, agenda, search, event editing, scheduling, and settings.
2. **A companion Omarchy Shell plugin** for the top bar, selected-day agenda, next-event countdown, meeting join actions, and quick event creation.

The existing `tmn73.calendar` bar plugin and its `calendar-events.json` contract are the starting point for the companion surface. The first app releases should preserve that contract so the existing bar remains useful while the new application is built.

## Product principles

- Beauty is a release requirement: every shipped surface must feel deliberate, cohesive, and worthy of daily use rather than merely functional.
- Fast enough to open as a transient tool, not only as a permanently running app.
- Offline-first: cached calendars remain fully browsable without a network connection.
- Every common action works from the keyboard.
- Calendar color carries meaning without becoming visual clutter.
- Omarchy themes apply immediately without restarting the app.
- Account credentials stay in Secret Service; the database never stores OAuth refresh tokens.
- Provider-specific behavior remains behind a provider interface.
- Background synchronization and reminders continue when the main window is closed.

## Visual quality bar

- Establish the visual language in a working static prototype before provider or database work begins.
- Use a restrained type scale, consistent spacing rhythm, clear hierarchy, and calendar colors that remain legible without overwhelming the interface.
- Treat hover, focus, selected, pressed, disabled, loading, empty, error, and offline states as designed states rather than implementation leftovers.
- Keep motion brief and purposeful, with reduced-motion support and no animation that delays input.
- Review every primary view at compact and comfortable density, light and dark compatible palettes, and 100%, 125%, 150%, and 200% scale.
- Require visual snapshots and an interactive design review before each phase exits; passing functional tests alone is insufficient.
- Measure perceived quality against macOS Calendar's clarity and direct manipulation while retaining Omarchy's typography, palette, keyboard focus, and compact desktop character.

## Recommended foundation

### Interface

- **Qt 6 and QML/Qt Quick** for the standalone application.
- Reusable QML components modeled on Omarchy Shell's state vocabulary: normal, hover, selected, focus, and pressed.
- Read colors from `~/.local/state/omarchy/current/theme/colors.toml` and size/type tokens from the active `shell.toml` plus `~/.config/omarchy/shell.toml`.
- Watch the active theme directory and update bindings live.
- Use the active Hyprland rounding and gap values for window and popover geometry.

### Application services

- A small C++ service using Qt Core, Network, SQL, DBus, and NetworkAuth.
- SQLite through Qt SQL, with migrations and WAL mode.
- Qt Network for Google REST requests.
- Qt Network Authorization for the desktop OAuth loopback flow.
- Secret Service through `qtkeychain-qt6` for refresh tokens.
- `libical` when CalDAV/iCalendar import and export arrive.
- Freedesktop notifications and activation tokens for reminders.

The application UI should be a client of the service over DBus. This keeps synchronization, reminders, and the local database alive when the window closes and gives the shell plugin a stable integration point.

## Process layout

```text
Google Calendar API
        │
        ▼
omarchy-calendar-service
  OAuth · sync · mutations · reminders
        │
        ├── SQLite database
        ├── Secret Service
        ├── DBus API
        └── calendar-events.json compatibility export
                 │
        ┌────────┴────────┐
        ▼                 ▼
omarchy-calendar     Omarchy bar plugin
Qt/QML desktop UI    next event / agenda / join
```

## Repository layout

```text
omarchy-calendar/
├── CMakeLists.txt
├── app/
│   ├── main.cpp
│   ├── qml/
│   │   ├── App.qml
│   │   ├── views/
│   │   ├── components/
│   │   └── dialogs/
│   └── resources/
├── service/
│   ├── db/
│   ├── providers/
│   │   ├── CalendarProvider.h
│   │   └── google/
│   ├── sync/
│   ├── reminders/
│   └── dbus/
├── shell-plugin/
├── packaging/
│   ├── arch/
│   ├── systemd/
│   └── desktop/
├── tests/
│   ├── unit/
│   ├── integration/
│   └── fixtures/
└── docs/
```

## Local data model

Store provider-neutral records and preserve the provider payload needed for lossless updates.

- `accounts`: provider, display identity, sync state, error state.
- `calendars`: provider ID, name, color, visibility, access role, timezone.
- `events`: provider ID, iCalendar UID, calendar ID, title, description, location, start/end, timezone, all-day state, event type, recurrence master, status, visibility, transparency, ETag, updated time.
- `attendees`: address, name, organizer flag, response, optional/required state.
- `reminders`: method and offset.
- `conference_data`: provider, join URL, phone/PIN details.
- `recurrence_rules` and exceptions.
- `sync_cursors`: a cursor per remote calendar.
- `pending_mutations`: durable create/update/delete/RSVP operations with retry state.

Times should be stored as UTC instants plus the original IANA timezone. All-day dates remain dates and must never be converted through UTC.

## Google integration

### Authentication

- Use Google's installed desktop application OAuth flow with a loopback listener on `127.0.0.1` and the system browser.
- Request the smallest useful scopes first: calendar list read access and event read/write access.
- Store only the refresh token and account identifier in Secret Service.
- Personal development builds can use a developer-provided OAuth client. A public release requires an Omarchy-owned Google Cloud project, a published consent screen, privacy documentation, and Google verification for sensitive scopes.

### Synchronization

- Perform a full sync once, persist the returned `syncToken`, then use incremental sync for subsequent passes.
- Sync on login, application focus, network restoration, manual refresh, and a five-minute background interval.
- Use pagination correctly before committing a new sync token.
- Treat an expired sync token as a signal to repeat the full sync for that calendar.
- Use a durable local mutation queue and optimistic UI updates.
- Use ETags or current remote versions to detect conflicting edits.
- Poll in the desktop release. Google push notifications require a public HTTPS webhook and are unnecessary infrastructure for the first version.

## Primary experience

### Window structure

- Narrow navigation rail: Today, Calendar, Agenda, Search, Settings.
- Calendar sidebar: accounts, calendars, visibility toggles, color indicators.
- Main canvas: day, week, month, and agenda views.
- Inspector/editor: a side sheet that preserves context instead of opening modal windows for routine edits.
- Compact command bar for date navigation, view switching, search, and quick creation.

### Essential interactions

- Click or press `N` to create an event.
- Drag across time to create with a duration.
- Drag an event to move it; drag its edge to resize it.
- Natural-language quick entry for common phrases, with an explicit preview before saving.
- Recurrence editor with clear choices for changing one occurrence, this and future occurrences, or the whole series.
- Search by title, attendee, location, and description from local data.
- Join button for active meetings.
- RSVP from the event inspector and invitation notifications.
- Undo for destructive local actions while the remote mutation is still queued.

### Keyboard baseline

- `T`: today.
- Arrow keys: move the calendar cursor.
- `J` / `K`: next/previous time range or agenda item.
- `1` / `2` / `3` / `4`: day/week/month/agenda.
- `N`: new event.
- `/`: search.
- `Enter`: open editor.
- `Delete`: delete with undo.
- `Alt` + arrow keys: move the selected event by 15 minutes or one day.
- `Alt` + `Shift` + up/down: resize the selected event by 15 minutes.
- `Ctrl+Enter`: save.
- `Escape`: close the current sheet or return focus to the calendar.

## Omarchy integration

- Preserve `~/.local/state/omarchy/calendar-events.json` version 1 during the transition.
- Evolve `tmn73.calendar` into the official companion plugin rather than embedding the entire application in Quickshell.
- Add bar actions for open calendar, quick add, join meeting, refresh, and selected-day agenda.
- Add a global shortcut for showing/hiding the calendar and another for quick add.
- Follow Omarchy palette, font, spacing, rounding, opacity, and active-border tokens.
- Use the Omarchy notification service for reminders, with actions for Open, Join, Snooze, and Dismiss.
- Package a `.desktop` entry, icons, DBus service file, systemd user service, and Arch `PKGBUILD`.

## Delivery phases

### Phase 0 — Product spike and contracts

- Create the Qt/QML shell and render static day, week, and month fixtures.
- Implement the Omarchy theme reader and live theme switching.
- Define the DBus interface and SQLite schema.
- Import the existing `calendar-events.json` as a read-only compatibility source.
- Validate keyboard navigation and rendering at 100%, 125%, 150%, and 200% scale.

**Exit:** the app looks native to Omarchy, opens quickly, and can browse fixture events without network access.

### Phase 1 — Read-only Google MVP

- OAuth onboarding and account removal.
- Calendar list and color synchronization.
- Full and incremental event sync.
- Day, week, month, and agenda views.
- Local search and calendar visibility controls.
- Compatibility export to the existing bar plugin.

**Exit:** a user can connect Google once, restart or go offline, and reliably browse the same events in the app and bar.

### Phase 2 — Event creation and editing

- Create, edit, move, resize, and delete events.
- All-day events and timezone-aware events.
- Recurring events and exceptions.
- Reminders, locations, descriptions, guests, and calendar selection.
- Durable offline mutation queue, retries, conflicts, and undo.

**Exit:** ordinary personal calendar management no longer requires opening Google Calendar in a browser.

### Phase 3 — Meetings and scheduling

- Invitations and RSVP.
- Google Meet creation and join actions.
- Attendee availability/free-busy view.
- Working location, focus time, and out-of-office event types where the API permits them.
- Notification actions and snoozing.

**Exit:** common work-calendar workflows are complete from the desktop app.

### Phase 4 — Omarchy product polish

- Quick-add overlay and global shortcuts.
- Refined motion, focus behavior, accessibility, density settings, and responsive layouts.
- First-run tour, actionable sync errors, account health, and diagnostics export.
- Arch package, clean install/upgrade/uninstall paths, and release automation.
- Migrate the shell plugin from file watching to DBus when that contract is stable.

**Exit:** the app feels like an Omarchy component rather than a themed third-party calendar.

### Phase 5 — Provider expansion

- CalDAV provider using the same service and data model.
- iCalendar import/export and subscription feeds.
- Optional local calendars.
- Provider capability flags so unsupported fields are disabled clearly.

**Exit:** Google-specific code is confined to its provider and the product works with major standards-based services.

## Verification strategy

- Unit tests for recurrence expansion, all-day boundaries, timezone conversion, DST transitions, conflict resolution, and mutation coalescing.
- Provider contract tests using recorded and redacted HTTP fixtures.
- SQLite migration tests from every released schema version.
- QML model tests and targeted interaction tests for drag, resize, keyboard focus, and editing.
- Integration tests against a dedicated Google test account for create/update/delete/RSVP flows.
- Visual snapshots for each view, Omarchy theme, density, and scale factor.
- Failure drills: expired OAuth token, revoked access, invalid sync token, rate limit, offline edits, process restart during mutation, and corrupted local cache.

## Major risks and decisions

1. **Google production OAuth verification was approved on October 4, 2026.** The remaining release work is package validation, publication, and Omarchy maintainer acceptance; see [the release guide](arch-release.md).
2. **Recurrence and timezone correctness are harder than rendering.** Build these as tested service-layer behavior before adding elaborate editor controls.
3. **A local desktop app cannot receive Google webhooks directly.** Incremental polling is the correct first architecture.
4. **Quickshell is the companion surface, not the full application runtime.** This limits shell crashes and keeps the calendar independently testable.
5. **Avoid KDE PIM and Evolution Data Server initially.** They solve provider breadth but impose architecture and interaction constraints that conflict with the product goal.
6. **Keep the DBus and JSON contracts versioned.** Omarchy Shell and the standalone application will update at different times.

## First implementation slice

The first build should be deliberately vertical rather than broad:

1. Scaffold the CMake project, app, service, DBus interface, and Arch development package.
2. Implement theme loading and a polished week view.
3. Create the SQLite schema and import the existing local event JSON.
4. Render those events in the app and retain the current shell-bar behavior.
5. Add keyboard navigation, Today, view switching, and a read-only event inspector.
6. Measure cold start, memory use, large-calendar rendering, and scaling before adding OAuth.

That slice proves the product shape and Omarchy integration before the project takes on Google's authentication and synchronization complexity.
