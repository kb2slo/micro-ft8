# RFC 0011: One station core for both main boards

* **Status:** Rejected 2026-10-02. The ADV was dropped (row za6n), so there is one main board and no second copy of the CONFIG procedure to unify. Kept whole as drafted; it assumes two boards.
* **Author / Lead:** Jeff Kalikstein, KB2SLO
* **Covers:** the station model of roadmap row `j4xh` — who owns the station's settings and how a CONFIG write reaches them on the ADV and the P4.
* **Does not cover:** the QMX USB transport (CAT meter, IDs, FIFO tuning), moving the six `main/storage` files into `components/`, or giving the P4 IgnoreList, RxTxLog and Cabrillo. They are the rest of row `j4xh`; the last depends on this RFC.

---

## 1. What exists

Facts are from the working tree at `efe8f02`.

* **One shared struct, two owners.** `StationSettings` ([main/storage/station.h:10](../../main/storage/station.h)) holds everything `Station.txt` persists. `station_parse()`, `station_serialize()`, `station_key_known()`, `act_refs_prefer_sota()`, `station_key_names_operating_band()` and `station_vfo_refusal()` are shared and host-tested (`host_mock/test_station.cpp`).
* **ADV: loose globals are the truth.** `main/main.cpp` owns `g_call`, `g_grid`, `g_band_sel`, `g_bands`, `g_cq_type`, `g_offset_hz` and the rest, about 270 references in all (`g_bands` 51, `g_band_sel` 28, `g_offset_hz` 21, `g_radio` 21). The Station menu and screens edit them directly. A `StationSettings` exists only transiently: `station_fill_from_globals()` and `station_apply_to_globals()` ([main/main.cpp:3803](../../main/main.cpp), [:3828](../../main/main.cpp)) copy to and from it for load, save, and CONFIG.
* **P4: a persistent `StationSettings` is the truth.** `s_station` in `poe_p4/main/porta_host_stubs.cpp:34`, seeded by hand (`ensure_seeded()`), mirrored to `g_call` for the shared code that still reads it. There is no menu, so every change arrives as a CONFIG write.
* **CONFIG write: two copies of one procedure.** `porta_host_config_set()` exists at `main/main.cpp:3976` and `poe_p4/main/porta_host_stubs.cpp:458`. Both: validate the key and lengths, `station_parse()` one line, derive `cq_type` on the `act_refs` empty→non-empty transition, push to autoseq, persist, and if the key is band-ish retune and return `station_vfo_refusal()` on a miss. `porta_host_config_snapshot()` is likewise duplicated, with live fields appended.
* **The copies have already drifted** (found reading both, 2026-10-02; none is a field report):
  - The ADV re-syncs autoseq after every write (`sync_autoseq_from_globals()`); the P4 only for an allow-list of keys, so a new key that autoseq reads is silently missed on the P4.
  - `clamp_ignore_prefix_text()` (`main/main.cpp:1649`) lives in the ADV's `main.cpp`, so only the ADV clamps `ignore_prefixes`. `station_parse()` already range-checks `band_sel` to the 12-band table and normalizes `gps_baud` (`station.cpp:136`, `:241`), so those two are shared. The ADV also re-checks `band_sel` against its live `g_bands` size on apply.
  - The ADV rebuilds the active-band list and ignore-prefix list after a write; the P4 does neither, because it consumes neither (the P4 stores `ignore_prefixes` and `rxtx_log` but does not honour them).

The ADV also holds state that is not in `StationSettings` and should stay out of it: `g_tune`, `g_beacon`, the GPS grid (`g_grid_from_gps`, `g_grid_gps_display8`), and `g_protocol` (applied only at boot; `protocol_ft4` is the pending value).

## 2. Problem

Row `j4xh` wants the P4 to honour IgnoreList, RxTxLog and Cabrillo, and wants `main/` and `poe_p4/main/` to stop carrying parallel station logic. With two copies of the CONFIG procedure, each such feature is built twice, and the copies disagree already (§1). The ADV's globals make a straight "share the state" move expensive: about 270 references across menus and screens that only the operator can verify.

## 3. Options

**A. The ADV adopts a station object.** Replace the globals with one `Station` instance; menus edit its fields. One truth, no fill/apply adapter. Cost: rewrites the ADV's menu and UI code (§1, ~270 references), all field-only to verify, for a board whose long-term support is itself in question (row `za6n`).

**B. Share the procedure, keep each owner (recommended).** Add one component, `components/station_core`, holding the CONFIG procedure as a function over `StationSettings`:

```
station_config_set(StationSettings* s, const char* key, const char* value,
                   const StationHooks* hooks) -> const char* refusal_or_null
station_config_snapshot(const StationSettings& s, ...)
```

Everything both boards do identically moves in: key and length validation, the `ignore_prefixes` clamp, the activation→CQ-type rule, and the decision of what a key requires (resync autoseq: always; VFO retune: band-ish keys; refusal text). The board supplies `StationHooks`: `persist(const StationSettings&)`, `sync_vfo(why_out)`, `apply(const StationSettings&)` (the ADV runs `station_apply_to_globals()` and rebuilds its lists; the P4 stores), and `tx_active()`. The ADV keeps its globals and its fill/apply adapter; the P4 keeps `s_station`. Nothing in the ADV's UI changes.

This removes the duplicated procedure and the drift (§1), and gives a single place where IgnoreList, RxTxLog and Cabrillo hooks attach for the P4. It does **not** make the P4 and ADV hold state the same way, which is what option A buys.

**C. Do nothing now; keep the two copies and a parity test.** A host test feeds both procedures the same key sequence. Cheap, but it tests the drift rather than removing it, and the P4's copy cannot be linked on the host.

## 4. Recommendation

B, then reconsider A only if the ADV is kept. B is host-testable end to end: the shared function takes plain data and hook function pointers, so `host_mock` can run a scripted key sequence through it with stub hooks (this is also where row `gfe7`'s simulator can drive it). Only the board hooks (`persist`, `sync_vfo`, retune) are field-only.

## 5. Slices (if accepted)

1. **S1: extract and test.** Add `components/station_core` with `station_config_set`/`station_config_snapshot` and a `host_mock` test pinning today's ADV behaviour (act_refs→cq_type, refusal strings, clamps). The ADV calls it through hooks; the P4 is untouched. Verification: host test, ADV build. Behaviour change: none intended, field check owed for the Station sheet and a phone CONFIG write.
2. **S2: P4 adopts it.** Replace `porta_host_config_set`/`_snapshot` in `porta_host_stubs.cpp`. This changes P4 behaviour in the ways listed in §1 (always resync, the ignore-prefix clamp). Field check owed.
3. **S3: `s_station` becomes a shared home** (P4 only) once the hooks cover seeding, so `ensure_seeded()` and the `g_call` mirror shrink. Optional; decide after S2.

## 6. Open questions — the Ask

1. Is B the right size, or is the ADV's globals-to-object rewrite (A) wanted for its own sake?
2. Where does the `ignore_prefixes` clamp belong: in `station_parse()`, as the other clamps are, or in the new function? Parse changes what a `Station.txt` load accepts on the ADV.
3. Should `station_core` also own the live-state lines appended to the snapshot (`streaming`, `cat_ready`, `tune`, …), or stay settings-only with the board appending? The ADV and P4 compute them differently today, so settings-only is the smaller cut.
4. Is the ADV's `g_protocol`/`protocol_ft4` split (boot-applied) part of this, or left to row `xph7`?
