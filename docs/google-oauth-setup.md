# Google Cloud and OAuth setup

This guide describes the Google Cloud configuration and local OAuth client file
needed by Gmail Notifier. It follows Google's current documentation for
**Desktop app** OAuth clients and the authorization-code flow with PKCE.

Checked against Google's published documentation on 2026-10-26. Google can
change console labels, quotas, and verification rules; follow the links in this
document if the console differs.

## Current implementation status

- [implemented] The application settings, separate OAuth client file,
  placeholder detection, HTTPS endpoint validation, and exact-scope validation
  exist. See `config/oauth-client.example.json`.
- [planned] The browser authorization flow, PKCE token exchange, Secure
  Service/Keyring token storage, and Add/Reauthorize/Remove account actions are
  the final credential-dependent step. Until that work is complete, the
  application starts idle and makes no Gmail requests.
- [blocked] No real Google authorization, consent, token exchange, keyring
  write, or Gmail API call has been performed while building this project.
  Those actions require your Google Cloud client and explicit consent.

Do not paste real credentials into this repository, an issue, a chat, or a log.
The only file that should contain them is your local
`~/.config/gmail-notifier/oauth-client.json`, mode `0600`.

## What the application requests

The application requests exactly one OAuth scope:

```
https://www.googleapis.com/auth/gmail.readonly
```

That scope is classified by Google as a **restricted** Gmail scope. It permits
reading Gmail messages, but the application deliberately uses only:

- `users.messages.list` with the inbox/category query;
- `users.messages.get` in `metadata` format for `From`, `Subject`,
  `internalDate`, labels, and a short `snippet`;
- `users.getProfile` after authorization to learn the account's email address.

It does not request send, delete, modify, settings, IMAP/SMTP, or full-mailbox
access. There is no backend server, analytics, or telemetry. The flow uses a
loopback redirect on `127.0.0.1`; no public callback URL is required.

## 1. Create a Google Cloud project

1. Open <https://console.cloud.google.com/projectcreate>.
2. Create a project (for example, `gmail-notifier`) and select it.
3. Keep the project ID and project number for your own reference. The
   application does not need them.

A personal project is sufficient for this application. If you use a Google
Workspace account, your administrator may restrict third-party app access or
restricted Gmail scopes; that policy is outside this application's control.

## 2. Enable the Gmail API

1. Open <https://console.cloud.google.com/apis/library/gmail.googleapis.com>.
2. Confirm the correct project is selected.
3. Click **Enable**.

If the page asks you to enable billing, review Google's current terms. This
application uses no paid service, but Google controls Gmail API quota and
billing policy.

## 3. Configure the OAuth consent screen / Google Auth Platform

Google has renamed parts of this UI to **Google Auth Platform**. The exact
navigation varies, but the required settings are the same.

1. Open <https://console.cloud.google.com/auth/overview>.
2. If prompted, click **Get started** and provide:
   - an app name, such as `Gmail Notifier`;
   - a user support email;
   - a developer contact email;
   - the audience described below.
3. Under **Audience**, choose:
   - **External** for a normal personal Gmail account;
   - **Internal** only when the project belongs to a Google Workspace or Cloud
     Identity organization and every user is inside that organization.
4. For an External app in **Testing**, add each Gmail address that will
   authorize the app under **Test users**. A test user must be added before
   consent; otherwise Google returns an access error.
5. Under **Data access** (or **Scopes**), add exactly:

   ```
   https://www.googleapis.com/auth/gmail.readonly
   ```

   Do not add broader Gmail scopes. The application rejects any scope other
   than the exact read-only value.

You do not need to add a web redirect URI. The application uses a Desktop OAuth
client and an ephemeral loopback port.

### Testing mode

An External app whose publishing status is **Testing**:

- can have up to 100 test users;
- shows a warning that the app is unverified;
- expires each authorization **seven days after consent**. If the app receives
  an offline refresh token, that refresh token also expires. See
  <https://support.google.com/cloud/answer/15549945>.

For a personal notifier, Testing is the simplest starting point, but you will
need to reauthorize the account with **Reauthorize** about once a week.

### Production mode and the unverified-app cap

If you publish the app to **In production**:

- the seven-day Testing expiry no longer applies;
- an unverified app that requests restricted scopes still shows the
  "Google hasn't verified this app" warning;
