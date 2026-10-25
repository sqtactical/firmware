# SX1280 fast modems: FLRC and GFSK presets for Meshtastic 2.7.26

This branch adds FLRC and GFSK as modulations independent of LoRa on SX128x radios (2.4 GHz). On a pair of
ProMicro nRF52840 + Ebyte E28-2G4M27S boards they confirm a direct message in **66-96 ms**, against 126 ms for the
fastest LoRa preset and 546 ms for MEDIUM_SLOW, with no loss over 240 acknowledged messages. LoRa presets keep their
original code path.

Base: Meshtastic `v2.7.26.54e0d8d`. Branch: `sx1280-fast-modems`, one commit per topic:

| Commit | What it does |
|---|---|
| variants: add nrf52_promicro_diy_sx1280 | Board variant for the ProMicro + E28 (SX1280 with external PA, RXEN/TXEN), GPS defaults to not present |
| mesh: FLRC and GFSK presets for SX128x | 8 presets, SX128x driver for both modulations, FLRC fragmentation, carrier sense, airtime, timing |
| serial: no debug text … one ToRadio per run | Removes the serial-log and API-burst stalls found by profiling |
| crypto: cache X25519 … keep CryptoCell initialised | PKI decrypt 81 → 11 ms, AES 98 → ~10 ms on nRF52 |
| nrf52: enter UF2 DFU on ProMicro DIY bootloaders | `--enter-dfu` works on this bootloader (RAM double-reset magic) |
| debug: per-packet profiler | `-DPROFILE_PACKETS` instrumentation, compiled out by default |

## Presets

The presets extend `Config.LoRaConfig.ModemPreset` locally (the official apps and protobufs do not know them), so they
are set by number. They need region `LORA_24` and an SX128x radio; otherwise the node falls back to LONG_FAST.

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

- **Frame:** 24-bit preamble, 4-byte sync word `2D 4B D4 93` (kept to reject 2.4 GHz noise), 2-byte CRC, variable length.
  GFSK uses modulation index 0.5 (deviation = bit rate / 4); FLRC uses BT 0.5 shaping.
- **FLRC fragmentation:** FLRC payloads stop at 127 bytes. Every FLRC packet carries a 1-byte header (`0x00` = whole
  packet; otherwise index, count and a 3-bit tag) and packets over 126 bytes are split into up to 3 equal fragments,
  10 ms apart, reassembled by the receiver. `RadioLibInterface` gains the frame hooks this needs
  (`startTransmitFrame`, `nextFrameDelayMsec`, `sendNextFrame`, `readFrame`, `NEXT_FRAME`).
- **Carrier sense:** no CAD outside LoRa. The channel is busy while a sync word is being received or RSSI is 10 dB over
  the larger of the preset's sensitivity and the measured noise floor.
- **Timing:** 1 ms CSMA slot, 1000 ms retransmission base (4500 ms for LoRa), 10 ms minimum gap before each
  transmission so a receiver can read a frame before the next one overwrites the chip buffer.
- **Link metrics:** RSSI is the packet's sync RSSI. The SX1280 has no SNR outside LoRa, so SNR is estimated as
  RSSI − sensitivity − 20 dB, clamped to −20…+10 dB, which keeps SNR-weighted rebroadcast delays working.

## Results

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

The ~30 kb/s stream ceiling is the PC side: the Python library hands the node one message every ~50 ms.

## Where the time went

Before the serial and crypto commits the same presets confirmed in 158-174 ms and GFSK lost ~45 % of back-to-back
broadcasts. The profiler (`-DPROFILE_PACKETS`, see below) showed:

| Cost | Measured | Fix |
|---|---|---|
| Debug text on the USB console with an API client connected | ~1.9 ms per line × 32-38 lines per message = 75-80 % of per-packet CPU | Dropped below WARN while the API is in protobuf mode |
| GPS probe for an absent module | main loop blocked up to 1.05 s | `GPS_DEFAULT_NOT_PRESENT` in the variant |
| A burst of ToRadio from the client | main loop held up to 400 ms | One ToRadio per `StreamAPI` run |
| X25519 per PKI DM | ~70 ms | 4-entry shared-key cache |
| CryptoCell init around every AES block | ~90 ms | Keep it initialised |

With those removed the receiver handles a packet in 2-5 ms, which is why the inter-frame gaps could drop from 30 to
10 ms. For reference, the same radios moved 934 kb/s (FLRC) and 1344 kb/s (GFSK) PC to PC with a minimal firmware; the
remaining gap is Meshtastic's per-packet processing.

## Build and flash

```bash
pio run -e nrf52_promicro_diy_sx1280
meshtastic --port COM14 --enter-dfu        # then copy the .uf2 to the drive that appears
```

## Profiler

Build with `-D PROFILE_PACKETS` (for example in the variant's `build_flags`). Three seconds after packet traffic stops
the node prints `PROF stage=… n=… avg_us=… max_us=… total_ms=…` and `PROF thread=…` lines on the USB serial;
`micros()` on nRF52 ticks in ~1 ms steps, so read averages over many packets.

## Known limits

- Range not tested: all results come from one room. GFSK_TURBO has ~23 dB less link budget than FLRC_LONG.
- The Android/iOS apps show the new presets as unknown; editing LoRa settings there overwrites them.
- Serial debug text is suppressed while an API client is connected (enable `security.debug_log_api_enabled` to get logs
  as protobuf records).
