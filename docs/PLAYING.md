# Mario Strikers: Battle League (PC recompilation)

## Install

You need 64-bit Windows 10 version 1903 or later, or Windows 11, a graphics driver that
supports Vulkan, and at least 20 GB of free space. The game's videos need Windows'
free **VP9 Video Extensions** (Microsoft Store; most PCs already have them). Each player must have their
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

## Play together

Local Wireless plays with PCs on the same home network and with players over
the internet. There are no accounts, codes or ports to set up.

1. Everyone uses the same release version.
2. One player opens **Quick Battle → Local Wireless** and creates a room.
3. The others open Local Wireless on their PCs and choose that room. Rooms on
   your own network and rooms on the internet show up in the same list.

**Play only with your friends:** everyone presses **F1**, opens **Online**,
types the same **room key** (any word you agree on) and chooses **Use this
key**. Then search for rooms again. With a room key you see only the rooms of
players who typed the same key. Leave it empty to see everyone's rooms.

**Internet rooms are coming soon.** The relay server is not online yet. Until it
is, rooms on your own network work and F1 → Online says **Not reachable** for
internet rooms. They switch on by themselves once the relay starts: nothing to
download or change.

**F1 → Online** also shows whether internet rooms are connected, and your ping.
Internet rooms go through the project's relay server. It sees your IP address
and the room's game traffic, and keeps no record of IP addresses. To play only
with PCs on your own network, untick **Internet rooms** there and restart
(`mode = lan` under `[Network]` in `strikers.ini`).

The first time, Windows may ask whether to allow the game on networks: allow it
on **private** networks. This is for rooms on your own network; internet rooms
work either way. If you said no, open **Windows Security → Firewall & network
protection → Allow an app through firewall** and tick **Private** for
`strikers_native.exe`. Guest or public Wi-Fi often keeps PCs on it apart; play
over the internet rooms there instead.

## Advanced: a private group over ZeroTier

Most players don't need this: internet rooms already play over the internet.
A group is for organizers who want their own invitation-only network. It needs
an organizer who runs the group service.

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

## Rooms in a group

In a group, one friend chooses **Create room** in Local Wireless. That person is the host;
any group member can host. The other friends choose the host's room and join.
Everyone should use the same release version and finish joining the group
before looking for rooms. Friends shown in F1 are group members; that list does
not itself mean a game room has been created.

An invitation in F1 tells a friend that someone is hosting. Close the overlay
and use the Local Wireless room menu to enter the room. This project uses
Local Wireless over the group's network. Nintendo Online Play and Nintendo
account sign-in are not part of this setup.

## When something fails

**Send a report to the developer:** press **F1**, open **Report**, say what
happened and press **Send report**. The report holds the game's logs, your
processor, graphics card, Windows version and memory, your settings (invite
code and room key removed) and, if you leave it ticked, a screenshot. Your
Windows user name and PC name are taken out of file paths. You get a report ID
to mention if you also post about the problem. Nothing is sent unless you press
Send. **F9** saves the same report in `userdata\reports` without sending it.

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
- **Videos are black:** install the free **VP9 Video Extensions** and **Web
  Media Extensions** from the Microsoft Store, then start the game again. The
  game plays its videos with them; without them each video plays as a black
  screen for its length and the game goes on. (Before 0.2.1 the loading screen
  after a match could wait for good instead: update to 0.2.1 or later.)
- **The game will not launch:** make sure the ZIP was completely extracted and
  setup finished. Send `userdata\last-game.log` to the organizer. A graphics
  driver without Vulkan support cannot run this renderer.
- **The game crashes:** the launcher saves a crash report in
  `userdata\crashes` and asks whether to send it to the developer. **Yes**
  sends the game's log, the crash report and a memory snapshot of the game when
  it crashed, with your user name and PC name taken out of file paths. **No**
  sends nothing. A memory snapshot can contain parts of the running game, so
  share the files in `userdata\crashes` only privately.
- **No room appears on the same network:** check that everyone uses the same
  release, that the PCs are on the same network (not guest Wi-Fi), that one
  player actually created a Local Wireless room, and that the game is allowed
  on private networks in Windows Firewall. `userdata\last-game.log` names the
  network adapter the game used, in a line starting with `[lan]`.
- **No internet room appears:** open **F1 → Online**. It should say
  **Connected**. Check that everyone uses the same release and the same room
  key (or that everyone left it empty), that one player created a room, and
  then search again. **Not reachable** means the game cannot reach the relay
  server. Check that the PC is online; some school, office and hotel networks
  block games. Rooms on your own network still work.
- **No room appears in a group:** check that everyone sees Connected, uses the same
  release and group, and that one friend actually created a Local Wireless
  room. Ask for a new invitation if yours expired. Do not change router,
  firewall or Windows settings as a first step.
- **Controls go to the wrong player:** open F1's controller settings and check
  the keyboard and pad assignments. Bring the game window to the foreground.

Never send your extracted game files, generated game/SDK DLLs or build folder,
`userdata\zerotier\identity.secret`, or `userdata\group-credential.dat` to
friends. Each player builds their own game from their own copy.
