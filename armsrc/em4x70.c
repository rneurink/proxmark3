//-----------------------------------------------------------------------------
// Copyright (C) 2020 sirloins based on em4x50
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Low frequency EM4170 commands
//-----------------------------------------------------------------------------

#include "fpgaloader.h"
#include "ticks.h"
#include "dbprint.h"
#include "lfadc.h"
#include "commonutil.h"
#include "em4x70.h"
#include "appmain.h" // tear

static em4x70_tag_t tag = { 0 };

// EM4170 requires a parity bit on commands, other variants do not.
static bool command_parity = true;

#define EM4X70_T_TAG_QUARTER_PERIOD         8
#define EM4X70_T_TAG_HALF_PERIOD            16
#define EM4X70_T_TAG_THREE_QUARTER_PERIOD   24
#define EM4X70_T_TAG_FULL_PERIOD            32
#define EM4X70_T_TAG_TWA                   128 // Write Access Time
#define EM4X70_T_TAG_DIV                   224 // Divergency Time
#define EM4X70_T_TAG_AUTH                 4224 // Authentication Time
#define EM4X70_T_TAG_WEE                  3072 // EEPROM write Time
#define EM4X70_T_TAG_TWALB                 672 // Write Access Time of Lock Bits

#define EM4X70_T_WAITING_FOR_SNGLLIW       160   // Unsure

#define TICKS_PER_FC                        12 // 1 fc = 8us, 1.5us per tick = 12 ticks
#define EM4X70_MIN_AMPLITUDE                10 // Minimum difference between a high and low signal

#define EM4X70_TAG_TOLERANCE                 8
#define EM4X70_TAG_WORD                     48

#define EM4X70_COMMAND_RETRIES               5 // Attempts to send/read command
#define EM4X70_MAX_RECEIVE_LENGTH           96 // Maximum bits to expect from any command


/**
 * These IDs are from the EM4170 datasheet
 * Some versions of the chip require a
 * (even) parity bit, others do not
 */
#define EM4X70_COMMAND_ID                   0x01
#define EM4X70_COMMAND_UM1                  0x02
#define EM4X70_COMMAND_AUTH                 0x03
#define EM4X70_COMMAND_PIN                  0x04
#define EM4X70_COMMAND_WRITE                0x05
#define EM4X70_COMMAND_UM2                  0x07

static uint8_t gHigh = 0;
static uint8_t gLow  = 0;

#define IS_HIGH(sample) (sample>gLow ? true : false)
#define IS_LOW(sample) (sample<gHigh ? true : false)
#define IS_TIMEOUT(timeout_ticks) (GetTicks() > timeout_ticks)
#define TICKS_ELAPSED(start_ticks) (GetTicks() - start_ticks)


static uint8_t bits2byte(const uint8_t *bits, int length);
static void bits2bytes(const uint8_t *bits, int length, uint8_t *out);
static int em4x70_receive(uint8_t *bits);
static bool find_listen_window(bool command);

static void init_tag(void) {
    memset(tag.data, 0x00, sizeof(tag.data)/sizeof(tag.data[0]));
}

static void EM4170_setup_read(void) {

    FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_ADC | FPGA_LF_ADC_READER_FIELD);

    // 50ms for the resonant antenna to settle.
    SpinDelay(50);

    // Now set up the SSC to get the ADC samples that are now streaming at us.
    FpgaSetupSsc(FPGA_MAJOR_MODE_LF_READER);

    FpgaSendCommand(FPGA_CMD_SET_DIVISOR, LF_DIVISOR_125);

    // Connect the A/D to the peak-detected low-frequency path.
    SetAdcMuxFor(GPIO_MUXSEL_LOPKD);

    // Steal this pin from the SSP (SPI communication channel with fpga) and
    // use it to control the modulation
    AT91C_BASE_PIOA->PIO_PER = GPIO_SSC_DOUT;
    AT91C_BASE_PIOA->PIO_OER = GPIO_SSC_DOUT;

    // Disable modulation at default, which means enable the field
    LOW(GPIO_SSC_DOUT);

    // Start the timer
    StartTicks();

    // Watchdog hit
    WDT_HIT();
}

