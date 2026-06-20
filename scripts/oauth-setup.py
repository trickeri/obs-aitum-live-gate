#!/usr/bin/env python3
"""One-time OAuth setup for OBS Aitum Live Gate title adapters.

Runs the authorization-code flow for a platform, captures the redirect on a tiny
local web server (so there is nothing to copy-paste), exchanges the code for a
refresh token, and writes the credentials into the plugin's settings.json.

Usage:
  scripts/oauth-setup.py twitch  --client-id ID --client-secret SECRET
  scripts/oauth-setup.py youtube --client-id ID --client-secret SECRET
  scripts/oauth-setup.py kick    --client-id ID --client-secret SECRET

The OAuth app's redirect URL must be exactly http://localhost:3000
(for Google, add it under the OAuth client's "Authorized redirect URIs").
"""

import argparse
import base64
import hashlib
import json
import os
import secrets
import sys
import threading
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer

REDIRECT_URI = "http://localhost:3000"
REDIRECT_PORT = 3000

PLATFORMS = {
    "twitch": {
        "authorize": "https://id.twitch.tv/oauth2/authorize",
        "token": "https://id.twitch.tv/oauth2/token",
        "scope": "channel:manage:broadcast",
        "pkce": False,
        "google_offline": False,
    },
    "youtube": {
        "authorize": "https://accounts.google.com/o/oauth2/v2/auth",
        "token": "https://oauth2.googleapis.com/token",
        "scope": "https://www.googleapis.com/auth/youtube",
        "pkce": False,
        "google_offline": True,  # access_type=offline + prompt=consent to get a refresh token
    },
    "kick": {
        "authorize": "https://id.kick.com/oauth/authorize",
        "token": "https://id.kick.com/oauth/token",
        "scope": "channel:write",
        "pkce": True,  # Kick is OAuth 2.1, requires PKCE
        "google_offline": False,
    },
}


def default_settings_path():
    override = os.environ.get("OBS_LIVE_GATE_SETTINGS")
    if override:
        return override
    base = os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config"))
    return os.path.join(base, "obs-studio", "plugin_config",
                        "obs-aitum-live-gate", "settings.json")


class _CaptureHandler(BaseHTTPRequestHandler):
    captured = {}

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        _CaptureHandler.captured = dict(urllib.parse.parse_qsl(parsed.query))
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        ok = "code" in _CaptureHandler.captured
        msg = ("Authorization received. You can close this tab and return to the terminal."
               if ok else "Authorization failed. Check the terminal.")
        self.wfile.write(f"<html><body><h2>{msg}</h2></body></html>".encode())

    def log_message(self, *args):
        pass  # keep the terminal quiet


def capture_redirect(expected_state):
    server = HTTPServer(("localhost", REDIRECT_PORT), _CaptureHandler)
    thread = threading.Thread(target=server.handle_request)  # serve exactly one request
    thread.start()
    thread.join(timeout=300)
    server.server_close()
    data = _CaptureHandler.captured
    if not data:
        sys.exit("Timed out waiting for the authorization redirect.")
    if data.get("state") != expected_state:
        sys.exit("State mismatch (possible CSRF); aborting.")
    if "code" not in data:
        sys.exit(f"Authorization error: {data.get('error_description', data)}")
    return data["code"]


def post_form(url, fields):
    body = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(url, data=body, headers={
        "Content-Type": "application/x-www-form-urlencoded",
        "Accept": "application/json",
    })
    try:
        with urllib.request.urlopen(req) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        sys.exit(f"Token request failed ({exc.code}): {exc.read().decode()}")


def twitch_broadcaster_id(client_id, access_token):
    req = urllib.request.Request("https://api.twitch.tv/helix/users", headers={
        "Authorization": f"Bearer {access_token}",
        "Client-Id": client_id,
    })
    with urllib.request.urlopen(req) as resp:
        data = json.loads(resp.read().decode())
    return data["data"][0]["id"]


def write_settings(path, platform, entry):
    settings = {}
    if os.path.exists(path):
        with open(path) as handle:
            settings = json.load(handle)
    adapters = settings.setdefault("titleAdapters", {})
    existing = adapters.get(platform, {})
    existing.update(entry)
    existing["enabled"] = True
    adapters[platform] = existing
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        json.dump(settings, handle, indent=4)
        handle.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("platform", choices=PLATFORMS.keys())
    parser.add_argument("--client-id", required=True)
    parser.add_argument("--client-secret", required=True)
    parser.add_argument("--settings", default=default_settings_path(),
                        help="path to settings.json (default: %(default)s)")
    args = parser.parse_args()

    cfg = PLATFORMS[args.platform]
    state = secrets.token_urlsafe(16)
    params = {
        "client_id": args.client_id,
        "redirect_uri": REDIRECT_URI,
        "response_type": "code",
        "scope": cfg["scope"],
        "state": state,
    }

    verifier = None
    if cfg["pkce"]:
        verifier = secrets.token_urlsafe(64)
        challenge = base64.urlsafe_b64encode(
            hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
        params["code_challenge"] = challenge
        params["code_challenge_method"] = "S256"
    if cfg["google_offline"]:
        params["access_type"] = "offline"
        params["prompt"] = "consent"

    authorize_url = cfg["authorize"] + "?" + urllib.parse.urlencode(params)
    print(f"\nOpening browser to authorize {args.platform}...")
    print("If it does not open, paste this URL manually:\n")
    print(authorize_url + "\n")
    webbrowser.open(authorize_url)

    code = capture_redirect(state)
    print("Authorization code received; exchanging for tokens...")

    fields = {
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": REDIRECT_URI,
        "client_id": args.client_id,
        "client_secret": args.client_secret,
    }
    if verifier:
        fields["code_verifier"] = verifier
    tokens = post_form(cfg["token"], fields)

    refresh = tokens.get("refresh_token")
    if not refresh:
        sys.exit("No refresh_token returned. For Google, set the OAuth app to "
                 "'In production' and retry so a refresh token is issued.")

    entry = {
        "clientId": args.client_id,
        "clientSecret": args.client_secret,
        "refreshToken": refresh,
    }
    if args.platform == "twitch":
        entry["broadcasterId"] = twitch_broadcaster_id(args.client_id, tokens["access_token"])
        print(f"Broadcaster ID: {entry['broadcasterId']}")
    if args.platform == "youtube":
        print("\nYouTube will auto-detect your live/upcoming broadcast at go-live; "
              "no broadcastId needed.")

    write_settings(args.settings, args.platform, entry)
    print(f"\nDone. Wrote {args.platform} credentials to:\n  {args.settings}")
    print("Restart OBS for the plugin to pick them up.")


if __name__ == "__main__":
    main()
