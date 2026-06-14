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

## Implemented adapters

### Twitch

Implemented via Twitch Helix **Modify Channel Information**:

```http
PATCH https://api.twitch.tv/helix/channels?broadcaster_id=...
```

You need:

- a Twitch Developer app / client ID
- your numeric broadcaster user ID
- a **user access token** for that broadcaster with scope:
  - `channel:manage:broadcast`

### YouTube

Implemented with YouTube Live Streaming API `liveBroadcasts.list` followed by
`liveBroadcasts.update` so the existing broadcast snippet is preserved and only
the title is changed.

You need:

- a Google Cloud project with YouTube Data API v3 enabled
- OAuth consent/app credentials
- an OAuth access token with YouTube scope, usually:
  - `https://www.googleapis.com/auth/youtube`
- the target `liveBroadcast` ID

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

- a Kick Dev app
- OAuth 2.1 user access token with scope:
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