static bool get_signalproperties(void) {

    // calculate signal properties (mean amplitudes) from measured data:
    // 32 amplitudes (maximum values) -> mean amplitude value -> gHigh -> gLow
    bool signal_found = false;
    int no_periods = 32, pct = 50, noise = 140; // pct originally 75, found 50 was working better for me
    uint8_t sample_ref = 127;
    uint8_t sample_max_mean = 0;
    uint8_t sample_max[no_periods];
    uint32_t sample_max_sum = 0;
    
    memset(sample_max, 0x00, sizeof(sample_max));

    // wait until signal/noise > 1 (max. 32 periods)
    for (int i = 0; i < TICKS_PER_FC * EM4X70_T_TAG_FULL_PERIOD * no_periods; i++) {

        // about 2 samples per bit period
        WaitTicks(TICKS_PER_FC * EM4X70_T_TAG_HALF_PERIOD);

        if (AT91C_BASE_SSC->SSC_RHR > noise) {
            signal_found = true;
            break;
        }

    }

    if (signal_found == false)
        return false;

    // calculate mean maximum value of 32 periods, each period has a length of
    // 3 single "full periods" to eliminate the influence of a listen window
    for (int i = 0; i < no_periods; i++) {

        uint32_t start_ticks = GetTicks();
        //AT91C_BASE_TC0->TC_CCR = AT91C_TC_SWTRG;
        while (TICKS_ELAPSED(start_ticks) < TICKS_PER_FC * 3 * EM4X70_T_TAG_FULL_PERIOD) {

            volatile uint8_t sample = (uint8_t)AT91C_BASE_SSC->SSC_RHR;

            if (sample > sample_max[i])
                sample_max[i] = sample;

        }

        sample_max_sum += sample_max[i];
    }

    sample_max_mean = sample_max_sum / no_periods;

    // set global envelope variables
    gHigh = sample_ref + pct * (sample_max_mean - sample_ref) / 100;
    gLow  = sample_ref - pct * (sample_max_mean - sample_ref) / 100;

    // Basic sanity check
    if(gHigh - gLow < EM4X70_MIN_AMPLITUDE) {
        return false;
    }

    Dbprintf("%s: gHigh %d gLow: %d", __func__, gHigh, gLow);
    return true;
}

/**
 *  get_pulse_length
 * 
 *      Times falling edge pulses
 */ 
static uint32_t get_pulse_length(void) {

    uint8_t sample;
    uint32_t timeout = GetTicks() + (TICKS_PER_FC * 3 * EM4X70_T_TAG_FULL_PERIOD);

    do {
        sample = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
    }while (IS_HIGH(sample) && !IS_TIMEOUT(timeout));

    if (IS_TIMEOUT(timeout))
        return 0;

    uint32_t start_ticks = GetTicks();
    timeout = start_ticks + (TICKS_PER_FC * 3 * EM4X70_T_TAG_FULL_PERIOD);

    do {
        sample = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
    }while (IS_LOW(sample) && !IS_TIMEOUT(timeout));

    if (IS_TIMEOUT(timeout))
        return 0;

    timeout = (TICKS_PER_FC * 3 * EM4X70_T_TAG_FULL_PERIOD) + GetTicks();
    do {
        sample = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
    }while (IS_HIGH(sample) && !IS_TIMEOUT(timeout));

    if (IS_TIMEOUT(timeout))
        return 0;

    return TICKS_ELAPSED(start_ticks);
}

/**
 *  get_pulse_invert_length
 * 
 *      Times rising edge pules
 *  TODO: convert to single function with get_pulse_length()
 */ 
static uint32_t get_pulse_invert_length(void) {

    uint8_t sample;
    uint32_t timeout = GetTicks() + (TICKS_PER_FC * 3 * EM4X70_T_TAG_FULL_PERIOD);

    do {
        sample = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
    }while (IS_LOW(sample) && !IS_TIMEOUT(timeout));

    if (IS_TIMEOUT(timeout))
        return 0;

    uint32_t start_ticks = GetTicks();
    timeout = start_ticks + (TICKS_PER_FC * 3 * EM4X70_T_TAG_FULL_PERIOD);

    do {
        sample = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
    }while (IS_HIGH(sample) && !IS_TIMEOUT(timeout));

    if (IS_TIMEOUT(timeout))
        return 0;

    timeout = GetTicks() + (TICKS_PER_FC * 3 * EM4X70_T_TAG_FULL_PERIOD);
    do {
        sample = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
    }while (IS_LOW(sample) && !IS_TIMEOUT(timeout));

    if (IS_TIMEOUT(timeout))
        return 0;

    return TICKS_ELAPSED(start_ticks);

}

