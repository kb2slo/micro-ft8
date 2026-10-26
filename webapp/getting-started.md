---
title: Getting started — Micro-FT8
description: Set up a Micro-FT8 station with a QMX and make your first FT8 QSO, from a phone browser.
pages: 1
---

<!--
  The one source of the site's Getting started page and of the printed sheet that goes in the box
  with a main board and sidekick. webapp/tools/getting_started.mjs builds it into
  webapp/getting-started.html (generated, never committed); with --pdf it also prints the sheet,
  webapp/dist/getting-started.pdf, which must fit `pages:` sides of US Letter.

  **Bold** is the station's own on-screen text and `code` is something to type or read exactly; the
  build fails if either is missing from the station's UI sources, so copy labels from the screen.
  *Italics* are labels on someone else's device (the phone, the QMX), and emphasis. An address to
  open is written in full and linked. ![caption](qr:<text>) prints <text> as a QR code. Each `##`
  is a section; `## 3. Pair your phone` gets a step badge and is never split across print columns.

  A boxed unit ships as sidekick/README.md (handing a unit on) leaves it: app installed, no WiFi
  saved, station reset.
-->

# Getting started

![Join the WiFi](qr:WIFI:T:nopass;S:micro-ft8.local;;) ![Open the app](qr:http://192.168.4.1/app.html)

Micro-FT8 is a headless FT8 station for your QMX. The *main board* (M5Stack Unit PoE-P4) decodes, transmits, works the QSOs and keeps the log. The *sidekick* (AtomS3 Lite, the small cube on the short cable) runs its own WiFi network and serves the app to your phone's browser, so there is nothing to install. Set it up, start it calling and put the phone away: the station keeps working. Building your own? Flash both boards first: [Downloads](https://micro-ft8.kalikstein.com/downloads).

## 1. Connect it

You need the main board and sidekick on their Grove cable, your QMX (tested on firmware 1_04_003) with its power and antenna, a USB-C data cable for it, 5 V USB-C power for the main board, and a phone or laptop.

1. Connect the antenna, then power the QMX and switch it on.
2. Plug 5 V USB-C power (a charger or power bank) into the main board's *PROG* port, beside the network jack, which is not used. The sidekick starts with it.
3. Last, connect the *Host* port, on the main board's side, to the QMX's USB; it does not power the board. The radio takes about 11 seconds to connect.

## 2. Open the app

- Join the phone to the WiFi network `micro-ft8.local` (open, no password, unless you set your own in **Settings**), open [`http://192.168.4.1`](http://192.168.4.1/) and tap **Operate in AP mode**, or scan the two codes at the top in order. Bookmark the app.
- Cellular internet comes and goes meanwhile. Android may warn that the network has no internet: stay connected.
- *App not installed?* With cellular data on, open [`http://192.168.4.1`](http://192.168.4.1/), tap **Install the app over your phone's cellular data**, press the sidekick's button, then **Install**. No internet? That page can use your home WiFi instead.

## 3. Pair your phone

- The bar at the bottom of **Operate** says **Not paired**, and the **Settings** tab has a dot. Press the button on top of the sidekick; within a few seconds both go.
- A paired browser can transmit and change settings; anyone on the network can watch. It also sets the station's clock from the phone, which FT8 needs (the main board has no clock), so open the app once after every power-up, with the phone's time set automatically.

## 4. Set up your station

A new or reset station is set to `YOURCALL` in grid `CM97`. Set yours before you transmit.

- Tap **Settings** in the tab bar. Enter your **Call** and tick the **Bands** your QMX covers; they save as you go.
- On **Operate**, tap **Band & radio** at the bottom, then **Station ›**, and enter your **Grid**; it saves when you leave the field. **Set from GPS →** uses the phone's location, and needs internet.

## 5. Check that it hears

- The bar at the bottom of **Operate** says when the QMX is ready. Tap it: **CAT** and **Audio** show ✓ once the QMX is connected (– not known yet, ✕ down).
- Tap a band there. The QMX tunes to its FT8 frequency, the bar at the top fills every 15 seconds, and decodes arrive after each slot.

## Work some stations

- *Answer a CQ:* tap the decode. A row you can answer ends in a blue ›, for about 30 seconds after its slot; a tap on its callsign shows the station instead. The row says **Queued**, and **Undo** takes it back for a few seconds. The station works the exchange and logs it.
- *Call CQ:* tap **Call CQ** on the last row of the queue. It sends as many CQs as **Tries** in the Station sheet (6 to start), every other slot, then stops. Stations that answer are worked first. On the row, tap the count to change it, or the slot to move the CQ to the other one.
- *Stop:* **Stop TX**, on the bar at the bottom, ends the transmission on the air (**Stop CQ** also ends the CQ), and × on a queue row ends that QSO or the CQ. **Tune**, under the bands, sends a carrier until you tap it again.
- Close the app or lock the phone, and the station keeps working. Open it again to see what it did.

## Your log

One ADIF file per UTC day, kept on the main board. On **QSOs**, **Download ADI** saves a day and **Download full ADI** saves them all, for your logger, QRZ or LoTW.

## Good to know

- *CAT or Audio not ✓:* unplug the QMX's USB at the main board and plug it back in. A restart of the main board can leave the QMX unconnected until you do.
- *A busy band but no decodes:* reload the app, so it sets the clock again.
- *Asked to pair again:* a new phone, a cleared browser or another address each take one button press.
- *Internet* is never needed to operate. When the phone has it, a tapped callsign shows QRZ details, and **Tools** adds space weather and, on its map, POTA and SOTA spots.
- *Two devices at most* can join the sidekick's network at once.
- *SWR sweep,* on **Tools**: set the QMX's *Tune* power to 25% or more first.
- *Home WiFi instead* (2.4 GHz only): in **Settings**, tap **Join a network →**, pick it and tap **Save and join**; then use [`http://micro-ft8.local`](http://micro-ft8.local/). Away from that network, the sidekick's own comes back within a minute of power-up.
- *Updates:* **Install / update**, at the bottom of **Settings**, loads the newest app (needs internet).
- *For now:* FT8 with a QMX only; leave **Protocol** at FT8. A CQ stops after its tries and each QSO message after its own, but you are still the control operator.

## Questions and feedback

KB2SLO · [micro-ft8.kalikstein.com/#contact](https://micro-ft8.kalikstein.com/#contact). If something goes wrong, note the time, what you tapped, and the **Firmware** and **App** lines in **Settings**.
