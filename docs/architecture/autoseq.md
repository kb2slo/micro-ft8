# Autoseq Architecture: Reference Project Design

This document describes the single-threaded state machine architecture used in the
DX-FT8 reference project and how Micro-FT8 implements the same pattern.

## Core Principle: Three Events, Strict Ordering

The reference project is a **single-threaded state machine** with exactly 3 events:

```
Event 1: 79 symbols received → start decoding
Event 2: decoding finished → autoseq parses decodes, populates TX message
Event 3: 15s slot boundary → check parity, TX if message ready, tick if was_txing
```

**Critical: Event 2 ALWAYS happens before Event 3.** This ordering eliminates race conditions.

## Event Flow Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         15-SECOND SLOT TIMELINE                          │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  SLOT N (RX)              │  SLOT N+1 (TX)           │  SLOT N+2 (RX)   │
│  ─────────────────────────┼──────────────────────────┼─────────────────  │
│                           │                          │                   │
│  [Receive 79 symbols]     │  [Transmit if ready]     │  [Receive again]  │
│           │               │         │                │                   │
│           ▼               │         │                │                   │
│  Event 1: Decode start    │         │                │                   │
│           │               │         │                │                   │
│           ▼               │         │                │                   │
│  Event 2: Decode done     │         │                │                   │
│    - autoseq_on_decodes() │         │                │                   │
│    - Check TX ready       │         │                │                   │
│    - Set QSO_xmit flag    │         │                │                   │
│           │               │         │                │                   │
│           └───────────────┼────►Event 3: Slot boundary                   │
│                           │    - if was_txing: tick()│                   │
│                           │    - if QSO_xmit &&      │                   │
│                           │      correct_parity:     │                   │
│                           │      start TX            │                   │
│                           │         │                │                   │
│                           │         ▼                │                   │
│                           │    [TX runs ~12.8s]      │                   │
│                           │         │                │                   │
│                           │         └────────────────┼──► tick() at      │
│                           │                          │    next boundary  │
└─────────────────────────────────────────────────────────────────────────┘
```

## Reference Project Code Pattern (main.c)

```c
// EVENT 3: Slot boundary detection and TX trigger
int current_slot = ft8_time / 15000 % 2;
if (current_slot != slot_state) {
    slot_state ^= 1;

    // If we were transmitting, call tick to pop completed TX
    if (was_txing) {
        autoseq_tick();  // Pops the entry that was just transmitted
    }
    was_txing = 0;
}

// TX trigger: check if we should transmit in this slot
if (QSO_xmit && target_slot == slot_state && FT_8_counter < 29) {
    setup_to_transmit_on_next_DSP_Flag();
    QSO_xmit = 0;
    was_txing = 1;
}

// EVENT 2: After decode finishes (decode_flag set by decoder)
if (decode_flag) {
    // Only process decodes when NOT transmitting
    if (!was_txing) {
        autoseq_on_decodes(new_decoded, master_decoded);

        // Check if TX is ready from autoseq
        if (autoseq_get_next_tx(autoseq_txbuf)) {
            queue_custom_text(autoseq_txbuf);
            QSO_xmit = 1;
        }
        // ELSE IF beacon on, add CQ
        else if (Beacon_On) {
            autoseq_start_cq();
            autoseq_get_next_tx(autoseq_txbuf);
            queue_custom_text(autoseq_txbuf);
            QSO_xmit = 1;
            target_slot = slot_state ^ 1;
        }
    }
    decode_flag = 0;
}
```

## CQ/Beacon Design: Short-Lived Entries

**Key insight: CQ entries are SHORT-LIVED.** They exist in the queue for exactly ONE transmission.

```
1. Beacon is ON, no active QSO
2. Decode callback: autoseq_get_next_tx() returns false (queue empty)
3. Since Beacon_On: autoseq_start_cq() adds CQ entry
   - state = CALLING
   - next_tx = TX6 (CQ message)
   - dxcall = "CQ"