static bool check_pulse_length(uint32_t pl, int length) {
    // check if pulse length <pl> corresponds to given length <length>
    return ((pl >= TICKS_PER_FC * (length - EM4X70_TAG_TOLERANCE)) & (pl <= TICKS_PER_FC * (length + EM4X70_TAG_TOLERANCE)));
}

static void em4x70_send_bit(bool bit) {

    // send single bit according to EM4170 application note and datasheet
    uint32_t start_ticks = GetTicks();

    if (bit == 0) {

        // disable modulation (drop the field) for 4 cycles of carrier
        LOW(GPIO_SSC_DOUT);
        while (TICKS_ELAPSED(start_ticks) <= TICKS_PER_FC * 4);

        // enable modulation (activates the field) for remaining first
        // half of bit period
        HIGH(GPIO_SSC_DOUT);
        while (TICKS_ELAPSED(start_ticks) <= TICKS_PER_FC * EM4X70_T_TAG_HALF_PERIOD);

        // disable modulation for second half of bit period
        LOW(GPIO_SSC_DOUT);
        while (TICKS_ELAPSED(start_ticks) <= TICKS_PER_FC * EM4X70_T_TAG_FULL_PERIOD);

    } else {

        // bit = "1" means disable modulation for full bit period
        LOW(GPIO_SSC_DOUT);
        while (TICKS_ELAPSED(start_ticks) <= TICKS_PER_FC * EM4X70_T_TAG_FULL_PERIOD);
    }
}

/**
 * em4x70_send_nibble
 * 
 *  sends 4 bits of data + 1 bit of parity (with_parity)
 * 
 */
static void em4x70_send_nibble(uint8_t nibble, bool with_parity) {
    int parity = 0;
    int msb_bit = 0;

    // Non automotive EM4x70 based tags are 3 bits + 1 parity.
    // So drop the MSB and send a parity bit instead after the command
    if(command_parity)
        msb_bit = 1;
    
    for (int i = msb_bit; i < 4; i++) {
        int bit = (nibble >> (3 - i)) & 1;
        em4x70_send_bit(bit);
        parity ^= bit;
    }

    if(with_parity)
        em4x70_send_bit(parity);
}

static void em4x70_send_word(const uint16_t word) {

    // Split into nibbles
    uint8_t nibbles[4];
    uint8_t j = 0;
    for(int i = 0; i < 2; i++) {
        uint8_t byte = (word >> (8*i)) & 0xff;
        nibbles[j++] = (byte >> 4) & 0xf;
        nibbles[j++] = byte & 0xf;
    }

    // send 16 bit word with parity bits according to EM4x70 datasheet
    // sent as 4 x nibbles (4 bits + parity)
    for (int i = 0; i < 4; i++) {
        em4x70_send_nibble(nibbles[i], true);
    }

    // send column parities (4 bit)
    em4x70_send_nibble(nibbles[0] ^ nibbles[1] ^ nibbles[2] ^ nibbles[3], false);

    // send final stop bit (always "0")
    em4x70_send_bit(0);
}

static bool check_ack(void) {

    // returns true if signal structue corresponds to ACK, anything else is
    // counted as NAK (-> false)
    uint32_t start_ticks = GetTicks();
    while (TICKS_ELAPSED(start_ticks) < TICKS_PER_FC * 4 * EM4X70_T_TAG_FULL_PERIOD) {
        /*
            ACK
              64 (48+16)
              64 (48+16)
            NACK
              64 (48+16)
              48 (32+16)
        */
        if (check_pulse_length(get_pulse_length(), 2 * EM4X70_T_TAG_FULL_PERIOD)) {

            // The received signal is either ACK or NAK.
            if (check_pulse_length(get_pulse_length(), 2 * EM4X70_T_TAG_FULL_PERIOD)) {
                return true;
            } else {
                // It's NAK -> stop searching
                break;
            }
        }
    }
    return false;
}

