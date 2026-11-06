# HL2 captured beside the P4, 2026-11-06

The P4 decoded the Hermes-Lite 2 while the computer between them, bridged into their cable, captured the HL2's stream: 12 m from 1:52 to 2:44 PM CST and 20 m from 2:45 to 3:06 PM, one receiver, the P4 on the decode budget of row [vnz7](../../../../docs/roadmap/several-channels.md#vnz7)'s first slice (P4 build `6f6f8c9-dirty`, `DECODE_BUDGET_UNITS` 6000). The same samples were then decoded on the host with no budget and with the P4's, which says what the budget costs on the air with nothing else changed; and the P4's own decodes were matched against the host's, slot by slot. How the capture works: [hl2.md](../../../../docs/architecture/hl2.md), **A capture beside the P4**.

**The capture is not in git.** It is one pcap of the HL2's packets to the P4 (`tcpdump -i br-hl2 -w hl2_12m.pcap udp src port 1024`), 1,833,684,134 bytes, SHA-256 `b49964ff320df6c13c2936b6ed70650d1cb9780823dd17ea98b30e08095f35b0`, kept by the maintainer and not published. `p4_events.jsonl` here is what the P4 sent the sidekick from 2:33 PM on (`GET /api/events`, every decode and log line, one event a line): 3,316 events, three polls lost to Wi-Fi and retried within 3 s, no event missed. The band changed to 20 m at 20:44:46.8 UTC, with the capture running; that slot and the next are not scored.

## Reproduce

```bash
python3 tools/hl2_pcap.py --dial-hz 24915000 --until 2026-11-06T20:44:45 --start-offset-s 0.7 --out c12 hl2_12m.pcap
python3 tools/hl2_pcap.py --dial-hz 14074000 --since 2026-11-06T20:45:15 --start-offset-s 0.7 --out c20 hl2_12m.pcap
python3 tools/decode_bench/capture_ab.py c12 --p4 tools/decode_bench/corpus/hl2-capture-2026-11-06/p4_events.jsonl
python3 tools/decode_bench/capture_ab.py c20 --p4 tools/decode_bench/corpus/hl2-capture-2026-11-06/p4_events.jsonl
```

`--start-offset-s` is where each slot starts after its UTC boundary; `--budget-units N` on `capture_ab.py` scores another budget. No packet was lost in either band, and no sample clipped converting.

## What the budget cost

On identical samples, the host with no budget against the host at 6,000 units:

| Band | Slots | Cut by the budget | No budget | Lost to the budget |
| --- | --- | --- | --- | --- |
| 12 m, slots cut on UTC | 208 | 208 | 827 | 1 (`CQ NM3B FN01`, −2 dB) |
| 12 m, slots cut 0.7 s late, as the P4 is | 207 | 207 | 827 | 0 |
| 20 m, on UTC | 83 | 34 | 1,932 | 2 (−7 and −3 dB) |
| 20 m, 0.7 s late | 83 | 35 | 1,954 | 2 (`KD3AIH WD4HMR R-17`, `CQ K4JMW EM77`, both −6 dB) |

2 messages in 2,781 on the P4's alignment, both on the busy band, in a second search the budget stopped, neither weak. On 12 m the budget cut every slot and cost nothing.

## The P4 against the host

The P4's own decodes against the host's at its budget, slots cut 0.7 s late, matched by the P4's slot index, every hashed call read as `<...>` (the P4 remembers calls across slots; the host decodes each file afresh):

| Band | Slots | Both | P4 only | Host only |
| --- | --- | --- | --- | --- |
| 12 m | 47 | 178 | 3 | 4 |
| 20 m | 83 | 1,874 | 78 | 78 |

What is left is alignment: the P4's clock sits behind UTC by however far its decode sync last stepped it ([time.md](../../../../docs/architecture/time.md), **Where it settles**), not a fixed 0.7 s, so its window and the host's differ by a little from slot to slot. On UTC itself the host's `dt` read +0.4 to +0.8 and the P4's −0.7 to 0.0; at 0.7 s they agree.

**The first score was wrong, and why.** Converted without `--normalize-db`, the HL2's channel came out of `iq_convert` with its band noise under one LSB of the 16-bit WAV (an RMS of 0.9 on a 12 m slot), and the host decoded 674 messages on 12 m to the P4's far more, with 40 the P4 alone heard, all −13 to −20 dB. The P4 decodes floats and never sees that. `capture_ab.py` now scales each slot to −24 dBFS first.

## Where the window starts

The same capture cut at each start offset after the UTC boundary. On a clock kept on UTC, a later window leaves less time before the boundary, so the budget at each offset keeps today's margin (about 0.46 s) at the P4's costs (159 ms plus 0.29 ms a unit, [decode.md](../../../../docs/architecture/decode.md), **The work budget**); 0.7 s at 6,000 is today's P4, whose clock and so its deadline are late by as much.

| Start | Budget on a UTC clock | 12 m | 20 m at 6,000 | 20 m at that budget |
| --- | --- | --- | --- | --- |
| + 0 | 6,000 | 826 | 1,930 | 1,930 |
| + 0.25 | 5,150 | 832 | 1,948 | 1,930 |
| + 0.5 | 4,280 | 834 | 1,946 | 1,875 |
| + 0.7 (today) | 6,000 | 827 | 1,952 | — |
| + 0.75 | 3,420 | 840 | 1,937 | 1,858 |

The window's place is worth 1 to 2% here, within about ±1% of noise between offsets (12 m at 0.7 s reads under both 0.5 and 0.75); on a UTC clock the budget it costs is worth more, 3 to 4% on 20 m at 0.5 s and later. On one core, the clock the decode sync moves decodes as well as any window on UTC, and pays for it by transmitting as late as its clock is. The deadline is taken as the boundary, where `TX;` goes; the first tone is 0.5 s later, and how late a message could still be chosen is not measured. One afternoon, one band each.
