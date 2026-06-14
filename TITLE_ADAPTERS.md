# Title adapter setup

OBS Aitum Live Gate reads title adapter settings from this plugin's OBS config:

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

### Trovo

Implemented via Trovo's official channel command endpoint using `settitle`:

```http
POST https://open-api.trovo.live/openplatform/channels/command
```

You need:

- a Trovo developer application / client ID
- your Trovo numeric channel ID
- a user access token with scope:
  - `manage_messages`

### Facebook

A guarded Facebook adapter is included for existing LiveVideo objects:

```http
POST https://graph.facebook.com/v25.0/{live-video-id}
```

You need:

- a Meta developer app if you are automating this beyond local/manual tokens
- a Page/User token that can edit the LiveVideo
- the `liveVideoId` you want to update

Facebook Live flows vary depending on whether you create the LiveVideo via API,
stream to an existing scheduled live, or use a Page workflow. Treat this adapter
as experimental until tested against your exact Facebook Live setup.

## Not implemented

### TikTok

No normal public TikTok LIVE creator API for changing live titles was found in
TikTok's public developer docs. The plugin intentionally does not use unofficial
TikTok scraping/private APIs. If TikTok grants a partner/API route, add it behind
the existing `tiktok` adapter key.

## Platform detection

For Aitum rows, the plugin infers platform from the output name and RTMP endpoint
(`twitch`, `youtube`, `kick`, `trovo`, `facebook`, `tiktok`). The inferred value
is saved as `platform` per row in `settings.json`; you can manually correct it if
your output name/endpoint is custom.

For the OBS main stream row, manually set the saved row's `platform` if you want
the adapter to update your primary platform title too.
