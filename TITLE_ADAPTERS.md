# Title adapter setup

OBS Aitum Live Gate currently targets the platforms you asked for:

- YouTube
- Kick
- X
- Twitch

It reads title adapter settings from this plugin's OBS config:

```text
%APPDATA%\obs-studio\plugin_config\obs-aitum-live-gate\settings.json
```

After you run OBS once with the plugin installed, copy the `titleAdapters` object
from `title-adapters.example.json` into that file. Keep `strictTitleUpdates` set
to `false` while testing; set it to `true` only if you want OBS to abort going
live whenever a selected platform title cannot be updated.

## Authentication: refresh tokens (recommended)

Access tokens are short-lived (Google ~1 hour, Twitch ~4 hours), so the plugin
authenticates with the OAuth **refresh token** grant instead of a static access
token. Store `clientId`, `clientSecret`, and `refreshToken` per platform; at
go-live the plugin trades the refresh token for a fresh access token, pushes the
title, and persists any rotated refresh token back to `settings.json` (Twitch and
Kick rotate theirs; Google keeps the same one).

You authorize once to obtain the refresh token. If an adapter instead only has a
static `accessToken` and no `refreshToken`, the plugin still uses that token
directly (handy for a quick test, but it will stop working once that token
expires).

> Google note: keep your OAuth app's publishing status set to **In production**.
> While it is in **Testing**, Google expires refresh tokens after 7 days.

## Implemented adapters

### Twitch

Implemented via Twitch Helix **Modify Channel Information**:

```http
PATCH https://api.twitch.tv/helix/channels?broadcaster_id=...
```

You need:

- a Twitch Developer app: its **client ID** and **client secret**
- your numeric broadcaster user ID
- a **refresh token** for that broadcaster, obtained by authorizing the
  `channel:manage:broadcast` scope (authorization code grant)

### YouTube

Implemented with YouTube Live Streaming API `liveBroadcasts.list` followed by
`liveBroadcasts.update` so the existing broadcast snippet is preserved and only
the title is changed.

Because YouTube creates a new broadcast object for every stream, the adapter
auto-selects the broadcast at go-live: it uses the currently `active` broadcast,
falling back to the next `upcoming` (scheduled) one. You can still pin a specific
broadcast by setting `broadcastId` in the adapter config, but normally you leave
it unset.

You need:

- a Google Cloud project with YouTube Data API v3 enabled
- OAuth client credentials: **client ID** and **client secret**
- a **refresh token** authorized for the YouTube scope:
  - `https://www.googleapis.com/auth/youtube`

### Kick

Implemented from the official Kick Dev public API:

```http
PATCH https://api.kick.com/public/v1/channels
```

Body:

```json
{ "stream_title": "..." }
```

You need:

- a Kick Dev app: its **client ID** and **client secret**
- a **refresh token** from the OAuth 2.1 flow authorized with scope:
  - `channel:write`

## X status

X is included as a first-class platform so it appears in the popup and can be
toggled for Aitum output startup. However, title publishing is currently manual:
X's public developer docs expose the X API for posts/users/etc., and X Help
documents Media Studio Producer for live RTMP workflows, but I did not find a
generally available public API endpoint to update a Producer/Media Studio live
broadcast title.

For now, set the title manually in X Producer / Media Studio before going live.
If X grants access to a live Producer API for your account, we can add it behind
the existing `x` adapter key.

## Platform detection

For Aitum rows, the plugin infers platform from the output name and RTMP endpoint
(`twitch`, `youtube`, `kick`, `x`). The inferred value is saved as `platform` per
row in `settings.json`; you can manually correct it if your output name/endpoint
is custom.

For the OBS main stream row, manually set the saved row's `platform` if you want
the adapter to update your primary platform title too.
