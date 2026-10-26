# Resource use, API cost, and troubleshooting

This document covers what the notifier sends to Google, how much it stores, how
to diagnose common failures, and how it behaves with suspend, logout, and Do
Not Disturb.

All figures below are estimates based on the current implementation and
Google's published limits on 2026-10-26. They are not a promise of future quota,
billing, or notification behavior.

## Check lifecycle

Each account is synchronized independently, but one scheduled check fans out to
all enabled accounts that have an access token.

1. **Startup check.** The scheduler performs an immediate check when it starts.
   If an account has never completed a successful synchronization, the first
   successful one is a **baseline**.
2. **Baseline.** The app lists every message matching the Gmail query and records
   those IDs as already seen. It does not fetch metadata and does not notify.
   Existing mail therefore cannot produce a burst of notifications.
3. **Regular check.** The app lists matching messages from the last successful
   synchronization minus a 24-hour overlap. IDs already recorded are skipped.
   Only unseen IDs get one metadata request each. New rows are inserted before
   notifications are dispatched.
4. **Notification.** A new matching message produces one notification. A
   message ID is unique per account, so repeated checks and restarts do not
   notify twice.
5. **Next check.** A successful check schedules the next one after the
   configured interval (5–1440 minutes, default 180). Any failed account makes
   the whole check unsuccessful for scheduling purposes, so the next attempt
   uses bounded exponential backoff. A later success resets the backoff.

The query is:

```
in:inbox -category:promotions -category:social -category:forums
```

When **Include Updates** is off, `-category:updates` is appended. Gmail excludes
Spam and Trash by default. The app also re-checks `INBOX`, `SPAM`, `TRASH`, and
category labels after fetching metadata, so a message that is archived or
reclassified between listing and fetching is not notified.

Read/unread state does not matter: a newly received matching message is
notified even if you read it before the next poll. Sent mail and drafts are not
in the query result set.

## API requests and quota

Gmail API quotas are measured in quota units, not requests. The current
per-method costs used by this app are:

| Operation | Quota units | When |
| --- | ---: | --- |
| `users.messages.list` | 5 per page | Every check (plus extra pages if needed) |
| `users.messages.get` (`format=metadata`) | 20 per message | Only for a not-yet-seen matching message |
| `users.getProfile` | 1 | Once per interactive authorization |

Google's published limits checked for this project are 1,200,000 units/minute
per project, 6,000 units/minute per user per project, and a daily billing
threshold of 80,000,000 units per project. See
<https://developers.google.com/workspace/gmail/api/reference/quota>. Google may
adjust these limits and describes the daily figure as a billing threshold rather
than a hard free quota.

### Default-volume estimate

A 180-minute interval gives approximately eight scheduled checks per account
per day, excluding the startup check, manual checks, reconnects, and retries.
Counting one startup check, one account uses roughly nine list requests per day
when no new mail arrives:

| Scenario per day | Requests | Quota units |
| --- | ---: | ---: |
| No new matching mail | 9 list | 45 |
| 20 new matching messages | 9 list + 20 metadata | 445 |
| 100 new matching messages | 9 list + 100 metadata | 2,045 |

These numbers assume one list page per check. More than 100 matching messages
in the 24-hour overlap require another list page (5 units each). A very large
first-run inbox can also need several list pages for the baseline; baseline
pages cost 5 units each and fetch no message metadata. Multiply all numbers by
the number of connected accounts.

Even the 100-new-message example is a tiny fraction of the published per-minute
and daily limits. Pagination, retries, and manual refreshes add requests. Do not
describe polling as free of Google quota or OAuth restrictions.

### OAuth token requests

Token exchange, refresh, and revocation use `oauth2.googleapis.com`, not the
Gmail API, so they are not counted in the Gmail quota-unit table above. They are
still subject to Google's OAuth rate limits and availability. After the
credential-dependent step is complete, expect roughly one refresh per account
per access-token lifetime (often about an hour) while the app is running, plus
retries after failures. The automated tests use fake OAuth endpoints and do not
make these requests.

### Backoff and offline behavior

- Network errors, HTTP 429, rate-limit/quota 403s, and 5xx responses are
  retryable.
- Retry delays grow 60 s, 120 s, 240 s, and so on, capped at 30 minutes.
- A `Retry-After` header can extend a delay, but never beyond the 30-minute cap.
- Manual checks are refused while a check is already running, preventing
  overlapping jobs.
- While the system reports offline, scheduled checks are deferred without HTTP
  requests. Reconnecting starts an immediate check.
- A misleading "online" signal cannot cause unbounded retries: actual request
  failures still drive backoff.

## Resource use

### Memory

The application is one Qt process. On the tested system, with no accounts and
an offscreen Qt platform, idle RSS measured approximately **34,640 KiB
(33.8 MiB)**. This includes shared Qt library pages and is a rough baseline, not
a guarantee for a live GNOME session. A sync holds at most the current page of
message IDs and metadata; full bodies and attachments are never downloaded.

No CPU benchmark is claimed. Between checks the process is event-driven: a
single-shot scheduler timer, the Qt event loop, D-Bus connections, and an
optional settings window. It does not busy-loop.

