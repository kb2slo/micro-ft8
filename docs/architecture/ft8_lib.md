# `ft8_lib`: the vendor boundary

`ft8_lib` is third-party protocol code. We pin it, wrap it, and keep our changes to it in a shape its authors would recognise.

## Layout

* **The pin** is the git submodule `components/ft8_lib/vendor` → [kb2slo/ft8_lib](https://github.com/kb2slo/ft8_lib), a fork of [kgoba/ft8_lib](https://github.com/kgoba/ft8_lib) (Karlis) by way of [wcheng95/ft8_lib](https://github.com/wcheng95/ft8_lib) (Wei). Clone this repo with `--recurse-submodules`.
* **Our wrappers** live in `components/ft8_lib/`, outside the submodule: `common/fft_wrapper`, `common/monitor` (static BSS arenas), the IDF `CMakeLists.txt`, and `common/stpcpy_compat` (Windows host builds only). Do not compile `vendor/common/monitor.c`.
* **Protocol patches** are rebaseable commits on `kb2slo/ft8_lib`: Wei's Field Day (`d8a41e6`, `bb3d94d`, also [kgoba#54](https://github.com/kgoba/ft8_lib/pull/54)) and DXpedition type 0.1 (`5d095f4`). Nonstd is already upstream (`9fec6ca`). Do not push DXpedition onto Wei's `master` or onto #54.

## Rules

* Karlis's files look like Karlis's files. Do not clang-format, re-indent or restructure the tree. ESP glue goes in our wrappers, never into the fork.
* Contribute back only Karlis-shaped fixes (correctness, protocol). Never "make it compile on ESP32-S3".
* **Wei patches his in-tree copy, not `wcheng95/ft8_lib`.** That repo is stale at `bb3d94d` (2026-03) and fully absorbed into the pin; his live protocol work lands in his Micro-FT8's `components/ft8_lib/ft8/`, which has no submodule. His fixes reach us only by porting them by hand into the fork (B41 was the first). The session-start watch in [README.md](../README.md) (**Upstream watches**) diffs his tree against the pin.

## Bumping the pin

1. `git -C components/ft8_lib/vendor fetch origin`
2. `git -C components/ft8_lib/vendor fetch https://github.com/kgoba/ft8_lib.git master`
3. Diff Wei's in-tree `components/ft8_lib/ft8/` against the pin ([README.md](../README.md), **Upstream watches**) and port anything of his by hand.
4. Three-way merge Karlis into the fork: `git -C components/ft8_lib/vendor merge FETCH_HEAD`. Keep protocol commits rebaseable.
5. Push the fork, then move the pin: `git add components/ft8_lib/vendor`.
6. **The goldens gate the pin.** `tx_e2e` golden RX and encode must be green before the pin moves. Goldens fail → no bump.

The M5 components (`M5Cardputer`, `M5GFX`, `M5Unified`) are vendored too, under the same "do not format, do not fork unless we must", but have no bump procedure here.
