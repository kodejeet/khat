# Architecture and internals

This document explains how khat is put together: the process model, the
subsystems and their sequence of operations, what is stored where, and how the
behavior is tested. It complements [operations.md](operations.md) (resource
use, quota, troubleshooting) and [notifications-and-dnd.md](notifications-and-dnd.md).

```
                    ┌──────────────────────── GNOME session ────────────────────────┐
                    │                                                              │
  config.json ──▶   │  main.cpp ─▶ AppController ─┬─▶ SyncScheduler ─▶ SyncService  │
  oauth-client.json │        │                    │            │            │      │
                    │        ├─▶ OAuthManager ◀───┼────────────┘ (token)    │      │
                    │        │        │           │                        ▼      │
                    │        │        ▼           │                 GmailClient──┼──▶ Google (HTTPS)
                    │        ├─▶ StateDatabase ◀──┴──▶ NotificationDispatcher      │
                    │        ├─▶ NotificationService ─▶ FreedesktopNotification    │
                    │        ├─▶ SingleInstance ─▶ .../khat/instance.sock          │
                    │        ├─▶ PowerMonitor ─▶ org.freedesktop.login1            │
                    │        │                  └▶ GNOME Shell (notifications, DND)│
                    └─────────────────────────────────────────────────────────────┘
                                  ▲                    ▲
                     NetworkManager │            Secret Service (keyring)
```

## Process model

One Qt event loop, no worker threads. Every long-running step (HTTP, D-Bus,
SQLite) is asynchronous: Qt network replies, `QDBusPendingCallWatcher`, and
single-shot `QTimer`s feed the loop. A sync therefore never blocks the UI or
the notification backend, and the process stays idle while it waits for the
next scheduled check.

- **Single instance** (`src/app/SingleInstance.{h,cpp}`): the first process
  creates `$XDG_RUNTIME_DIR/khat` (owner-only) and starts a private
  `QLocalServer` at `instance.sock`. Later launches forward `--settings` /
  `--check-now` to the running process over that socket and exit
  immediately.
- **Unix signals** (`src/app/UnixSignalHandler.{h,cpp}`): SIGTERM/SIGINT/SIGHUP
  are converted into a normal Qt quit so state is flushed by the regular
  shutdown path.
- **PowerMonitor** (`src/app/PowerMonitor.{h,cpp}`): subscribes to logind's
  `PrepareForSleep`, so the scheduler can pause before suspend and run an
  immediate check on resume.

## Modules

| Directory | Responsibility |
| --- | --- |
| `src/app` | Controller wiring, single-instance socket, suspend handling, signal handling |
| `src/config` | `config.json` and `oauth-client.json` store/parsing, `AppPaths` |
| `src/auth` | OAuth (PKCE loopback), token client, Secret Service storage, proactive refresh |
| `src/sync` | Scheduler (intervals/backoff), sync job state machine, exponential backoff |
| `src/gmail` | Gmail API client: history/list/metadata/profile requests, error mapping, query building |
| `src/state` | SQLite persistence (accounts, seen messages, history cursor) |
| `src/notify` | Notification content builder, dedupe dispatcher, freedesktop D-Bus backend |
| `src/ui` | The minimal settings window |
| `src/connectivity` | NetworkManager online/offline indication (simulatable) |
| `src/model` | The `MessageSummary` value type shared by all subsystems |

## Startup sequence

`main()` parses `--settings` / `--background` / `--check-now`, then:

1. `AppController` resolves paths (`~/.config/khat`, `~/.local/share/khat`,
   `$XDG_RUNTIME_DIR/khat`) and opens the SQLite state database.
2. `OAuthManager::loadTokens()` asks to load the tokens for each configured
   account from Secret Service in parallel.
3. `applyConfiguration()` computes, per account, whether an access token is
   available; the scheduler then runs the first check immediately (per
   account baseline, see below) and schedules the next one by the configured
   interval.
4. The single-instance socket and the power monitor are installed so a second
   launch cannot double-poll, and suspend/resume keeps checks aligned.
5. `--settings` shows the window; `--check-now` is forwarded to the scheduler;
   `--background` stays headless. In every mode the settings window survives
   being closed — polling continues.

## Configuration files

- `~/.config/khat/config.json`: accounts
  (`{id, email, display_name, enabled}`) plus
  `notifications {enabled, include_updates, show_preview,
  poll_interval_minutes}`. Written atomically by `ConfigStore` on every
  settings change ("Changes are saved automatically").
- `~/.config/khat/oauth-client.json`: the
  Google OAuth *Desktop* client (fields of a downloaded `client_secret.json`
  are accepted directly). Mode 0600, never committed;
  `config/oauth-client.example.json` shows the shape.

