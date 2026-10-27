# Install GTA San Andreas FPV

Fly an FPV drone through San Andreas with your own radio, gamepad or
keyboard.

**[Download gtasa-fpv.zip](https://github.com/hammerill/gtasa-fpv/releases/latest/download/gtasa-fpv.zip)**

## 1. Have the right game

You need **GTA San Andreas 1.0 US** (`gta_sa.exe` of 14,383,616 bytes), the
version almost every PC mod needs. Steam and Rockstar Launcher copies must be
downgraded to it first. With any other version the mod logs it and stays off.

## 2. Have an ASI loader

Most modded installs already have one (CLEO and Modloader setups do). If not,
get the x86 [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)
and put its DLL next to `gta_sa.exe` as its readme says.

## 3. Install GTA San Andreas FPV

**With [Modloader](https://github.com/thelink2012/modloader) (recommended):**
extract `gtasa-fpv.zip` into the `modloader` folder. You get:

```
GTA San Andreas/
├── gta_sa.exe
└── modloader/
    └── FPV Drone/
        ├── GTASA.FPV.asi
        ├── GTASA.FPV.core.dll
        ├── libfpv.dll
        └── libfpv.LICENSE.txt
```

**Without Modloader:** put the four files from the `FPV Drone` folder next to
`gta_sa.exe`.

## 4. Fly

Start the game, load a save and press **F7**. CJ launches the drone.

| Key | Action |
|---|---|
| F7 | Launch the drone / return to CJ |
| F8 | Settings: radio setup, rates, OSD, camera |
| C | Cycle cameras: FPV, chase, line of sight |
| R | Respawn the drone, repaired |
| Y | Move CJ to the drone |

- **Radio:** plug it in (USB joystick mode), press F8 and follow the radio
  setup wizard once. Besides arm, it asks for angle mode, beeper, turtle, air
  mode and OSD disable switches; skip any you don't want.
- **Keyboard:** Enter arms, W/S is throttle, A/D is yaw, and the arrow keys
  are pitch and roll.
- **Gamepad:** works too.

Fly more than 20 m away and the world treats the drone as CJ: gangs, peds
and police react to it, and the city streams in around it.

## Tune it in Betaflight Configurator

1. In the game, launch the drone with F7.
2. In [Betaflight Configurator](https://app.betaflight.com), open Options and
   turn on **manual connection mode**.
3. Pick **Manual Selection**, enter `ws://localhost:5761` and press Connect.

While Configurator is connected Betaflight won't arm, the same as a real
quad. Keep the motor protocol on PWM and ARM on AUX1.

## Updating

Extract the newest `gtasa-fpv.zip` over the old files. Your settings are kept.

## Uninstalling

Delete the `modloader/FPV Drone` folder (or, without Modloader, the four
files plus `GTASA.FPV.core.live.dll`, `GTASA.FPV.log` and `fpv-data/`).

## Something wrong?

- **F7 does nothing:** check the log `GTASA.FPV.log` next to the `.asi`. If
  there is no log, the ASI loader didn't load the mod. If it says the exe is
  not 1.0 US, see step 1.
- **Anything else:**
  [open an issue](https://github.com/hammerill/gtasa-fpv/issues) and attach
  `GTASA.FPV.log`.
