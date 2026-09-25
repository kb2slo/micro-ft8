# Radio control: backends behind one table of operations

The core (slot loop, TX scheduler, HUD) talks to the radio only through `components/radio_control`. It never asks which radio it is.

## How it works

* **One table of operations per radio**, `radio_control_ops_t` (`components/radio_control/include/radio_control_backend.h`): `ready`, `on_audio_start`, `sync_frequency_mode`, `begin_tx`, `set_tone_hz`, `end_tx`, `set_tune`, `set_time`, the TX power/SWR trio `reset_tx_power_swr` / `poll_tx_power_swr` / `get_tx_power_swr`, and `begin_cpfsk_tx`. A null pointer means "not supported", never a crash.
* **Capabilities** (`radio_control_capabilities_t`, `radio_control.h`) are what the core and HUD ask instead of the radio's name: `has_wattmeter`, `has_swr`, `can_set_time`, `audio_is_uac`, `can_md8_tune`, `can_ps0`.
* **One `.cpp` per radio**: `radio_control_qmx.cpp`, `radio_control_qdx.cpp`. QMX and QMX+ are one backend until their CAT differs.
* **The product picker** is the table in `main/radio/radio_profile.h`: one row per radio, binding its name, audio source and backend.

## Rules

* **No `if (QMX)` in the core** — not in `main.cpp`, `poe_p4/main`, or the dispatcher in `radio_control.cpp`. Radio-specific behaviour is an operation or a capability.
* **QMX-only features go through a capability flag.** The SWR curve (I8) calls `MD8;` only where `can_md8_tune` is set; a CAT power-off (`PS0;`, I10) only where `can_ps0` is set.
* **Adding a radio** means filling in a new operations table and capabilities, and adding a row to `radio_profile.h` — no edit to the slot loop. This is the design's test; no second radio family has been added yet to prove it.
* **Why a table, not `class Radio`:** the variants are a closed set, and a C struct of function pointers is what the rest of this ESP-IDF tree already uses. [STYLE.md](STYLE.md) (**Structure**) allows inheritance only for closed sets like this one, and the table is the lighter form of it.
