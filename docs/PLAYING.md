# Mario Strikers: Battle League (PC recompilation)

## Install

You need 64-bit Windows 10 version 1903 or later, or Windows 11, a graphics driver that
supports Vulkan, and at least 20 GB of free space. Each player must have their
own copy of the game and files copied from their own Switch:

- **The ExeFS folder** of the game's update **v393216** (shown in the game as
  **1.3.2**): the files `main`, `main.npdm`, `rtld`, `sdk` and `subsdk0`.
- **The RomFS folder** of the game **with that update applied**: the folders
  `Characters`, `FrontEnd`, `Movies`, `wwiseaudio` and the others, and
  `global.data`.

Extract both on your own Switch with a homebrew dump tool (nxdumptool, for
example: dump the update's ExeFS, and the game's RomFS with the update applied)
and copy them to your PC. This project reads no game package and no console
key, and decrypts nothing; it does not supply game files. For the game's own
icon on the shortcut and windows, also put the game's `icon_AmericanEnglish.dat`
(from its control data) next to those folders; without it Windows' plain icon
is used.

Download the newest release ZIP from the project's Releases page on GitHub.
Extract the whole ZIP to a folder you can write to, such as
`Documents\Strikers Recomp`. Open `StrikersSetup.exe` (the programs are not
code-signed: if Windows SmartScreen stops it, choose **More info**, then **Run
anyway**). Choose the two folders
and press **Check files and build**. Do not open the ZIP and run Setup from
inside it. Keep the PC awake until Setup finishes. Setup copies the game's data
into its own folder (on the same drive it links the files instead, using no
extra space), so you may move your extracted folders afterwards.

Setup checks the game's identity and file integrity, then builds your game DLL
on your PC. Its progress estimate can change; a first build can take 10–60
minutes or longer on a slower PC. Your game files stay on your PC. Setup
does not upload them and does not change their originals. Build helpers stay
inside the extracted folder; you do not install Visual Studio or Python.

Your first launch prepares graphics for this PC. The launcher's elapsed time
shows that it is working; this can take several minutes on a slower PC or after
a driver update. Keep the `cache` folder so launches can reuse that work.

## First launch

Open **Strikers Recomp** in the extracted folder. You can copy that shortcut
to your Desktop or Start menu yourself. Keep the extracted folder in place.
The portable setup writes only inside that folder, so it does not automatically
write a shortcut to your Windows profile.

The launcher shows **Preparing graphics...** with an elapsed time until the
first game frame is ready. Graphics may need preparing again after a driver
update or a change of PC.

Your settings, saves and group identity are stored in `userdata` inside that
folder. To update to a new release, extract it into a new folder, run setup
there with the same ExeFS and RomFS folders, then copy your old `userdata`
folder into the new one. Do not copy a friend's `userdata` folder.

## Controls

Plug in a controller before launching. Xbox, PlayStation and Switch controllers
recognized by SDL can be assigned in the F1 controller settings. By default,
buttons normally follow their position: the **right** face button is Nintendo
A/confirm, and the **bottom** face button is B/back. GameCube controllers can
use their printed labels. Check the menu prompts before starting a match.

Keyboard input works while the game window has focus. Without a pad, the
keyboard normally controls player 1. With a pad, the keyboard takes the next
free player slot unless you change its assignment in F1.

| Keyboard | Switch control |
| --- | --- |
| Arrow keys | Left stick / movement |
| Z / X | A / B; confirm / back in menus |
| C / V | X / Y |
| Q / E | L / R |
| A / S | ZL / ZR |
| Enter / Backspace | Plus / Minus |
| I / J / K / L | Directional pad up / left / down / right |

The game's training and Game Guide explain passes, shots, tackles and other
match actions. Press **F1** to open PC graphics, controller and Online options;
press F1 again to return to the game. Start with the default window size and
resolution if you are unsure which graphics settings to choose.

## PC features

Press **F1** in the game for the PC settings. A setting marked **(restart)**
takes effect the next time you open the game. "The extracted folder" below is
the folder you extracted the release ZIP to.

- **Resolution:** Graphics > **Internal resolution** renders the game from 0.5x
  (960 x 540) to 2x (3840 x 2160, 4K); **Match window at startup** fits it to
  the window. Higher settings need a stronger graphics card.
- **Upscaling:** Graphics > **Final image scaling** chooses how the picture is
  fitted to the window: linear, AMD FSR 1 (with **FSR sharpness**), nearest or
  integer.
- **Ultrawide (experimental):** Display > **Ultrawide** shows more of the
  stadium at the sides, at 21:9, 32:9 or the window's shape. The interface is
  stretched to fit (player markers stay exact) or centred at 16:9 (markers
  drift inward).
- **HDR10:** turn on HDR in Windows' display settings first, then Graphics >
  **HDR**. Set **peak brightness** to your monitor's; **paper white** sets how
  bright the picture's whites are.
