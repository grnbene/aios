# Gmail, Calendar, Contacts and Notes

`aios` signs in to a Google account with OAuth 2.0 and keeps that account's mail, calendar, contacts and
notes in sync with the machine. It is the kernel's own code over the kernel's own sockets and TLS: there is
no Python, no IMAP library, no browser engine and no helper program in the path, so it is the same code on a
hosted machine and on bare metal.

## One-time setup (yours; it needs your Google account)

1. In [Google Cloud Console](https://console.cloud.google.com/) pick or create a project.
2. Enable the **Google Calendar API** and the **People API** for it (mail goes over IMAP/SMTP and needs no API).
3. Configure the OAuth consent screen. On a Google Workspace domain choose **Internal**; for a personal
   account choose **External** and move it from *Testing* to *In production*. An app left in *Testing* has its
   refresh tokens expire after 7 days, and you would be signing in weekly.
4. Credentials → Create credentials → OAuth client ID → Application type **Desktop app**. Download the JSON.
5. Save it as `<root>/oauth/google.json` (the `--root` you run `aios` with).

## Signing in

    aios --root R exec 'google auth you@gmail.com'

It prints an address. Open it in any browser, sign in and agree. The browser is sent back to a loopback
address that `aios` is listening on, and the sign-in completes. If the browser is on another machine, open the
address there; the page it ends on will not load, so copy that page's whole address from the address bar and run

    aios --root R exec 'google auth finish "<that address>"'

(Google refuses sign-in from embedded or unfamiliar browsers, so the sign-in itself always happens in a real
browser; `aios` never sees your password.)

## Signing in on a server (sending mail from ASQL)

A mailer (`CREATE MAILER ... OAUTH 'google' ACCOUNT '<email>'`, see [asql.md - Mailers](asql.md#mailers)) sends with
the tokens that `google auth` keeps under the root of the machine that runs ASQL. On a server this has four
differences from the desktop sign-in:

- **Run it as the user the service runs as**, with the full path and `--root`. As root it would leave a vault the
  service cannot read:

      runuser -u aios -- /opt/aios/bin/aios --root /opt/aios/root-v8-full exec 'google auth support@qbc.com'

  The `<root>/oauth` directory must be owned by that user (`chown aios:aios`, mode 0700), and so must
  `google.json` (mode 0600).
- **Ask for the mail scope only.** Without a `scopes` list the client asks for mail, Calendar and Contacts. A
  mailer needs just `https://mail.google.com/`, so give the client file
  `"scopes": ["https://mail.google.com/", "openid", "email"]` inside its `installed` block before signing in.
  `google status` then lists only those scopes.
- **The browser is on another machine.** The consent page redirects to `http://127.0.0.1:<port>/` on the server,
  which a laptop cannot load; that failed page is expected. Copy its whole address from the address bar. While the
  `google auth` process is still waiting (up to 10 minutes) you can hand it to the listener from the server:
  `curl 'http://127.0.0.1:<port>/?state=...&code=...'`; otherwise run `google auth finish '<address>'` as the
  same user. The code works once.
- **Workspace accounts:** create the client as a **Desktop app** and set the consent screen to **Internal**, so the
  refresh token does not expire after 7 days. A Web-application client does not work: it needs registered
  redirect addresses and this flow uses a loopback port chosen at run time.

If the sign-in later lapses or is revoked, a mailer's act fails with `run aios google auth <email> on this machine`
in `SHOW CELLS <name> ACTS`, and tries again at the key's next change.

## Using it

    google sync                      mail, calendar, contacts and notes
    google sync calendar contacts    just those
    google watch every=300           sync every five minutes until stopped
    google status                    who is signed in, token state, when each part last synced
    google logout                    revoke the token at Google and forget it here

Signing in also creates the mail account, so `mail list`, `mail read`, `mail send` and the desktop's mail
window work on the synced mailbox. Mail is fetched and sent with the OAuth token (SASL XOAUTH2); no password is
stored anywhere.

## What stays where

- **Tokens**: `<root>/oauth/tokens.vault`, sealed with ChaCha20-Poly1305 under a random key in `vault.key`
  beside it (mode 0600). It survives reboots. It keeps tokens out of backups, logs and repositories; it does not
  protect them from someone who can already read that directory as you, because the key has to be reachable
  without a passphrase at boot. `google logout` revokes the refresh token at Google.
- **What was last synced**: `<root>/google/state.json` (ids, etags, hashes; no secrets).
- **Conflicts**: when something was changed on both sides since the last sync, Google's version wins and your
  version is saved in `<root>/google/conflicts/`. It is also counted in the sync's output.

## How each part behaves

| | |
|---|---|
| Mail | IMAP (`imap.gmail.com`) and SMTP (`smtp.gmail.com`) with XOAUTH2. Incremental by UID; headers for everything, bodies for the last 90 days, the rest on demand. Attachments are listed but a *sent* message carries only text: the compose model holds an attachment's name and size, not its contents. |
| Calendar | Google Calendar API. A window of 30 days back to a year ahead, recurring events expanded into their occurrences. Edits, deletes and new events go back up, with `sendUpdates=none` so a sync never sends invitations. Times are the wall-clock time the event is written in. Editing one occurrence works; changing a recurrence rule from `aios` does not. |
| Contacts | People API. Every contact is pulled; edits, deletes and new contacts go back up. An update changes only the fields `aios` holds (names, first personal and work email and phone, organisation, first address, birthday, note) and leaves everything else about the person alone. |
| Notes | The `Notes` mailbox on the IMAP account (Gmail shows it as the *Notes* label), one message per note, in the format Apple Notes uses, so other clients can read them. Edits replace the message; deleted notes go to Trash. |

## Not covered

- Sign-in inside `aios`'s own browser: Google's sign-in page needs a browser Google trusts. Use any real browser.
- Google Keep (it has no consumer API) — notes sync through the Notes mailbox instead.
- Charsets other than UTF-8, ASCII, ISO-8859-1/2/15, windows-1250/1251/1252 and KOI8-R in received mail: their
  non-ASCII text is replaced with U+FFFD rather than guessed.

## Checking it

    python3 scripts/mail-stack-check.py build

runs every check against mock servers (a stand-in for Google, IMAP and SMTP): the MIME and JSON parsers against
Python's, the IMAP/SMTP clients in every transport, OAuth, the two-way sync, and the whole flow through the
`aios` command line. It does not talk to Google; the real sign-in is yours to run.
