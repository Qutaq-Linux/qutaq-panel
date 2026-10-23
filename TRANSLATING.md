# Translating qutaq-panel

The panel is written in English; translations live in `translations/qutaq-panel_<lang>.ts`
(Qt Linguist format) and are built into the program.

Available: English (built in), Russian (`ru`), Ukrainian (`uk`), German (`de`).

## Fix or finish an existing translation

1. Open `translations/qutaq-panel_<lang>.ts` in **Qt Linguist** (package `qt6-tools` on Arch,
   `linguist-qt6` on Debian/Ubuntu, `qt6-linguist` on Fedora) or in any text editor.
2. Edit the `<translation>` texts. Keep placeholders as they are: `%1`, `%2`, `\n`, and `%%`.
   A trailing `…` means the item opens a window or dialog.
3. Rebuild and run (see below), then open a pull request.

## Tools

`lupdate` and `lrelease` are not in `PATH` on most distributions. Set their folder once:

```sh
QTBIN=/usr/lib/qt6/bin        # Arch, Debian, Ubuntu
QTBIN=/usr/lib64/qt6/bin      # Fedora
```

## Add a new language

```sh
cd qutaq-panel
cp translations/qutaq-panel_de.ts translations/qutaq-panel_fr.ts
```

In the new file, change `language="de"` to your language code (`fr`, `pt_BR`, `kk`…) and
replace every `<translation>` with your own. CMake picks up new `.ts` files automatically.

To start from empty translations instead, generate the file with `lupdate`:

```sh
$QTBIN/lupdate -no-obsolete -locations none src -ts translations/qutaq-panel_fr.ts
```

## After changing strings in the code

Developers: wrap every user-visible string in `tr("…")` (or `QCoreApplication::translate("Context", "…")`
outside a `Q_OBJECT` class), then refresh all `.ts` files:

```sh
for ts in translations/*.ts; do
  $QTBIN/lupdate -no-obsolete -locations none src -ts "$ts"
done
```

New strings appear with `type="unfinished"`; until translated, the panel shows them in English.

## Test your translation

```sh
cmake -B build && cmake --build build
LANG=fr_FR.UTF-8 ./build/qutaq-panel
```

Or, without rebuilding: compile your file with `lrelease` and put the `.qm` file into
`~/.local/share/qutaq-panel/translations/`. It takes priority over the built-in one:

```sh
mkdir -p ~/.local/share/qutaq-panel/translations
$QTBIN/lrelease translations/qutaq-panel_fr.ts \
  -qm ~/.local/share/qutaq-panel/translations/qutaq-panel_fr.qm
```

The panel log (`/tmp/qutaq-panel.log`) shows which file was loaded: `translation: …`.

The language follows the system locale (`LANG`). To force a language, add to
`~/.config/qutaq-panel/panel.conf`:

```ini
[panel]
language=fr
```