4. autoseq_get_next_tx() now returns true (CQ ready)
5. Set QSO_xmit = 1, target_slot = opposite parity
6. At slot boundary: TX starts, was_txing = 1
7. TX completes (12.8 seconds)
8. Next slot boundary: autoseq_tick() is called
9. tick() sees state=CALLING, transitions to IDLE, pops entry
10. Queue is now empty
11. Next decode: back to step 2, new CQ is added
```

**Why short-lived?**
- Makes beacon flag changes take effect immediately
- If beacon is turned OFF, the CQ in queue transmits once, then tick() removes it
- No stale CQ entries accumulating

## Queue Sorting Rules

The queue is sorted with this priority (descending):
```
IDLE (6)      → highest priority (gets popped first)
SIGNOFF (5)
ROGERS (4)
ROGER_REPORT (3)
REPORT (2)
REPLYING (1)
CALLING (0)   → lowest priority (CQ stays at bottom)
```

**Rationale:**
- IDLE entries are popped immediately by sort_and_clean()
- More advanced QSOs (closer to completion) get priority
- CQ (CALLING) stays at bottom, only transmitted when no active QSO

## autoseq_tick() Behavior

Called ONCE per TX slot, at slot boundary AFTER TX completes:

```c
void autoseq_tick(void) {
    if (queue_size == 0) return;

    ctx_t *ctx = &ctx_queue[0];  // Always operates on FRONT entry

    switch (ctx->state) {
        case AS_REPLYING:
        case AS_REPORT:
        case AS_ROGER_REPORT:
        case AS_ROGERS:
            // Increment retry counter or transition to IDLE if max retries
            if (ctx->retry_counter < ctx->retry_limit) {
                ctx->next_tx = TX_for_state;
                ctx->retry_counter++;
            } else {
                ctx->state = AS_IDLE;
            }
            break;

        case AS_CALLING:   // CQ - only once (controlled by beacon)
        case AS_SIGNOFF:   // 73 - only once
            ctx->state = AS_IDLE;
            ctx->next_tx = TX_UNDEF;
            break;
    }

    if (ctx->state == AS_IDLE) {
        pop();  // Remove from queue
    }
}
```

**Key points:**
- tick() always operates on queue[0] (the front)
- CALLING (CQ) immediately transitions to IDLE and gets popped
- This is why CQ is short-lived: one TX, one tick, gone

## Why This Design Prevents Race Conditions

**Problem with immediate TX scheduling:**
```
1. Decode finishes, TX is scheduled with delay
2. During delay, ANOTHER decode happens
3. New decode adds entries, queue is re-sorted
4. Original TX entry is no longer at queue[0]
5. tick() operates on wrong entry
6. Bug: CQ never gets popped, or wrong entry gets popped
```

**Reference design solution:**
```
1. Decode finishes, just SET FLAGS (QSO_xmit, target_slot)
2. Another decode happens - flags might be updated, that's fine
3. At slot boundary: check flags, start TX if conditions met
4. TX runs for entire slot
5. Next slot boundary: tick() operates on queue[0]
6. Since Event 2 always before Event 3, queue is stable when tick() runs
```

## Micro-FT8 Implementation

Micro-FT8 now follows the same pattern:

```cpp
// State machine variables
static volatile bool g_qso_xmit = false;        // TX is pending
static volatile int g_target_slot_parity = 0;   // Parity for TX
static volatile bool g_was_txing = false;       // For tick timing
static int g_last_slot_parity = -1;             // Slot boundary detection (parity only)

// Called from main loop every tick
static void check_slot_boundary() {
    int64_t slot_idx = now_ms / 15000;
    int slot_parity = slot_idx & 1;

    // Detect slot boundary (parity change)
    if (slot_parity != g_last_slot_parity) {
        g_last_slot_parity = slot_parity;

        // If we were transmitting, call tick
        if (g_was_txing) {
            autoseq_tick(slot_idx, slot_parity, 0);
            g_was_txing = false;
        }
    }

    // TX trigger
    if (g_qso_xmit && g_target_slot_parity == slot_parity &&
        slot_ms < 4000 && s_tx_task_handle == NULL) {
        g_qso_xmit = false;
        tx_start_immediate(skip_tones);
    }
}