## OAuth subsystem

`OAuthManager` (`src/auth`) coordinates authorization and token lifetime and
is the only gate through which UI and scheduler obtain an access token.

- **Authorization**: fresh state + PKCE per attempt, a temporary loopback HTTP
  server (`src/auth/OAuthLoopbackServer.{h,cpp}`, ephemeral port) receives
  Google's redirect, `OAuthTokenClient` exchanges the code against
  `oauth2.googleapis.com/token`, and a profile read identifies the mailbox
  before an account is accepted: a duplicate mailbox reuses its stored
  account id, and authorizing a different mailbox than the configured one is
  rejected in tests (`reauthorizeRejectsDifferentAccount`).
- **Storage**: `OAuthTokenStore`-based — the production store
  (`SecretServiceTokenStore`) talks to `org.freedesktop.secrets` over
  QtDBus and keeps one collection item per account (`khat token...`,
  attributes `application=khat`), schema `org.freedesktop.Secret.Generic`.
  Tokens are serialized `OAuthToken` JSON inside the item's secret, never in
  files or logs. Tests inject a fake store.
- **Lifetime**: `accessToken()` never hands out an expired (or missing)
  token. Timers run `scheduleProactiveRefresh` ~5 minutes before the known
  expiry (and immediately when an expired token is loaded), so Gmail requests
  almost never start with a 401. Failures that Google marks
  `invalid_grant`/`unauthorized_client` (the refresh token is truly dead)
  raise `authorizationRequired`, drop the cached token, delete the keyring
  item, and the UI shows "use Reauthorize".
- **Revocation**: removing an account calls
  `oauth2.googleapis.com/revoke` for the stored grant and then deletes the
  keyring item.

## Sync subsystem

Two cooperating classes, plus `BackoffPolicy`.

### SyncScheduler (`src/sync/SyncScheduler.cpp`)

- Owns the single-shot check timer; interval clamp 5–1440 minutes.
- One check fans out to every enabled account that currently has an access
  token; a check runs while *any* account job is active and finishes when all
  accounts reported (`checkFinished`).
- Failed checks schedule the next attempt with bounded exponential backoff
  (60 s base doubling, 30-minute cap, `Retry-After` respected but capped);
  any later success resets backoff.
- While offline (per `ConnectivityMonitor` or manual simulation), checks are
  *deferred* — no requests happen; reconnecting triggers an immediate check.
- Suspend/resume: checks are paused before suspend and resumed immediately on
  wake.

### SyncService job state machine (`src/sync/SyncService.cpp`)

Each account gets a `Job` with stages:

```
                 ┌──────────────────────────────────────────────┐
 history cursor >│  History                                     │
 exists?         │  (users.history.list, messageAdded, INBOX)   │
      │ no?      └───────────────┬─────────────── 404 ────────┬──────────────────┐
      ▼                          │ ok                         ▼                  │
 ┌────────────┐                  ▼                    ┌───────────────┐          │
 │  Listing   │◀──────────────────┘                     │ (continue)    │          │
 │ 24 h overlap│        dedupe ↓                        └───────────────┘          │
 └────────────┘                                       ▼                          │
      │ all pages                            unseen ids only                   │
      │ profile: capture cursor                                                │
      ▼                                                                        │
 ┌──────────────┐   metadata ×n   ┌──────────────────────────────────────────┐  │
 │  Metadata    │──────────────▶  │ finalize: INSERT rows, markNotified,     │  │
 │ (messages.get│                 │ advance cursor, emit newMessages         │  │
 │ format=metadata)              └──────────────────────────────────────────┘  │
 └──────────────┘                                                              │
      baseline path ───────────────────────────────────────────────────────────┘
```

- **History stage (the fast path)**: with a stored cursor
  (`startHistoryId`), one field-masked `users.history.list` call
  (`historyTypes=messageAdded`, `labelId=INBOX`) returns *exactly* the inbox
  additions since the last check — no full listing, no dependence on Google's
  search index, so manual checks surface fresh mail immediately. The reply's
  `historyId` becomes the next cursor. Multi-page feeds are followed via
  `nextPageToken` with loop protection.
- **Listing stage (baseline and fallback)**: an account without a baseline,
  or whose cursor aged out (Google answers HTTP `404`), runs the
  `in:inbox -category:promotions -category:social -category:forums` query
  (optionally `-category:updates`), bounded by an `after:` date of the last
  successful sync minus a 24-hour overlap. A baseline records the ids as seen
  *without* fetching metadata — existing mail is never notified retroactively.
