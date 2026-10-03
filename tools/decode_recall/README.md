# Decode recall

Scores the firmware decoder against pinned `jt9` truth. How it fits the decode budget is in [docs/architecture/decode.md](../../docs/architecture/decode.md) (**Recall against jt9**).

```bash
python3 tools/decode_recall/report.py
```

Today's firmware settings are the default. Flags (`--candidates`, `--time-osr`, `--freq-osr`, `--decodes`, `--min-score`, `--ldpc`, `--sample-rate`, `--agc on|off`, `--levels`, `--subtract-passes`, `--subtract-neighbors`, `--subtract-fraction`) select another configuration and skip the CI floor check. The firmware searches twice (`--subtract-passes 2`), lowering each decoded signal by half its estimated SNR (`--subtract-fraction 0.5`) in the decoded `time_sub` and the neighbouring one (`--subtract-neighbors 1`). `--subtract-passes 0` is one search and no subtraction. `--discordant a.log b.log` counts messages one saved run found and the other missed.

`--write-truth` needs `jt9` from WSJT-X 3.0.1 on `PATH` and is not a CI step. `--write-baseline` records the floors CI enforces.
