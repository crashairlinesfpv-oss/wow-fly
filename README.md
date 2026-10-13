# wow-fly

Fly a Velocidrone quad through your own World of Warcraft 3.3.5a world. This repository is the bridge: an AzerothCore module, a Velocidrone plugin, and a small patch for the OpenWoW client. It does not contain AzerothCore, OpenWoW, Velocidrone, a virtual machine, or any game data.

You run the world yourself. Velocidrone and OpenWoW stay on the same computer, because the first-person camera is a local packet. The worldserver can be that same computer or another one.

## ChromieCraft and OpenWoW

[ChromieCraft](https://github.com/ChromieCraft) here means a World of Warcraft 3.3.5a install: a folder that contains `Wow.exe`, `Data/` (the MPQ archives), `Interface/`, and `WTF/`. Any other 3.3.5a client folder with those same files works the same way. This repository does not include that folder.

[OpenWoW](https://github.com/rkabachenko/OpenWow-snapshot) is a separate 3.3.5a client program. You build it yourself. You then start `openwow-client` with the ChromieCraft folder as the current directory. OpenWoW reads `Data/`, `Interface/`, and `WTF/` from that folder and logs into your realm through the realm list already stored there, usually `Data/enUS/realmlist.wtf`:

```
set realmlist 127.0.0.1
```

Use your worldserver address on that line when the world is on another machine.

The drone camera ships in this repo as `client/openwow/openwow.patch` plus `drone_fpv.cpp` and `drone_fpv.h`. Those changes are applied to the OpenWoW source, then you compile `openwow-client`. That program is what draws the quad view, including roll, pitch, and field of view. `Wow.exe` in the ChromieCraft folder cannot roll the camera, so the flight uses the patched OpenWoW binary, launched from the ChromieCraft directory:

```bash
cd /path/to/ChromieCraft_3.3.5a
/path/to/OpenWow-snapshot/build/release/apps/client/openwow-client
```

```powershell
cd C:\Path\To\ChromieCraft_3.3.5a
C:\Path\To\OpenWow-snapshot\build\release\apps\client\openwow-client.exe
```

Section 3 is how to build that binary. The files in `client/openwow/` are the camera addition to OpenWoW.

## What you provide

- An [AzerothCore](https://www.azerothcore.org/) 3.3.5a world you built, with maps extracted from your own client.
- A character on that realm. The module moves only the name you put in its config. An empty name moves nobody.
- [Velocidrone](https://www.velocidrone.com/) with [BepInEx 5](https://docs.bepinex.dev/) for Unity Mono, 64-bit, installed in the game directory (`BepInEx/core/BepInEx.dll`). The IL2CPP pack and BepInEx 6 will not load this plugin.
- [OpenWoW](https://github.com/rkabachenko/OpenWow-snapshot) checked out at commit `2521e1f72eeffd661913be63d1e2b374073c316c` (tag line `OpenWoW 0.1.2`). The patch is against that snapshot.
- A 3.3.5a game folder such as ChromieCraft, with your own `Data/` archives. OpenWoW is started from that folder. This repo does not ship the archives.

## 1. World module

On Windows, run the world in WSL or in Docker, using AzerothCore's own install guide. The commands below are the same inside that Linux environment. A native Visual Studio build of worldserver is not covered here.

Copy the module into your AzerothCore tree and build it the way that project builds modules:

```bash
cp -a server/mod-drone-pose /path/to/azerothcore-wotlk/modules/mod-drone-pose
cd /path/to/azerothcore-wotlk
./acore.sh compiler build
```

The cmake summary has to list `mod-drone-pose`. If it is absent, the folder was not picked up and the world will ignore the pose.

After a successful build, find the installed config and make a real copy next to it:

```bash
find /path/to/azerothcore-wotlk -name drone_pose.conf.dist
cp /path/to/that/drone_pose.conf.dist /path/to/that/drone_pose.conf
```

Edit `drone_pose.conf`:

```
DronePose.Enable = 1
DronePose.CharacterName = "YourCharacter"
DronePose.Bind = 0.0.0.0
DronePose.Port = 47561
DronePose.Scale = 1.0
```

`CharacterName` is the exact character name. Leave it empty and the module applies the pose to nobody. Do not put an account password in this file.

Restart worldserver. Its log should contain `DronePose listening`.

Optional check from the computer that will run Velocidrone, after that line appears. Use the worldserver's address if it is not this machine:

```bash
python3 tools/pose_packet.py --send 127.0.0.1 47561
```

The world log should show `DronePose seq=7 unity=(1.000, 2.000, 3.000) wow=(3.000, -1.000, 2.000)`. That is the axis check from `docs/pose-protocol.md`.

## 2. Velocidrone plugin

On Linux, `mono-devel` must provide `mcs` and `mono`. Point the build at your Velocidrone directory if it is not `~/Apps/velocidrone/app`:

```bash
export VelocidroneDir="$HOME/Apps/velocidrone/app"
bash bridge/PoseBridge/build.sh
```

On Windows, install BepInEx 5 into the Velocidrone folder, then build with the .NET Framework compiler. Python 3 has to be on `PATH` (`py` or `python`):

```powershell
.\bridge\PoseBridge\build.ps1 -VelocidroneDir "C:\Path\To\Velocidrone"
```

Either script compiles `PoseBridge.dll`, checks the packet layout, and copies the DLL to `BepInEx\plugins`. Restart Velocidrone after every new DLL. A running game keeps the old one.

On the first launch the plugin writes `BepInEx/config/com.crash.velocidrone.posebridge.cfg`.

| Key | Default | Meaning |
|---|---|---|
| `RemoteHost` | `127.0.0.1` | Address of the machine running worldserver |
| `RemotePort` | `47561` | Pose port. Match `DronePose.Port` |
| `CameraHost` | `127.0.0.1` | Local camera. Leave this on this computer |
| `CameraPort` | `47562` | Local camera port |
| `OpenSky` | `true` | While armed, Velocidrone scenery does not stop the quad |

If worldserver is on another machine, set `RemoteHost` to that machine and open UDP `47561` to it. `CameraHost` stays `127.0.0.1`, and OpenWoW runs beside Velocidrone.

In Velocidrone, turn on manual arm. Armed means the camera and the character follow the quad. Disarmed, or sitting still on the pad, the plugin stops publishing. After about a second the character is yours again and can walk. The radio still works while the world window is focused.

Zero throttle in Velocidrone still falls. That fall is the simulator. This bridge does not make the quad collide with Azeroth's ground.

## 3. OpenWoW camera

Clone the snapshot, pin the commit, apply the patch, and add the two new files.

```bash
git clone https://github.com/rkabachenko/OpenWow-snapshot.git
cd OpenWow-snapshot
git checkout 2521e1f72eeffd661913be63d1e2b374073c316c
git apply /path/to/wow-fly/client/openwow/openwow.patch
cp /path/to/wow-fly/client/openwow/drone_fpv.cpp src/openwow/game/drone_fpv.cpp
cp /path/to/wow-fly/client/openwow/drone_fpv.h src/openwow/game/drone_fpv.h
```

On Windows, from PowerShell, with Git for Windows installed:

```powershell
git clone https://github.com/rkabachenko/OpenWow-snapshot.git
cd OpenWow-snapshot
git checkout 2521e1f72eeffd661913be63d1e2b374073c316c
git apply C:\path\to\wow-fly\client\openwow\openwow.patch
copy C:\path\to\wow-fly\client\openwow\drone_fpv.cpp src\openwow\game\drone_fpv.cpp
copy C:\path\to\wow-fly\client\openwow\drone_fpv.h src\openwow\game\drone_fpv.h
```

Build the client the way OpenWoW documents in `docs/BUILDING.md`. On Windows that means Visual Studio 2022 with the C++ workload, a Developer command prompt so Ninja can find `cl.exe`, and `bootstrap-vcpkg.bat` instead of the shell bootstrap.

```bash
cmake --preset release
cmake --build build/release --target openwow-client
```

Launch the built client from the ChromieCraft directory, the one that contains `Wow.exe` and `Data/`. Set `set realmlist` in that folder's `Data/enUS/realmlist.wtf` to your worldserver, then log in as the character named in `DronePose.CharacterName`.

```bash
cd /path/to/ChromieCraft_3.3.5a
/path/to/OpenWow-snapshot/build/release/apps/client/openwow-client
```

```powershell
cd C:\Path\To\ChromieCraft_3.3.5a
C:\Path\To\OpenWow-snapshot\build\release\apps\client\openwow-client.exe
```

The first time the drone view runs, the client writes `drone_fpv.txt` in that directory:

```
pitch 55
fov 137
height 1.6
```

`pitch` is degrees up from the quad nose. `fov` is vertical degrees. `height` is the eye above the character's feet, in yards. While flying, `[` and `]` change pitch, `-` and `=` change field of view, and Page Up / Page Down change height. The file is reloaded when you edit it.

Add a `realm` line only if this client should refuse every world except one:

```
realm 127.0.0.1
```

With no `realm` line, the camera follows whichever realm you selected.

## Fly

1. Start worldserver and confirm `DronePose listening`.
2. Log in with OpenWoW and stand where the flight should begin. That spot becomes the pad.
3. Start Velocidrone, arm, and fly. The view switches when the quad leaves the pad (about 1.5 m/s). Cut the throttle and disarm, or bring the quad back to the pad, and the character walks again.

On your own realm, `.cheat god` keeps that character from dying. It is not required for the pose.

The character's body is hidden during the drone view and shown again when you walk. The view follows the quad. It does not stop at walls or terrain in the world.
