# BBSC on BBport for Linux

Test build. BBSC's mod (the same code as `bbsc.dll`) built as `libbbsc.so` for the Linux build of
[BBport](https://github.com/Supermedo/bloodborne_pc) (the AppImage or a source build). Same packets
and servers as shadPS4 and BBport for Windows players.

## How it is loaded

- BBport's `run.sh` starts `BB_PROBE` in place of its `bb-probe` when that variable is set (BBport
  for Windows is hooked the same way, with `bbsc-launch.exe`).
- `bbsc-probe` (`emulator/mod/linux/`) preloads `libbbsc.so` into `bb-probe` (`LD_PRELOAD`), passes
  `run.sh`'s arguments on unchanged and tells the mod the user folder `bb-probe` gets (`--user`),
  where the character's saves are.
- `libbbsc.so` then does what the DLL does: finds the game image, writes the byte patches, installs
  the hooks and wraps the game's import slots. The OS calls are in `bbsc_platform.h` (Windows) and
  `bbsc_platform_linux.cpp`.

## Build (Arch Linux)

```bash
sudo pacman -S --needed base-devel git
git submodule update --init emulator/externals/fmt emulator/externals/xbyak
emulator/mod/build-mod.sh
```

Output: `build/mod-linux/BBSC-<version>-BBport-Linux.tar.gz` (and its folder), the package for
players: `libbbsc.so`, `bbsc-probe`, `install.sh`, `README.txt`, `LICENSE.txt`, `NOTICE.txt`,
`SUPPORTERS.txt`. `<version>` is `emulator/mod/mod.json`'s. The C++ runtime is linked in, so the
library also loads in the AppImage, which runs `bb-probe` on its own glibc (2.42).

## Install (players)

Extract the package anywhere and, in its folder:

```bash
./install.sh
```

It looks for `Bloodborne-bbport*.AppImage` next to the folder, in `~/Applications`, `~/Downloads`,
`~/Games` and `~` (otherwise: `./install.sh /path/to/Bloodborne-bbport-x86_64.AppImage`; for a
source build of BBport, its `launcher/bb-launcher.sh`). It copies the mod to
`~/.local/share/bbport/bbsc` (logs, captures and save backups go there) and adds
**Bloodborne (BBSC)** to the application menu: BBport's own launcher, started with `BB_PROBE`
pointing at `bbsc-probe`, so every game it starts has BBSC. BBport started any other way stays
without it. From a terminal: `~/.local/share/bbport/bbsc/bbsc-play` (`--play` skips the launcher
window). `./install.sh --uninstall` removes the menu entry.

Without the menu entry: put `BB_PROBE=<mod folder>/bbsc-probe` in the launcher's Settings >
Developer > "Extra variables (NAME=value, space-separated)"; BBport then always starts with BBSC.

Online play is set up as for BBport itself (Online page: account, servers, "Seamless co-op
(BBSC)"). BBSC reads the `host_overrides.json` that `run.sh` writes for the launch, connects to the
BBSC relay of that game server (port 20444) and falls back to the game's own P2P connection when
there is none. `BBSC_RELAY=host:port` in the same field overrides the relay address.

The switches of `emulator/mod/mod.json` are environment variables in the same field as well, for
example `SHADPS4_BBSC_SCALING=0`.

## Check that it runs

Start the game from Bloodborne (BBSC) and look at the newest
`~/.local/share/bbport/bbsc/logs/bbsc-mod-*.log` (the mod's log; `bbport-*.log` next to it is the
game's own output, ending with how `bb-probe` ended):

- `BBSC mod in pid ... (.../bb-probe)` and `Game image found at 0x800000000`
- `Bloodborne RE trace installed 159/159 hooks`
- `BBSC mod ready after ... ms`
- online: `Relay: connected to <server>:20444`, or `P2P channel: ...` on servers without a relay

`BBSC bell check: failed to hook 0x157f6d1` is expected (a trace hook on bytes the seamless
patches already changed; the same on Windows).

## Co-op test (two PCs)

Both players: same server and account settings in the launcher, BBSC installed as above, the same
BBSC build. Host rings the Hunt Bell, the guest the Kin Bell. Then check warps between lamps, a
boss, a death of each player and the Hunter's Dream. Send both players' `bbsc-mod-*.log` and
`bbport-*.log` for any problem.

## Differences from Windows

- The keyboard and mouse camera (F1) and the F8 unstuck key are Windows only: they use the game
  window's messages. The controller camera (L3 + R3) works; L3 + R3 also opens BBport's menu.
- Where the desktop portal grants no realtime time (`RTTimeUSecMax` 0 and no rtkit), PipeWire's
  client module sets the game's `RLIMIT_RTTIME` to 0 and the kernel kills it (SIGKILL) a few
  minutes in. `bbsc-probe` therefore starts the game with `SDL_AUDIO_DRIVER=pulseaudio,pipewire`
  and SDL threads without realtime priority, unless those variables are set.
- Discord status talks to the Discord socket in `$XDG_RUNTIME_DIR` (Flatpak and Snap Discord too).
- Watching another hunter while dead (0.0.2): on Linux only the controller switches hunters
  (R1 / L1); the Tab / E / Q / F / mouse keys read the game window and are Windows only.
- The Windows 0.0.2 package ships a patched BBport (`bbport/bbport-1.7.2-bbsc.patch`: UDP 3478
  relay fallback, signaling rejoin fix, NpUtility). The Linux package runs the stock BBport
  AppImage, without those network fixes; the mod side (aimed host placement, safe import slots,
  area list cap, timeline) is the same.
