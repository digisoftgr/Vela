# Vela

Tabbed KDE Plasma file manager with built-in file and folder previews. Dolphin-compatible, fully customizable shortcuts, Wayland-first.

Vela is written in C++17 with Qt 6 and KDE Frameworks 6. It uses KIO for all file operations, so anything Dolphin can reach (local disks, removable media, sftp, smb, ...) works the same way. It shares Dolphin's Places and bookmarks.

**Status:** version 0.7.2, pre-1.0. Expect rough edges and report them.

## Features

### Browsing

- **Tabs and split panels.** Open tabs with Ctrl+T, close with Ctrl+W, cycle with Ctrl+Tab / Ctrl+Shift+Tab, jump with Ctrl+1 to Ctrl+9. Drag a tab to the edge of any panel to split the window there, drop it in the centre to merge it, or tear it out into its own window.
- **Four view modes.** Details, Medium Icons, Large Icons and Extra Large Icons. A slider in the status bar switches between them. Icon views show thumbnails, with a size limit you can set in Settings.
- **Breadcrumb address bar.** Click a segment to jump there. Ctrl+L focuses the bar for typing a path.
- **Per-folder sorting.** Each folder remembers its own sort column and order.
- **Quick filter.** Start typing in a file view and the list filters live. Esc clears the filter.
- **Recursive search.** Ctrl+F searches below the current folder by name, case-insensitively. A term containing `*` or `?` is treated as a glob, anything else as a subsequence match. Results stream in while the search runs. By default the search stays on the current filesystem, so searching `/` does not wander into `/proc` or network mounts.
- **Hidden files.** Toggle with Ctrl+H or Ctrl+. (or set the default in Settings).

### Preview column

Press **Space** on a selection to open or close the preview column. It follows the selected item:

- **Folders** show a capped, unwatched snapshot of their first entries (folders first, natural sort order). Click, focus or right-click into it and it becomes a full file view, with tabs, context menu and drag and drop. A folder selected inside that view opens another preview below it, to any depth.
- **Images** are scaled to fit. Large photos are decoded at a bounded size, with EXIF orientation applied, so a 60-megapixel picture does not eat your memory.
- **PDFs** scroll continuously, fit to width, and render only the pages on screen (Poppler).
- **Text and source files** open in an **editor** with syntax highlighting (KSyntaxHighlighting). Save with Ctrl+S. Saving is atomic, line endings (LF or CRLF) are preserved, and a file that is not valid UTF-8 is opened read-only so a save cannot silently corrupt it.
- **Everything else** gets an icon and metadata card.

### File operations

- Copy, cut, paste, rename (F2), trash (Delete), permanent delete (Shift+Delete), new folder (F7 or Ctrl+N for an auto-named one), new file (Ctrl+Alt+N) and properties, all through KIO.
- **Copy / Move to Other Panel.** With a split layout, send the selection straight to the other panel's folder. Move is bound to F6, Copy has no default key.
- **Undo** (Edit menu) reverses the last copy, move, rename, trash, new folder or new file.
- **Operations window** lists running transfers with progress. A toolbar indicator shows how many are running. An option keeps file operations out of Plasma's notification area.
- **Open In.** Pin your own applications (an editor, an IDE, ...) into the folder and file context menus, independent of the system "Open With" list. Configure in Settings.
- **Open Terminal Here** with the terminal of your choice (default: `konsole`).
- Archive compress/extract entries appear in the context menu when Ark (or another KIO service-menu provider) is installed.

### Sidebar

- **Places, bookmarks and devices** from KDE's own Places model, shared with Dolphin. Mount, unmount and eject from the sidebar, with errors shown instead of swallowed.
- **Recent folders** (can be switched off).
- Drag folders onto the sidebar to bookmark them.

### Integration

- **Single instance.** Launching `vela` again raises the existing window and opens the requested path there.
- **org.freedesktop.FileManager1.** Browsers, editors and other apps that ask the file manager to "show in folder" are served by Vela, even when it is not running yet (D-Bus activation). Settings has a switch to make Vela your default file manager.
- **Global shortcut** "Focus/Show Vela Window", unassigned by default. Assign it in the shortcut editor.

### Customization

