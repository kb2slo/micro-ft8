---
title: Getting started — micro-FT8
description: Set up a micro-FT8 station with a QMX and make your first FT8 QSO, from a phone browser.
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

![Open the app](qr:http://micro-ft8.local/app.html)

micro-FT8 is a headless FT8 station for your QMX. The *main board* (M5Stack Unit PoE-P4) decodes, transmits, sequences each QSO and keeps the log. The *sidekick* (AtomS3 Lite, the small cube on the short cable) runs its own WiFi network and serves the app to your phone's browser, so there is nothing to install. The queue and the sequencer are on the station, so a locked phone does not drop a QSO. Building your own? Flash both boards first: [Downloads](https://micro-ft8.kalikstein.com/downloads).

## 1. Connect it

You need the main board and sidekick on their Grove cable, your QMX (validated on firmware 1.04.010) with its power and antenna, a USB-C data cable for it, 5 V USB-C power for the main board, and a phone or laptop.

1. Connect the antenna, then power the QMX and switch it on.
2. Plug 5 V USB-C power (a charger, a power bank or your phone's USB-C port) into the main board's *PROG* port, beside the network jack, which is not used. The sidekick starts with it.
3. Last, connect the *Host* port, on the main board's side, to the QMX's USB; it does not power the board. The radio takes about 11 seconds to connect.

## 2. Open the app

- Join the phone to the WiFi network `micro-ft8.local` and four characters (open, unless you set a password in **Settings**). Android may warn that it has no internet: stay connected.
- In the phone's browser, type [http://micro-ft8.local](http://micro-ft8.local/) in full and tap **Operate in AP mode**, or scan the code at the top.
- *It must start with `http://`, not `https://`,* which the station does not have. A browser may change it by itself (Safari may): if the page does not open, type the address again, `http://` included. If the name is not found, as on some Android phones, open [`http://192.168.4.1`](http://192.168.4.1/) instead.
- *Add the app to your Home Screen now, before you pair:* it then opens full screen with one tap, with no address to type. iPhone: in Safari, tap *Share*, then *Add to Home Screen*. Android: in Chrome, tap the three-dot menu, then *Add to Home screen*. From here on, open the app from that icon.

## 3. Pair your phone

- Open the app from its Home Screen icon. **Operate**'s bottom bar says **Not paired** until you press the pairing button, on top of the sidekick.
- A paired browser can transmit and change settings; anyone on the network can watch. It also sets the station's clock from the phone, which FT8 needs (the main board has no clock), so open the app once after every power-up, with the phone's time set automatically.

## 4. Set up your station

A new or reset station is set to `YOURCALL` in grid `CM97`, and will not transmit, not even Tune, until you set your callsign.

- Tap **Settings**, then **Station**. Enter your **Call** and your **Grid**; each saves when you leave the field. **Set from GPS →** uses the phone's location, and needs internet.
- Back in **Settings**, tap **Radio & bands** and turn on the **Bands** you want: once the QMX is connected, only its own bands are offered.

## 5. Check that it hears

- Tap **Operate**'s bottom bar: **CAT** and **Audio** show ✓ once the QMX is connected (– not known yet, ✕ down).
- Tap a band there. The QMX tunes to its FT8 frequency, the top bar fills every 15 seconds, and decodes arrive after each slot.
- *CAT or Audio not ✓:* switch the QMX off and on. *A busy band but no decodes:* reload the app, so it sets the clock again.

## Work some stations

- *Answer a CQ:* tap a decode that ends in a blue ›, for about 30 seconds after its slot (**Undo** takes it back). The station works the exchange and logs it.
- *Call CQ:* tap **Call CQ** on the last row of the queue. It sends as many CQs as **Tries** in **Settings**, **Station** (6 to start), every other slot, then stops. Stations that answer are worked first. On the row, tap the count to change it, or the slot to move the CQ to the other one.
- *Stop:* **Stop TX** ends the transmission on the air (**Stop CQ** also ends the CQ), and × on a queue row ends that QSO or the CQ. **Tune** sends a carrier until you tap it again.
- Close the app or lock the phone, and a QSO in progress still finishes and logs. Open it again and carry on.

## Your log

Your log is kept on the main board. On **QSOs**, newest first, each day's **Download ADI** saves that day and **Download full ADI** saves them all, for your logger or LoTW. With a **QRZ logbook key** in Settings, **Send** uploads what QRZ lacks, and **Import from QRZ** brings in the FT8 and FT4 QSOs you logged elsewhere.

## Home WiFi and updates

- *Home WiFi* (2.4 GHz only): in **Settings**, tap **Network**, then **Join a network →**, pick it and tap **Save and join**. With the phone on that network too, the same address and Home Screen icon open the app, still paired. Away from that network, the sidekick's own comes back within a minute of power-up.
- *Updates:* in **Settings**, **Software update**, then **Update** updates everything (needs internet; it switches the radio off at the end, so switch it back on when it is done). Flashing over USB, from the downloads page, erases the board: the sidekick starts again as a new unit.

## Questions and feedback

KB2SLO · [micro-ft8.kalikstein.com/#contact](https://micro-ft8.kalikstein.com/#contact). If something goes wrong, note the time and what you tapped, then in **Settings**, **Help & about**, tap **Email**, which fills in your versions, or report it on GitHub: tap **Versions** there to copy them.