static int write(const uint16_t word, const uint8_t address) {

    // writes <word> to specified <address>
    if (find_listen_window(true)) {

        // send write command
        em4x70_send_nibble(EM4X70_COMMAND_WRITE, true);

        // send address data with parity bit
        em4x70_send_nibble(address, true);

        // send data word
        em4x70_send_word(word);

        // Wait TWA 
        WaitTicks(TICKS_PER_FC * EM4X70_T_TAG_TWA);

        // look for ACK sequence
        if (check_ack()) {

            // now EM4x70 needs T0 * EM4X70_T_TAG_TWEE (EEPROM write time)
            // for saving data and should return with ACK
            WaitTicks(TICKS_PER_FC * EM4X70_T_TAG_WEE);
            if (check_ack()) {

                return PM3_SUCCESS;
            }
        }
    }
    return PM3_ESOFT;
}


static bool find_listen_window(bool command) {
    
    int cnt = 0;
    while(cnt < EM4X70_T_WAITING_FOR_SNGLLIW) {
        /*
        80 ( 64 + 16 )
        80 ( 64 + 16 )
        Flip Polarity
        96 ( 64 + 32 )
        64 ( 32 + 16 +16 )*/

        if ( check_pulse_length(get_pulse_invert_length(), 80) &&
             check_pulse_length(get_pulse_invert_length(), 80) &&
             check_pulse_length(get_pulse_length(), 96) &&
             check_pulse_length(get_pulse_length(), 64) )
            {

                if(command) {
                    /* Here we are after the 64 duration edge.
                        *   em4170 says we need to wait about 48 RF clock cycles.
                        *   depends on the delay between tag and us
                        * 
                        *   I've found between 4-5 quarter periods (32-40) works best
                        */
                    WaitTicks(TICKS_PER_FC * 4 * EM4X70_T_TAG_QUARTER_PERIOD);
                    // Send RM Command
                    em4x70_send_bit(0);
                    em4x70_send_bit(0);
                }
                return true;
            }
        cnt++;
    }

    return false;
}

static void bits2bytes(const uint8_t *bits, int length, uint8_t *out) {
    
    if(length%8 != 0) {
        Dbprintf("Should have a multiple of 8 bits, was sent %d", length);
    }
    
    int num_bytes = length / 8; // We should have a multiple of 8 here

    for(int i=1; i <= num_bytes; i++) {
        out[num_bytes-i] = bits2byte(bits, 8);
        bits += 8;
    } 
}

static uint8_t bits2byte(const uint8_t *bits, int length) {

    // converts <length> separate bits into a single "byte"
    uint8_t byte = 0;
    for (int i = 0; i < length; i++) {

        byte |= bits[i];

        if (i != length - 1)
            byte <<= 1;
    }

    return byte;
}

static bool send_command_and_read(uint8_t command, uint8_t resp_len_bits, uint8_t *out_bytes) {
    
    int retries = EM4X70_COMMAND_RETRIES;
    while(retries) {
        retries--;

        if(find_listen_window(true)) {
            uint8_t bits[EM4X70_MAX_RECEIVE_LENGTH] = {0};
            em4x70_send_nibble(command, command_parity);
            int len = em4x70_receive(bits);
            if(len < resp_len_bits) {
                Dbprintf("Invalid data received length: %d", len);
                return false;
            }
            bits2bytes(bits, len, out_bytes);
            return true;
        }
    }
    return false;
}



/**
 * em4x70_read_id
 * 
 *  read pre-programmed ID (4 bytes)
 */ 
static bool em4x70_read_id(void) {

    return send_command_and_read(EM4X70_COMMAND_ID, 32, &tag.data[4]);

}

/**
 *  em4x70_read_um1
 * 
 *  read user memory 1 (4 bytes including lock bits)
 */
static bool em4x70_read_um1(void) {

    return send_command_and_read(EM4X70_COMMAND_UM1, 32, &tag.data[0]);

}


/**
 *  em4x70_read_um2
 * 
 *  read user memory 2 (8 bytes)
 */