// In decode_monitor_results (Event 2)
if (!g_was_txing) {
    if (!to_me.empty()) {
        autoseq_on_decodes(to_me);
    }

    AutoseqTxEntry pending;
    if (autoseq_fetch_pending_tx(pending)) {
        g_qso_xmit = true;
        g_target_slot_parity = pending.slot_id & 1;
    } else if (g_beacon != BeaconMode::OFF) {
        enqueue_beacon_cq();
        if (autoseq_fetch_pending_tx(pending)) {
            g_qso_xmit = true;
            g_target_slot_parity = pending.slot_id & 1;
        }
    }
}
```

## Audio Sample-Driven Timing (Critical Design Detail)

The reference project uses **audio sample timing** to drive all RX/TX symbol processing,
NOT wall clock timing. This is critical for proper synchronization.

### Reference Design: I2S-Driven Event Loop

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    AUDIO SAMPLE TIMING ARCHITECTURE                      │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  I2S DMA Interrupt (32kHz sample rate)                                   │
│  ├── Fires every 40ms (1280 samples per half-buffer)                    │
│  ├── Sets DSP_Flag in ISR                                               │
│  └── Main loop checks DSP_Flag                                          │
│                                                                          │
│  Symbol Timing (4 × 40ms = 160ms per symbol)                            │
│  ├── frame_counter increments on each DSP_Flag                          │
│  ├── When frame_counter == 2, send next TX tone                         │
│  └── 79 tones × 160ms = 12,640ms TX duration                           │
│                                                                          │
│  RX Symbol Counting                                                      │
│  ├── FT_8_counter increments per 160ms of audio                         │
│  ├── At 91 symbols (14,560ms), decode is triggered                      │
│  └── decode_flag is set for main loop                                   │
│                                                                          │
│  Slot Boundary (HAL_GetTick based, 15s intervals)                       │
│  ├── ft8_time = HAL_GetTick() - start_time                              │
│  ├── current_slot = ft8_time / 15000 % 2                                │
│  └── When slot changes: reset counters, call tick if was_txing         │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### Key Insight: TX Duration is Guaranteed by Sample Counting

In the reference design:
1. **TX starts** when `setup_to_transmit_on_next_DSP_Flag()` is called
2. **Each DSP_Flag** (40ms) advances `frame_counter`
3. **Every 4 DSP_Flags** (160ms), a tone is sent
4. **After 79 tones**, TX automatically stops: `ft8_xmit_counter == 79`
5. **Slot boundary detection** happens AFTER TX completes because:
   - 79 tones × 160ms = 12,640ms
   - Slot boundary at 15,000ms
   - TX completes ~2,360ms before slot boundary

### Reference Code: TX Tone Timing

```c
if (DSP_Flag) {
    DSP_Flag = 0;
    I2S2_RX_ProcessBuffer(buff_offset);

    if (xmit_flag) {
        if (ft8_xmit_delay >= 28) {  // ~1.1s startup delay
            if ((ft8_xmit_counter < 79) && (frame_counter == 2)) {
                set_FT8_Tone(tones[ft8_xmit_counter]);
                ft8_xmit_counter++;
            }
            if (ft8_xmit_counter == 79) {
                ft8_receive_sequence();
                receive_sequence();
                xmit_flag = 0;
            }
        } else {
            ++ft8_xmit_delay;
        }
    }
}
```

### Why This Matters for Micro-FT8

**Problem with wall clock timing:**
- TX starts at slot boundary (t=0ms)
- `tx_tick()` uses `rtc_now_ms()` to check if 160ms elapsed
- Slot boundary detected at t=15000ms via wall clock
- BUT: Wall clock and audio timing can drift apart
- If TX hasn't sent all 79 tones when slot boundary hits, `tick()` is called too early

**Solution: Sample-Driven Timing**
- Count UAC audio samples instead of wall clock time
- 48kHz × 0.160s = 7,680 samples per symbol
- 79 symbols × 7,680 = 606,720 samples for full TX
- TX completion is guaranteed before slot boundary detection

### Micro-FT8 Implementation Options

**Option 1: Sample-Counted TX**
```cpp
// In UAC audio callback (fires every block of samples)
static int g_tx_sample_count = 0;
static int g_tx_current_tone = 0;

void on_audio_block(int samples_received) {
    if (g_tx_active) {
        g_tx_sample_count += samples_received;
        if (g_tx_sample_count >= 7680) {  // 160ms at 48kHz
            g_tx_sample_count -= 7680;
            send_next_tone();
            g_tx_current_tone++;
            if (g_tx_current_tone >= 79) {
                tx_complete();
            }
        }
    }
}
```

**Option 2: Block-Counted TX**
```cpp
// Block size = 1920 samples at 48kHz = 40ms per block
// 4 blocks = 160ms = 1 symbol
static int g_tx_block_count = 0;

