# SX1280 fast modems: FLRC and GFSK presets

This branch adds FLRC and GFSK as modulations independent of LoRa on SX128x radios (2.4 GHz). On a pair of
ProMicro nRF52840 + Ebyte E28-2G4M27S boards they confirmed a direct message in **66-96 ms**, against 126 ms for the
fastest LoRa preset and 546 ms for MEDIUM_SLOW, with no loss over 240 acknowledged messages. LoRa presets keep their
original code path.

Base: `meshtastic/firmware` `develop` (2.8.2, `eaba8b9`). Branch: `sx1280-fast-modems`, one commit per topic:

| Commit | What it does |
|---|---|
| variants: nrf52_promicro_diy-sx1280-e28m27 env | Extends upstream's `nrf52_promicro_diy-sx1280` env: SX128X_MAX_POWER 3 for the module's PA, GPS defaults to not present |
| mesh: FLRC and GFSK presets for SX128x | 8 presets, SX128x driver for both modulations, FLRC fragmentation, carrier sense, airtime, timing |
| serial: handle one ToRadio per StreamAPI run | Removes the API-burst stall found by profiling |
| nrf52: keep CryptoCell up between uses | AES 98 → ~10 ms; also covers CC310 X25519, Ed25519 verify and the RNG, with a 2 s idle power-down |
| nrf52: enter UF2 DFU on ProMicro DIY bootloaders | `--enter-dfu` works on this bootloader (RAM double-reset magic) |