- **SMAA:** Graphics > **SMAA anti-aliasing** smooths jagged edges.
- **Anisotropic filtering:** set `anisotropic_filtering` to 2, 4, 8 or 16 in
  `strikers.ini` in the extracted folder (`game` keeps the game's own).
- **Frame rate:** the game runs at 60 fps, as on the Switch. Display >
  **Presentation cap** can lower it to 30; the game's clock stays at 60 Hz.
  **Vsync** is on the same tab.
- **Performance readout:** Graphics > **Performance readout**; F10 shows or
  hides it in the game.
- **Fast-forward:** hold **Tab** to run the game 2x to 8x faster, for scenes
  you cannot skip (Audio / controls > **Fast-forward**). It is off in Local
  Wireless rooms.
- **Saves from an emulator:** the **Saves** tab finds saves from Ryujinx, yuzu,
  Eden and the other yuzu forks on this PC, or choose a `strkrs.save` file (an
  emulator save, or a backup of your Switch save). Your current save is kept as
  a backup, and you can switch back to it there.
- **Mods:** put each mod in its own folder as `mods\<mod name>\romfs\...` in
  the extracted folder, the way yuzu and Ryujinx keep them, and switch them on
  or off in the **Mods** tab. Everyone in a Local Wireless room needs the same
  mods, or none.
- **Cheats:** cheat files in the Atmosphere format go in the `cheats` folder
  (the **Cheats** tab opens it), named by the game's build ID. Cheats wait
  while you are in a Local Wireless room.
- **Texture packs:** PNG files in a `textures` folder in the extracted folder
  replace the game's textures; each file is named by its texture's 16-digit
  hash. To find those names, make a `textures\dump` folder: the game then saves
  every texture it loads there, as `<hash>_<width>x<height>.png`. Delete the
  `dump` folder again when you have what you need.
- **Controls:** Audio / controls holds **Button pictures** (automatic, Switch,
  Xbox or PlayStation), **Pad layout**, the keyboard bindings (click a key to
  change it, right-click to clear it) and mouse clicks in simple menus (left
  confirms, right goes back).
- **Sound:** Audio / controls > **Speakers** chooses stereo or 5.1 surround;
  automatic follows Windows' speaker setup.

## Play on the same network

Local Wireless plays with PCs on the same home network (Wi-Fi or cable), with
no accounts and nothing to set up. This is the default (`mode = lan` under
`[Network]` in `strikers.ini`).

1. Everyone uses the same release version.
2. One player opens **Quick Battle → Local Wireless** and creates a room.
3. The others open Local Wireless on their PCs and choose that room.

The first time, Windows may ask whether to allow the game on networks: allow it
on **private** networks. If you said no, open **Windows Security → Firewall &
network protection → Allow an app through firewall** and tick **Private** for
`strikers_native.exe`. Guest or public Wi-Fi often keeps PCs apart; a home
network does not.

## Play over the internet with a group

Friends in different places play Local Wireless over a private group network
instead. This needs an organizer who runs the group service; if you only play
with PCs on the same network, skip this section.

Your organizer supplies a `strikers.ini` with `mode = zerotier`, the group service address and
private invitation code filled in. Keep that code within your group. Nobody
needs a ZeroTier account or the separate ZeroTier desktop app. The organizer
deploys the group service, makes a code and sends the ZIP and INI; the free
ad-hoc network is selected automatically after enrollment.

1. Open the game and press **F1**.
2. Open **Online**, enter your own display name and choose **Join**.
3. Wait until the status says **Connected**. On the first visit this can take
   a little while. If the code expired, ask the organizer for a fresh one.
4. Close F1 and open **Quick Battle → Local Wireless**.

F1 shows a message such as **Connected. 2 friends online.** IDs are available
only under Connection details. If the service is temporarily unavailable,
existing friends remain usable from the saved membership list, including on
the next launch. New enrollment and removal updates need the service to return.
A removed player is ignored after clients next refresh their membership list.

An existing enrollment has its own saved credential. Replacing the invitation
code does not repair a removed enrollment or lost identity; ask the organizer
for help. Do not delete or share identity files to try to repair it.

## Host or join a room

One friend chooses **Create room** in Local Wireless. That person is the host;
any group member can host. The other friends choose the host's room and join.
Everyone should use the same release version and finish joining the group
before looking for rooms. Friends shown in F1 are group members; that list does
not itself mean a game room has been created.

An invitation in F1 tells a friend that someone is hosting. Close the overlay
and use the Local Wireless room menu to enter the room. This project uses
Local Wireless over the group's network. Nintendo Online Play and Nintendo
account sign-in are not part of this setup.

## When something fails

- **Setup stops:** read its one-sentence message. Keep
  `installer\setup.log` and send it to your organizer if you choose. Damaged
  game files need a fresh copy from your own Switch. Restore a damaged
  cached helper by extracting the complete release ZIP again into a new folder;
  setup checks the pinned hashes before using any helper.
- **The first build seems slow:** wait while the progress changes. Large
  functions can take several minutes to compile. The estimate is approximate.
- **A build was interrupted:** reopen setup in the same folder and choose the
  same originals. Setup validates them again and resumes completed steps;
  incomplete compiler outputs are rebuilt. Keep the originals outside the
  installer's `build`, `installer`, `toolchain` and `userdata` folders.
- **The game will not launch:** make sure the ZIP was completely extracted and
  setup finished. Send `userdata\last-game.log` to the organizer. A graphics
  driver without Vulkan support cannot run this renderer.
- **The game crashes:** the launcher saves a local report in
  `userdata\crashes`. Send the latest `.dmp` and matching `.txt` if the organizer
  asks. They are never uploaded automatically. A report can contain parts of
  the running process, so share it privately.
- **No room appears on the same network:** check that everyone uses the same
  release, that the PCs are on the same network (not guest Wi-Fi), that one
  player actually created a Local Wireless room, and that the game is allowed
  on private networks in Windows Firewall. `userdata\last-game.log` names the
  network adapter the game used, in a line starting with `[lan]`.
- **No room appears in a group:** check that everyone sees Connected, uses the same
  release and group, and that one friend actually created a Local Wireless
  room. Ask for a new invitation if yours expired. Do not change router,
  firewall or Windows settings as a first step.
- **Controls go to the wrong player:** open F1's controller settings and check
  the keyboard and pad assignments. Bring the game window to the foreground.

Never send your extracted game files, generated game/SDK DLLs or build folder,
`userdata\zerotier\identity.secret`, or `userdata\group-credential.dat` to
friends. Each player builds their own game from their own copy.
