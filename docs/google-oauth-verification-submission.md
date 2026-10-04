# Google OAuth verification submission

This is the review package for the public Omarchy Calendar OAuth client in
Google Cloud project `omarchy-calendar-509223`. Keep this page beside the Google
Auth Platform verification form and update the status table as the review
advances.

## Submission identity

| Field | Production value | Status |
| --- | --- | --- |
| Application name | Omarchy Calendar | Configured |
| Application type | Desktop app | Configured |
| Publisher domain | `lastrefuge.ai` | Verified through Cloudflare DNS on 2026-09-21 |
| Homepage | `https://lastrefuge.ai/projects/omarchy-calendar` | Live and configured |
| Privacy policy | `https://lastrefuge.ai/privacy` | Live and configured |
| Terms | `https://lastrefuge.ai/terms` | Live and configured |
| Public support | `jason@greatspark.com` | Published on site and project Editor; Google consent screen uses account email |
| Developer contact | `jason@greatspark.com` | Configured |
| Application icon | `docs/omarchy-calendar-oauth-logo.png` | Uploaded; branding verified and published |
| Audience | External | In production |

## Requested scopes and form copy

Request only these four scopes:

| Scope | Why Omarchy Calendar needs it |
| --- | --- |
| `openid` | Establishes a stable identity for the Google account the user intentionally connects. |
| `https://www.googleapis.com/auth/userinfo.email` | Displays the connected email address so the user can identify and disconnect the correct account. It is not used for advertising, profiling, or sharing. |
| `https://www.googleapis.com/auth/calendar.calendarlist.readonly` | Discovers the user's calendars and reads their names, colors, visibility, access roles, and default reminder settings so the application can present an accurate calendar list. |
| `https://www.googleapis.com/auth/calendar.events` | Reads and manages the calendar events the user chooses to work with, including creation, editing, deletion, invitations, responses, recurrence, reminders, and meeting details. It also supports the local offline mutation queue and reconciliation after connectivity returns. |

Suggested sensitive-scope explanation:

> Omarchy Calendar is a native Linux calendar client. Users connect their own
> Google account and use the visible calendar interface to read and manage
> events. Calendar-list read access discovers and labels their calendars.
> Calendar-events access powers event display, creation, editing, deletion,
> invitations, RSVP, recurrence, reminders, meeting links, and an offline queue
> that uploads only the user's requested changes. OpenID and email identify and
> label the connected account. Data is stored locally on the user's computer;
> the developer does not operate a calendar-data backend and does not sell,
> advertise against, or share Google user data.

## Demo recording script

Record a readable video with the browser address bar and the full application
window visible where relevant. Shorten idle pauses without omitting consent
steps. Hide the transient authorization callback URL and unrelated event details.
Use a dedicated test calendar or an explicitly authorized personal calendar. Upload the result to YouTube as
**Unlisted** and keep its URL; Google's form requires a YouTube link. The
unverified-app warning is expected and must remain visible in the recording.
The project currently has one OAuth client, the Desktop app client, and the
recording must show that client in use.

1. Open the public product page, privacy policy, and terms on
   `lastrefuge.ai`. Show that they load without authentication and that the
   privacy policy identifies each category of Google data and its use.
2. Launch a clean Omarchy Calendar profile. Show the welcome screen and choose
   **Connect Google Calendar**.
3. Keep recording as the app opens Google in the browser. Show the account
   chooser, the Omarchy Calendar identity, any unverified-app warning, and the
   complete consent workflow in English. Before granting access, expand every
   permission detail and any section describing access already granted. Pause
   long enough to read the email/identity permissions, calendar-list permission,
   and event read/write permission. Keep the address bar visible. If Google
   collapses previously granted permissions and they cannot be shown clearly,
   use a fresh test account or remove this app's grant from the test account
   before starting a new take. Do not remove a daily-use account's grant just
   to reset the demo. Only continue after the permission evidence is captured.
4. Return to the application. Show the connected account label and synchronized
   calendar list, then briefly disable the network and show that previously
   synchronized events remain available locally.
5. Create a clearly named test event. Edit its time and details, add or respond
   to an invitation if the test setup permits, then show the same change in
   Google Calendar.
6. Delete the test event and show that deletion reaches Google Calendar.
7. Disconnect the account in Omarchy Calendar. Explain that the local account
   cache and Secret Service refresh token are removed, and show the returned
   disconnected state.

Keep the recording concise. Narrate which permission enables each visible step;
do not add marketing material or unrelated product features.

Google's follow-up specifically rejected the original video because the requested
scopes were not visible during consent. Showing source code or the Cloud Console
scope list alone does not address that finding. Verify the finished recording
contains the actual consent screens and working scope-dependent features before
uploading it. Text captions can map Google's permission descriptions to the exact
scope names above without covering the consent text.

## Replacement-video follow-up

