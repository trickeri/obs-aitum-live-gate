# OBS Aitum Live Gate

OBS Aitum Live Gate is a companion OBS Studio plugin for creators using the
[Aitum Multistream](https://github.com/Aitum/obs-aitum-multistream) plugin.
It aims to recreate the Streamlabs OBS-style preflight flow: pressing OBS's
**Start Streaming** button opens a popup where you can choose which configured
platforms go live and enter stream titles before OBS starts broadcasting.

## Current behavior

This repository is the initial implementation scaffold.

- Intercepts the OBS **Start Streaming** button in the main window.
- Shows a Qt **Go Live** dialog before streaming begins.
- Lists:
  - the OBS main stream, and
  - Aitum Multistream outputs from the active OBS profile's Aitum config.
- Persists per-profile platform choices and title text to this plugin's config.
- Starts OBS programmatically when **Go Live** is accepted.
- After OBS reports streaming started, clicks the matching Aitum Multistream dock
  output buttons for the selected Aitum outputs.
- Logs queued titles so platform title publishing can be wired behind a stable
  adapter without blocking the start-gate UI work.

## Important integration notes

Aitum Multistream 1.0.8 stores its configured main outputs in:

```text
%APPDATA%/obs-studio/plugin_config/aitum-multistream/config.json
```

The first version reads that file to discover configured outputs. Aitum does not
currently expose a public main-output start proc-handler in the code inspected
for 1.0.8, so this plugin starts selected Aitum outputs by finding Aitum's dock
buttons (`objectName == "canvasStream"`) and clicking the buttons whose parent
`QGroupBox` object name matches the configured output name.

That is intentionally isolated in `LiveGateController::applyAitumSelections()`
so it can be replaced later if Aitum adds a stable public API.

## Title publishing adapter seam

The popup captures a title per platform, but Aitum output config only contains
stream endpoints/keys, not platform OAuth credentials or metadata APIs. The
current code persists and logs titles. The next implementation step is to add
provider adapters, for example:

- Twitch Helix `Modify Channel Information`
- YouTube Live Broadcast metadata update
- Kick/TikTok/Facebook equivalents where API access is available

Adapters should run before `obs_frontend_streaming_start()` and fail visibly if a
selected platform's title cannot be updated.

## Build

This repo is based on the official OBS plugin template and currently targets OBS
Studio 31.1.1 dependencies from `buildspec.json`.

Windows quick path from a Visual Studio Developer PowerShell:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

Or use the template script:

```powershell
.\.github\scripts\Build-Windows.ps1
```

## Installed Aitum companion plugin

For this workstation, Aitum Multistream 1.0.8 was installed from the official
GitHub release `aitum-multistream-windows-programdata.zip` into the per-user OBS
plugin directory:

```text
%APPDATA%\obs-studio\plugins\aitum-multistream
```

Downloaded asset SHA-256:

```text
892D03E200E7CFAEEB43C410D75749EE0BDDB9913259F0BECC76BC4A3182719D
```

## License

GPL-2.0-or-later, matching the OBS plugin template licensing.
