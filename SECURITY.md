# Security Policy

## Supported versions

Security fixes are provided for the latest published Omarchy Calendar release.
Pre-release builds from `main` receive fixes as they are developed, but are not
supported releases.

## Reporting a vulnerability

Please report security issues privately through
[GitHub Security Advisories](https://github.com/last-refuge/omarchy-calendar/security/advisories/new).
Do not include access tokens, refresh tokens, OAuth client credentials, calendar
content, or other personal data in a public issue.

Include the affected version, operating system, steps to reproduce, expected and
observed behavior, and any logs you can safely share. Redact account identity and
calendar content. We will acknowledge a report as soon as practical, investigate
it, and coordinate disclosure and a fixed release with the reporter.

## Data and credentials

Omarchy Calendar stores calendar data locally in SQLite and stores Google refresh
tokens through the desktop Secret Service. Access tokens and refresh tokens are
excluded from the app's diagnostics export. Official packages may include the
public OAuth identity required by Google's installed-app flow; user tokens remain
local to the user's system.
