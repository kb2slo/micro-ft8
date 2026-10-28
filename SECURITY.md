# Security

Micro-FT8 keys a transmitter, so a flaw that lets someone on the network transmit, change settings or
replace firmware without pairing matters more than it would in most hobby firmware.

## Reporting a vulnerability

Report it privately with GitHub's **Report a vulnerability** button on this repository's **Security** tab,
not in a public issue. Say what an attacker can do, what they need (on the station's WiFi, a paired phone,
physical access) and how to reproduce it. You will get an answer, and credit in the fix if you want it.

## What is in scope

The main board and sidekick firmware, the web app, and the site's Worker. How pairing and the trust model
work: [docs/architecture/sidekick.md](docs/architecture/sidekick.md) and
[docs/architecture/system.md](docs/architecture/system.md). Only the tip of `main` is supported; the project
is pre-alpha and has no maintained releases.