### Disk

| Data | Location | Approximate size |
| --- | --- | --- |
| App settings | `~/.config/gmail-notifier/config.json` | Under 1 KiB |
| OAuth client | `~/.config/gmail-notifier/oauth-client.json` | Under 1 KiB |
| SQLite state | `~/.local/share/gmail-notifier/state.db` | Grows with seen messages |
| Tokens | Secret Service / GNOME Keyring | One small item per account |

The SQLite state stores one row per seen message per account: two IDs, two
timestamps, and a notification flag. Including SQLite page and index overhead,
allow roughly 100–200 bytes per message; a 10,000-message history is typically a
few megabytes. There is currently **no automatic pruning** of `seen_messages`.
The database uses WAL mode, so short-lived `state.db-wal` and `state.db-shm`
files can also exist. WAL is checkpointed by SQLite during normal use.

Only message IDs, internal dates, first-seen times, notification flags, and
per-account synchronization status are persisted. Subjects, snippets, senders,
and message bodies are not written to the database. Notification text is sent
over the session bus to GNOME, which may keep its own notification history
according to the desktop's settings.

### Network

A no-new-mail check downloads one field-masked list response, typically a few
KiB. Each new message adds one metadata response containing only `From`,
`Subject`, `internalDate`, labels, ID, and `snippet`, typically a few KiB. Exact
sizes depend on Gmail and header lengths. No attachments, MIME parts, or full
bodies are requested. HTTPS certificate validation uses Qt's normal system
trust store.

## Status and logs

The **Status** tab shows the last successful synchronization, next scheduled
check, online/offline state, and the last synchronization error. These values
come from the scheduler and persisted account state and survive restarts.

The accounts list shows per-account synchronization status and the persisted
last error. Closing the settings window does not stop polling.

For the systemd service:

```sh
journalctl --user -u gmail-notifier.service -b --no-pager
journalctl --user -u gmail-notifier.service --since "24 hours ago" --no-pager
```

Application logs contain operational errors such as HTTP status, configuration
problems, and notification delivery failures. They must not contain access
tokens, refresh tokens, OAuth client secrets, or email bodies. Do not enable
verbose network logging in a bug report without reviewing it first.

## Troubleshooting

### The app starts but never checks Gmail

Until OAuth and Secret Service are finished, this is expected: the app starts
idle. Once they are available, verify `~/.config/gmail-notifier/oauth-client.json`
exists, has valid non-placeholder values, mode `0600`, and the exact
`gmail.readonly` scope. Also confirm at least one account is present and
enabled. The startup log distinguishes "no accounts", "accounts not authorized",
and "no enabled accounts are authorized". See
[Google Cloud and OAuth setup](google-oauth-setup.md).

### `--check-now` reports no authorized accounts

Either no account is configured, an account is disabled, or no usable access
token is available. Reauthorize the account if its refresh token was revoked or
expired. A Testing-mode OAuth app loses refresh tokens seven days after
consent; see the OAuth guide.

### `Cannot load configuration: ...`

The app refuses to start with malformed or invalid `config.json` rather than
silently overwriting it. Check the reported key, then fix the JSON or move the
file aside if you intentionally want defaults. Settings are validated for
interval bounds, account ID/email uniqueness, and the exact scope. Keep a backup
before editing.

### `Cannot open state database` or SQLite errors

Check that `~/.local/share/gmail-notifier` exists, is writable by your user, and
has not been replaced with a file. The Qt SQLite driver (`QSQLITE`) must be
available; `qt6-base-dev` normally provides it. Do not loosen permissions to
world-writable. If the database is corrupt, stop the app, back up `state.db` and
any WAL/SHM files, and inspect with the `sqlite3` CLI if installed
(`PRAGMA integrity_check;`). Deleting state causes the next successful check to
become a new baseline, so recent messages will be marked seen without
notifications.

### `invalid_grant`, repeated 401, or "reauthorize"

The refresh token is expired, revoked, or invalid. Common causes: Testing-mode
seven-day expiry, access removed at
<https://myaccount.google.com/permissions>, a deleted or rotated OAuth client,
or an account policy change. Use **Reauthorize**. If the OAuth client itself was
deleted, create a new Desktop client and update `oauth-client.json`.

### `redirect_uri_mismatch`

The OAuth client is probably a **Web application** client rather than a
**Desktop app** client, or the redirect URI sent in the authorization and token
requests differs. Create a Desktop client. The app uses
`http://127.0.0.1:<ephemeral-port>` consistently for both requests.

### `access_denied`, `admin_policy_enforced`, or `org_internal`

The user cancelled consent, the account is not a test user, the app is Internal
but the account is outside the organization, or a Workspace administrator blocks
the scope. Add the account as a test user for Testing mode, or have the
administrator allow the client. The application cannot override these policies.

### Gmail API disabled or access denied (HTTP 403)

Enable the Gmail API in the same Cloud project as the OAuth client. A 403 can
also mean the wrong account authorized, a missing scope, a Workspace restriction,
or rate limiting. The app's error message distinguishes rate-limit/quota reasons
when Gmail supplies them.

