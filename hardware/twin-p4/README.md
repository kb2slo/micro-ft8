# Twin-P4 board

Two Unit PoE-P4 main boards and the AtomS3 Lite sidekick on one proto board, run from the QMX's own supply.
It is the board for joining two P4s, a direction the several-channels epic keeps
([several-channels.md](../../docs/roadmap/several-channels.md), **Several P4s joined**).

**Status: being built, not yet powered.** Nothing here is verified on the bench, and no firmware drives the
link between the P4s yet.

The netlist below is the source. `power.svg` and `links.svg` are drawn from it by `draw_schematic.py`
(`python3 hardware/twin-p4/draw_schematic.py`), and a change edits the netlist, the script and the drawings in the
same commit.

## Mechanics

Each module's headers are female sockets. Male pin strips are soldered to the proto board, short end down, and
each module pushes onto them, so it comes off to be flashed or reworked. P4-B sits rotated 180° beside P4-A,
which puts its G8–G13 row on the board's far edge. Pins soldered are the two single-row headers on top of each
P4 (SDIO row: G13–G8, G27, G26, 5V; ISP row: GND, G35, RST, G38, G37, 3V3) and the AtomS3's two (GND, 5V, G38,
G39; G8–G5, 3V3).

To solder: push the pins into the module, set it on the board, tack one pin at each end of each row, lift the
module off and finish the joints without it, so no heat reaches its sockets. Key each position (a mark, or one
pin left out with its socket blanked): a P4 seated the wrong way round puts 5V and 3V3 on the wrong pins.

## Power

![Power input schematic](power.svg)

One 5 V supply feeds all three boards, and nothing else is on that rail. The P4's PROG port brings its 5 V in
through a diode (the board's printed label), so a USB cable on PROG cannot fight the rail. Whether the AtomS3's
USB-C has the same diode is not known: do not plug it into a computer while the board's 5 V is on until it is.

| Ref | Fit | Part | Why |
|---|---|---|---|
| J1 | Always | DC barrel jack, the QMX's plug (5.5 × 2.1 mm, centre positive, to confirm) | Supply in, 9–14 V |
| J2 | Always | The same jack | Pass-through to the QMX, tapped ahead of F1 so its transmit current, over 1 A, never passes the fuse |
| F1 | Always | Resettable fuse, 1 A hold, 16 V or more (Bourns MF-R110) | A short on the 5 V branch, the commonest proto-board fault |
| D1 | Always | SS34 Schottky, 3 A 40 V | A supply plugged in reversed; about 0.4 V drop |
| U1 | Always | Traco TSR 3-2450: 5 V, 3 A, 6.5–36 V in, SIP-3 at 0.1" | The regulator. Pin 1 Vin, 2 GND, 3 Vout, to confirm against the datasheet |
| C3 | Always | 100 µF electrolytic, 10 V or more | Bulk for the fast current steps of three boards, the sidekick's WiFi bursts among them |
| L1 | If noisy | 10 µH power inductor, 3 A, or a 3 A ferrite bead | With C1 and C2, keeps U1's switching off the 12 V line the QMX shares; left out, a wire bridges its pads |
| C1 | If noisy | 100 µF electrolytic, 25 V | Input filter, with L1 |
| C2 | If noisy | 10 µF ceramic X7R, 25 V | Input filter, with L1; mounted at U1's pins |
| D2 | Field | SMBJ16A TVS, unidirectional | Spikes over 16 V from a car battery or a generator; after F1, it also blows F1 on a reversed supply |
| C4 | Spare | 10 µF ceramic | Beside C3; each module has its own |
| R1–R3 | If ringing | 33 Ω, 1/8 W | Series damping on the link's SCLK, MOSI and MISO, at the driving end (**Links**) |

Wire J1 to J2 in 22 AWG or heavier, not along proto-board pads. L1, C1 and C2 are fitted when the board, against
a USB power bank, raises the QMX's noise floor or costs decodes on a quiet band; leave room for them.

## Links

![Board links schematic](links.svg)

**P4 to P4** is six wires run straight, pin to pin, G8 to G13 on each board. Which end drives each wire is
the firmware's choice, so the same wires carry SPI or a UART with flow control, crossed in software. Drawn as
SPI with P4-A the master: G8 SCLK, G9 MOSI, G10 MISO, G11 CS, G12 READY (P4-B asking for a transfer, since an
SPI slave cannot start one), G13 spare. SPI is for raw I/Q crossing the boards: one 48 kHz receiver at 24 bits
is about 290 KB/s, over half a 5 Mbaud UART, where SPI at 20 MHz is about 2.5 MB/s. Run the six wires short
and together, with a GND wire beside them tied to each board's GND. R1 to R3, 33 Ω at the driving end of
SCLK, MOSI (P4-A) and MISO (P4-B), damp ringing; fit them if a link test fails at speed. G26/G27, the P4's
full-speed USB pins, and the 2x8 Hat2-Bus socket are left free.

**The sidekick** stays on P4-A's Grove port (Port A, [sidekick.md](../../docs/architecture/sidekick.md)), as on a
station with one P4. The Grove cable carries P4-A's 5V and GND, the same nets as the header pins.

Not connected on any board: 3V3; on the P4s, G35 (BOOT, a strapping pin and Ethernet's TXD1), RST (a P4 reset
wedges the QMX), G37 and G38 (UART0, which the ROM bootloader writes at every reset).

## Netlist

R1 to R3, where fitted, sit in series at the driving end, the first pin of their net; left out, the net is one wire.

| Net | Pins |
|---|---|
| VIN | J1 +, J2 +, F1 in |
| VIN_F | F1 out, D1 anode, D2 cathode |
| VIN_D | D1 cathode, L1 in |
| VIN_U | L1 out, C1 +, C2, U1 pin 1 |
| +5V | U1 pin 3, C3 +, C4, P4-A 5V (SDIO row), P4-B 5V (SDIO row), AtomS3 5V; star from C3 |
| GND | J1 −, J2 −, D2 anode, C1 −, C2, U1 pin 2, C3 −, C4, P4-A GND (ISP row), P4-B GND (ISP row), AtomS3 GND |
| LINK_SCLK | P4-A G8, R1, P4-B G8 |
| LINK_MOSI | P4-A G9, R2, P4-B G9 |
| LINK_MISO | P4-B G10, R3, P4-A G10 |
| LINK_CS | P4-A G11, P4-B G11 |
| LINK_READY | P4-B G12, P4-A G12 |
| LINK_SPARE | P4-A G13, P4-B G13 |
| PORTA_HOST_TX | P4-A G53 (Grove), AtomS3 G2 (Grove), over the Grove cable |
| PORTA_SK_TX | AtomS3 G1 (Grove), P4-A G54 (Grove), over the Grove cable |

## Before the first power-up

1. With no module seated, check continuity between each pad and its neighbours, and from VIN and +5V to GND:
   none shorts.
2. From a bench supply at 12 V, current limited to 200 mA: U1's output reads 4.9 to 5.1 V, and so does each
   module's 5V pad.
3. Seat one module at a time, watching the current.
4. On a quiet band, compare the noise floor and decode count on this supply against a USB power bank; fit L1, C1
   and C2 if this one costs decodes.
