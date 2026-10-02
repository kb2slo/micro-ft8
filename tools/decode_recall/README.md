# Decode recall

Scores the firmware decoder against pinned `jt9` truth. How it fits the decode budget is in [docs/architecture/decode.md](../../docs/architecture/decode.md) (**Recall against jt9**).

```bash
python3 tools/decode_recall/report.py
```

Today's firmware settings are the default. Flags (`--candidates`, `--time-osr`, `--freq-osr`, `--decodes`, `--min-score`, `--ldpc`, `--sample-rate`, `--agc on|off`, `--levels`) select another configuration and skip the CI floor check. `--discordant a.log b.log` counts messages one saved run found and the other missed.

`--write-truth` needs `jt9` from WSJT-X 3.0.1 on `PATH` and is not a CI step. `--write-baseline` records the floors CI enforces.