### Rate limits, quota errors, or repeated server failures

The scheduler backs off automatically and caps retries at 30 minutes. Check the
Cloud console quota usage and Gmail API status. A shorter polling interval
increases requests; the minimum is 5 minutes, but the default 180 minutes is
appropriate for a notifier. Do not repeatedly use `--check-now` during an
outage.

### Offline behavior

While offline, the app defers checks rather than failing repeatedly. Manual
checks are refused with a message that the check will run after reconnection.
Reconnecting starts a check immediately. If the desktop's online signal is
wrong, real request failures still trigger backoff. Check NetworkManager and
`systemctl --user status gmail-notifier.service` if checks never resume.

### Notifications do not appear

Check, in order:

1. The **Notifications** tab has desktop notifications enabled.
2. GNOME's global notification banners are enabled:
   `gsettings get org.gnome.desktop.notifications show-banners`.
3. The per-application notification source is enabled in GNOME Settings.
4. A notification service is present on the session bus
   (`org.freedesktop.Notifications`).
5. Do Not Disturb or a busy presence is active.

The app uses normal urgency and does not bypass Do Not Disturb. See
[Notifications and Do Not Disturb](notifications-and-dnd.md). Notification
failures are logged but do not stop polling; a message whose delivery failed is
marked seen and is not automatically retried.

### Duplicate notifications

The state database deduplicates by `(account_id, message_id)` and marks a
notification after the backend accepts it. Duplicates should not occur across
normal polling or restarts. Do not delete or replace `state.db` while trying to
fix duplicates; losing it forces a new baseline and can cause messages arriving
before that baseline to be missed. If duplicates persist, record the time,
account count, and whether the database or config was moved, but do not attach
email content.

### Clicking a notification does not open Gmail

The click action opens
`https://mail.google.com/mail/?authuser=<account>#inbox/<message-id>` through the
desktop's default URL handler. Verify a default browser is configured and that
the browser can open Gmail. The app validates that the URL host is
`mail.google.com`; it does not open arbitrary links from messages.

### The service does not start at login

The unit is installed but not enabled. Run:

```sh
systemctl --user daemon-reload
systemctl --user enable --now gmail-notifier.service
systemctl --user status gmail-notifier.service
```

The unit is bound to `graphical-session.target`. If it starts before the
session's notification or keyring services are ready, `Restart=on-failure`
retries according to systemd policy. Check the journal. See
[Install, run, and remove Gmail Notifier](installation.md).

### A second launch exits immediately

That is intentional. One process owns a lock and a private socket; a second
`--settings` asks the first process to show its window, and a second
`--background` pings the owner and exits. If the owner is unhealthy, stop the
stale process and remove only the socket under
`$XDG_RUNTIME_DIR/gmail-notifier/` if it remains. Do not remove another
application's runtime files.

## Suspend, logout, and Do Not Disturb

- **Suspend/resume:** The app subscribes to logind's `PrepareForSleep` signal.
  It pauses polling before suspend unless a check is already in flight; after
  resume it starts an immediate check. If logind or the signal is unavailable,
  it logs that suspend monitoring is unavailable and continues with normal
  timers. Real suspend was not triggered during development; the bridge was
  tested with a fake logind service.
- **Logout:** The systemd user unit is `PartOf=graphical-session.target` and is
  stopped at logout. SIGTERM/SIGINT/SIGHUP are converted into a normal Qt quit,
  which stops the scheduler, closes the socket and lock, and flushes SQLite
  state. The settings window is not required for shutdown.
- **Do Not Disturb:** GNOME 50.1 combines the global `show-banners` setting with
  the per-application setting, so a per-app preference cannot override global
  DND. The app deliberately uses normal urgency and never changes GNOME
  settings. Critical urgency is not used merely to bypass DND.

## Backup, reset, and privacy

- Back up `config.json` and `oauth-client.json` as sensitive files. Stop the
  service before copying `state.db`; include `state.db-wal` and `state.db-shm`
  if present, or use `sqlite3 state.db ".backup backup.db"` when the CLI is
  available. A copy of only the main database while WAL is active can miss
  recent state.
- To reset synchronization history, stop the service, move `state.db` and its
  WAL/SHM files aside, and restart. The next successful check becomes a new
  baseline and will not notify about existing messages. Keep the config and
  tokens if you only want a baseline reset.
- The app requests only `gmail.readonly`. It does not store bodies, does not
  send telemetry, and has no backend. Google still receives API requests, and
  GNOME's notification service receives the sender, subject, and optional
  snippet for display. Disable previews if lock-screen or notification-history
  exposure is a concern.
- Remove the Google grant at <https://myaccount.google.com/permissions> when
  retiring the app, then delete local data using the uninstall steps.

## Related documentation

- [Install, run, and remove Gmail Notifier](installation.md)
- [Google Cloud and OAuth setup](google-oauth-setup.md)
- [Notifications and Do Not Disturb](notifications-and-dnd.md)
- [GNOME Online Accounts investigation](goa-investigation.md)