- **Every action is rebindable** through the standard KDE shortcut editor (Settings, Configure Keyboard Shortcuts), with multiple bindings per action.
- **Command palette** (Ctrl+Shift+P) opens a searchable list of every command and its shortcut.
- **Settings** (Ctrl+,): default view mode, single-click to open, hidden files, recent tracking, delete confirmation, date format (DD/MM/YYYY or MM/DD/YYYY), light, dark or system theme, font, alternating row colours, toolbar icon size and labels, window border colour and thickness, restoring tabs on startup, terminal command, operation window behaviour, thumbnail size limit, and the Open In lists.

## Keyboard shortcuts

All of these can be changed.

| Action | Shortcut |
|---|---|
| Toggle preview column | Space |
| New tab / close tab | Ctrl+T / Ctrl+W |
| Next / previous tab | Ctrl+Tab / Ctrl+Shift+Tab |
| Go to tab 1 to 9 | Ctrl+1 to Ctrl+9 |
| Back / forward | Alt+Left / Alt+Right |
| Up one folder | Alt+Up or Backspace |
| Home folder | Alt+Home |
| Refresh | F5 |
| Focus address bar | Ctrl+L |
| Search below current folder | Ctrl+F |
| Quick filter | Just start typing |
| Command palette | Ctrl+Shift+P |
| Toggle sidebar | F9 |
| Show hidden files | Ctrl+H or Ctrl+. |
| Open selection | Enter |
| Rename | F2 |
| New folder / new folder (auto-named) | F7 / Ctrl+N |
| New file | Ctrl+Alt+N |
| Copy / cut / paste | Ctrl+C / Ctrl+X / Ctrl+V |
| Move to other panel | F6 |
| Move to trash | Delete |
| Delete permanently | Shift+Delete |
| Undo | Ctrl+Z |
| Settings | Ctrl+, |

## Installation

### Arch Linux (AUR)

```bash
paru -S vela
```

or with any other AUR helper, or manually:

```bash
git clone https://aur.archlinux.org/vela.git
cd vela
makepkg -si
```

### Build from source

Dependencies on Arch Linux:

```bash
sudo pacman -S cmake extra-cmake-modules qt6-base qt6-tools \
    kf6-kbookmarks kf6-kcolorscheme kf6-kconfig kf6-kconfigwidgets \
    kf6-kcoreaddons kf6-kglobalaccel kf6-kguiaddons kf6-ki18n kf6-kio \
    kf6-kjobwidgets kf6-kservice kf6-kwidgetsaddons kf6-kxmlgui \
    kf6-solid kf6-syntax-highlighting poppler-qt6
```

On other distributions, install the equivalent development packages for Qt 6 (Core, Widgets, Concurrent, DBus), the KDE Frameworks 6 modules listed above, Extra CMake Modules and Poppler's Qt 6 bindings.

Build and install:

```bash
git clone https://github.com/digisoftgr/Vela.git
cd Vela
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

### Run without installing

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
bash run.sh
```

`run.sh` rebuilds if needed and starts Vela from the build tree.

## Optional packages

- `ark`: compress and extract entries in the context menu.
- `konsole`: the default terminal for "Open Terminal Here". Any other terminal works, set it in Settings.
- `kio-extras`: more thumbnail types and network protocols (sftp, smb, ...).
- A KDE Plasma session gives full icon theme and colour scheme integration. Vela also runs under other desktops, but looks best with Plasma's theme.

## Known limitations

- Search matches file names only, and only on local filesystems. Baloo is not used.
- Remote folders (sftp, smb, ...) can be browsed and previewed, but not searched.
- The text editor reloads a file when it changes on disk, **including over unsaved edits**. A banner tells you when it happens.
- The global "focus window" shortcut must be assigned by hand.
- No batch rename and no built-in archive handling, beyond the Ark integration.

## Contributing

Issues and pull requests are welcome. The code follows KDE coding style, see `.clang-format`. There is no automated test suite yet, so describe how you tested a change in the pull request.

Source layout:

- `src/core/`: models, search, file operations, settings, D-Bus interfaces (no widgets)
- `src/ui/`: main window, panels, sidebar, dialogs
- `src/ui/preview/`: the preview column (folder, image, PDF, text)
- `resources/`: desktop file, icon, D-Bus service files, KXmlGui layout

## License

Vela is free software, released under the [GNU General Public License v3.0 or later](LICENSE).
