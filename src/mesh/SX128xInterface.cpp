#if RADIOLIB_EXCLUDE_SX128X != 1
#include "SX128xInterface.h"
#include "Throttle.h"
#include "configuration.h"
#include "error.h"
#include "mesh/NodeDB.h"

#if ARCH_PORTDUINO
#include "PortduinoGlue.h"
#endif

// Particular boards might define a different max power based on what their hardware can do
#if ARCH_PORTDUINO
#define SX128X_MAX_POWER portduino_config.sx128x_max_power
#endif
#ifndef SX128X_MAX_POWER
#define SX128X_MAX_POWER 13
#endif

template <typename T>
SX128xInterface<T>::SX128xInterface(LockingArduinoHal *hal, RADIOLIB_PIN_TYPE cs, RADIOLIB_PIN_TYPE irq, RADIOLIB_PIN_TYPE rst,
                                    RADIOLIB_PIN_TYPE busy)
    : RadioLibInterface(hal, cs, irq, rst, busy, &lora), lora(&module)
{
    LOG_DEBUG("SX128xInterface(cs=%d, irq=%d, rst=%d, busy=%d)", cs, irq, rst, busy);
}

/// Initialise the Driver transport hardware and software.
/// Make sure the Driver is properly configured before calling init().
/// \return true if initialisation succeeded.
template <typename T> bool SX128xInterface<T>::init()
{
#ifdef SX128X_POWER_EN
    pinMode(SX128X_POWER_EN, OUTPUT);
    digitalWrite(SX128X_POWER_EN, HIGH);
#endif

#ifdef RF95_FAN_EN
    pinMode(RF95_FAN_EN, OUTPUT);
    digitalWrite(RF95_FAN_EN, 1);
#endif

#if ARCH_PORTDUINO
    if (portduino_config.lora_rxen_pin.pin != RADIOLIB_NC) {
        pinMode(portduino_config.lora_rxen_pin.pin, OUTPUT);
        digitalWrite(portduino_config.lora_rxen_pin.pin, LOW); // Set low before becoming an output
    }
    if (portduino_config.lora_txen_pin.pin != RADIOLIB_NC) {
        pinMode(portduino_config.lora_txen_pin.pin, OUTPUT);
        digitalWrite(portduino_config.lora_txen_pin.pin, LOW); // Set low before becoming an output
    }
#else
#if defined(SX128X_RXEN) && (SX128X_RXEN != RADIOLIB_NC) // set not rx or tx mode
    pinMode(SX128X_RXEN, OUTPUT);
    digitalWrite(SX128X_RXEN, LOW); // Set low before becoming an output
#endif
#if defined(SX128X_TXEN) && (SX128X_TXEN != RADIOLIB_NC)
    pinMode(SX128X_TXEN, OUTPUT);
    digitalWrite(SX128X_TXEN, LOW);
#endif
#endif

    RadioLibInterface::init();

    limitPower(SX128X_MAX_POWER);

    preambleLength = 12; // 12 is the default for this chip, 32 does not RX at all

    int res = beginModem();
    // \todo Display actual typename of the adapter, not just `SX128x`
    LOG_INFO("SX128x init result %d", res);
    if (res == RADIOLIB_ERR_CHIP_NOT_FOUND || res == RADIOLIB_ERR_SPI_CMD_FAILED)
        return false;

    if ((config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_LORA_24) && (res == RADIOLIB_ERR_INVALID_FREQUENCY)) {
        LOG_WARN("Radio only supports 2.4GHz LoRa. Adjusting Region and rebooting");
        config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_LORA_24;
        nodeDB->saveToDisk(SEGMENT_CONFIG);
        delay(2000);
#if defined(ARCH_ESP32)
        ESP.restart();
#elif defined(ARCH_NRF52)
        NVIC_SystemReset();
#else
        LOG_ERROR("FIXME implement reboot for this platform. Skip for now");
#endif
    }

    LOG_INFO("Frequency set to %f", getFreq());
    LOG_INFO("Bandwidth set to %f", bw);
    LOG_INFO("Power output set to %d", power);

#if defined(SX128X_TXEN) && (SX128X_TXEN != RADIOLIB_NC) && defined(SX128X_RXEN) && (SX128X_RXEN != RADIOLIB_NC)
    if (res == RADIOLIB_ERR_NONE) {
        lora.setRfSwitchPins(SX128X_RXEN, SX128X_TXEN);
    }
#elif ARCH_PORTDUINO
    if (res == RADIOLIB_ERR_NONE && portduino_config.lora_rxen_pin.pin != RADIOLIB_NC &&
        portduino_config.lora_txen_pin.pin != RADIOLIB_NC) {
        lora.setRfSwitchPins(portduino_config.lora_rxen_pin.pin, portduino_config.lora_txen_pin.pin);
    }
#endif

    if (res == RADIOLIB_ERR_NONE)
        res = lora.setCRC(2);

    if (res == RADIOLIB_ERR_NONE)
        startReceive(); // start receiving

    return res == RADIOLIB_ERR_NONE;
}

