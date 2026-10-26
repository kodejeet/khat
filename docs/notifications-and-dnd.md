# Notifications and GNOME Do Not Disturb

Verified on Ubuntu 26.04.1 LTS with GNOME Shell 50.1 on 2026-10-26.

## Delivery and click behavior

- Delivery uses the freedesktop.org `org.freedesktop.Notifications` session-bus
  interface through QtDBus. No browser is started when a message arrives.
- Each notification shows the sender (display name when the `From` header has
  one), the subject, an optional Gmail snippet, and the account email only when
  more than one account exists.
- The snippet is collapsed to one line and truncated to 180 UTF-16 code units;
  the subject is limited to 200 and the sender to 120. An ellipsis marks
  truncation.
- Message text is HTML-escaped because GNOME treats the notification body as
  Pango markup. Only the Gmail `snippet` field is fetched, never the full body.
- Normal urgency (`1`) is always used. The app does not request critical
  urgency, does not use `resident` or `transient`, and does not replace system
  notification policy.
- The notification supplies the `default` action "Open in Gmail". Clicking the
  notification or the action emits `ActionInvoked`; the app opens
  `https://mail.google.com/mail/?authuser=<account>#inbox/<message-id>`.
  URLs are restricted to `https://mail.google.com` before opening. If account
  information is unavailable, the generic `/mail/u/0/` URL is used.
- Delivery failures are exposed as `notificationFailed`/`failed` signals. The
  dispatcher still marks a message as notified once the notification server
  accepts the call, preserving at-most-once behavior across crashes.

Server capabilities observed with `gdbus`:

```
actions, body, body-markup, icon-static, persistence, sound
```

## Do Not Disturb investigation

Read-only checks performed:

```sh
gsettings get org.gnome.desktop.notifications show-banners
gsettings get org.gnome.desktop.notifications show-in-lock-screen
gsettings list-recursively org.gnome.desktop.notifications
```

Results on the installed session:

```
show-banners = false
show-in-lock-screen = false
```

GNOME Shell 50.1 provides relocatable per-application settings at
`org.gnome.desktop.notifications.application:/org/gnome/desktop/notifications/application/<id>/`
with keys including:

- `enable` — whether the application's notification source exists;
- `show-banners` — per-application banner preference;
- `enable-sound-alerts`;
- `force-expanded`;
- `show-in-lock-screen`;
- `details-in-lock-screen`.

The Shell 50.1 implementation (`js/ui/messageTray.js`) combines these settings
with the global policy:

```js
get showBanners() {
    return this._masterSettings.get_boolean('show-banners') &&
        this._settings.get_boolean('show-banners');
}
```

It also suppresses banners when the session presence is busy unless the
notification has critical urgency. Therefore a per-application `show-banners`
setting cannot override global DND/banner suppression: both must be true.
`enable` can remove a source entirely, but cannot bypass DND. Critical urgency
is the only documented banner bypass, and this application intentionally does
not use it.

**Conclusion:** there is no supported per-application DND bypass for standard
freedesktop notifications on GNOME Shell 50.1. DND behavior is respected; the
app uses normal urgency and never changes GNOME settings. DND bypass remains a
documented limitation, not a requirement.

The per-application schema applies once GNOME can associate notifications with
an installed desktop entry/application ID. Session/desktop-file integration is
handled with the background-startup work; it does not change the DND finding
above.

## Test coverage

- `test-notifications` uses an in-process fake backend and a fake Gmail server.
  It verifies formatting, escaping/truncation, URL validation, click-to-open,
  preview toggle, notification disable, account labels, dedupe, failure
  handling, and dispatcher/sync integration.
- `test-freedesktop-notification` runs under `dbus-run-session` with a fake
  `org.freedesktop.Notifications` service. It verifies the real QtDBus call
  arguments (normal urgency, actions, body), click activation, and graceful
  failure when no notification server exists. The test disables D-Bus service
  auto-start so it cannot launch a real notification daemon.
- No test changes GSettings/dconf, enables DND bypass, or marks notifications
  critical.
