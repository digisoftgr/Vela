# Installing Vela from source

Vela is built from source: you download the code and compile it on your own machine. It takes a few minutes.

These instructions cover **Arch Linux** and **Kubuntu**. Other distributions work too, as long as they provide Qt 6 and KDE Frameworks 6 development packages (see [Other distributions](#other-distributions)).

## Requirements

- A 64-bit Linux system with KDE Frameworks 6 development packages
- A C++17 compiler, CMake 3.20 or newer, and git
- A few hundred MB of free disk space for the build, plus a few minutes of compile time
- `sudo` rights for installing packages and for the final install step

## Arch Linux

1. **Install the dependencies:**
   ```bash
   sudo pacman -S --needed git cmake extra-cmake-modules qt6-base qt6-tools \
       kbookmarks kcolorscheme kconfig kconfigwidgets kcoreaddons \
       kglobalaccel kguiaddons ki18n kio kjobwidgets kservice \
       kwidgetsaddons kxmlgui solid syntax-highlighting poppler-qt6
   ```
   On Arch the KDE Frameworks 6 packages have no `kf6-` prefix.

2. **Get the code, build and install:** see [Build and install](#build-and-install) below.

**Arch alternative:** build a real pacman package, which you can remove later with `sudo pacman -R vela`. Run this in an **empty folder**, not inside the cloned repository, because makepkg uses a `src/` folder and the project has its own:
```bash
mkdir vela-build && cd vela-build
curl -LO https://raw.githubusercontent.com/digisoftgr/Vela/main/PKGBUILD
makepkg -si
```
This installs the build dependencies for you and replaces steps 1 and 2.

## Kubuntu

Kubuntu **25.10 or newer** is required. Kubuntu 26.04 LTS ships KDE Frameworks 6.24, which is what these instructions were checked against. **Kubuntu 24.04 and older do not ship the KDE Frameworks 6 development packages**, so the build will not work there without a backports source.

1. **Install the dependencies:**
   ```bash
   sudo apt update
   sudo apt install build-essential git cmake extra-cmake-modules \
       qt6-base-dev qt6-tools-dev \
       libkf6bookmarks-dev libkf6colorscheme-dev libkf6config-dev \
       libkf6configwidgets-dev libkf6coreaddons-dev libkf6globalaccel-dev \
       libkf6guiaddons-dev libkf6i18n-dev libkf6jobwidgets-dev libkf6kio-dev \
       libkf6service-dev libkf6solid-dev libkf6syntaxhighlighting-dev \
       libkf6widgetsaddons-dev libkf6xmlgui-dev libpoppler-qt6-dev
   ```

2. **Get the code, build and install:** see [Build and install](#build-and-install) below.

> **Note:** these package names were checked against the Ubuntu 26.04 package index. The Kubuntu build itself has not been tested by the maintainer yet. If something fails, please open an issue with the output.

## Build and install

The same on every distribution, once the dependencies are installed:

```bash
git clone https://github.com/digisoftgr/Vela.git
cd Vela
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

What each command does:

| Command | What it does |
|---|---|
| `git clone` / `cd Vela` | Downloads the code and enters the folder. |
| `cmake -B build ...` | Checks that every library is present and prepares the build in a new `build/` folder. `Release` turns on optimizations. `/usr` is where the files will be installed. |
| `cmake --build build` | Compiles Vela. This is the slow step. |
| `sudo cmake --install build` | Copies the program, icon, desktop entry and D-Bus service files into `/usr`. |

## Starting Vela

- From a terminal: `vela`
- From the application menu: search for **Vela** (category System, File Manager)
- If the menu entry does not appear, run `kbuildsycoca6` or log out and back in.
- To use Vela as your default file manager, open **Settings** in Vela (Ctrl+,) and enable "Set Vela as the default file manager".

## Running without installing

Useful for trying it out or for development. Nothing is copied outside the project folder:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
bash run.sh
```

`run.sh` rebuilds if needed and starts Vela from the build folder.

## Updating

```bash
cd Vela
git pull
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

If the build fails after an update, re-run the `cmake -B build ...` configure command from above first.

## Uninstalling

From the same `Vela` folder, as long as the `build/` folder still exists:

```bash
sudo xargs rm < build/install_manifest.txt
```

This removes every installed file. It leaves any empty folders behind. Your package manager does not know about these files, which is why this extra step is needed.

## Optional packages

| Feature | Arch | Kubuntu |
|---|---|---|
| Compress and extract archives from the context menu | `ark` | `ark` |
| Default terminal for "Open Terminal Here" | `konsole` | `konsole` |
| More thumbnail types, network protocols (sftp, smb) | `kio-extras` | `kio-extras` |

## Troubleshooting

| Problem | Likely cause and fix |
|---|---|
| `Could not find a package configuration file provided by "ECM"` | `extra-cmake-modules` is not installed. |
| `Could not find a package configuration file provided by "KF6..."` | A KDE Frameworks 6 development package is missing, or your distribution is too old to have them (Kubuntu before 25.10). |
| `Could NOT find Poppler` | Install `poppler-qt6` (Arch) or `libpoppler-qt6-dev` (Kubuntu). |
| `vela: command not found` after install | Check that `/usr/bin/vela` exists. If you used a different install prefix, add its `bin` folder to your `PATH`. |
| Build fails with an unclear error | Delete the `build/` folder, run the configure command again, and open an issue with the full output. |

## Other distributions

You need:

- Qt 6 (Core, Widgets, Concurrent, DBus) development files
- Extra CMake Modules 6
- KDE Frameworks 6 development files for: Bookmarks, ColorScheme, Config, ConfigWidgets, CoreAddons, GlobalAccel, GuiAddons, I18n, JobWidgets, KIO, Service, Solid, SyntaxHighlighting, WidgetsAddons, XmlGui
- Poppler Qt 6 bindings
- CMake 3.20 or newer and a C++17 compiler

Package names differ per distribution, for example `kf6-kio-devel` on Fedora. Then follow [Build and install](#build-and-install).
