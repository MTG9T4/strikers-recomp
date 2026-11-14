STRIKERS RECOMP — PLAYER SETUP (Mario Strikers: Battle League on PC)

Use 64-bit Windows 10 version 1903 or later, or Windows 11, with a Vulkan-capable
graphics driver and at least 20 GB free space. Extract the ZIP into a folder you can write
to, such as a folder in Documents. Do not run Setup from inside the ZIP.

1. On your own Switch, extract the game's files with a homebrew dump tool
   (nxdumptool, for example): the ExeFS of update v393216 (1.3.2) - main,
   main.npdm, rtld, sdk, subsdk0 - and the game's RomFS with that update
   applied. Copy both folders to your PC. This project reads no game package
   and no console key, decrypts nothing, and supplies no game files. Put the
   game's icon_AmericanEnglish.dat (from its control data) next to them for
   the game's own icon. Open StrikersSetup.exe and choose the two folders.
2. Choose Check files and build. Keep the PC on. Setup takes about 10 minutes,
   and can take longer on a slower PC. It also prepares your graphics, so your
   first match runs smoothly. Downloads are build tools only. Your game files are
   never uploaded. Their originals are read-only.
3. When ready, open Strikers Recomp in this folder. You may copy its
   shortcut to your Desktop. Keep the game folder where you extracted it.
   The first launch prepares graphics. The game window shows Preparing
   graphics with an elapsed time; this can take several minutes on a slower PC
   or after a driver update. Keep the cache folder with the game to reuse that work.

CONTROLS: Arrow keys move; Z confirms/A, X backs out/B, C is X, V is Y;
Q/E are L/R, A/S are ZL/ZR; Enter is +, Backspace is -. I/J/K/L are the
directional pad. Plug in a controller before opening the game. Positional
mapping normally means the right face button confirms. The game opens on the
PC start screen: PLAY, ONLINE, SETTINGS and QUIT. When a new version is out,
UPDATE appears there too; your saves and settings stay. In the game, F1 or Minus +
Plus opens the PC menu, with RESUME, ONLINE, SETTINGS and QUIT TO DESKTOP.
The in-game guide explains passes, shots and tackles.
F10 shows or hides a performance readout: frames a second, the time the game,
the render thread and the graphics card take, and which of them limits the game.
Speakers set to 5.1 in Windows get the game's 5.1 mix (Settings > Audio).
Hold Tab to fast-forward (4x; Settings > Audio / controls): the way past scenes
the game does not let you skip. Not in Local Wireless rooms.
Button pictures follow your pad: Xbox letters or PlayStation symbols in the
game's prompts (Settings > Audio / controls, restart).
Settings > Graphics: SMAA smooths jagged edges; HDR works when HDR is switched
on for your monitor in Windows' display settings (set peak to your monitor's).
Settings > Display > Ultrawide (experimental, restart): 21:9, 32:9 or the window's
shape shows more of the stadium at the sides. The interface is stretched to fit (player
markers stay on their players); "Centred" keeps its shapes but the markers drift inward.

SAVES FROM AN EMULATOR: Settings > Saves lists the Mario Strikers saves of Ryujinx,
yuzu, Eden and the other yuzu forks on this PC; Choose a file takes any strkrs.save
(a portable emulator, or a backup of your Switch save). Your current save is kept
and listed there as a Backup, so you can switch back.

MODS AND CHEATS: put mods in mods\<mod name>\romfs\..., the way yuzu and
Ryujinx keep them, and cheats (Atmosphere format) in cheats\<build id>.txt.
Settings > Mods and Settings > Cheats switch them on and off. For online play,
everyone needs the same mods; cheats wait while you are in a Local Wireless room.
Textures: make a folder textures\dump and the game saves every texture it loads
there as <code>_<size>.png; a textures\<code>.png replaces that texture (ASTC,
RGBA and one-channel textures, the most of this game's).

PLAY TOGETHER: Your organizer sends you an invite code. On the start screen
choose ONLINE (in the game: F1, then ONLINE), paste the code, enter your name
and choose Join. Wait for Connected.
One friend chooses Quick Battle > Local Wireless and creates a room. Everyone
else selects that room. Any friend can host. Use the same release version.
This is Local Wireless over the private group, not Nintendo Online Play.

IF SOMETHING LOOKS WRONG IN THE GAME: press F9. It saves one file,
userdata\reports\report-<date>.zip, with a screenshot, the logs and your settings
(your invite code is left out). Send it to your organizer if you choose.

IF SOMETHING FAILS: Send installer\setup.log for setup errors, or
userdata\last-game.log (previous-game.log holds the run before) and the latest
userdata\crashes report for game crashes
to your organizer if you choose. Reports are never sent automatically.
A crash report can show your Windows user name and your friends' display names.
Never send your game files, userdata\zerotier\identity.secret, or
userdata\group-credential.dat. For an expired group code, ask for a new code;
do not reinstall or copy another friend's userdata. See docs\PLAYING.md.

Each player builds their own game DLL locally. Never redistribute that DLL,
the generated build folder or game assets. This is an unofficial project, not
affiliated with Nintendo.
