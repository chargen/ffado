/*
 * Copyright (C) 2005-2009 by Jonathan Woithe
 * Copyright (C) 2005-2008 by Pieter Palmers
 *
 * This file is part of FFADO
 * FFADO = Free Firewire (pro-)audio drivers for linux
 *
 * FFADO is based upon FreeBoB.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#warning RME support is at an early development stage and is not functional

#include "config.h"

#include "rme/rme_avdevice.h"
#include "rme/fireface_def.h"
#include "rme/fireface_settings_ctrls.h"

#include "libieee1394/configrom.h"
#include "libieee1394/ieee1394service.h"

#include "debugmodule/debugmodule.h"

#include "devicemanager.h"

#include <string>
#include <stdint.h>
#include <assert.h>
#include "libutil/ByteSwap.h"

#include <iostream>
#include <sstream>

#include <libraw1394/csr.h>

// Known values for the unit version of RME devices
#define RME_UNITVERSION_FF800  0x0001
#define RME_UNITVERSION_FF400  0x0002

namespace Rme {

// The RME devices expect async packet data in little endian format (as
// opposed to bus order, which is big endian).  Therefore define our own
// 32-bit byteswap function to do this.
#if __BYTE_ORDER == __BIG_ENDIAN
static inline uint32_t
ByteSwapToDevice32(uint32_t d)
{
    return byteswap_32(d);
}
ByteSwapFromDevice32(uint32_t d)
{
    return byteswap_32(d);
}
#else
static inline uint32_t
ByteSwapToDevice32(uint32_t d)
{
    return d;
}
static inline uint32_t
ByteSwapFromDevice32(uint32_t d)
{
    return d;
}
#endif

Device::Device( DeviceManager& d,
                      std::auto_ptr<ConfigRom>( configRom ))
    : FFADODevice( d, configRom )
    , m_rme_model( RME_MODEL_NONE )
    , num_channels( 0 )
    , frames_per_packet( 0 )
    , speed800( 0 )
    , iso_tx_channel( -1 )
    , iso_rx_channel( -1 )
    , m_MixerContainer( NULL )
    , m_ControlContainer( NULL )
{
    debugOutput( DEBUG_LEVEL_VERBOSE, "Created Rme::Device (NodeID %d)\n",
                 getConfigRom().getNodeId() );
}

Device::~Device()
{
    if (iso_tx_channel>=0 && !get1394Service().freeIsoChannel(iso_tx_channel)) {
        debugOutput(DEBUG_LEVEL_VERBOSE, "Could not free tx iso channel %d\n", iso_tx_channel);
    }
    if (iso_rx_channel>=0 && !get1394Service().freeIsoChannel(iso_rx_channel)) {
        debugOutput(DEBUG_LEVEL_VERBOSE, "Could not free rx iso channel %d\n", iso_rx_channel);
    }

    destroyMixer();

    if (dev_config != NULL) {
        switch (rme_shm_close(dev_config)) {
            case RSO_CLOSE:
                debugOutput( DEBUG_LEVEL_VERBOSE, "Configuration shared data object closed\n");
                break;
            case RSO_CLOSE_DELETE:
                debugOutput( DEBUG_LEVEL_VERBOSE, "Configuration shared data object closed and deleted (no other users)\n");
                break;
        }
    }
}

bool
Device::buildMixer() {
    signed int i;
    bool result = true;

    destroyMixer();
    debugOutput(DEBUG_LEVEL_VERBOSE, "Building an RME mixer...\n");


    // Non-mixer device controls
    m_ControlContainer = new Control::Container(this, "Control");
    if (!m_ControlContainer) {
        debugError("Could not create control container\n");
        destroyMixer();
        return false;
    }

    result &= m_ControlContainer->addElement(
        new RmeSettingsCtrl(*this, RME_CTRL_INFO_MODEL, 0,
            "Model", "Model ID", ""));
    result &= m_ControlContainer->addElement(
        new RmeSettingsCtrl(*this, RME_CTRL_INFO_TCO_PRESENT, 0,
            "TCO_present", "TCO is present", ""));

    result &= m_ControlContainer->addElement(
        new RmeSettingsCtrl(*this, RME_CTRL_PHANTOM_SW, 0,
            "Phantom", "Phantom switches", ""));
    result &= m_ControlContainer->addElement(
        new RmeSettingsCtrl(*this, RME_CTRL_INPUT_LEVEL, 0,
            "Input_level", "Input level", ""));
    result &= m_ControlContainer->addElement(
        new RmeSettingsCtrl(*this, RME_CTRL_OUTPUT_LEVEL, 0,
            "Output_level", "Output level", ""));
    result &= m_ControlContainer->addElement(
        new RmeSettingsCtrl(*this, RME_CTRL_PHONES_LEVEL, 0,
            "Phones_level", "Phones level", ""));

    if (m_rme_model == RME_MODEL_FIREFACE400) {
        // Instrument input options
        for (i=3; i<=4; i++) {
            char path[32], desc[64];
            snprintf(path, sizeof(path), "Chan%d_opt_instr", i);
            snprintf(desc, sizeof(desc), "Chan%d instrument option", i);
            result &= m_ControlContainer->addElement(
                new RmeSettingsCtrl(*this, RME_CTRL_FF400_INSTR_SW, i,
                    path, desc, ""));
            snprintf(path, sizeof(path), "Chan%d_opt_pad", i);
            snprintf(desc, sizeof(desc), "Chan%d pad option", i);
            result &= m_ControlContainer->addElement(
                new RmeSettingsCtrl(*this, RME_CTRL_FF400_PAD_SW, i,
                    path, desc, ""));
        }

        // Input/output gains
        result &= m_ControlContainer->addElement(
            new RmeSettingsMatrixCtrl(*this, RME_MATRIXCTRL_GAINS, "Gains"));
    }

    if (!result) {
        debugWarning("One or more device control elements could not be created\n");
        destroyMixer();
        return false;
    }

    if (!addElement(m_ControlContainer)) {
        debugWarning("Could not register mixer to device\n");
        // clean up
        destroyMixer();
        return false;
    }

    return true;
}

bool
Device::destroyMixer() {
    bool ret = true;
    debugOutput(DEBUG_LEVEL_VERBOSE, "destroy mixer...\n");

    if (m_MixerContainer == NULL) {
        debugOutput(DEBUG_LEVEL_VERBOSE, "no mixer to destroy...\n");
    } else
    if (!deleteElement(m_MixerContainer)) {
        debugError("Mixer present but not registered to the avdevice\n");
        ret = false;
    } else {
        // remove and delete (as in free) child control elements
        m_MixerContainer->clearElements(true);
        delete m_MixerContainer;
        m_MixerContainer = NULL;
    }

    // remove control container
    if (m_ControlContainer == NULL) {
        debugOutput(DEBUG_LEVEL_VERBOSE, "no controls to destroy...\n");
    } else
    if (!deleteElement(m_ControlContainer)) {
        debugError("Controls present but not registered to the avdevice\n");
        ret = false;
    } else {
        // remove and delete (as in free) child control elements
        m_ControlContainer->clearElements(true);
        delete m_ControlContainer;
        m_ControlContainer = NULL;
    }

    return false;
}

bool
Device::probe( Util::Configuration& c, ConfigRom& configRom, bool generic )
{
    if (generic) {
        return false;
    } else {
        // check if device is in supported devices list.  Note that the RME
        // devices use the unit version to identify the individual devices. 
        // To avoid having to extend the configuration file syntax to
        // include this at this point, we'll use the configuration file
        // model ID to test against the device unit version.  This can be
        // tidied up if the configuration file is extended at some point to
        // include the unit version.
        unsigned int vendorId = configRom.getNodeVendorId();
        unsigned int unitVersion = configRom.getUnitVersion();

        Util::Configuration::VendorModelEntry vme = c.findDeviceVME( vendorId, unitVersion );
        return c.isValid(vme) && vme.driver == Util::Configuration::eD_RME;
    }
}

FFADODevice *
Device::createDevice(DeviceManager& d, std::auto_ptr<ConfigRom>( configRom ))
{
    return new Device(d, configRom );
}

bool
Device::discover()
{
    signed int i;
    unsigned int vendorId = getConfigRom().getNodeVendorId();
    // See note in Device::probe() about why we use the unit version here.
    unsigned int unitVersion = getConfigRom().getUnitVersion();

    Util::Configuration &c = getDeviceManager().getConfiguration();
    Util::Configuration::VendorModelEntry vme = c.findDeviceVME( vendorId, unitVersion );

    if (c.isValid(vme) && vme.driver == Util::Configuration::eD_RME) {
        debugOutput( DEBUG_LEVEL_VERBOSE, "found %s %s\n",
                     vme.vendor_name.c_str(),
                     vme.model_name.c_str());
    } else {
        debugWarning("Device '%s %s' unsupported by RME driver (no generic RME support)\n",
                     getConfigRom().getVendorName().c_str(), getConfigRom().getModelName().c_str());
    }

    if (unitVersion == RME_UNITVERSION_FF800) {
        m_rme_model = RME_MODEL_FIREFACE800;
    } else
    if (unitVersion == RME_MODEL_FIREFACE400) {
        m_rme_model = RME_MODEL_FIREFACE400;
    } else {
        debugError("Unsupported model\n");
        return false;
    }

    // Set up the shared data object for configuration data
    i = rme_shm_open(&dev_config);
    if (i == RSO_OPEN_CREATED) {
        debugOutput( DEBUG_LEVEL_VERBOSE, "New configuration shared data object created\n");
    } else
    if (i == RSO_OPEN_ATTACHED) {
        debugOutput( DEBUG_LEVEL_VERBOSE, "Attached to existing configuration shared data object\n");
    }
    if (dev_config == NULL) {
        debugOutput( DEBUG_LEVEL_WARNING, "Could not create/access shared configuration memory object, using process-local storage\n");
        memset(&local_dev_config_obj, 0, sizeof(local_dev_config_obj));
        dev_config = &local_dev_config_obj;
    }
    settings = &dev_config->settings;
    tco_settings = &dev_config->tco_settings;

    // If device is FF800, check to see if the TCO is fitted
    if (m_rme_model == RME_MODEL_FIREFACE800) {
        dev_config->tco_present = (read_tco(NULL, 0) == 0);
    }
    debugOutput(DEBUG_LEVEL_VERBOSE, "TCO present: %s\n",
      dev_config->tco_present?"yes":"no");

    init_hardware();

    if (!buildMixer()) {
        debugWarning("Could not build mixer\n");
    }

    // This is just for testing
    read_device_flash_settings(NULL);

    return true;
}

int
Device::getSamplingFrequency( ) {

    // Retrieve the current sample rate.  For practical purposes this
    // is the software rate currently in use.
    return dev_config->software_freq;
}

int
Device::getConfigurationId()
{
    return 0;
}

bool
Device::setDDSFrequency( int dds_freq )
{
    // Set a fixed DDS frequency.  If the device is the clock master this
    // will immediately be copied to the hardware DDS register.  Otherwise
    // it will take effect as required at the time the sampling rate is 
    // changed or streaming is started.

    // If the device is streaming, the new DDS rate must have the same
    // multiplier as the software sample rate
    if (hardware_is_streaming()) {
        if (multiplier_of_freq(dds_freq) != multiplier_of_freq(dev_config->software_freq))
            return false;
    }

    dev_config->dds_freq = dds_freq;
    if (settings->clock_mode == FF_STATE_CLOCKMODE_MASTER) {
        if (set_hardware_dds_freq(dds_freq) != 0)
            return false;
    }

    return true;
}

bool
Device::setSamplingFrequency( int samplingFrequency )
{
    // Request a sampling rate on behalf of software.  Software is limited
    // to sample rates of 32k, 44.1k, 48k and the 2x/4x multiples of these. 
    // The user may lock the device to a much wider range of frequencies via
    // the explicit DDS controls in the control panel.  If the explicit DDS
    // control is active the software is limited to the "standard" speeds
    // corresponding to the multiplier in use by the DDS.
    //
    // Similarly, if the device is externally clocked the software is 
    // limited to the external clock frequency.
    //
    // Otherwise the software has free choice of the software speeds noted
    // above.

    bool ret = -1;
    signed int i, j;
    signed int mult[3] = {1, 2, 4};
    signed int base_freq[3] = {32000, 44100, 48000};
    signed int freq = samplingFrequency;
    FF_state_t state;
    signed int fixed_freq = 0;

    get_hardware_state(&state);

    // If device is locked to a frequency via external clock, explicit
    // setting of the DDS or by virtue of streaming being active, get that
    // frequency.
    if (state.clock_mode == FF_STATE_CLOCKMODE_AUTOSYNC) {
        // FIXME: if synced to TCO, is autosync_freq valid?
        fixed_freq = state.autosync_freq;
    } else
    if (dev_config->dds_freq > 0) {
        fixed_freq = dev_config->dds_freq;
    } else
    if (hardware_is_streaming()) {
        fixed_freq = dev_config->software_freq;
    }

    // If the device is running to a fixed frequency, software can only
    // request frequencies with the same multiplier.  Similarly, the
    // multiplier is locked in "master" clock mode if the device is
    // streaming.
    if (fixed_freq > 0) {
        signed int fixed_mult = multiplier_of_freq(fixed_freq);
        if (multiplier_of_freq(freq) != multiplier_of_freq(fixed_freq))
            return -1;
        for (j=0; j<3; j++) {
            if (freq == base_freq[j]*fixed_mult) {
                ret = 0;
                break;
            }
        }
    } else {
        for (i=0; i<3; i++) {
            for (j=0; j<3; j++) {
                if (freq == base_freq[j]*mult[i]) {
                    ret = 0;
                    break;
                }
            }
        }
    }
    // If requested frequency is unavailable, return -1
    if (ret == -1)
        return false;

    // If a DDS frequency has been explicitly requested this is always
    // used to programm the hardware DDS regardless of the rate requested
    // by the software.  Otherwise we use the requested sampling rate.
    if (dev_config->dds_freq > 0)
        freq = dev_config->dds_freq;
    if (set_hardware_dds_freq(freq) != 0)
        return false;

    dev_config->software_freq = samplingFrequency;
    return true;
}

std::vector<int>
Device::getSupportedSamplingFrequencies()
{
    std::vector<int> frequencies;
    signed int i, j;
    signed int mult[3] = {1, 2, 4};
    signed int freq[3] = {32000, 44100, 48000};
    FF_state_t state;

    get_hardware_state(&state);

    // Generate the list of supported frequencies.  If the device is
    // externally clocked the frequency is limited to the external clock
    // frequency.  If the device is running the multiplier is fixed.
    if (state.clock_mode == FF_STATE_CLOCKMODE_AUTOSYNC) {
        // FIXME: if synced to TCO, is autosync_freq valid?
        frequencies.push_back(state.autosync_freq);
    } else
    if (state.is_streaming) {
        unsigned int fixed_mult = multiplier_of_freq(dev_config->software_freq);
        for (j=0; j<3; j++) {
            frequencies.push_back(freq[j]*fixed_mult);
        }
    } else {
        for (i=0; i<3; i++) {
            for (j=0; j<3; j++) {
                frequencies.push_back(freq[j]*mult[i]);
            }
        }
    }
    return frequencies;
}

FFADODevice::ClockSourceVector
Device::getSupportedClockSources() {
    FFADODevice::ClockSourceVector r;
    return r;
}

bool
Device::setActiveClockSource(ClockSource s) {
    return false;
}

FFADODevice::ClockSource
Device::getActiveClockSource() {
    ClockSource s;
    return s;
}

bool
Device::lock() {

    return true;
}


bool
Device::unlock() {

    return true;
}

void
Device::showDevice()
{
    unsigned int vendorId = getConfigRom().getNodeVendorId();
    unsigned int modelId = getConfigRom().getModelId();

    Util::Configuration &c = getDeviceManager().getConfiguration();
    Util::Configuration::VendorModelEntry vme = c.findDeviceVME( vendorId, modelId );

    debugOutput(DEBUG_LEVEL_VERBOSE,
        "%s %s at node %d\n", vme.vendor_name.c_str(), vme.model_name.c_str(), getNodeId());
}

bool
Device::prepare() {

    signed int mult, bandwidth;
    signed int freq;
    signed int err = 0;

    debugOutput(DEBUG_LEVEL_NORMAL, "Preparing Device...\n" );

    freq = getSamplingFrequency();
    if (freq <= 0) {
        debugOutput(DEBUG_LEVEL_ERROR, "Can't continue: sampling frequency not set\n");
        return false;
    }

    // The number of frames transmitted in a single packet is solely
    // determined by the sample rate.
    mult = multiplier_of_freq(getSamplingFrequency());
    switch (mult) {
        case 2: frames_per_packet = 15; break;
        case 4: frames_per_packet = 25; break;
    default:
        frames_per_packet = 7;
    }

    // The number of active channels depends on sample rate and whether
    // bandwidth limitation is active.  First set up the number of analog
    // channels (which differs between devices), then add SPDIF channels if
    // relevant.  Finally, the number of channels available from each ADAT
    // interface depends on sample rate: 0 at 4x, 4 at 2x and 8 at 1x.
    if (m_rme_model == RME_MODEL_FIREFACE800)
        num_channels = 10;
    else
        num_channels = 8;
    if (settings->limit_bandwidth != FF_SWPARAM_BWLIMIT_ANALOG_ONLY)
        num_channels += 2;
    if (settings->limit_bandwidth==FF_SWPARAM_BWLIMIT_NO_ADAT2 ||
        settings->limit_bandwidth==FF_SWPARAM_BWLIMIT_SEND_ALL_CHANNELS)
        num_channels += (mult==4?0:(mult==2?4:8));
    if (settings->limit_bandwidth==FF_SWPARAM_BWLIMIT_SEND_ALL_CHANNELS)
        num_channels += (mult==4?0:(mult==2?4:8));

    // Bandwidth is calculated here.  For the moment we assume the device 
    // is connected at S400, so 1 allocation unit is 1 transmitted byte.
    // There is 25 allocation units of protocol overhead per packet.  Each
    // channel of audio data is sent/received as a 32 bit integer.
    bandwidth = 25 + num_channels*4*frames_per_packet;

    // Both the FF400 and FF800 require we allocate a tx iso channel.  The
    // rx channel is also allocated for the FF400.  The FF800 chooses
    // the rx channel to be used but does not handle the bus-level
    // channel/bandwidth allocation/
    if (iso_tx_channel < 0) {
        iso_tx_channel = get1394Service().allocateIsoChannelGeneric(bandwidth);
    }

    if (iso_rx_channel < 0) {
        if (m_rme_model == RME_MODEL_FIREFACE800) {
            unsigned int stat[4];
            get_hardware_streaming_status(stat, 4);
            // CHECKME: does this work before streaming has been initialised?
            iso_rx_channel = stat[2] & 63;
            iso_rx_channel = get1394Service().allocateFixedIsoChannelGeneric(iso_rx_channel, bandwidth);
        } else {
            iso_rx_channel = get1394Service().allocateIsoChannelGeneric(bandwidth);
        }
    }
  
    if (iso_tx_channel>=0 && iso_rx_channel>=0) {
        err = hardware_init_streaming(dev_config->hardware_freq, iso_tx_channel) != 0;
        if (err) {
            debugFatal("Could not intialise device streaming system\n");
        }
    } else {
        err = 1;
        debugFatal("Could not allocate iso channels\n");
    }

    if (err) {
        if (iso_tx_channel >= 0) 
            get1394Service().freeIsoChannel(iso_tx_channel);
        if (iso_rx_channel >= 0)
            get1394Service().freeIsoChannel(iso_rx_channel);
        return false;
    }

    // get the device specific and/or global SP configuration
    Util::Configuration &config = getDeviceManager().getConfiguration();
    // base value is the config.h value
    float recv_sp_dll_bw = STREAMPROCESSOR_DLL_BW_HZ;
    float xmit_sp_dll_bw = STREAMPROCESSOR_DLL_BW_HZ;

    // we can override that globally
    config.getValueForSetting("streaming.spm.recv_sp_dll_bw", recv_sp_dll_bw);
    config.getValueForSetting("streaming.spm.xmit_sp_dll_bw", xmit_sp_dll_bw);

    // or override in the device section
    config.getValueForDeviceSetting(getConfigRom().getNodeVendorId(), getConfigRom().getModelId(), "recv_sp_dll_bw", recv_sp_dll_bw);
    config.getValueForDeviceSetting(getConfigRom().getNodeVendorId(), getConfigRom().getModelId(), "xmit_sp_dll_bw", xmit_sp_dll_bw);

    // Other things to be done:
    //  * create a receive stream processor, set DLL bandwidth, add ports
    //  * create a transmit stream processor, set DLL bandwidth, add ports

    return true;
}

int
Device::getStreamCount() {
 	return 0; // one receive, one transmit
}

Streaming::StreamProcessor *
Device::getStreamProcessorByIndex(int i) {
    return NULL;
}

bool
Device::startStreamByIndex(int i) {
    // The RME does not allow separate enabling of the transmit and receive
    // streams.  Therefore we start all streaming when index 0 is referenced
    // and silently ignore the start requests for other streams
    // (unconditionally flagging them as being successful).
    if (i == 0) {
        if (hardware_start_streaming(iso_rx_channel) != 0)
            return false;
    }
    return true;
}

bool
Device::stopStreamByIndex(int i) {
    // See comments in startStreamByIndex() as to why we act only when stream
    // 0 is requested.
    if (i == 0) {
        if (hardware_stop_streaming() != 0)
            return false;
    }
    return true;
}

unsigned int 
Device::readRegister(fb_nodeaddr_t reg) {

    quadlet_t quadlet;
    
    quadlet = 0;
    if (get1394Service().read(0xffc0 | getNodeId(), reg, 1, &quadlet) <= 0) {
        debugError("Error doing RME read from register 0x%06x\n",reg);
    }
    return ByteSwapFromDevice32(quadlet);
}

signed int 
Device::readBlock(fb_nodeaddr_t reg, quadlet_t *buf, unsigned int n_quads) {

    unsigned int i;

    if (get1394Service().read(0xffc0 | getNodeId(), reg, n_quads, buf) <= 0) {
        debugError("Error doing RME block read of %d quadlets from register 0x%06x\n",
            n_quads, reg);
        return -1;
    }
    for (i=0; i<n_quads; i++) {
       buf[i] = ByteSwapFromDevice32(buf[i]);
    }

    return 0;
}

signed int
Device::writeRegister(fb_nodeaddr_t reg, quadlet_t data) {

    unsigned int err = 0;
    data = ByteSwapToDevice32(data);
    if (get1394Service().write(0xffc0 | getNodeId(), reg, 1, &data) <= 0) {
        err = 1;
        debugError("Error doing RME write to register 0x%06x\n",reg);
    }
    return (err==0)?0:-1;
}

signed int
Device::writeBlock(fb_nodeaddr_t reg, quadlet_t *data, unsigned int n_quads) {
//
// Write a block of data to the device starting at address "reg".  Note that
// the conditional byteswap is done "in place" on data, so the contents of
// data may be modified by calling this function.
//
    unsigned int err = 0;
    unsigned int i;

    for (i=0; i<n_quads; i++) {
      data[i] = ByteSwapToDevice32(data[i]);
    }
    if (get1394Service().write(0xffc0 | getNodeId(), reg, n_quads, data) <= 0) {
        err = 1;
        debugError("Error doing RME block write of %d quadlets to register 0x%06x\n",
          n_quads, reg);
    }
    return (err==0)?0:-1;
}
                  
}
