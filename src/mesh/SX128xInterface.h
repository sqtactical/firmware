#pragma once

#include "RadioLibInterface.h"

/**
 * \brief Adapter for SX128x radio family. Implements common logic for child classes.
 * \tparam T RadioLib module type for SX128x: SX1280.
 */
template <class T> class SX128xInterface : public RadioLibInterface
{
  public:
    SX128xInterface(LockingArduinoHal *hal, RADIOLIB_PIN_TYPE cs, RADIOLIB_PIN_TYPE irq, RADIOLIB_PIN_TYPE rst,
                    RADIOLIB_PIN_TYPE busy);

    /// Initialise the Driver transport hardware and software.
    /// Make sure the Driver is properly configured before calling init().
    /// \return true if initialisation succeeded.
    virtual bool init() override;

    virtual bool wideLora() override;

    bool supportsFastModems() override { return true; }

    /// Apply any radio provisioning changes
    /// Make sure the Driver is properly configured before calling init().
    /// \return true if initialisation succeeded.
    virtual bool reconfigure() override;

    /// Prepare hardware for sleep.  Call this _only_ for deep sleep, not needed for light sleep.
    virtual bool sleep() override;

    bool isIRQPending() override { return lora.getIrqFlags() != 0; }

  protected:
    /**
     * Specific module instance
     */
    T lora;

    int16_t getCurrentRSSI() override;

    /**
     * Glue functions called from ISR land
     */
    virtual void disableInterrupt() override;

    /**
     * Enable a particular ISR callback glue function
     */
    virtual void enableInterrupt(void (*callback)()) { lora.setDio1Action(callback); }

    /** can we detect a LoRa preamble on the current channel? */
    virtual bool isChannelActive() override;

    /** are we actively receiving a packet (only called during receiving state) */
    virtual bool isActivelyReceiving() override;

    /**
     * Start waiting to receive a message
     */
    virtual void startReceive() override;

    /**
     *  We override to turn on transmitter power as needed.
     */
    virtual void configHardwareForSend() override;

    /**
     * Add SNR data to received messages
     */
    virtual void addReceiveMetadata(meshtastic_MeshPacket *mp) override;

    virtual void setStandby() override;

    uint32_t getPacketTime(uint32_t pl, bool received) override
    {
        return isFastModem() ? fastPacketTimeMsec(pl) : computePacketTime(lora, pl, received);
    }

    // FLRC frames carry at most 127 bytes, so FLRC packets get a 1-byte fragment header and are split when needed.
    int16_t startTransmitFrame(size_t numbytes) override;
    uint32_t nextFrameDelayMsec() override;
    bool sendNextFrame() override;
    int readFrame(size_t &length) override;

  private:
    static const uint8_t FLRC_MAX_FRAME = 127;
    static const uint8_t FAST_PREAMBLE_BITS = 24;
    // Receivers service RX from the cooperative main loop; give them time to read a fragment before the next one
    // overwrites the SX128x buffer (continuous RX keeps writing at the same address)
    static const uint8_t FRAG_GAP_MSEC = 10;

    RadioModulation activeModulation = MODULATION_LORA; // modem currently programmed in the chip
    uint8_t frameBuf[FLRC_MAX_FRAME];
    size_t txFragLen = 0;
    uint8_t txFragCount = 0, txFragNext = 0, txFragTag = 0;
    uint8_t rxFrag[MAX_LORA_PAYLOAD_LEN];
    size_t rxFragLen = 0;
    uint8_t rxFragTag = 0, rxFragCount = 0, rxFragNext = 0xFF;

    int16_t beginModem();
    bool usesFragments() const { return fastModem.modulation == MODULATION_FLRC; }
    size_t maxFrameLen() const { return usesFragments() ? FLRC_MAX_FRAME : MAX_LORA_PAYLOAD_LEN; }
    uint8_t fragmentCount(size_t len) const { return (len + FLRC_MAX_FRAME - 2) / (FLRC_MAX_FRAME - 1); }
    int16_t transmitFragment();
    uint32_t fastPacketTimeMsec(uint32_t pl);
};