static bool em4x70_read_um2(void) {

    return send_command_and_read(EM4X70_COMMAND_UM2, 64, &tag.data[24]);

}

static bool find_EM4X70_Tag(void) {
    // function is used to check wether a tag on the proxmark is an
    // EM4170 tag or not -> speed up "lf search" process
    return find_listen_window(false);
}

static int em4x70_receive(uint8_t *bits) {

    uint32_t pl;
    int bit_pos = 0;
    uint8_t edge = 0;
    bool foundheader = false;

    // Read out the header
    //   12 Manchester 1's (may miss some during settle period)
    //    4 Manchester 0's
    
    // Skip a few leading 1's as it could be noisy
    WaitTicks(TICKS_PER_FC * 3 * EM4X70_T_TAG_FULL_PERIOD);

    // wait until we get the transition from 1's to 0's which is 1.5 full windows
    int pulse_count = 0;
    while(pulse_count < 12){
        pl = get_pulse_invert_length();
        pulse_count++;
        if(check_pulse_length(pl, 3 * EM4X70_T_TAG_HALF_PERIOD)) {
            foundheader = true;
            break;
        }
    }

    if(!foundheader) {
        Dbprintf("Failed to find read header");
        return 0;
    }

    // Skip next 3 0's, header check consumes the first 0
    for(int i = 0; i < 3; i++) {
        get_pulse_invert_length();
    }

    // identify remaining bits based on pulse lengths
    // between two listen windows only pulse lengths of 1, 1.5 and 2 are possible
    while (bit_pos < EM4X70_MAX_RECEIVE_LENGTH) {

        if(edge)
            pl = get_pulse_length();
        else
            pl = get_pulse_invert_length();

        if (check_pulse_length(pl, EM4X70_T_TAG_FULL_PERIOD)) {

            // pulse length = 1
            bits[bit_pos++] = edge;

        } else if (check_pulse_length(pl, 3 * EM4X70_T_TAG_HALF_PERIOD)) {

            // pulse length = 1.5 -> flip edge detection
            if(edge) {
                bits[bit_pos++] = 0;
                bits[bit_pos++] = 0;
                edge = 0;
            } else {
                bits[bit_pos++] = 1;
                bits[bit_pos++] = 1;
                edge = 1;
            }

        } else if (check_pulse_length(pl, 2 * EM4X70_T_TAG_FULL_PERIOD)) {

            // pulse length of 2
            if(edge) {
                bits[bit_pos++] = 0;
                bits[bit_pos++] = 1;
            } else {
                bits[bit_pos++] = 1;
                bits[bit_pos++] = 0;
            }

        } else if ( (edge && check_pulse_length(pl, 3 * EM4X70_T_TAG_FULL_PERIOD)) ||
                    (!edge && check_pulse_length(pl, 80))) {

            // LIW detected (either invert or normal)
            return --bit_pos;
        }
    }

    // Should not get here
    return --bit_pos;
}

void em4x70_info(em4x70_data_t *etd) {

    uint8_t status = 0;
    
    // Support tags with and without command parity bits
    command_parity = etd->parity;

    init_tag();
    EM4170_setup_read();

    // Find the Tag
    if (get_signalproperties() && find_EM4X70_Tag()) {
        // Read ID, UM1 and UM2
        status = em4x70_read_id() && em4x70_read_um1() && em4x70_read_um2();
    }

    StopTicks();
    lf_finalize();
    reply_ng(CMD_LF_EM4X70_INFO, status, tag.data, sizeof(tag.data));
}

void em4x70_write(em4x70_data_t *etd) {

    uint8_t status = 0;

    command_parity = etd->parity;

    init_tag();
    EM4170_setup_read();

    // Find the Tag
    if (get_signalproperties() && find_EM4X70_Tag()) {
        
        // Write
        status = write(etd->word, etd->address) == PM3_SUCCESS;

        if(status) {
            // Read Tag after writing
            em4x70_read_id();
            em4x70_read_um1();
            em4x70_read_um2();
        }

    }

    StopTicks();
    lf_finalize();
    reply_ng(CMD_LF_EM4X70_WRITE, status, tag.data, sizeof(tag.data));
}

