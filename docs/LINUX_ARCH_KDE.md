# Arch + KDE Linux handoff

This repo has not been compiled on the Arch/KDE dual boot from Windows. These
notes are intended for the Linux-side agent to run and verify locally.

## Target platform

- Distro/session: Arch Linux + KDE Plasma
- OBS package assumption: native Arch `obs-studio` package, not Flatpak
- Preferred install scope while testing: per-user OBS plugin directory

Per-user OBS plugin root:

```text
~/.config/obs-studio/plugins
```

Flatpak OBS plugin root, if OBS is installed as Flatpak instead:

```text
~/.var/app/com.obsproject.Studio/config/obs-studio/plugins
```

The helper scripts support Flatpak/custom roots with:

```bash
export OBS_USER_PLUGIN_ROOT="$HOME/.var/app/com.obsproject.Studio/config/obs-studio/plugins"
```

## Arch packages to install first

Start with:

```bash
sudo pacman -S --needed \
  base-devel \
  cmake \
  ninja \
  git \
  curl \
  jq \
  zsh \
  qt6-base \
  obs-studio \
  libarchive
```

Notes:

- `libarchive` provides `bsdtar`; the Aitum helper can also fall back to `ar` +
  `tar` if available.
- CMake must be >= 3.28 for the OBS plugin template presets.
- If the OBS template build complains about missing dev headers/libraries, check
  the exact error and install the matching Arch package rather than changing the
  plugin code first.

## Build OBS Aitum Live Gate

From the repo root:

```bash
cmake --preset linux-x86_64
cmake --build --preset linux-x86_64 --config RelWithDebInfo
```

If the generic preset is not available for any reason, the inherited template
Ubuntu preset should also work on Linux hosts:

```bash
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64 --config RelWithDebInfo
```

## Install Aitum Multistream on Arch

Aitum currently publishes a Linux `.deb`, not an Arch package. The helper script
extracts the `.deb` and installs the OBS plugin files into the per-user OBS
plugin layout:

```bash
scripts/linux/install-aitum-arch.sh
```

For a system-wide native Arch OBS install instead:

```bash
scripts/linux/install-aitum-arch.sh --global
```

The default helper pins Aitum Multistream `1.0.8` and verifies the known SHA-256
for `aitum-multistream-linux-gnu.deb`.

## Install OBS Aitum Live Gate

Per-user install:

```bash
scripts/linux/install-live-gate.sh
```

System-wide install:

```bash
scripts/linux/install-live-gate.sh --global
```

## Expected installed layout

Per-user Aitum:

```text
~/.config/obs-studio/plugins/aitum-multistream/bin/64bit/aitum-multistream.so
~/.config/obs-studio/plugins/aitum-multistream/data/locale/en-US.ini
```

Per-user Live Gate:

```text
~/.config/obs-studio/plugins/obs-aitum-live-gate/bin/64bit/obs-aitum-live-gate.so
~/.config/obs-studio/plugins/obs-aitum-live-gate/data/locale/en-US.ini
```

Native Arch global OBS paths:

```text
/usr/lib/obs-plugins/*.so
/usr/share/obs/obs-plugins/<plugin-name>/locale/en-US.ini
```

## Runtime verification checklist

1. Close OBS fully.
2. Start OBS from a terminal so plugin load errors are visible:

   ```bash
   obs 2>&1 | tee /tmp/obs-live-gate.log
   ```

3. Confirm the OBS log contains both plugins:
   - `Aitum Multistream` or `aitum-multistream`
   - `OBS Aitum Live Gate loaded`
4. In OBS, check **Docks** for Aitum Multistream.
5. In OBS, open **Tools -> Preview Go Live Gate**.
6. Verify the preview dialog:
   - lists OBS main stream
   - lists Aitum outputs
   - does not show RTMP server addresses in platform labels
   - closes without starting any stream
7. Configure Aitum outputs for YouTube, Kick, X, and Twitch.
8. Reopen the preview and verify the rows are clean and recognizable.

## Known Linux risks for the next agent

- Aitum's `.deb` binary may have shared library dependencies that differ on Arch.
  If OBS does not load it, inspect with:

  ```bash
  ldd ~/.config/obs-studio/plugins/aitum-multistream/bin/64bit/aitum-multistream.so
  ```

- Our plugin is built against the OBS template dependencies. If Arch OBS is a
  newer minor version than the template's `buildspec.json`, runtime validation is
  required even if compilation succeeds.
- The current start interception watches the visible OBS Start Streaming button.
  Hotkeys/websocket-triggered starts are still a known limitation.
- Title adapters require manual OAuth token config; Linux support here is about
  build/install/load and UI behavior, not OAuth onboarding.