template <typename T> int16_t SX128xInterface<T>::beginModem()
{
    // Not LoRa-compatible on air: a different sync word keeps FLRC/GFSK meshes apart from other 2.4 GHz traffic
    static uint8_t fastSyncWord[4] = {0x2D, 0x4B, 0xD4, 0x93};
    int16_t res;

    if (power > SX128X_MAX_POWER)
        power = SX128X_MAX_POWER;

    switch (fastModem.modulation) {
    case MODULATION_FLRC: {
        // RadioLib FLRC coding rate codes: 2 = 1/2, 3 = 3/4, 4 = 1/1
        uint8_t flrcCr = fastModem.crNum == fastModem.crDen ? 4 : (fastModem.crNum == 3 ? 3 : 2);
        res = lora.beginFLRC(getFreq(), fastModem.bitrateKbps, flrcCr, power, FAST_PREAMBLE_BITS, RADIOLIB_SHAPING_0_5);
        break;
    }
    case MODULATION_GFSK:
        // deviation = bitrate / 4 gives modulation index 0.5 (MSK-like, narrowest spectrum for the bitrate)
        res = lora.beginGFSK(getFreq(), fastModem.bitrateKbps, fastModem.bitrateKbps / 4.0f, power, FAST_PREAMBLE_BITS);
        break;
    default:
        res = lora.begin(getFreq(), bw, sf, cr, syncWord, power, preambleLength);
        activeModulation = MODULATION_LORA;
        return res;
    }

    if (res == RADIOLIB_ERR_NONE)
        res = lora.setSyncWord(fastSyncWord, sizeof(fastSyncWord));
    if (res == RADIOLIB_ERR_NONE)
        res = lora.variablePacketLengthMode(maxFrameLen());
    if (res == RADIOLIB_ERR_NONE)
        activeModulation = fastModem.modulation;
    LOG_INFO("SX128x %s %u kb/s, result %d", fastModem.modulation == MODULATION_FLRC ? "FLRC" : "GFSK", fastModem.bitrateKbps,
             res);
    return res;
}

template <typename T> bool SX128xInterface<T>::reconfigure()
{
    RadioLibInterface::reconfigure();

    // set mode to standby
    setStandby();

    if (isFastModem() || activeModulation != MODULATION_LORA) {
        // switching modem type needs the full begin sequence (packet type, modulation and packet params)
        int err = beginModem();
        if (err == RADIOLIB_ERR_NONE)
            err = lora.setCRC(2);
        if (err != RADIOLIB_ERR_NONE)
            RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_INVALID_RADIO_SETTING);
        startReceive();
        return RADIOLIB_ERR_NONE;
    }

    // configure publicly accessible settings
    int err = lora.setSpreadingFactor(sf);
    if (err != RADIOLIB_ERR_NONE)
        RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_INVALID_RADIO_SETTING);

    err = lora.setBandwidth(bw);
    if (err != RADIOLIB_ERR_NONE)
        RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_INVALID_RADIO_SETTING);

    err = lora.setCodingRate(cr, cr != 7); // use long interleaving except if CR is 4/7 which doesn't support it
    if (err != RADIOLIB_ERR_NONE)
        RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_INVALID_RADIO_SETTING);

    err = lora.setSyncWord(syncWord);
    if (err != RADIOLIB_ERR_NONE)
        LOG_ERROR("SX128X setSyncWord %s%d", radioLibErr, err);
    assert(err == RADIOLIB_ERR_NONE);

    err = lora.setPreambleLength(preambleLength);
    if (err != RADIOLIB_ERR_NONE)
        LOG_ERROR("SX128X setPreambleLength %s%d", radioLibErr, err);
    assert(err == RADIOLIB_ERR_NONE);

    err = lora.setFrequency(getFreq());
    if (err != RADIOLIB_ERR_NONE)
        RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_INVALID_RADIO_SETTING);

    if (power > SX128X_MAX_POWER) // This chip has lower power limits than some
        power = SX128X_MAX_POWER;

    err = lora.setOutputPower(power);
    if (err != RADIOLIB_ERR_NONE)
        LOG_ERROR("SX128X setOutputPower %s%d", radioLibErr, err);
    assert(err == RADIOLIB_ERR_NONE);

    startReceive(); // restart receiving

    return RADIOLIB_ERR_NONE;
}