- a project-wide cap of **100 new users** applies for the lifetime of the
  project when the unverified warning is shown. Google states that this cap
  cannot be reset or changed. See
  <https://support.google.com/cloud/answer/15549945#oauth-user-cap>.

Google's help says apps for **personal use with fewer than 100 users** may
continue without verification, with users clicking through the unverified-app
warning. That exception is not a statement that any use is exempt from Google's
API User Data Policy or from Workspace administrator controls. See
<https://support.google.com/cloud/answer/13464323>.

### Verification and restricted-scope review

If you distribute the app more broadly, or if Google determines that your use
requires review, an app requesting `gmail.readonly` must complete OAuth app
verification. Restricted scopes can also require an annual security assessment
through the App Defense Alliance's CASA framework. See:

- <https://support.google.com/cloud/answer/13464325> (restricted scopes)
- <https://support.google.com/cloud/answer/13464321> (verification requirements)
- <https://support.google.com/cloud/answer/13465431> (security assessment)

Verification and CASA are Google processes; this project does not automate or
guarantee them. They are generally unnecessary for a single-user local build
that stays within the personal-use/testing limits, but Google makes the final
determination.

## 4. Create a Desktop OAuth client

1. Open <https://console.cloud.google.com/auth/clients>.
2. Click **Create client**.
3. Select application type **Desktop app**.
4. Give it a name such as `Gmail Notifier Desktop` and click **Create**.
5. Download the JSON credentials immediately. Depending on when the client was
   created, Google may show the client secret only once. If you lose it, use
   the client-secret rotation action rather than creating unnecessary clients.

Do **not** choose **Web application**. Desktop clients are public clients and
Google recommends PKCE; the loopback redirect can use a random local port. A
web client expects pre-registered HTTPS redirect URIs and will fail with
`redirect_uri_mismatch`.

The downloaded file looks similar to this (values abbreviated):

```json
{
  "installed": {
    "client_id": "1234567890-abc.apps.googleusercontent.com",
    "project_id": "gmail-notifier",
    "auth_uri": "https://accounts.google.com/o/oauth2/v2/auth",
    "token_uri": "https://oauth2.googleapis.com/token",
    "auth_provider_x509_cert_url": "https://www.googleapis.com/oauth2/v1/certs",
    "client_secret": "GOCSPX-...",
    "redirect_uris": ["http://localhost"]
  }
}
```

The application does not read this nested file directly. Copy the required
values into its own flat configuration file in the next section. Do not commit
the downloaded file either.

## 5. Create the local OAuth client file

Create the configuration directory and a mode-`0600` file from the example:

```sh
install -d -m 700 "$HOME/.config/gmail-notifier"
install -m 600 config/oauth-client.example.json \
    "$HOME/.config/gmail-notifier/oauth-client.json"
${EDITOR:-vi} "$HOME/.config/gmail-notifier/oauth-client.json"
```

Replace all placeholders with values from your Desktop client:

```json
{
  "client_id": "1234567890-abc.apps.googleusercontent.com",
  "client_secret": "GOCSPX-your-desktop-client-secret",
  "auth_uri": "https://accounts.google.com/o/oauth2/v2/auth",
  "token_uri": "https://oauth2.googleapis.com/token",
  "scope": "https://www.googleapis.com/auth/gmail.readonly"
}
```

Fields:

| Field | Required value |
| --- | --- |
| `client_id` | The Desktop client ID; it must end in `.apps.googleusercontent.com` |
| `client_secret` | The Desktop client secret from the same client |
| `auth_uri` | `https://accounts.google.com/o/oauth2/v2/auth` |
| `token_uri` | `https://oauth2.googleapis.com/token` |
| `scope` | Exactly `https://www.googleapis.com/auth/gmail.readonly` |

Notes:

- Google treats desktop client secrets as non-confidential because they ship
  with installed software. This project still keeps the file mode `0600` and
  refuses to log it. The current validator requires `client_secret` because a
  downloaded Desktop client includes one; it is not an authorization token.
- The loader rejects placeholder text, a client ID with the wrong suffix,
  non-HTTPS endpoints (loopback HTTP is allowed only for local tests), broader
  scopes, and token fields such as `access_token` or `refresh_token`.
- Keep `oauth-client.json` separate from `config.json`. Account refresh tokens
  belong in Secret Service, never in either JSON file.
