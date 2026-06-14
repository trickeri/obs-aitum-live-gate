# Architecture Notes

## Goal

Provide a Streamlabs OBS-style "go live gate" for OBS users who also use Aitum
Multistream: the user presses Start Streaming once, confirms titles and platform
toggles in one modal, then OBS and selected Aitum outputs start in sequence.

## Flow

1. A Qt event filter watches for the OBS main-window Start Streaming button.
2. The original click is consumed.
3. `GoLiveDialog` is populated from:
   - OBS main stream row, always present.
   - Aitum Multistream profile outputs read from Aitum's JSON config.
   - This plugin's saved per-platform state.
4. On accept:
   - selected titles/toggles are saved,
   - selected Aitum output names are staged,
   - OBS main streaming starts via `obs_frontend_streaming_start()`.
5. On `OBS_FRONTEND_EVENT_STREAMING_STARTING`, titles are currently logged.
6. On `OBS_FRONTEND_EVENT_STREAMING_STARTED`, selected Aitum dock buttons are
   clicked to start matching companion outputs.

## Known limitations

- Intercepts the visible OBS Start Streaming button. Starts triggered by hotkeys,
  websocket, or menu actions are not blocked yet.
- Aitum main-output start is button-driven because Aitum 1.0.8 does not expose a
  public main-output proc handler analogous to its vertical output handlers.
- Title fields are captured and persisted, but provider metadata APIs are not
  implemented yet.
- Platform identity is inferred from configured output names/endpoints. Provider
  adapters will need explicit account binding.

## Next milestones

1. Add title provider adapters and a credentials/settings page.
2. Add a non-button start path guard if OBS exposes a cancellable pre-start hook
   or if replacing OBS's start action proves safer.
3. Add tests around config parsing and settings persistence.
4. Package plugin artifacts with GitHub Actions once the first build is green.
