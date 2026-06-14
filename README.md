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
- Publishes titles before OBS starts when a matching title adapter is enabled.
- Includes title adapters for Twitch, YouTube, and Kick, plus first-class X platform detection/manual-title handling.
- Warns and continues by default when title adapters are missing; `strictTitleUpdates` can abort going live on title failures.

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

## Title publishing adapters

Title adapters are configured in this plugin's OBS config. See `TITLE_ADAPTERS.md` and `title-adapters.example.json` for required app registrations, OAuth scopes, and token fields.

Implemented title update paths:

- Twitch Helix Modify Channel Information
- YouTube Live Streaming API `liveBroadcasts.update`
- Kick public API `PATCH /public/v1/channels`

X is detected and shown in the popup, but title updates are manual until X exposes or grants access to a public live Producer/Media Studio title API.

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

Linux / Arch+KDE handoff:

```bash
cmake --preset linux-x86_64
cmake --build --preset linux-x86_64 --config RelWithDebInfo
scripts/linux/install-aitum-arch.sh
scripts/linux/install-live-gate.sh
```

See `docs/LINUX_ARCH_KDE.md` for Arch packages, Flatpak/native OBS paths,
Aitum `.deb` extraction, and the runtime verification checklist.

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