void on_audio_block() {
    if (g_tx_active) {
        g_tx_block_count++;
        if (g_tx_block_count >= 4) {  // 4 × 40ms = 160ms
            g_tx_block_count = 0;
            send_next_tone();
        }
    }
}
```

## Inactive queue: keeping QSO metadata across retry exhaustion

### Problem Statement

The autoseq engine stores all per-QSO metadata (dxcall, dxgrid, snr_tx, snr_rx,
logged, is_fd) in queue entries (`QsoContext`). When a context exhausts its retries,
it transitions to IDLE and is popped from the queue, destroying all metadata.

If DX is more patient than us and continues retrying, their message creates a
"reincarnated" context with default values (snr_tx=-99, logged=false). This
reincarnated context can complete the QSO and log an ADIF entry with wrong metadata.

#### The Concrete Bug: REPORT + TX3 Reincarnation

```
T=0:00   We send TX2 (rst_sent=-12), retry 1
T=0:30   DX sends TX3 (R-08) — we don't decode (QRM)
T=1:00   We send TX2, retry 2
T=1:30   DX sends TX3 — we don't decode
  ...
T=2:30   Retries exhausted → IDLE → pop. snr_tx=-12 is gone forever.
T=3:00   DX sends TX3 — propagation improves, we decode!
         → on_decode: no matching ctx → new ctx (snr_tx=-99)
         → REPORT+TX3 → ROGERS → log_qso_if_needed
         → ADIF entry has rst_sent=-99 ← WRONG, no prior correct entry exists
```

#### Root Cause

Our retry window (~2.5 min) can be shorter than DX's retry window (WSJT-X: up to
7+ min with 15 retries). After our context is popped, DX is still actively working
the QSO. The queue's lifetime is bounded by OUR retry limit, but the QSO's real
lifetime is bounded by DX's.

#### Safety Invariant Analysis

Every pop from the queue falls into one of these cases:

| State at exhaustion | Logged? | DX received our TX | DX did NOT receive our TX |
|---|---|---|---|
| CALLING (CQ) | No | DX sends TX1 → normal new QSO | Nothing happens |
| REPLYING (TX1) | No | DX sends TX2 → new ctx, fresh exchange, consistent | DX resends CQ → not addressed to us |
| REPORT (TX2) | No | DX sends TX3 → new ctx, **snr_tx=-99 (BUG)** | DX resends TX2/TX1 → new ctx, snr_tx from msg.snr, OK |
| ROGER_REPORT (TX3) | No | DX sends TX4 → signoff guard filters it | DX resends TX2 → new ctx, fresh exchange, OK |
| ROGERS (TX4) | **Yes** | DX sends TX4/TX5 → signoff guard filters it | DX resends TX3 → **duplicate ADIF, snr_tx=-99** (original correct entry exists) |
| SIGNOFF (TX5) | **Yes** | QSO done | DX sends TX4/TX5 → signoff guard filters it |

The **REPORT + DX received** row is the critical gap: no prior correct ADIF entry
exists, and the reincarnated context has lost snr_tx.

### Solution: Active + Inactive Queue

Instead of popping contexts when retries exhaust, move them to an **inactive zone**
at the back of the queue. The single `s_queue[]` array serves two logical partitions:

```
s_queue[0 .. s_active_count-1]   → Active zone: has retries, gets scheduled by tick()
s_queue[s_active_count .. s_queue_size-1] → Inactive zone: retries exhausted, metadata preserved
```

#### Key Design Rules

1. **Retry exhaustion → inactive, not IDLE.** When tick() exhausts retries, the
   context is marked `inactive = true` and moved to the back via sort. It is NOT
   popped. All metadata (snr_tx, snr_rx, dxgrid, logged, is_fd) is preserved.

2. **on_decode finds inactive contexts.** The existing dxcall-matching loop in
   on_decode() already scans the full queue. When DX retries, the dormant context
   is found, generate_response() advances its state, `inactive` is cleared, and
   it moves back to the active zone with fresh retries.

3. **Eviction only when safe.** A context can be evicted (popped) only if:
   - `logged == true` — ADIF has the correct entry, safe to discard
   - State is CALLING or REPLYING — no reports exchanged yet. CALLING is a bare
     CQ. REPLYING means we only sent TX1 (grid); if DX retries with TX2, a fresh
     context gets correct snr_tx from msg.snr, so nothing valuable is lost.
   - Contexts in REPORT or beyond (sent TX2+) must be preserved — they carry the
     snr_tx we actually transmitted, which would be lost on eviction.

4. **Reject only R+report (TX3) from unknown DX.** If DX sends TX3 (R+report)
   and has no context in either active or inactive zone, reject it — TX3 skips
   the snr_tx latch, producing wrong ADIF metadata. All other message types are
   allowed for new context creation:
   - TX1 (grid): new QSO start, correct metadata
   - TX2 (report): DX skipped TX1 (WSJT-X "skip TX1" feature), snr_tx set from
     msg.snr, snr_rx from report text — correct metadata
   - FD exchange: equivalent to TX2 in Field Day mode — correct metadata
   - TX4/TX5 (RR73/73): filtered separately by signoff guard

5. **Time-based expiry.** Inactive contexts expire after a configurable window.
   Expired inactive contexts are evicted regardless of logged state — if we haven't
   heard from DX in that long, the QSO is truly dead.

#### Queue Layout: Dynamic Boundary

The active and inactive zones share a single array and grow from opposite ends.
No fixed partition — both zones expand until their pointers meet.

```
s_queue[]:
 index 0                                               index AUTOSEQ_MAX_QUEUE-1
 ┌──────────────────────┬────────────┬──────────────────────┐
 │   Active zone        │  (free)    │   Inactive zone      │
 │   grows →            │            │            ← grows   │
 └──────────────────────┴────────────┴──────────────────────┘
       s_active_count ──┘            └── s_inactive_start
