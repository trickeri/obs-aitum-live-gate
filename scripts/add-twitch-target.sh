#!/usr/bin/env bash
# Authorize a second Twitch channel for a Stream Target.
#
# The Stream Targets dock (nul-stream-targets) swaps which Twitch channel the
# main OBS output goes to. Live Gate keeps a separate title per target, and a
# Twitch token is bound to one channel -- so each extra channel needs its own
# refresh token stored as a per-target override.
#
# Usage:
#   scripts/add-twitch-target.sh [broadcasterId]
#
# With no argument it uses whichever target the dock currently has active.
# Sign your default browser into the channel you are authorizing FIRST; the
# script verifies the account that authorized matches the target and refuses
# to write on a mismatch.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugin_config"
settings="${OBS_LIVE_GATE_SETTINGS:-$config/obs-aitum-live-gate/settings.json}"
targets="$config/nul-stream-targets/targets.json"

target="${1:-$(python3 -c '
import json, sys
try:
    print(json.load(open(sys.argv[1])).get("activeUserId", ""))
except OSError:
    print("")
' "$targets")}"

if [ -z "$target" ]; then
	echo "No target given and no active target in $targets" >&2
	exit 1
fi

read -r client_id client_secret < <(python3 -c '
import json, sys
adapter = json.load(open(sys.argv[1]))["titleAdapters"]["twitch"]
print(adapter["clientId"], adapter["clientSecret"])
' "$settings")

echo "Authorizing Twitch for Stream Target $target"
echo "Make sure your browser is signed in as that channel."
exec python3 "$here/oauth-setup.py" twitch \
	--target "$target" \
	--client-id "$client_id" \
	--client-secret "$client_secret" \
	--settings "$settings"
