# MergeDesk localization

MergeDesk 1.0.0 starts in English and includes Hungarian, German and French.
Choose a language in **Edit → Options → General**, then restart MergeDesk to
apply it to all menus and dialogs. The application language is independent of
the operating system's window decorations and native file dialogs.

## Translation sources

| Language | Qt locale | Source catalog |
| --- | --- | --- |
| English | `en_US` | English strings in `app/` |
| Hungarian | `hu` | `app/i18n/mergedesk_hu.ts` |
| German | `de` | `app/i18n/mergedesk_de.ts` |
| French | `fr` | `app/i18n/mergedesk_fr.ts` |

The compiled `.qm` catalogs are embedded in the executable. Qt's own translated
widget strings are packaged separately under `translations/`. System-provided
file dialogs can follow the desktop's language rather than MergeDesk's choice.

## Updating a catalog

Install the Qt 6 Linguist tools used by the build. From the repository root,
refresh the catalogs after adding or changing translatable application strings:

```sh
lupdate app -no-obsolete -ts app/i18n/mergedesk_hu.ts app/i18n/mergedesk_de.ts app/i18n/mergedesk_fr.ts
lrelease app/i18n/mergedesk_hu.ts
lrelease app/i18n/mergedesk_de.ts
lrelease app/i18n/mergedesk_fr.ts
```

Some distributions install these tools in `/usr/lib/qt6/bin`; use that full
path if the unqualified command is unavailable. Edit the `.ts` files with Qt
Linguist or an XML-aware editor, and commit both `.ts` and `.qm` files.
`lupdate` may discard XML comments: retain or restore the copyright and
translation provenance comments after refreshing each catalog.

Preserve substitution tokens such as `%1`, `%2`, `%n` and literal command-line
switches. Preserve meaningful newlines and HTML tags. Menu mnemonics use `&`;
choose unique letters within each menu, and write `&&` for a literal ampersand.
Keep **MergeDesk**, **WinMerge**, file extensions and the author name **davoda1**
unchanged. Original third-party legal notices must remain intact.

## Checking a translation

The regression runner exercises menus, About, Options and Qt widget strings in
all four supported languages:

```sh
packaging/run_selftests.sh /absolute/path/to/mergedesk
```

For an interactive check, override the saved language for that launch:

```sh
MERGEDESK_LANGUAGE=hu /absolute/path/to/mergedesk
```

Check the minimum window size in both light and dark modes, long paths, menu
shortcuts, plural wording and confirmation dialogs. Automated catalog and
layout checks complement review by a fluent speaker.