```

- `s_active_count`: number of active entries at the front (grows right)
- `s_inactive_start`: index of first inactive entry at the back (grows left)
- Free space: `s_inactive_start - s_active_count`
- When both pointers meet (free space = 0), queue is full:
  evict the oldest inactive entry (rightmost) to make room.

Queue size: **120 entries.** This accommodates a busy 1-hour activation.
QsoContext is ~80 bytes with std::string SSO, so 120 entries ≈ 10 KB —
negligible on ESP32-S3 (512 KB SRAM).

#### Sort Order

Active entries are sorted by state priority (same as before):
```
  IDLE > SIGNOFF > ROGERS > ROGER_REPORT > REPORT > REPLYING > CALLING
```

Inactive entries are stored at the back in insertion order (oldest at the
rightmost end). When eviction is needed, the oldest (rightmost) entry is
removed first.

#### Modified tick() Behavior

```
Before:  retry exhausted → state = IDLE → pop_front()
After:   retry exhausted + state > REPLYING → move to inactive zone at back
         retry exhausted + state <= REPLYING → evict (no reports exchanged)
```

tick() only processes the front of the active zone.

#### Modified on_decode() Behavior

```
Before:  no matching ctx + not signoff → append_ctx + generate_response(override=true)
After:   no matching ctx → check message type:
           TX4/TX5 (RR73/73): reject (signoff guard, as before)
           TX3 (R+report): reject (snr_tx would be -99, reincarnation bug)
           TX1/TX2/FD exchange: allow (fresh context gets correct metadata)