template <typename T> void SX128xInterface<T>::disableInterrupt()
{
    lora.clearDio1Action();
}

template <typename T> bool SX128xInterface<T>::wideLora()
{
    return true;
}

template <typename T> void SX128xInterface<T>::setStandby()
{
    checkNotification(); // handle any pending interrupts before we force standby

    int err = lora.standby();

    if (err != RADIOLIB_ERR_NONE)
        LOG_ERROR("SX128x standby %s%d", radioLibErr, err);
    assert(err == RADIOLIB_ERR_NONE);
#if ARCH_PORTDUINO
    if (portduino_config.lora_rxen_pin.pin != RADIOLIB_NC) {
        digitalWrite(portduino_config.lora_rxen_pin.pin, LOW);
    }
    if (portduino_config.lora_txen_pin.pin != RADIOLIB_NC) {
        digitalWrite(portduino_config.lora_txen_pin.pin, LOW);
    }
#else
#if defined(SX128X_RXEN) && (SX128X_RXEN != RADIOLIB_NC) // we have RXEN/TXEN control - turn off RX and TX power
    digitalWrite(SX128X_RXEN, LOW);
#endif
#if defined(SX128X_TXEN) && (SX128X_TXEN != RADIOLIB_NC)
    digitalWrite(SX128X_TXEN, LOW);
#endif
#endif
    isReceiving = false; // If we were receiving, not any more
    activeReceiveStart = 0;
    disableInterrupt();
    completeSending(); // If we were sending, not anymore
    RadioLibInterface::setStandby();
}

/**
 * Add SNR data to received messages
 */
template <typename T> void SX128xInterface<T>::addReceiveMetadata(meshtastic_MeshPacket *mp)
{
    if (isFastModem()) {
        // FLRC/GFSK report no SNR. Derive one from the margin over sensitivity so SNR-weighted
        // rebroadcast delays keep working (far nodes, small margin, relay first).
        float rssi = lora.getRSSI();
        mp->rx_rssi = lround(rssi);
        mp->rx_snr = constrain(rssi - fastModem.sensitivityDbm - 20.0f, -20.0f, 10.0f);
        return;
    }
    // LOG_DEBUG("PacketStatus %x", lora.getPacketStatus());
    mp->rx_snr = lora.getSNR();
    mp->rx_rssi = lround(lora.getRSSI());
    LOG_DEBUG("Corrected frequency offset: %f", lora.getFrequencyError());
}

/** We override to turn on transmitter power as needed.
 */
template <typename T> void SX128xInterface<T>::configHardwareForSend()
{
#if ARCH_PORTDUINO
    if (portduino_config.lora_txen_pin.pin != RADIOLIB_NC) {
        digitalWrite(portduino_config.lora_txen_pin.pin, HIGH);
    }
    if (portduino_config.lora_rxen_pin.pin != RADIOLIB_NC) {
        digitalWrite(portduino_config.lora_rxen_pin.pin, LOW);
    }

#else
#if defined(SX128X_TXEN) && (SX128X_TXEN != RADIOLIB_NC) // we have RXEN/TXEN control - turn on TX power / off RX power
    digitalWrite(SX128X_TXEN, HIGH);
#endif
#if defined(SX128X_RXEN) && (SX128X_RXEN != RADIOLIB_NC)
    digitalWrite(SX128X_RXEN, LOW);
#endif
#endif

    RadioLibInterface::configHardwareForSend();
}

// For power draw measurements, helpful to force radio to stay sleeping
// #define SLEEP_ONLY

