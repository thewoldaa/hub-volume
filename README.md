# Hub Volume

A small Windows mixer that can **lock** the volume of output devices, microphones, and individual apps. If something changes a locked level — media keys, the Windows mixer, another app, headset software — Hub Volume puts it straight back.

<p align="center">
  <img src="assets/screenshot.png" width="392" alt="Hub Volume mixer">
</p>

Windows 10/11 · native C++ · single executable · no installer required

## Features

- **Per-target locks** for playback devices, microphones, and application sessions. The lock button captures the current level and mute state.
- **Event-driven**, not polling. Locks use Core Audio callbacks, so an idle process costs 0% CPU.
- **Survives sessions and devices coming and going.** App locks are keyed by executable name; device locks by endpoint ID, so they reapply when Discord restarts or a headset is reconnected.
- **Works with every active endpoint**, not just the default device.
- **Tray first.** The mixer window is created when opened and destroyed when closed, along with its GDI+ session.
- **Optional autostart** per user, without administrator rights or a scheduled task.
- **DPI aware** and keyboard/scroll friendly, with a dark flyout UI.
- **One binary.** Static CRT, no .NET, no WebView2, no Qt.

## Install

Download from [Releases](https://github.com/thewoldaa/hub-volume/releases):

| File | Description |
| --- | --- |
| `HubVolume-Setup-1.0.0.exe` | Per-user installer. No admin prompt. |
| `HubVolume-1.0.0-portable.zip` | Extract anywhere and run `HubVolume.exe`. |

The binaries are not code-signed, so Windows SmartScreen may show a warning on first run. The source is in this repository and the build is reproducible with the steps below.

## Usage

1. Start `HubVolume.exe`. A speaker icon appears near the clock.
2. Click the icon to open the mixer.
3. Set a level, then press **Kunci** on that row. The lock icon turns amber and the value is protected.
4. Press **Terkunci** to release it. **Buka kunci semua** is available from the tray menu.

The bottom bar has **Mulai bersama Windows** (start with Windows) and **Keluar** (quit). Locks only work while Hub Volume is running; quitting with active locks shows a confirmation.

Settings and the lock list are stored in `%LOCALAPPDATA%\HubVolume\config.txt`.

## Build

Requirements:

- Windows 10/11
- Visual Studio 2022 with the Desktop C++ workload (or MinGW-w64)
- CMake 3.20+
- NSIS 3.x (only for the installer)

```powershell
# Portable exe + zip, and the installer if makensis is available
.\scripts\build.ps1

# Portable only
.\scripts\build.ps1 -SkipInstaller
```

Output lands in `release/`. A manual CMake build works too:

```powershell
cmake -S . -B dist/build -G "Visual Studio 17 2022" -A x64
cmake --build dist/build --config Release
```

## How it works

Each endpoint gets an `IAudioEndpointVolume` callback, and each app session an `IAudioSessionEvents` callback. Callbacks never touch audio APIs or application state; they post a message with a numeric target ID to the main window. The main thread reads the actual value, compares it with the locked value, and writes the correction with a private event GUID so its own changes do not bounce back.

Corrections for the same target are rate-limited to roughly 80 per second. Targets that are not locked are only read while the mixer window is open. A lock is a plain record in `config.txt`, written atomically through a temporary file.

## Limitations

- Not a service: locks stop when the process exits.
- Per-user only. Installing for all users is not supported.
- Volume is controlled through WASAPI; apps that bypass it (exclusive mode, some DAWs) are outside its reach.
- On Windows 10, window corners are square because rounded corners are a Windows 11 DWM feature.

## Sponsor

Hub Volume is free and GPL-3.0. If it saves your ears, you can support development on [Patreon](https://www.patreon.com/c/Craftkal).

## License

[GPL-3.0](LICENSE). Mascot artwork by CraftKal.