The work was first done on v2.7.26 (branch `sx1280-fast-modems-2.7.26`, which also has the per-packet profiler used for
the measurements below). Moving to develop dropped three of its changes that upstream now covers: the board variant
(upstream added the SX1280 env in #11137), the serial-log suppression with an API client connected (`log_to_serial`
already drops it), and the X25519 shared-key cache (#11979).

## Presets

The presets extend `Config.LoRaConfig.ModemPreset` locally (the official apps and protobufs do not know them), so they
are set by number. They are part of region `LORA_24`'s own preset profile but left out of the region preset map sent to
clients. On a radio other than SX128x the node falls back to the region default (LONG_FAST).

| Preset | No. | Modulation | Bit rate | Coding | Sensitivity (datasheet) |
|---|---|---|---|---|---|
| FLRC_TURBO | 64 | FLRC | 1300 kb/s | CR 1/1 | −96 dBm |
| FLRC_FAST | 65 | FLRC | 1300 kb/s | CR 1/2 | −99 dBm |
| FLRC_MEDIUM | 66 | FLRC | 650 kb/s | CR 1/2 | −104 dBm |
| FLRC_LONG | 67 | FLRC | 260 kb/s | CR 1/2 | −106 dBm |
| GFSK_TURBO | 72 | GFSK | 2000 kb/s | – | −83 dBm |
| GFSK_FAST | 73 | GFSK | 1000 kb/s | – | −87 dBm |
| GFSK_MEDIUM | 74 | GFSK | 500 kb/s | – | −90 dBm |
| GFSK_LONG | 75 | GFSK | 250 kb/s | – | −92 dBm |

```bash
meshtastic --port COM14 --set lora.region LORA_24
meshtastic --port COM14 --set lora.modem_preset 66
```

Each preset derives its own default channel name, as stock presets do, so nodes on different presets do not hear each
other. FLRC CR 3/4 was left out: it lost 20-30 % of exchanges when TX and RX alternate on the bench.

## Design

- **Frame:** 4-byte sync word `2D 4B D4 93` (kept to reject 2.4 GHz noise), 2-byte CRC, variable length. FLRC uses a
  32-bit preamble (16 and 24 bits lost every packet at 650 kb/s CR 1/2 on the bench) and BT 0.5 shaping; GFSK a 24-bit
  preamble and modulation index 0.5 (deviation = bit rate / 4).
- **Driver:** `SX128xInterface::beginModem()` runs the full begin sequence for the configured modem; `reconfigure()`
  and the chip-state recovery go through it whenever FLRC/GFSK is involved, since the packet type changes.
- **FLRC fragmentation:** FLRC payloads stop at 127 bytes. Every FLRC packet carries a 1-byte header (`0x00` = whole
  packet; otherwise index, count and a 3-bit tag) and packets over 126 bytes are split into up to 3 equal fragments,
  10 ms apart. The SX128x sends the first in `launchTransmit()` and the rest through two new `RadioLibInterface` hooks,
  `nextFrameDelayMsec()` and `sendNextFrame()` (a `NEXT_FRAME` notification; the post-TX work moved into
  `onTransmitDone()`). It reassembles in `acceptFrame()`, which works on `radioBuffer`, so frames taken out by the RX
  readout task and frames read on the radio thread go through the same path.
- **Carrier sense:** no CAD outside LoRa. The channel is busy while a sync word is being received or RSSI is 10 dB over
  the larger of the preset's sensitivity and the measured noise floor.
- **Timing:** 1 ms CSMA slot, 1000 ms retransmission base (4500 ms for LoRa), 10 ms minimum gap before each
  transmission so a receiver can read a frame before the next one overwrites the chip buffer.
- **Link metrics:** RSSI is the packet's sync RSSI. The SX1280 has no SNR outside LoRa, so `getRxSNR()` estimates it as
  RSSI − sensitivity − 20 dB, clamped to −20…+10 dB, which keeps SNR-weighted rebroadcast delays working. The RX
  readout task takes its SNR from the same hook.

## Results (v2.7.26 branch)

Two nodes in one room at −18 dBm (RSSI −28 to −32 dBm), 20 messages of 200 bytes per test, measured end to end through
the serial API. "ACK" is a direct message waiting for its acknowledgement; "stream" is 20 broadcasts queued at once.

| Preset | ACK delivered | Median ACK latency | Stream received | Stream rate |
|---|---|---|---|---|
| GFSK_TURBO | 20/20 | 66 ms | 20/20 | 30.6 kb/s |
| GFSK_FAST | 20/20 | 67 ms | 20/20 | 30.2 kb/s |
| GFSK_MEDIUM | 20/20 | 70 ms | 20/20 | 30.7 kb/s |
| GFSK_LONG | 20/20 | 73 ms | 20/20 | 29.7 kb/s |
| FLRC_TURBO | 20/20 | 76 ms | 20/20 | 29.3 kb/s |
| FLRC_FAST | 20/20 | 79 ms | 20/20 | 29.6 kb/s |
| FLRC_MEDIUM | 20/20 | 81 ms | 20/20 | 29.3 kb/s |
| FLRC_LONG | 20/20 | 96 ms | 20/20 | 28.8 kb/s |
| SHORT_TURBO (LoRa) | 20/20 | 126 ms | 20/20 | 26.2 kb/s |
| SHORT_FAST (LoRa) | 20/20 | 167 ms | 20/20 | 18.9 kb/s |
| MEDIUM_FAST (LoRa) | 20/20 | 327 ms | 20/20 | 7.5 kb/s |
| MEDIUM_SLOW (LoRa) | 20/20 | 546 ms | 20/20 | 4.1 kb/s |

The ~30 kb/s stream ceiling is the PC side: the Python library hands the node one message every ~50 ms. The develop
port builds (`nrf52_promicro_diy-sx1280-e28m27`, flash 95.6 %) but has not been re-measured on hardware yet.

## Where the time went

Before the serial and crypto changes the same presets confirmed in 158-174 ms and GFSK lost ~45 % of back-to-back
broadcasts. Profiling showed:

| Cost | Measured | Fix |
|---|---|---|
| Debug text on the USB console with an API client connected | ~1.9 ms per line × 32-38 lines per message = 75-80 % of per-packet CPU | Upstream develop already drops it |
| GPS probe for an absent module | main loop blocked up to 1.05 s | `GPS_DEFAULT_NOT_PRESENT` in the env |
| A burst of ToRadio from the client | main loop held up to 400 ms | One ToRadio per `StreamAPI` run |
| X25519 per PKI DM | ~70 ms | Upstream develop's shared-secret cache |
| CryptoCell init around every AES block | ~90 ms | Keep it up, power down after 2 s idle |

With those removed the receiver handles a packet in 2-5 ms, which is why the inter-frame gaps could drop from 30 to
10 ms. Develop's RX readout task takes frames out of the chip as soon as they arrive, so the 10 ms gaps may now be
larger than needed; that is untested. For reference, the same radios moved 934 kb/s (FLRC) and 1344 kb/s (GFSK) PC to
PC with a minimal firmware; the remaining gap is Meshtastic's per-packet processing.

## Build and flash

```bash
pio run -e nrf52_promicro_diy-sx1280-e28m27
meshtastic --port COM14 --enter-dfu        # then copy the .uf2 to the drive that appears
```

## Known limits

- Range not tested: all results come from one room. GFSK_TURBO has ~23 dB less link budget than FLRC_LONG.
- The Android/iOS apps show the new presets as unknown; editing LoRa settings there overwrites them.