template <typename T> void SX128xInterface<T>::startReceive()
{
#ifdef SLEEP_ONLY
    sleep();
#else

    setStandby();

#if ARCH_PORTDUINO
    if (portduino_config.lora_rxen_pin.pin != RADIOLIB_NC) {
        digitalWrite(portduino_config.lora_rxen_pin.pin, HIGH);
    }
    if (portduino_config.lora_txen_pin.pin != RADIOLIB_NC) {
        digitalWrite(portduino_config.lora_txen_pin.pin, LOW);
    }

#else
#if defined(SX128X_RXEN) && (SX128X_RXEN != RADIOLIB_NC) // we have RXEN/TXEN control - turn on RX power / off TX power
    digitalWrite(SX128X_RXEN, HIGH);
#endif
#if defined(SX128X_TXEN) && (SX128X_TXEN != RADIOLIB_NC)
    digitalWrite(SX128X_TXEN, LOW);
#endif
#endif

    RadioLibIrqFlags_t irqFlags = MESHTASTIC_RADIOLIB_IRQ_RX_FLAGS;
    if (isFastModem()) {
        // a transmit leaves its own length as the RX max-length filter; restore the real maximum
        lora.variablePacketLengthMode(maxFrameLen());
        irqFlags |= (1UL << RADIOLIB_IRQ_SYNC_WORD_VALID);
    }
    int err = lora.startReceive(RADIOLIB_SX128X_RX_TIMEOUT_INF, irqFlags);

    if (err != RADIOLIB_ERR_NONE)
        LOG_ERROR("SX128X startReceive %s%d", radioLibErr, err);
    assert(err == RADIOLIB_ERR_NONE);

    RadioLibInterface::startReceive();

    // Must be done AFTER, starting transmit, because startTransmit clears (possibly stale) interrupt pending register bits
    enableInterrupt(isrRxLevel0);
    checkRxDoneIrqFlag();
#endif
}

/** Is the channel currently active? */
template <typename T> bool SX128xInterface<T>::isChannelActive()
{
    if (isFastModem()) {
        // no CAD outside LoRa: listen-before-talk on energy, plus any sync word already being received
        if (!isReceiving) {
            startReceive();
            delayMicroseconds(100);
        }
        if (isActivelyReceiving())
            return true;
        int32_t threshold = max((int32_t)fastModem.sensitivityDbm + 10, getNoiseFloor() + 10);
        int16_t rssi = getCurrentRSSI();
        if (rssi > threshold) {
            LOG_DEBUG("Channel busy, RSSI %d > %d dBm", rssi, threshold);
            return true;
        }
        return false;
    }

    // check if we can detect a LoRa preamble on the current channel
    ChannelScanConfig_t cfg = {.cad = {.symNum = NUM_SYM_CAD_24GHZ,
                                       .detPeak = 0,
                                       .detMin = 0,
                                       .exitMode = 0,
                                       .timeout = 0,
                                       .irqFlags = RADIOLIB_IRQ_CAD_DEFAULT_FLAGS,
                                       .irqMask = RADIOLIB_IRQ_CAD_DEFAULT_MASK}};
    int16_t result;

    setStandby();
    result = lora.scanChannel(cfg);
    if (result == RADIOLIB_LORA_DETECTED)
        return true;
    if (result != RADIOLIB_CHANNEL_FREE)
        LOG_ERROR("SX128X scanChannel %s%d", radioLibErr, result);
    assert(result != RADIOLIB_ERR_WRONG_MODEM);

    return false;
}

/** Could we send right now (i.e. either not actively receiving or transmitting)? */
template <typename T> bool SX128xInterface<T>::isActivelyReceiving()
{
    if (isFastModem())
        return receiveDetected(lora.getIrqStatus(), RADIOLIB_SX128X_IRQ_SYNC_WORD_VALID, RADIOLIB_SX128X_IRQ_SYNC_WORD_VALID);
    return receiveDetected(lora.getIrqStatus(), RADIOLIB_SX128X_IRQ_HEADER_VALID, RADIOLIB_SX128X_IRQ_PREAMBLE_DETECTED);
}

template <typename T> bool SX128xInterface<T>::sleep()
{
    // Not keeping config is busted - next time nrf52 board boots lora sending fails  tcxo related? - see datasheet
    // \todo Display actual typename of the adapter, not just `SX128x`
    LOG_DEBUG("SX128x entering sleep mode"); // (FIXME, don't keep config)
    setStandby();                            // Stop any pending operations

    // turn off TCXO if it was powered
    // FIXME - this isn't correct
    // lora.setTCXO(0);

    // put chipset into sleep mode (we've already disabled interrupts by now)
    bool keepConfig = true;
    lora.sleep(keepConfig); // Note: we do not keep the config, full reinit will be needed

#ifdef SX128X_POWER_EN
    digitalWrite(SX128X_POWER_EN, LOW);
#endif

    return true;
}

template <typename T> int16_t SX128xInterface<T>::getCurrentRSSI()
{
    float rssi = lora.getRSSI(false);
    return (int16_t)round(rssi);
}

/*
 * FLRC framing: byte 0 = 0x00 for an unfragmented packet, otherwise
 * 1 | index(2 bits) | count-1 (2 bits) | tag (3 bits), followed by an equal share of the packet so no
 * fragment falls under FLRC's 6-byte minimum.
 */
