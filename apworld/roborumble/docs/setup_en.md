# RoboRumble Setup Guide

## Requirements

- RoboRumble from GOG or Steam
- Archipelago 0.6.7 or newer, with `roborumble.apworld` in `custom_worlds`

## Playing

1. Open the Archipelago Launcher and start **RoboRumble Client**; connect to the room.
2. Type `/run` in the client. It finds the GOG or Steam install, puts the game patch (`winmm.dll`) into the game's
   `DATA` folder and starts the game. `/run software`, `/run glide` or `/run direct3d` picks a renderer.
3. Load or start a campaign. Talk to the robot Zuom IV (rightmost portrait in the briefing) and choose
   **Choose a mission** to pick an unlocked mission.

If the game isn't found (or you want another copy), type `/path` to pick the game folder, or
`/path D:\Games\RoBoRumble`. The client remembers it. `/unpatch` takes the patch out again, `/game` shows what the
client found.

## GOG and Steam

- **GOG** ships nGlide, so the Glide renderer is used, and the CD soundtrack as `DATA\MUSIC\TrackNN.ogg`, which the
  patch plays.
- **Steam** has no Glide wrapper, so the software renderer is used (install nGlide for Glide). It has no music files:
  with the original CD in the drive the game plays the CD audio itself, or put the tracks as `Track02.ogg` ...
  `Track09.ogg` into `DATA\MUSIC`.

Settings live in `DATA
rpatch.ini` (window size, controls, music...). In `host.yaml`, `roborumble_options` has
`game_path` (if the install isn't found), `renderer` and `bridge_port`.
