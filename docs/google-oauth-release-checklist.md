# Google OAuth public-release checklist

Google production OAuth approval was reported by the publisher on October 4,
2026 and confirmed in Google’s Verification Center. Release validation and
package publication are tracked separately. Google requires a production app
to have a public homepage on a
verified domain, a privacy policy on that same domain, accurate consent-screen
branding, and verification for sensitive scopes.

Version [1.0.0 is published](https://github.com/last-refuge/omarchy-calendar/releases/tag/v1.0.0).
[Omarchy inclusion PR #804](https://github.com/omacom/omarchy-pkgs/pull/804) awaits
maintainer review and repository publication. Direct AUR submission is deferred.

Current status:

- [x] Publisher domain selected: `lastrefuge.ai`
- [x] Product homepage, privacy policy, terms, contact, and security pages are public
- [x] Public support address selected: `jason@greatspark.com`
- [x] App, package metadata, and release workflow use the production URLs
- [x] Production OAuth homepage, privacy, terms, and authorized domain configured
- [x] Production OAuth data access reduced to the four required scopes
- [x] 120×120 OAuth application icon prepared for upload
- [x] Domain ownership confirmed in Search Console through Cloudflare DNS
- [x] `jason@greatspark.com` added as a project Editor and developer contact
- [ ] Public support address made eligible in Google Cloud and selected in Branding
- [x] OAuth application icon uploaded; branding verified and published
- [x] App published from Testing to Production
- [x] Verification submitted and replacement scope-demonstration video provided
- [x] Google production OAuth approval received (publisher confirmation, 2026-10-04)
- [x] Production sign-in, sync, create/edit/delete, restart, and disconnect retested
- [ ] Remaining fresh-desktop acceptance checks completed (see [release evidence](arch-release.md#production-validation-on-october-4-2026))

## 1. Choose and verify the public domain

1. The publisher domain is `lastrefuge.ai`. The public product page is
   `https://lastrefuge.ai/projects/omarchy-calendar`; privacy and terms are at
   `https://lastrefuge.ai/privacy` and `https://lastrefuge.ai/terms`.
2. Confirm all three pages remain public without a login before submission.
3. Release builds embed those values from the release workflow without a source
   edit; verify them with `omarchy-calendar --release-info`.
4. Verify domain ownership in Google Search Console using an account that is an
   owner or editor of the Google Cloud project.

Google Search Console verified `lastrefuge.ai` through its Cloudflare
integration on September 21, 2026. Keep the generated TXT record in place so
the ownership proof remains valid.

## 2. Create a separate production project

Google recommends separate Cloud projects for development/testing and
production. In the production project:

1. Enable the Google Calendar API.
2. Open **Google Auth Platform → Branding**. Set the app name to **Omarchy
   Calendar**, add the final icon, support contact, homepage, privacy policy,
   and terms URLs, and add the verified domain.
3. Set **Audience** to external and keep the app in testing until the review
   package is ready.
4. In **Data Access**, request only:
   - `openid`
   - `https://www.googleapis.com/auth/userinfo.email`
   - `https://www.googleapis.com/auth/calendar.calendarlist.readonly`
   - `https://www.googleapis.com/auth/calendar.events`
5. Create a **Desktop app** OAuth client. Configure the resulting client ID and
   client secret in a release-only configuration file; never commit production
   configuration to this repository. The release generator includes exactly
   those two Desktop app settings in the published source archive, so both AUR
   and official binary builds install the same configuration. These installed
   app settings are distributed to users; they are not a confidential server
   secret. Never include user access tokens, refresh tokens, or service-account
   keys. See the [Arch release guide](arch-release.md).

The production project is `omarchy-calendar-509223`. Its authorized domain,
public URLs, external audience, Desktop app client, minimal data-access scope
set, application icon, and verified public branding are configured. The app is
in production. Google still offers only `jason.alexander@gmail.com` and Google
Groups managed by that account in the support-address selector.
`jason@greatspark.com` is a project Editor and the configured developer contact,
but Google does not offer it in that selector. Google's automated branding
review passed with this configuration.

## 3. Prepare the review evidence

The exact form copy and recording sequence are maintained in
[`google-oauth-verification-submission.md`](google-oauth-verification-submission.md).

Record a concise demonstration showing the complete consent sequence:

1. The public homepage, privacy policy, and terms.
2. Starting Google authorization from the Omarchy Calendar welcome screen.
3. The consent screen and every requested scope.
4. Calendar list synchronization and offline display.
5. Creating, editing, responding to, and deleting an event.
6. Disconnecting the account and explaining local cache/token deletion.

The scope justification should state that Calendar List read access discovers
and labels the user's calendars, while Calendar Events access powers the visible
read/write calendar experience and offline mutation queue. OpenID and email are
used only to identify and label the connected account.

## 4. Submit and release

1. Submit brand and sensitive-scope verification from Google Auth Platform.
2. Keep the development client available for test builds while Google reviews
   the production project.
3. After approval, inject the production desktop client at release build time,
   run the live create/update/RSVP/delete lifecycle on a dedicated test account,
   and confirm account revocation returns the app to an actionable error state.
4. Bump all release metadata to `1.0.0`, commit, and tag `v1.0.0`. The release
   workflow builds the source archive, checksummed AUR bundle, and binary
   package, then creates a **draft** GitHub release. Review it and finish the
   live checks before publication. Confirm the public source downloads work
   before submitting the generated `PKGBUILD` and `.SRCINFO` to AUR. Google
   has approved the production OAuth integration.

Official references:

- [Google OAuth 2.0 policies](https://developers.google.com/identity/protocols/oauth2/policies)
- [Verification requirements](https://support.google.com/cloud/answer/13464321)
- [Sensitive-scope verification](https://developers.google.com/identity/protocols/oauth2/production-readiness/sensitive-scope-verification)
- [Google API Services User Data Policy](https://developers.google.com/terms/api-services-user-data-policy)