Original submitted video: https://youtu.be/k07isTwB5aI. Google requested a new
recording showing the requested scopes in the OAuth consent workflow. The request
was reported on October 2, 2026. The replacement was published as Unlisted and
saved in Google Cloud on the same date. The reply to Google is prepared for
Jason to send in the existing verification thread.

Replacement video: https://youtu.be/-wH1yxoX1Fo (6:00, 1440×1080). YouTube
reported no issues in its checks; playback and Unlisted visibility were verified.
Google Cloud confirmed “Data access changes saved!” after replacing the URL.

| Video position | Evidence |
| --- | --- |
| 0:00 | Application welcome screen and Connect Google |
| 0:17 | Google account chooser and personal-account selection |
| 0:36 | Unverified-app warning and continuation |
| 1:06–2:39 | Actual consent screen with all four permissions expanded and explained |
| 2:39–3:09 | Consent completion and successful local callback |
| 3:09 | Calendar list and synchronized events |
| 3:31 | Temporary event creation on the personal calendar |
| 4:35 | Read and edit the test event |
| 5:19 | Delete the test event |
| 5:43 | Connected account and synchronized calendars |

The opening was re-recorded to keep the account chooser in frame. Idle pauses
were shortened. The callback address and unrelated event details were hidden.
The test event was removed, and the service reported no pending mutations or
mutation errors. This demonstration does not establish OAuth approval.

Upload the reviewed replacement to the same YouTube channel as **Unlisted**.
Confirm the full video is processed, readable, and accessible without a channel
login. Update the demo-video URL in Google Auth Platform's Data Access page and
reply in the existing verification email thread. Keep the old video until the
replacement has been reviewed; do not claim Google approval from an upload alone.

Prepared reply (not sent to Google):

> Hello Google OAuth Verification Team,
>
> Thank you for your feedback. I have re-recorded the Omarchy Calendar demo and
> updated the demo-video link in our verification submission.
>
> Updated video: https://youtu.be/-wH1yxoX1Fo
>
> The updated video shows the application from its welcome screen, the complete Google OAuth
> consent workflow in English with the requested permissions visible, and how
> the application uses those permissions to identify the connected account,
> display its calendar list, and read, create, edit, and delete calendar events.
>
> OAuth consent and requested permissions: 0:17–3:09 (expanded scopes: 1:06–2:39)
> Application use of the requested permissions: 3:09–6:00
>
> Please use this video in place of the previous demo at
> https://youtu.be/k07isTwB5aI for project `omarchy-calendar-509223`.
>
> Thank you,
> Jason Alexander

Reference: [Google's demo-video requirements](https://support.google.com/cloud/answer/13804565).

## Final console sequence

- [x] Verify `lastrefuge.ai` in Search Console through the Cloudflare DNS flow.
- [x] Save `jason@greatspark.com` as a project Editor.
- [ ] Select `jason@greatspark.com` as the OAuth user-support email if Google
      makes it eligible; otherwise use a Google Group at the public support
      address and document its ownership.
- [x] Upload the prepared 120×120 application icon in Branding and save.
- [x] Reopen Branding and Data Access to confirm the production URLs, authorized
      domain, support contact, icon, and exact four-scope set.
- [x] Verify and publish the OAuth branding.
- [x] Publish the app from Testing to Production.
- [x] Enter the sensitive-scope justification in Google's review form.
- [x] Record the initial demonstration and provide its unlisted YouTube URL.
- [x] Complete the brand and sensitive-scope verification form using the scope
      explanations above, attach the unlisted demo-video URL, and submit.
- [x] Replace the demo after Google's scope-visibility feedback and save the new
      URL in the verification request.
- [ ] Send the prepared reply in the existing Google verification email thread.
- [ ] Record the submission date, Google's case/reference number, and every
      follow-up request in this document.

## Approval and release record

| Milestone | Date | Evidence or reference |
| --- | --- | --- |
| Domain verified | 2026-09-21 | Google Search Console, Domain name provider method |
| Branding verified and published | 2026-09-21 | Google Auth Platform automated branding verification |
| App moved to production | 2026-09-21 | Google Auth Platform Audience |
| Verification submitted | Date not recorded | Initial demo: https://youtu.be/k07isTwB5aI |
| Replacement demo requested | Reported 2026-10-02 | Google: requested scopes not shown in OAuth consent workflow |
| Replacement demo published and saved | 2026-10-02 | https://youtu.be/-wH1yxoX1Fo; Google Cloud save confirmed |
| Google follow-up answered | Pending Jason's reply | Reply prepared below replacement-video record |
| Verification approved | Reported 2026-10-04 | Publisher confirmation and Google Verification Center: branding and data access verified |
| Production lifecycle retest passed | — | — |
| `v1.0.0` released | — | — |

After approval, run the real-account create, update, RSVP, delete, resync, and
revocation checks from the public-release checklist. Only then bump the release
metadata to `1.0.0` and create the `v1.0.0` tag.