- To check only JSON syntax without displaying secrets:

  ```sh
  python3 -m json.tool "$HOME/.config/gmail-notifier/oauth-client.json" >/dev/null
  ```

  This prints nothing on success. Avoid commands that echo the whole file into
  terminal scrollback or shell history.

## 6. How authorization will work

The planned flow, once the credential-dependent step is implemented:

1. In **Settings → Accounts**, choose **Add Account...**.
2. The app generates a random PKCE verifier, an S256 challenge, and a random
   `state` value.
3. It starts a one-shot HTTP listener on `127.0.0.1` with an ephemeral port and
   opens your default browser at Google's authorization endpoint. The app does
   not embed a browser or read browser cookies.
4. You sign in and grant the single read-only Gmail scope. If the app is
   unverified, Google shows its warning screen first.
5. Google redirects the browser to the local listener with a code and the
   original `state`. The app rejects a mismatched state, then exchanges the code
   together with the PKCE verifier and the same loopback redirect URI.
6. The app uses the access token to read the Gmail profile email address, stores
   the refresh token in Secret Service, and keeps the short-lived access token
   in memory.
7. The account appears in Settings. The first Gmail synchronization records a
   baseline; it does not notify about existing mail.

Reauthorization repeats the flow for an existing account, using the account as
a login hint. Removing an account revokes the Google grant when possible,
deletes the Secret Service item, and deletes local synchronization state.

The planned code uses the authorization-code flow only. It does not use the
deprecated out-of-band (copy/paste) flow, a custom URI scheme, or the implicit
flow. It does not request an identity scope just to learn the email address.

## 7. Token lifecycle, revocation, and security

- Access tokens are short-lived. The app must refresh them with the stored
  refresh token; a revoked or expired refresh token requires reauthorization.
- Testing-mode refresh tokens expire after seven days. A revoked grant, a
  deleted OAuth client, a changed password under some account policies, and
  other Google security events can also invalidate tokens.
- Removing the app's access at
  <https://myaccount.google.com/permissions> invalidates its tokens even if the
  local copy remains. Remove the grant there if you delete the application or
  lose the keyring item.
- In GNOME, Secret Service is normally provided by GNOME Keyring. Tokens are
  stored as items belonging to `gmail-notifier`; **Passwords and Keys**
  (Seahorse) can show or delete them. Never print a token to a terminal or
  paste it into a bug report.
- If the OAuth client secret or a token is exposed, revoke the grant, delete the
  exposed client secret or rotate it in the Cloud console, remove the local
  client file, and authorize again.
- OAuth clients that are unused for six months can be deleted automatically by
  Google. See
  <https://support.google.com/cloud/answer/15549257#unused-client-deletion>.

## 8. What you must provide to finish the integration

To exercise real authorization, you must:

1. create the Google Cloud project and enable the Gmail API;
2. configure the consent screen and add your account as a test user (or
   publish the app);
3. create a **Desktop app** OAuth client;
4. put its values in `~/.config/gmail-notifier/oauth-client.json` with mode
   `0600`;
5. explicitly authorize the account in the app and complete Google's consent
   screen.

Those steps require your credentials and consent. They are intentionally not
performed by the automated tests or by the implementation process. The test
suite uses loopback fake OAuth and Gmail endpoints and a private D-Bus fake
Secret Service, so it never contacts Google or writes to your real keyring.

## Related documentation

- [Install, run, and remove Gmail Notifier](installation.md)
- [Resource use, API cost, and troubleshooting](operations.md)
- [Notifications and Do Not Disturb](notifications-and-dnd.md)
- [GNOME Online Accounts investigation](goa-investigation.md)

## References

- OAuth 2.0 for Desktop Apps:
  <https://developers.google.com/identity/protocols/oauth2/native-app>
- Gmail API usage limits:
  <https://developers.google.com/workspace/gmail/api/reference/quota>
- Gmail API scopes:
  <https://developers.google.com/identity/protocols/oauth2/scopes#gmail>
- Manage app audience and publishing status:
  <https://support.google.com/cloud/answer/15549945>
- Restricted scopes:
  <https://support.google.com/cloud/answer/13464325>
- When verification is not needed:
  <https://support.google.com/cloud/answer/13464323>
- OAuth app verification:
  <https://support.google.com/cloud/answer/13463073>
- Security assessment (CASA):
  <https://support.google.com/cloud/answer/13465431>
