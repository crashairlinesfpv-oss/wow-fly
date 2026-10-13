# Install mod-drone-pose

Full setup, including the plugin and the OpenWoW client, is in the repository `README.md`. This page is only the world module.

Copy `mod-drone-pose` into your AzerothCore 3.3.5a `modules/` directory and build:

```bash
cp -a server/mod-drone-pose /path/to/azerothcore-wotlk/modules/mod-drone-pose
cd /path/to/azerothcore-wotlk && ./acore.sh compiler build
```

The cmake summary must list `mod-drone-pose`. After the build, copy the installed `drone_pose.conf.dist` to `drone_pose.conf` in the same directory and set `DronePose.CharacterName` to the pilot. Leave it empty to apply the pose to nobody. Do not put a password in that file.

Restart worldserver. When the log shows `DronePose listening`, the axis check from this repo is:

```bash
python3 tools/pose_packet.py --send 127.0.0.1 47561
```

Use the worldserver address instead of `127.0.0.1` when it is another machine. The expected log line is `DronePose seq=7 unity=(1.000, 2.000, 3.000) wow=(3.000, -1.000, 2.000)`.
