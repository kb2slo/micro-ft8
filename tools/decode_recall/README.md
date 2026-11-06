# Decode recall

Scores the firmware decoder against pinned `jt9` truth: what it decodes, and how far its SNR sits from `jt9`'s. How it fits the decode budget is in [docs/architecture/decode.md](../../docs/architecture/decode.md) (**Recall against jt9**).

```bash
python3 tools/decode_recall/report.py
```

Today's firmware settings are the default. Flags (`--candidates`, `--time-osr`, `--freq-osr`, `--decodes`, `--min-score`, `--ldpc`, `--sample-rate`, `--agc on|off`, `--levels`, `--subtract-passes`, `--subtract-neighbors`, `--subtract-fraction`, `--budget-units`, `--cost-search`, `--cost-candidate`) select another configuration and skip the CI floor check. The firmware searches up to twice (`--subtract-passes 2`; a search that subtracts nothing is the last), lowering each decoded signal by half its estimated SNR (`--subtract-fraction 0.5`) in the decoded `time_sub` and the neighbouring one (`--subtract-neighbors 1`). `--subtract-passes 0` is one search and no subtraction. `--discordant a.log b.log` counts messages one saved run found and the other missed.

`budget.py` decodes both corpora under a range of decode budgets and prints recall and the slots each cut; `--units` adds each slot's work with no budget. `test_budget.py` is its host test, run beside the report by `tools/check.sh` ([decode.md](../../docs/architecture/decode.md), **The work budget**).

The report also runs both corpora as synthetic I/Q through the receive front end ([tools/iq_convert](../iq_convert/README.md)), balanced and with a 0.5 dB, 2° mixer imbalance, with the corpus's next slot in the mirror channel; the balanced arm has floors, the imbalanced one is reported.

`--write-truth` needs `jt9` from WSJT-X 3.0.1 on `PATH` and is not a CI step. `--write-baseline` records the floors CI enforces, including the SNR's mean error and spread against `jt9`; rerun it after a deliberate SNR change.
