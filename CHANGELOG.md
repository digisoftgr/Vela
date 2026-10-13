# Changelog

All notable changes to Vela are listed here, newest first.
Versions follow [Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

## [0.7.3] - 2026-10-13

### Added

- Terminal panel: press F4, or click Terminal in the status bar, for an embedded Konsole below the panels. Ctrl+Shift+F4 moves the keyboard focus into it.
- The terminal follows the active panel, and a `cd` in the terminal moves the panel. Folder changes made in the shell show up right away for bash, zsh and fish, without changes to your shell configuration files. Following the terminal never takes the keyboard focus away from where you are typing.
- Sync only happens while the shell waits at its prompt. While a program runs, or inside ssh, tmux/screen, a container or another user's shell, or on a remote folder, sync pauses and the terminal's header says why. A Sync Now button catches up afterwards, and a Sync toggle keeps terminal and panels independent.
- Hiding the terminal ends its shell and anything running in it. Whether it is open, its height and the Sync setting are restored with the session.

### Changed

- Files and folders created, deleted or changed by other programs, or from the terminal, appear in the file view almost instantly instead of after most of a second. In folders that change constantly or are very large, updates return to the usual pace until things calm down. Network folders keep the usual pace.

### Fixed

- Arch package: dependencies use the correct package names.

## [0.7.2] - 2026-10-13

First public release.

### Added

- Tabs (Ctrl+T, Ctrl+W, Ctrl+Tab, Ctrl+1 to Ctrl+9) and split panels. Drag a tab to a panel edge to split, to the centre to merge, or out of the window to tear it off.
- Four view modes: Details, Medium Icons, Large Icons and Extra Large Icons, with a status bar slider and thumbnails in the icon views.
- Breadcrumb address bar, per-folder sort preferences, a live quick filter, and recursive name search (Ctrl+F) with glob support.
- Preview column (Space): folder snapshots that turn into full file views, scaled images, continuously scrolling PDFs, and an editable text view with syntax highlighting and atomic saves.
- File operations through KIO: copy, cut, paste, rename, trash, permanent delete, new folder and new file, plus Copy and Move to Other Panel, Undo, and an Operations window.
- "Open In" lists for pinning your own applications into the context menus, and "Open Terminal Here".
- Sidebar with Places, bookmarks and devices shared with Dolphin, mount and eject, and recent folders.
- Single-instance behaviour and the `org.freedesktop.FileManager1` D-Bus interface, so other applications can ask Vela to show a file or folder.
- Rebindable keyboard shortcuts, a searchable command palette (Ctrl+Shift+P), and a Settings dialog with theme, font, appearance, tab-restore and advanced options.
- Install guide for Arch Linux and Kubuntu.
