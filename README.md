![Ship of Harkinian](docs/shiptitle.darkmode.png#gh-dark-mode-only)
![Ship of Harkinian](docs/shiptitle.lightmode.png#gh-light-mode-only)

* [Website](https://www.shipofharkinian.com)
* [Discord](https://discord.com/invite/harbourmasters)

If you're having any trouble after reading through this `README`, feel free to ask for help in the Support text channels. Please keep in mind that we do not condone piracy.

# Reel Fishing III mod (this fork)

This fork (branch `mod/reel-fishing-3`) adds a mod that brings Reel Fishing III's fishing (PS2, 2003) into Ocarina of
Time. With it you get:
- RF3's cast, approach, hook set, and fight;
- RF3's rods, lines, lures, flies, baits, and floats, picked in RF3's own tackle box screen;
- RF3's fish, with their models, motions, and free swimming;
- a rod that works on any water in Hyrule, where fish live in regions;
- tackle earned by catches, records per species, and season, time and weather;
- RF3's sounds, vibration, and music;
- randomizer checks and co-op fishing through Anchor.

### What you need
- Your Ocarina of Time ROM, as for SoH.
- Your own **Reel Fishing III (USA), SLUS-20555**, as a `.bin` (MODE2/2352) or `.iso` disc image. Nothing from RF3 is
  included; the mod builds what it needs from your disc.

### First start
1. Right after SoH's own ROM step, a **"Reel Fishing III setup"** window opens.
2. Choose your RF3 disc image and press **Build**. Four files go into the `mods` folder:
   - `rf3_tackle.o2r`: the tackle box screen's pictures and texts;
   - `rf3_sounds.o2r`: RF3's sound effects;
   - `rf3_models.o2r`: fish, rods and tackle models;
   - `rf3_music.o2r`: optional, allows you to use Reel Fishing III's songs in the song randomizer.
3. The game starts with them loaded.

- **Not now:** skip for this start.
- **Don't ask again:** stop asking.
- Later: **Enhancements → Minigames → Fishing → Reel Fishing III Disc Setup**.

### Getting the rod
- Pay for fishing at the Fishing Pond once and Link keeps the Fishing Pole. RF3's ten rods are earned by catching
  fish, each with its own goal.
- Turn on **Enhancements → Items → Equip Items on Dpad**.
- In the pause menu, **tap C-Up on the Equipment page** to open the tackle box. Press a **D-pad button** there to put
  the rod on it. With **Settings → Controls → D-pad Support on Pause Screen** on, hold C-Up while pressing the D-pad
  button (as for any D-pad equip).

### Fishing controls

The keyboard keys are SoH's defaults (see [Configuration](#configuration)).

| Action | Controller | Keyboard |
|---|---|---|
| Take the rod out / put it away (line in) | the rod's D-pad button | T / F / G / H |
| Cast: hold through the swing, let go early for a shorter cast | hold **B** | hold C |
| Fly rods: false-cast, then let go | hold **B** | hold C |
| Pull the lure back (in the air, in the water or on land) | **B** | C |
| Reel in | hold **A** | hold X |
| Let the lure rest (bait is best left still) | let go of **A** | let go of X |
| Set the hook: during the rumble after a bite | **stick down** (jerk the rod) or **A** | S or X |
| Raise the rod (more pressure) | hold **stick down** | hold S |
| Lower the rod (give line) | hold **stick up** | hold W |
| Change the lure camera's view | **Z** | Z |

**In the fight**, watch the rod bend, feel the rumble, and listen for the reel's warning: too much tension breaks
the line.

### Tackle box (pause menu)
| Action | Controller |
|---|---|
| Open / close the tackle box | tap **C-Up** on the Equipment page |
| Move between rows (line, lure / fly, float, bait, Finish) | **stick up / down** |
| Change the tackle box (rod) | **stick left / right** |
| Change the target fish | **C-Left / C-Right** |
| Open a row; choose in a list | **A** |
| Back out of a list | **B** |
| Item types in the item list | **stick left / right** |
| An item's help | **L** |
| Put the rod on a D-pad button | **D-pad** |
| Back to the Equipment page | **A** on Finish |

### Settings
| Setting | Where | Default |
|---|---|---|
| Reel Fishing III Mechanics | Enhancements → Minigames → Fishing | on |
| Reel Fishing III Tension Gauge | Enhancements → Minigames → Fishing | off |
| Reel Fishing III Disc Setup | Enhancements → Minigames → Fishing | (window) |
| Write Reel Fishing III logs (for bug reports) | Enhancements → Minigames → Fishing | off (on in Debug builds) |
| RF3 Sound Volume | Settings → Audio | 100 % |
| Shuffle RF3 Tackle, RF3 Check Mode, RF3 Catch Checks | Randomizer settings | on, Advanced, 20 |

Debug builds also have the Reel Fishing III Debug Window, the Fish Regions editor, and a CSV log.

**Reporting a bug:** turn on **Write Reel Fishing III logs**, play until it happens, and send `logs/rf3_*.txt`.

# Quick Start

The Ship does not include any copyrighted assets.  You are required to provide a supported copy of the game.

### 1. Verify your ROM dump
You can verify you have dumped a supported copy of the game by using the compatibility checker at https://ship.equipment/. If you'd prefer to manually validate your ROM dump, you can cross-reference its `sha1` hash with the hashes [here](docs/supportedHashes.json).

### 2. Download The Ship of Harkinian from [Releases](https://github.com/HarbourMasters/Shipwright/releases)

### 3. Launch the Game!
#### Windows
* Extract the zip
* Launch `soh.exe`

#### Linux
* Execute `soh.appimage`.  You may have to `chmod +x` the appimage via terminal.
* When prompted, select your supported copy of the game.
* Saves, settings and `oot.o2r` are stored in `~/.local/share/soh/`. If the folder you launch from already has `shipofharkinian.json`, `oot.o2r` or `oot-mq.o2r`, that folder is used instead. Set `SHIP_HOME` to choose another folder.

#### macOS
* Run `soh.app`. When prompted, select your supported copy of the game.
* You should see a notification saying `Processing OTR`, then, once the process is complete, you should get a notification saying `OTR Successfully Generated`, then the game should start.

### 4. Play!

Congratulations, you are now sailing with the Ship of Harkinian! Have fun!

# Configuration

### Default keyboard configuration
| N64 | A | B | Z | Start | Analog stick | C buttons | D-Pad |
| - | - | - | - | - | - | - | - |
| Keyboard | X | C | Z | Space | WASD | Arrow keys | TFGH |

### Other shortcuts
| Keys | Action |
| - | - |
| ESC | Toggle menu |
| F2 | Toggle capture mouse input |
| F5 | Save state |
| F6 | Change state |
| F7 | Load state |
| F9 | Toggle Text-to-Speech |
| F11 | Fullscreen |
| Tab | Toggle Alternate assets |
| Ctrl+R | Reset |

# Project Overview
Ship of Harkinian (SOH) is built atop a custom library dubbed libultraship (LUS). Back in the N64 days, there was an SDK distributed to developers named libultra; LUS is designed to mimic the functionality of libultra on modern hardware. In addition, we are dependent on the source code provided by the OOT decompilation project.

In order for the game to function, you will require a **legally acquired** ROM for Ocarina of Time. Click [here](https://ship.equipment/) to check the compatibility of your specific rom. Any copyrighted assets are extracted from the ROM and reformatted as a .o2r archive file which the code uses.

### Graphics Backends
Currently, there are three rendering APIs supported: DirectX11 (Windows), OpenGL (all platforms), and Metal (MacOS). You can change which API to use in the `Settings` menu of the menubar, which requires a restart.  If you're having an issue with crashing, you can change the API in the `shipofharkinian.json` file by finding the line `gfxbackend:""` and changing the value to `sdl` for OpenGL. DirectX 11 is the default on Windows.

# Custom Assets

Custom assets are packed in `.otr` archive files. To use custom assets, place them in the `mods` folder.

If you're interested in creating and/or packing your own custom asset `.otr` files, check out the following tools:
* [**retro - OTR generator**](https://github.com/HarbourMasters64/retro)
* [**fast64 - Blender plugin**](https://github.com/HarbourMasters/fast64)

# Development
### Building

If you want to manually compile SoH, please consult the [building instructions](docs/BUILDING.md).

### Playtesting
If you want to playtest a continuous integration build, you can find them at the links below. Keep in mind that these are for playtesting only, and you will likely encounter bugs and possibly crashes. 

* [Windows](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-windows.zip)
* [macOS](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-mac.zip)
* [Linux](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-linux.zip)

### Further Reading
More detailed documentation can be found in the 'docs' directory, including the aforementioned [building instructions](docs/BUILDING.md).

* [Credits](docs/CREDITS.md)
* [Custom Music](docs/CUSTOM_MUSIC.md)
* [Formatting](docs/FORMATTING.md)
* [Controller Mapping](docs/GAME_CONTROLLER_DB.md)
* [Modding](docs/MODDING.md)
* [Versioning](docs/VERSIONING.md)

<a href="https://github.com/Kenix3/libultraship/">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/poweredbylus.darkmode.png">
    <img alt="Powered by libultraship" src="./docs/poweredbylus.lightmode.png">
  </picture>
</a>