template <typename T> int16_t SX128xInterface<T>::startTransmitFrame(size_t numbytes)
{
    if (!usesFragments())
        return RadioLibInterface::startTransmitFrame(numbytes);

    txFragLen = numbytes;
    txFragCount = fragmentCount(numbytes);
    txFragNext = 0;
    txFragTag = (txFragTag + 1) & 0x07;
    return transmitFragment();
}

template <typename T> int16_t SX128xInterface<T>::transmitFragment()
{
    size_t per = (txFragLen + txFragCount - 1) / txFragCount;
    size_t off = txFragNext * per;
    size_t len = min(per, txFragLen - off);

    frameBuf[0] = txFragCount == 1 ? 0 : (0x80 | (txFragNext << 5) | ((txFragCount - 1) << 3) | txFragTag);
    memcpy(frameBuf + 1, ((uint8_t *)&radioBuffer) + off, len);
    txFragNext++;
    return lora.startTransmit(frameBuf, len + 1);
}

template <typename T> uint32_t SX128xInterface<T>::nextFrameDelayMsec()
{
    return (usesFragments() && sendingPacket && txFragNext < txFragCount) ? FRAG_GAP_MSEC : 0;
}

template <typename T> bool SX128xInterface<T>::sendNextFrame()
{
    if (!usesFragments() || !sendingPacket || txFragNext >= txFragCount)
        return false;

    int16_t res = transmitFragment();
    if (res != RADIOLIB_ERR_NONE) {
        LOG_ERROR("FLRC fragment %u/%u failed, error=%d", txFragNext, txFragCount, res);
        return false;
    }
    return true;
}

template <typename T> int SX128xInterface<T>::readFrame(size_t &length)
{
    if (!usesFragments())
        return RadioLibInterface::readFrame(length);

    if (length < 2 || length > FLRC_MAX_FRAME)
        return RADIOLIB_ERR_PACKET_TOO_LONG;
    int state = lora.readData(frameBuf, length);
    if (state != RADIOLIB_ERR_NONE)
        return state;

    uint8_t hdr = frameBuf[0];
    size_t len = length - 1;
    if (!(hdr & 0x80)) {
        memcpy(&radioBuffer, frameBuf + 1, len);
        length = len;
        return RADIOLIB_ERR_NONE;
    }

    uint8_t idx = (hdr >> 5) & 0x03, count = ((hdr >> 3) & 0x03) + 1, tag = hdr & 0x07;
    if (idx == 0) {
        rxFragTag = tag;
        rxFragCount = count;
        rxFragNext = 0;
        rxFragLen = 0;
    }
    if (idx != rxFragNext || tag != rxFragTag || count != rxFragCount || rxFragLen + len > sizeof(rxFrag)) {
        LOG_DEBUG("Drop FLRC fragment %u/%u", idx, count);
        rxFragNext = 0xFF;
        return RX_FRAME_PARTIAL;
    }
    memcpy(rxFrag + rxFragLen, frameBuf + 1, len);
    rxFragLen += len;
    if (++rxFragNext < rxFragCount)
        return RX_FRAME_PARTIAL;

    memcpy(&radioBuffer, rxFrag, rxFragLen);
    length = rxFragLen;
    rxFragNext = 0xFF;
    return RADIOLIB_ERR_NONE;
}

/// Time on air from the SX1280 datasheet formulas (FLRC 7.3.4, GFSK 7.1), validated against measurements on hardware.
template <typename T> uint32_t SX128xInterface<T>::fastPacketTimeMsec(uint32_t pl)
{
    uint32_t us = 0;
    if (fastModem.modulation == MODULATION_FLRC) {
        uint8_t count = fragmentCount(pl);
        uint32_t per = (pl + count - 1) / count;
        for (uint8_t i = 0; i < count; i++) {
            uint32_t len = min(per, pl - i * per) + 1;
            uint32_t uncoded = FAST_PREAMBLE_BITS + 21 + 32 + 16;
            uint32_t raw = (len + 2) * 8 + (fastModem.crNum == fastModem.crDen ? 0 : 6);
            uint32_t coded = raw * fastModem.crDen / fastModem.crNum;
            us += (uncoded + coded) * 1000 / fastModem.bitrateKbps;
        }
        us += (count - 1) * FRAG_GAP_MSEC * 1000;
    } else {
        uint32_t bits = FAST_PREAMBLE_BITS + 32 + 9 + 8 * pl + 16;
        us = bits * 1000 / fastModem.bitrateKbps;
    }
    return max((uint32_t)1, (us + 999) / 1000);
}
#endif