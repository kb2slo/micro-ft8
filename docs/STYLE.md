# Coding style

Small written preferences for *our* C/C++. Do not apply to vendored `M5*` / `ft8_lib`. Radio boundary: [architecture/radio.md](architecture/radio.md).

Baseline is ESP-IDF’s guide where it matches this tree. We do not adopt Google function naming or a full LLVM restyle.

## Prefer `switch` over chained `if`

When a value is matched against a closed set of constants (`char`, enum, integer
mode, tag name already reduced to an enum, …), use `switch`. Do not write a
chain of `if` / `else if` (or `||` equality tests) for that set.

Use `if` when the condition is not a constant match: ranges, compound
predicates, or a single test.

Do not use locale `isspace` unless those extra characters are intended.

Exhaustive enum `switch`: no `default`, so `-Wswitch` catches new enumerators.
Non-enum `switch`: `default` required. Annotate intentional fall-through.

```cpp
bool is_space(char c) {
    switch (c) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            return true;
        default:
            return false;
    }
}
```

## Naming

Document the mix. Do not rename existing files to match a purer scheme.

- `g_` — process-lifetime app state in `poe_p4/main`
- `s_` — file-static
- `snake_case` functions
- `CamelCase` types and enums
- Component prefix on exported symbols (`adif_`, `storage_`, `autoseq_`, `radio_control_`)

Prefer a full word over an unexplained abbreviation in new and extracted names (`capabilities` not `caps`). Documented prefixes (`g_`, `s_`) stay. Do not restyle vendored `ft8_lib` to match.

New extracts follow this on day one. Old names change only when that function moves.

## Headers and includes

Keep `.h`. `#pragma once`. `extern "C"` only on headers that C must include.

New files, include order: C/POSIX `<>`, then IDF, then other components, then own public, then own private. `""` for project and IDF headers.

## Braces and language

New files: function opening brace on its own line; `if` / `switch` / `for` brace on the same line (ESP-IDF). Until a file is extracted, match that file.

C++17. No exceptions, no RTTI. `std::string` / `std::vector` are fine in host-linkable units. Four spaces, LF, ~120 columns. No drive-by wrap of old lines.

No formatter on `ft8_lib`. Optional `.clang-format` later, only on new extract directories.

## Structure

File size is a ratchet, not a rewrite target. New files stay under about 500 lines of real code. `poe_p4/main/porta_host.cpp`, the largest, may only shrink. Do not split a file just to game the number.

Require a **module**, not a class. New behavior gets a name and a header API. One tested function is enough. A strategy interface for one implementation is not.

Inheritance only when there is a closed set of variants that already exists or is about to (radio backends, later modes). Radio uses `radio_control_ops_t`, not `class Radio`. A station/mode object model is I6 and needs its own RFC.

No new host-testable logic in `poe_p4/main/`: wiring and board-only code only.

**Where a module lives.** A `components/<name>/` directory with a `CMakeLists.txt` and an `include/` level. `host_mock/Makefile` compiles sources by path, so a component is host-tested directly. Board-only code (the entry point, the QMX USB transport, the TX scheduler, the Port A host actions) stays in `poe_p4/main/`. `ft8_lib` is vendored third-party: [architecture/ft8_lib.md](architecture/ft8_lib.md) vendor boundary — do not format, do not restructure.

**One main board** (2026-10-02, after row za6n dropped the ADV). Logic that is not specific to this board's hardware goes in a component, so a host test can reach it, rather than in `poe_p4/main/`. `porta_proto` is also compiled by the sidekick, so it is a component for that reason too.

**No HTML, CSS or JavaScript inside C or C++ source** (2026-09-12). A page is a `.html` file under `<project>/main/web/`. On the sidekick, CMake globs that directory into `EMBED_TXTFILES` and a generated seed table — drop a file in `web/` and it is embedded, hydrated onto LittleFS, and served at `/<filename>` (templated URLs like `/` and `/log` still need handlers). Values are interpolated with `{{name}}` placeholders, not `printf`. After `web_fs_init()` the LittleFS `web` partition is the sole HTTP origin ([architecture/sidekick.md](architecture/sidekick.md) (**Web root**)).

The cost of the old way was not ugliness. CSS had to be written `width:100%%` to survive `snprintf`, which put every page outside the reach of a formatter, a validator, a browser or a linter — a page's JavaScript could not even be checked for a syntax error. A page split across four fragments could not be read as a document, and every wording change was a C change. The move also closed a live hole: the status page interpolated the stored SSID raw, and an SSID is whatever a nearby access point broadcast, so substitution now escapes by default and passing markup through takes an explicit flag.

Two things stay in C: the choice of which page to serve, and the values substituted into it. Building markup in C by concatenation is what this rule exists to stop, with one exception — the scan list, which is a repeated element and is assembled in `wifi_prov.c` with each SSID escaped as it is rendered.

Dead code is deleted in the unit you are extracting. No repo-wide unused-function hunt mixed with a feature.

Fixes that belong inside an extract go with that extract (B5 overlapping `sscanf` with Station parse). Unrelated fixes stay out.

Restyle and rename only the unit you are already changing.