- **Cursor capture**: after every listing-based success (baseline or
  fallback), one field-masked `users.getProfile` call
  (`fields=emailAddress,historyId`) stores a fresh cursor. History-based
  checks carry their cursor in the response and need no extra request.
- **Metadata stage**: only unseen, still-inbox ids are fetched
  (`format=metadata`, `metadataHeaders=From&Subject`), and the labels are
  re-checked client-side so a message that moved to spam/trash/promotions
  between listing and fetch is not notified. Results and the notification
  flag are recorded in one atomic `recordMessages` call, and `newMessages`
  is emitted for the dispatcher.
- **Overlaps are safe**: `(account_id, message_id)` deduplicates across
  checks, restarts, and the 24-hour overlap window.

## Gmail client layer (`src/gmail`)

- `GmailClient` builds one request at a time against `gmail.googleapis.com`
  (replaceable base URL for tests) with a Bearer token and strict field
  masks, keeping responses small.
- `GmailQuery` composes the search query; int64 fields (internalDate,
  historyId) accept both JSON string and number forms.
- `errorFromReply` maps HTTP outcomes into `GmailErrorKind` (unauthorized,
  rate limited, quota exceeded, forbidden, not found, rate-limit hint via
  `Retry-After`, network, invalid response) so the scheduler and UI can react
  without string matching.

## Notification pipeline (`src/notify`)

1. `SyncService::newMessages(accountId, QList<MessageSummary>)` feeds
   `NotificationDispatcher`.
2. The dispatcher re-checks the database (per-account `notified` flag) so two
   dispatch paths can never double-notify.
3. `buildNotificationContent` composes the display text:
   - **title**: sender display name (falls back to the bare address, then
     "New Gmail message"), truncated with an ellipsis;
   - **body lines**: subject (≤120 chars), preview (≤160 chars, cut at the
     last sentence boundary inside the limit with `…` only when text was
     cut), optional account line; everything is single-lined, Pango-escaped,
     and the click URL is `mail.google.com/mail/?authuser=<account>#inbox/<id>`.
4. `NotificationService` hands it to the `NotificationBackend`; the production
   backend talks to `org.freedesktop.Notifications` (hints: default timeout,
   "Open in Gmail" action, normal urgency — deliberately not critical, so DND
   is respected). The success path marks the row `notified`; a failed display
   does not retry but is logged.

## Persistence (`src/state/StateDatabase.{h,cpp}`)

SQLite (`QSQLITE`, WAL mode), one connection per `StateDatabase` instance,
owner-only file permissions, schema in `PRAGMA user_version`:

- **v2** (current): `accounts(account_id PK, email, baseline_done,
  last_sync_epoch, last_attempt_epoch, last_error, history_id)` and
  `seen_messages(account_id, message_id, internal_date, first_seen_epoch,
  notified, PK(account_id, message_id) + date index)`.
- v1 databases gain the `history_id` column by an in-place
  `migrateSchema()` (rows preserved); `history_id 0`/NULL means "no cursor —
  use the listing path".
- Nothing that appears in a notification (sender/subject/snippet) is stored:
  the database keeps ids, timestamps, and flags only. Bodies and attachments
  are never downloaded anywhere in the pipeline.

## Threading re-entrancy notes

- Each sync job owns its `GmailClient`; completion handlers verify the job is
  still registered under the account id, so a removed/replaced account cannot
  resurrect a finished job's callbacks.
- Account-level mutual exclusion: `syncAccount()` refuses an account already
  syncing ("already running"), which is what makes manual checks safe while a
  scheduled check is in flight.
- `OAuthManager::refreshAccessToken()` guards in-flight refreshes the same
  way, so the scheduler's 401 fallback and the proactive timer can never send
  two token requests at once.

## Testing strategy

All behavior is hosted in unit tests (`tests/`, QtTest): 18 targets exercise
the config store, scheduler (timers/backoff/offline deferral), sync state
machine, dedupe, notification content/dispatch/backend, connectivity, power
bridge, single-instance protocol, unix-signal shutdown, OAuth PKCE/loopback/loop
flows, proactively refreshed tokens, schema migration, and the settings window
(widget tests).

Network-facing classes are driven by local doubles:

- `tests/support/FakeGmailServer.h`: a minimal TCP HTTP/1.1 server speaking
  fake Gmail JSON on loopback — handlers script status/429-`Retry-After`/
  pagination/token flows, and request-history assertions ignore the meta
  profile read via `syncRequestCount()`.
- Fake secret-service: a private D-Bus session serving
  `org.freedesktop.secrets` so real keyrings are never touched.
- `FakeOAuthTokenStore` / `FakeTokenProvider`: in-memory stores so tests never
  write personalized data.

Everything runs with `ctest --test-dir build`; no test contacts Google.
