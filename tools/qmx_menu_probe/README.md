# QMX menu probe

Reads a QMX's configuration menus over CAT with the Menu Manager (`MM`, `ML`) and prints them: the
**Band config.** table in every column, with each row's allowed values, and the menus under **System config**.
It is how the paths and replies the main board's link-up read relies on were found
([radio.md](../../docs/architecture/radio.md), **Menu Manager**), and reruns that on another firmware.

It only reads: no `MM` Set, no `MU;`, no `TX;`. It reads the radio's state (`IF;`) before and after and says
if the read changed it.

## Run

The QMX on this computer's USB instead of the main board's. Python 3 with pyserial:

```bash
python3 tools/qmx_menu_probe/menu_probe.py --port <QMX serial port>
```

`--walk "<path>"` lists another menu instead of System config (repeatable), for example `--walk Audio`.
