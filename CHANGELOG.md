# Changelog

All notable changes to Vela are listed here, newest first.
Versions follow [Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

## [0.7.6] - 2026-10-14

### Changed

- Building Vela needs KF6 Parts, required since 0.7.3 for the terminal panel (Arch: `kparts`, Kubuntu: `libkf6parts-dev`). If updating from an older version fails, install it, or run the dependency installation step from the install guide again.
- The update instructions say what to do when an update fails: install any new dependencies, reconfigure, and build again.

## [0.7.5] - 2026-10-14

### Changed

- The README shows how to update an existing installation.

## [0.7.4] - 2026-10-14

### Added

- Back and forward restore each folder's selection and scroll position, and going up selects the folder you came from.
- Each tab keeps its own filter and search when you switch tabs.
- The arrow keys and Enter keep working on the file list while you type in the quick filter.
- Deleting on a location without a trash (sftp, smb, ...) explains why and offers permanent deletion instead.
- The address bar accepts `~` and paths relative to the current folder.
- Saving in the text editor asks before overwriting a file that changed on disk since it was opened.

### Changed

- The five most recently used tabs of a panel stay loaded: switching back to one shows it instantly, as you left it, and then picks up any changes. Older tabs are read from disk again when shown.
- The quick filter clears when you change folder.
- Subfolder search shows the nearest folders' results first, stops at 10,000 results with a hint to refine the term, and updates the list more smoothly while results stream in.
- A folder's saved sort order is applied as soon as it opens, so large folders no longer re-sort and jump once they finish loading.
- Light and Dark themes prefer Breeze when several colour schemes are installed.
- Making Vela the default file manager uses the installed application entry, and removes an outdated copy left by earlier versions.
- Lower disk and CPU use: file icons and types no longer read file contents while you scroll, folder previews and PDF scrolling do less work, and finishing a command in the terminal no longer re-reads local folders.

### Fixed

- Folder sizes disappeared for folders that changed, or never appeared for new ones, while size calculation was on.
- Items shown as cut stayed dimmed in one panel after something else was cut in another.
- A failed refresh of a network folder showed an error over valid contents and stopped live updates.
- Remote folder previews showed an arbitrary selection of entries instead of the first ones in sort order.
- The text editor could load a file far above its size limit if the file grew while open.
- Creating a folder or file directly in `/` did not select it afterwards.
- Unmounting a device left the preview's folder view showing it.
- Subfolder search on a remote folder reported "0 results" instead of saying it is not supported there.
- A stuck network mount could stall the Type column or all PDF previews.
- Large selections made copy, cut, delete and the context menu slow.

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
