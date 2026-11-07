# Optional desktop integration

MergeDesk is a compiled Qt 6 application. Run the executable directly; no launcher script is required. File-manager context-menu actions are always optional. The DEB installs an application-menu entry, AppStream metadata and icons with the package. Portable distributions install no desktop integration automatically.

Open **Desktop Integration** from the application menu to install or remove shortcuts for your current user. All changes stay in the current user profile. You can select several installed file managers. Reinstall the integration after moving the executable: the actions store its absolute location.

## GNU/Linux

Select two or three local files or folders, then choose **Compare with MergeDesk**.

| Desktop / file manager | Installed files | Requirements |
| --- | --- | --- |
| Application menu | `$XDG_DATA_HOME/applications/mergedesk.desktop` and hicolor icons | A desktop following the freedesktop specification |
| KDE Dolphin | `$XDG_DATA_HOME/kio/servicemenus/mergedesk-{2,3}.desktop` | Dolphin with KIO service-menu support |
| GNOME Files (Nautilus) | `$XDG_DATA_HOME/nautilus-python/extensions/mergedesk.py` | The distribution's `python3-nautilus` package; Nautilus 3 or 4 API |
| Cinnamon Nemo | `$XDG_DATA_HOME/nemo/actions/mergedesk-{2,3}.nemo_action` | Nemo Actions |
| Xfce Thunar | MergeDesk action in `$XDG_CONFIG_HOME/Thunar/uca.xml` | Close Thunar before changing actions to avoid its process saving an older configuration |

The defaults are `~/.local/share` for data and `~/.config` for configuration. Restart the affected file manager after installation or removal. Installation does not install system packages or stop another application.

The Nautilus extension passes local paths directly as process arguments with `shell=False`. It does not invoke a shell or wrapper. The other file managers launch the same real executable through their native action format. Remote URLs are not downloaded by the integration.

For command-line installation, run the compiled binary from its distribution directory:

```sh
./mergedesk --install-desktop-integration
./mergedesk --install-file-manager-integration=dolphin
./mergedesk --install-file-manager-integration=nautilus
./mergedesk --install-file-manager-integration=nemo
./mergedesk --install-file-manager-integration=thunar
```

Each operation also has a matching `--remove-desktop-integration` or `--remove-file-manager-integration=<manager>` command. For an isolated profile, supply `--data-home=/absolute/directory` and `--config-home=/absolute/directory`. No GUI display is required for these commands.

## Windows Explorer

The optional Explorer entries are stored under `HKEY_CURRENT_USER\Software\Classes`, so administrator access is not required. The workflow is:

1. Right-click one file or folder and choose **Select for MergeDesk comparison**.
2. Right-click the other item and choose **Compare with selected in MergeDesk**.

On Windows 11 these classic context-menu actions may appear under **Show more options**. The remembered selection stays in the current user's MergeDesk configuration. If it no longer exists, MergeDesk opens its native path-selection screen with the new path. Use the application to open a three-way comparison.

```powershell
.\mergedesk.exe --install-file-manager-integration=explorer
.\mergedesk.exe --remove-file-manager-integration=explorer
```

## Updates and removal

Files and registry keys carry a MergeDesk ownership marker. Removal touches only matching entries. Foreign files, unrelated Thunar actions, and different icons remain in place. Invalid Thunar XML is rejected. File writes are atomic and installation refuses symbolic links along target paths. These checks prevent an ordinary install or uninstall from overwriting an unrelated file; they do not claim protection against another process racing directory changes.

An isolated regression check is available on GNU/Linux:

```sh
./mergedesk --selftest-file-manager-integration
```

It tests generated formats, repeated installation, removal, executable-path quoting, preservation of unrelated actions, malformed configuration and symlink guards in a temporary directory. It never changes the current user's desktop settings or launches an installed action. Interactive appearance in the actual file managers and Explorer still requires testing on those desktops.

## License and source

Copyright (C) 2026 davoda1. GPL-3.0-or-later. The application-menu integration builds on LibreMerge by Iago Passos and contributors; upstream notices remain in the source. Generated Nautilus code carries its license notice.

Format references: [Dolphin service menus](https://develop.kde.org/docs/apps/dolphin/service-menus/), [Nemo action format](https://github.com/linuxmint/nemo/blob/master/files/usr/share/nemo/actions/sample.nemo_action), [Thunar custom actions](https://docs.xfce.org/xfce/thunar/custom-actions), and [Nautilus Python API](https://gnome.pages.gitlab.gnome.org/nautilus-python/).