```

#### Reincarnation Eliminated

The reincarnation problem disappears because:
- Dormant contexts stay in the queue with all metadata
- on_decode finds them by dxcall, reactivates them
- Mid-QSO messages from unknown DX are rejected (no memory-less reincarnation)
- The only new contexts created are for TX1 (fresh QSO starts) where default
  metadata is correct

### Invariant: Active entries always have a valid next_tx

The autoseq engine maintains this invariant for every active queue entry:

> **If `ctx` is in the active zone and `state != IDLE`, then `next_tx != TX_UNDEF`.**

This invariant is what lets `autoseq_fetch_pending_tx()` safely return the TX
at `s_queue[0]` without checking for degenerate states. Violating it causes a
hard deadlock: `fetch_pending_tx` returns false, no TX fires, `tick()` never
runs to advance the state, and the dead entry blocks every subsequent TX —
including beacon CQs sorted behind it.

#### How the invariant was maintained (pre-inactive-queue)

1. `set_state()` is the only legitimate way to change `state`, and it always
   writes `next_tx` at the same time.
2. The override path in `generate_response()` sets `next_tx = TX_UNDEF` as a
   "clean slate" but is always immediately followed by a state machine
   transition that restores a valid `next_tx`.
3. `autoseq_tick()` refreshes `next_tx` idempotently based on `state` on every
   call (within retry limit).
4. For an existing active ctx passed to `generate_response(override=false)`,
   the invariant holds by induction: if the state machine transitions, the new
   `set_state` maintains it; if it falls to `default`, `next_tx` was already
   valid from the prior tick or transition.

#### How the inactive queue almost broke it

`move_to_inactive()` clobbers `next_tx = TX_UNDEF` before parking an entry.
When DX retries and `on_decode()` reactivates the dormant context, the
reactivated entry lands in the active zone with `next_tx == TX_UNDEF`.
`generate_response(override=false)` then runs the state machine; if the
(state, rcvd) combination falls to `default: return false`, `next_tx` is
*never* restored. The active zone now contains an entry violating the
invariant, and the queue deadlocks.

#### Fix: make `generate_response` locally total

Every state handler in `generate_response()` must explicitly handle every
possible `rcvd` type — there is no implicit `default` that silently leaves
`next_tx` stale. For `(state, rcvd)` combinations that don't advance the QSO,
the handler refreshes `next_tx` to the state's canonical value and returns
false. The canonical mapping:

| State | Canonical next_tx | Meaning |
|---|---|---|
| CALLING | TX6 | Send CQ |
| REPLYING | TX1 | Send our grid |
| REPORT | TX2 | Send our report |
| ROGER_REPORT | TX3 | Send R+report |
| ROGERS | TX4 | Send RR73 |
| SIGNOFF | TX5 | Send 73 |

The semantics of every "no-op" case is identical: **"DX sent something that
doesn't advance our state — they probably didn't decode our last message.
Keep sending what matches our current state."** Encoding this in the
switch/case (rather than a centralized fallback) makes every state handler
self-contained and locally reasonable — when you read the REPORT case, you
see exactly what happens for every possible input, including the ones that
don't advance the QSO.

The invariant is restored even across the inactive queue boundary: if
`reactivate()` brings back an entry with stale `next_tx`, the very next call
to `generate_response()` repairs it unconditionally.

**Note on CALLING (CQ):** The CALLING state handler does NOT refresh `next_tx`
in its default case, because CALLING is unreachable via reactivation by
construction (CQ ctxs have dxcall="CQ", never enter the inactive zone, and
the override path always transitions immediately). See inline comment in
`generate_response()` for the full proof.

### Formal design notes

#### Orthogonality of logical position and scheduling state

A QsoContext has two independent coordinates:

- **Logical position** (`state`): where we are in the FT8 protocol exchange.
  Maps to the conversational order TX6 → TX1 → TX2 → TX3 → TX4 → TX5.
  Active ctxs always have a well-defined position in {CALLING..SIGNOFF}.

- **Scheduling state**: whether the ctx is actively participating in TX
  scheduling. Three modes: active (in active zone, `tick()` processes it,
  `fetch_pending_tx` can select it), inactive (in inactive zone, dormant,
  preserving metadata for reactivation), or eliminated (popped from queue).

These two dimensions are **orthogonal**. A ctx at logical position REPORT can
be active (retrying TX2) or inactive (parked after retry exhaustion). Its
logical position doesn't change when it moves between active and inactive —
only its scheduling state does. When reactivated, it resumes at the same
logical position with the same metadata.

#### Semantics of TX_UNDEF

`TX_UNDEF` in the `next_tx` field means **"this context has no TX to schedule
and should be eliminated from the active zone on the next cleanup."** It is a
scheduling-layer tombstone, not a logical position.

Specifically: `(next_tx == TX_UNDEF) ⇔ (state == IDLE, about to be popped)`.

TX_UNDEF is NOT a protocol position. It should never appear on an active ctx
with state != IDLE. The idempotent `generate_response()` state machine
enforces this by refreshing `next_tx` to the canonical value for the current
state whenever the state machine runs.

**Pragmatic exception:** `autoseq_start_cq()` sets `next_tx = TX6` on CALLING
ctxs so that CQ text can be generated through the same `format_tx_text()`
path as QSO messages. This reuses the TX-generation pipeline uniformly for
all message types, including the broadcast initiator (CQ). CALLING ctxs are
one-shot (tick immediately transitions to IDLE → pop) and never retried or
moved to inactive, so this does not violate the scheduling semantics of
TX_UNDEF.

## Summary: The Golden Rules

1. **Decode sets flags, slot boundary triggers TX** - Never start TX immediately after decode
2. **tick() at slot boundary AFTER TX** - Not during TX, not after decode
3. **CQ is short-lived** - Added when needed, transmitted once, popped by tick
4. **Queue[0] is always the active entry** - tick() and fetch_pending_tx() both use queue[0]
5. **Event 2 before Event 3** - Decode processing completes before TX decision is made
6. **Audio samples drive symbol timing** - RX/TX duration determined by sample count, not wall clock
7. **Slot boundary uses wall clock** - 15s intervals based on system tick/RTC
