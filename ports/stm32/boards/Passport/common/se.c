// SPDX-FileCopyrightText: 2020 Foundation Devices, Inc.  <hello@foundationdevices.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: 2018 Coinkite, Inc.  <coldcardwallet.com>
// SPDX-License-Identifier: GPL-3.0-only
//
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal_uart.h"
#include "stm32h7xx_hal_uart_ex.h"

#include "delay.h"
#include "pprng.h"
#include "sha256.h"
#include "secrets.h"
#include "se.h"
#include "se-config.h"
#include "utils.h"

#ifndef PASSPORT_BOOTLOADER
#include "lcd-sharp-ls018B7dh02.h"
#endif /* PASSPORT_BOOTLOADER */

/*
 * This bit should be define in the STM32H7 header files but it is not...
 * somehow was missed. It is a valid bit in the interrupt status register
 * so we'll define it here so as not to mess with the micropython HAL
 * installation.
 */
#define UART_FLAG_RTOF                      ((uint32_t)0x00000800U)

// "one wire" is on PA0 aka. UART4
#define MY_UART         UART4

#define STATS(x)

uint32_t crc_errors;
uint32_t not_ready_n;
uint32_t short_error;
uint32_t len_error;
uint32_t len_error_two;
uint32_t ln_retry;
uint32_t retry_out;
uint32_t wdgtimeout;

uint32_t rtof;
uint32_t rxne;
uint32_t notrxne;

const char *copyright_msg = "(C) 2020 Foundation Devices Inc.";

static seopcode_t current_opcode;

// Bit patterns to be sent
#define BIT0    0x7d
#define BIT1    0x7f

// These control the direction of the single wire bus
typedef enum {
    IOFLAG_CMD      = 0x77,
    IOFLAG_TX       = 0x88,
    IOFLAG_IDLE     = 0xBB,
    IOFLAG_SLEEP    = 0xCC,
} ioflag_t;

static inline void _send_byte(uint8_t ch)
{
    // reset timeout timer (Systick)
    uint32_t    ticks = 0;
    SysTick->VAL = 0;

    while (!(MY_UART->ISR & UART_FLAG_TXE)) {
        // busy-wait until able to send (no fifo?)
        if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) {
            // failsafe timeout
            ticks += 1;
            if(ticks > 10) break;
        }
    }
    MY_UART->TDR = ch;
}

static void _send_bits(uint8_t tx)
{
    // serialize and send one byte
    uint8_t     mask = 0x1;

    for (int i=0; i<8; i++, mask <<= 1) {
        uint8_t h = (tx & mask) ? BIT1 : BIT0;

        _send_byte(h);
    }
}

static void _send_serialized(const uint8_t *buf, int len)
{
    for (int i=0; i<len; i++) {
        _send_bits(buf[i]);
    }
}

// Return -1 in case of timeout, else one byte.
//
static inline int _read_byte(void)
{
    uint32_t    ticks = 0;

    // reset timeout timer (Systick)
    SysTick->VAL = 0;

    while (!(MY_UART->ISR & UART_FLAG_RXNE) && !(MY_UART->ISR & UART_FLAG_RTOF)) {
        // busy-waiting

        if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) {
            ticks += 1;
            if (ticks >= 5) {
                // a full Xms has been wasted; give up.

                // NOTE: this is a failsafe long timeout, not reached in
                // practise because the bit-time timeout from UART (RTOF)
                ++notrxne;
                return -1;
            }
        }
    }

    if (MY_UART->ISR & UART_FLAG_RXNE) {
        ++rxne;
        return MY_UART->RDR & 0x7f;
    }
    if (MY_UART->ISR & UART_FLAG_RTOF) {
        // "fast" timeout reached, clear flag
        ++rtof;
        MY_UART->ICR = USART_ICR_RTOCF;
        return -1;
    }
#ifdef FIXME
    INCONSISTENT("rxf");
#endif
    return -1;
}

// Return a deserialized byte, or -1 for timeout.
//
static void deserialize(const uint8_t *from, int from_len, uint8_t *into, int max_into)
{
    while (from_len > 0) {
        uint8_t rv = 0, mask = 0x1;

        for (int i=0; i<8; i++, mask <<= 1) {
#if 1
            if ((from[i] ^ 0x7F) < 2)
                rv |= mask;
#else
            if (from[i] == BIT1) {
                rv |= mask;
            }
#endif
        }

        *(into++) = rv;
        from += 8;
        from_len -= 8;

        max_into --;
        if (max_into <= 0) break;
    }
}

static inline void _flush_rx(void)
{
    // reset timeout timer (Systick)
    SysTick->VAL = 0;

    while (!(MY_UART->ISR & UART_FLAG_TC)) {
        // wait for last bit(byte) to be serialized and sent

        if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) {
            // full 1ms has passed -- timeout.
            break;
        }
    }

    // We actualy need this delay here!
    for (int i=0; i<48; i++) {
        __NOP();
    }

    // clear junk in rx buffer
    MY_UART->RQR = USART_RQR_RXFRQ;

    // clear overrun error
    // clear rx timeout flag
    // clear framing error
    MY_UART->ICR = USART_ICR_ORECF | USART_ICR_RTOCF | USART_ICR_FECF;
}

// Read upto N bytes of response. Suspress echo of 0x88 and
// return actual number of (deserialized) bytes received.
// We ignore extra bytes not expected, and always read until a timeout.
// Cmds to chip can be up to 155 bytes, but not clear what max len for responses.
//
static int se_read_response(uint8_t *buf, int max_len)
{
    int max_expect = (max_len+1) * 8;
    uint8_t raw[max_expect];

    // tell chip to write stuff to bus
    _send_bits(IOFLAG_TX);

    // kill first byte which we expect to be IOFLAG_TX echo (0x88)
    _flush_rx();

    // It takes between 64 and 131us (tTURNAROUND) for the chip to recover
    // and start sending bits to us. We're blocked on reading
    // them anyway, so no need to delay. Also a danger of overruns here.

    int actual = 0;
    for (uint8_t *p = raw; ; actual++) {
        int ch = _read_byte();
        if (ch < 0) {
            break;
        }

        if (actual < max_expect) {
            *(p++) = ch;
        }
    }

    // Sometimes our framing is not perfect.
    // We might get a spurious bit at the leading edge (perhaps an echo
    // of part of the 0x88??) or junk at the end.
    actual &= ~7;
    deserialize(raw, actual, buf, max_len);

    return actual / 8;
}

static bool check_crc(const uint8_t *data, uint8_t length)
{
    uint8_t obs[2] = { 0, 0 };

    if (data[0] != length) {
        // length is wrong
        return false;
    }

    se_crc16_chain(length-2, data, obs);

    return (obs[0] == data[length-2] && obs[1] == data[length-1]);
}

void se_write(seopcode_t opcode, uint8_t p1, uint16_t p2, uint8_t *data, uint8_t data_len)
{
    // all commands will have this fixed header, which includes just one layer of framing
    struct {
        uint8_t	ioflag;
        uint8_t	framed_len;
        uint8_t	op;
        uint8_t	p1;
        uint8_t	p2_lsb;
        uint8_t	p2_msb;
    } known = {
        .ioflag = IOFLAG_CMD,
        .framed_len = (data_len + 7),		// 7 = (1 len) + (4 bytes of msg) + (2 crc)
        .op = opcode,
        .p1 = p1,
        .p2_lsb = p2 & 0xff,
        .p2_msb = (p2 >> 8) & 0xff,
    };
#ifdef FIXME
    STATIC_ASSERT(sizeof(known) == 6);
#endif
    STATS(last_op = opcode);
    STATS(last_p1 = p1);
    STATS(last_p2 = p2);

    current_opcode = opcode;

    /*
     * Wake up the chip...
     * If it was in sleep mode it starts the watchdog.
     * If it was in idle mode it resumes the watchdog.
     */
    se_wake();

    _send_serialized((const uint8_t *)&known, sizeof(known));

    // CRC will start from frame_len onwards
    uint8_t crc[2] = {0, 0};
    se_crc16_chain(sizeof(known)-1, &known.framed_len, crc);

    // insert a variable-length body area (sometimes)
    if (data_len) {
        _send_serialized(data, data_len);
    
        se_crc16_chain(data_len, data, crc);
    }

    // send final CRC bytes
    _send_serialized(crc, 2);

#ifndef PASSPORT_BOOTLOADER
    lcd_show_busy_bar();
#endif /* PASSPORT_BOOTLOADER */
}

int se_read(uint8_t *data, uint8_t len)
{
    uint8_t tmp[1 + len + 2]; /* msg length + data length + checksum length */
    int retry;

    for (retry=100; retry >= 0; retry--) {
        int actual;

        actual = se_read_response(tmp, len+3);
        if (actual < 4) {
            if (actual == 0) {
                /* No data...probably still processing the command */
                ERR("not ready2");
                not_ready_n++;
            } else {
                // a weird short-read? probably fatal, but retry
                ERR("too short");
                short_error++;
            }
            goto try_again;
        }

        /*
         * The OP_Info response does not follow the normal response
         * format that includes a length and checksum. So we'll bypass
         * the length and checksum processing for the info command.
         */
        if (current_opcode != OP_Info)
        {
            uint8_t resp_len = tmp[0];
            if (resp_len != (len + 3)) {
                len_error++;
                if (resp_len == 4) {
                    /* Error code returned */
                    ERRV(tmp[1], "ae errcode");
                    len_error_two++;

                    if (tmp[1] == 0xEE)
                        wdgtimeout++;
                    return -1;
                }
                ERRV(tmp[0], "wr len");
                goto try_again;
            }

            if (!check_crc(tmp, actual)) {
                ERR("bad crc");
                crc_errors++;
                goto try_again;
            }
        }

        memcpy(data, tmp + 1, actual - 3);

        /* 
         * Pause the watchdog in case there's more to do
         * NOTE: Requires a wake commmand to resume!
         */
        _send_bits(IOFLAG_IDLE);

        return 0;

try_again:
        ln_retry++;
    }
    retry_out++;
    return -1;
}

int se_read1(void)
{
    int rc;
    uint8_t data;

    rc = se_read(&data, 1);
    if (rc < 0)
        return -1;
    return data;
}

void se_crc16_chain(uint8_t length, const uint8_t *data, uint8_t crc[2])
{
    uint8_t counter;
    uint16_t crc_register = 0;
    uint16_t polynom = 0x8005;
    uint8_t shift_register;
    uint8_t data_bit, crc_bit;

    crc_register = (((uint16_t) crc[0]) & 0x00FF) | (((uint16_t) crc[1]) << 8);

    for (counter = 0; counter < length; counter++) {
        for (shift_register = 0x01; shift_register > 0x00; shift_register <<= 1) {
            data_bit = (data[counter] & shift_register) ? 1 : 0;
            crc_bit = crc_register >> 15;

            // Shift CRC to the left by 1.
            crc_register <<= 1;

            if ((data_bit ^ crc_bit) != 0)
                crc_register ^= polynom;
        }
    }

    crc[0] = (uint8_t) (crc_register & 0x00FF);
    crc[1] = (uint8_t) (crc_register >> 8);
}

void se_sleep(void)
{
    _send_bits(IOFLAG_SLEEP);
}

void se_idle(void)
{
    _send_bits(IOFLAG_IDLE);
}

int se_wake(void)
{
    // send zero (all low), delay 2.5ms
    _send_byte(0x00);

#ifdef PASSPORT_BOOTLOADER
    delay_us(2500);
#else
    delay_us(100);
#endif

    return 0;
}

void se_keep_alive(void)
{
    se_idle();
}

void se_reset_chip(void)
{
    _send_bits(IOFLAG_SLEEP);
}

int se_config_read(uint8_t *config)
{
    int rc;
    int rval = 0;

    for (int blk=0; blk<4; blk++) {
        /* Read 32 bytes (aligned) from config "zone" */
        se_write(OP_Read, 0x80, blk<<3, NULL, 0);

        rc = se_read(&config[32 * blk], 32);
        if (rc < 0)
        {
            rval = -1;
            goto out;
        }
    }
out:
    _send_bits(IOFLAG_SLEEP);
    return rval;
}

// Load Tempkey with a nonce value that we both know, but
// is random and we both know is random! Tricky!
//
int se_pick_nonce(
    uint8_t *num_in,
    uint8_t *tempkey
)
{
    int rc;

    // We provide some 20 bytes of randomness to chip
    // The chip must provide 32-bytes of random-ness,
    // so no choice in args to OP.Nonce here (due to ReqRandom).
    se_write(OP_Nonce, 0, 0, num_in, 20);

    // Nonce command returns the RNG result, but not contents of TempKey
    uint8_t randout[32];
    rc = se_read(randout, 32);
    se_sleep();
    if (rc < 0)
        return -1;

    // Hash stuff appropriately to get same number as chip did.
    //  TempKey on the chip will be set to the output of SHA256 over
    //  a message composed of my challenge, the RNG and 3 bytes of constants:
    //
    //		return sha256(rndout + num_in + b'\x16\0\0').digest()
    //
    SHA256_CTX ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, randout, 32);
    sha256_update(&ctx, num_in, 20);
    const uint8_t fixed[3] = { 0x16, 0, 0 };
    sha256_update(&ctx, fixed, 3);
    sha256_final(&ctx, tempkey);

    return 0;
}

int se_gendig_slot(
    int slot_num,
    const uint8_t *slot_contents,
    uint8_t *digest
)
{
    // Construct a digest on the device (and here) that depends on the secret
    // contents of a specific slot.
    uint8_t num_in[20], tempkey[32];
    int rc;

    rng_buffer(num_in, sizeof(num_in));
    rc = se_pick_nonce(num_in, tempkey);
    if (rc < 0)
        return -1;

    //using Zone=2="Data" => "KeyID specifies a slot in the Data zone"
    se_write(OP_GenDig, 0x2, slot_num, NULL, 0);

    rc = se_read1();
    se_sleep();
    if (rc != 0)
        return -1;

    // we now have to match the digesting (hashing) that has happened on
    // the chip. No feedback at this point if it's right tho.
    //
    //   msg = hkey + b'\x15\x02' + ustruct.pack("<H", slot_num)
    //   msg += b'\xee\x01\x23' + (b'\0'*25) + challenge
    //   assert len(msg) == 32+1+1+2+1+2+25+32
    //
    SHA256_CTX ctx;
    sha256_init(&ctx);

    uint8_t args[7] = { OP_GenDig, 2, slot_num, 0, 0xEE, 0x01, 0x23 };
    uint8_t zeros[25] = { 0 };

    sha256_update(&ctx, slot_contents, 32);
    sha256_update(&ctx, args, sizeof(args));
    sha256_update(&ctx, zeros, sizeof(zeros));
    sha256_update(&ctx, tempkey, 32);

    sha256_final(&ctx, digest);

    return 0;
}

// Check that TempKey is holding what we think it does. Uses the MAC
// command over contents of Tempkey and our shared secret.
//
bool se_is_correct_tempkey(
    const uint8_t *expected_tempkey
)
{
    const uint8_t mode =   (1<<6)     // include full serial number
                         | (0<<2)     // TempKey.SourceFlag == 0 == 'rand'
                         | (0<<1)     // first 32 bytes are the shared secret
                         | (1<<0);    // second 32 bytes are tempkey
    uint8_t resp[32];
    int rc;

    se_write(OP_MAC, mode, KEYNUM_pairing, NULL, 0);
    rc = se_read(resp, 32);
    se_sleep();
    if (rc < 0)
        return false;

    // Duplicate the hash process, and then compare.
    SHA256_CTX ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, rom_secrets->pairing_secret, 32);
    sha256_update(&ctx, expected_tempkey, 32);

    const uint8_t fixed[16] = { OP_MAC, mode, KEYNUM_pairing, 0x0,
                                    0,0,0,0, 0,0,0,0,       // eight zeros
                                    0,0,0,                  // three zeros
                                    0xEE };
    sha256_update(&ctx, fixed, sizeof(fixed));
    sha256_update(&ctx, ((const uint8_t *)rom_secrets->se_serial_number)+4, 4);
    sha256_update(&ctx, ((const uint8_t *)rom_secrets->se_serial_number)+0, 4);

    uint8_t actual[32];
    sha256_final(&ctx, actual);

    return check_equal(actual, resp, 32);
}

// Do a dance that unlocks access to the private key for signing.
// Purpose is to show we are a pair of chips that belong together.
//
int se_pair_unlock()
{
    int rc;
    int attempts = 3;
    for (int i = 0; i < attempts; i++) {
        rc = se_checkmac(KEYNUM_pairing, rom_secrets->pairing_secret);
        if (rc == 0)
            return 0;
    }

    return -1;
}

// CAUTION: The result from this function could be modified by an
// active attacker on the bus because the one-byte response from the chip
// is easily replaced. This command is useful for us to authorize actions
// inside the 508a/608a, like use of a specific key, but not for us to
// authenticate the 508a/608a or its contents/state.
//
int se_checkmac(
    uint8_t keynum,
    const uint8_t *secret
)
{
    int rc;

    // Since this is part of the hash, we want random bytes
    // for our "other data". Also a number for "numin" of nonce
    uint8_t od[32], numin[20];

    rng_buffer(od, sizeof(od));
    rng_buffer(numin, sizeof(numin));

    // - load tempkey with a known nonce value
    uint8_t zeros[8] = {0};
    uint8_t tempkey[32];
    rc = se_pick_nonce(numin, tempkey);
    if (rc < 0)
        return -1;

    // - hash nonce and lots of other bits together
    SHA256_CTX ctx;
    sha256_init(&ctx);

    // shared secret is 32 bytes from flash
    sha256_update(&ctx, secret, 32);
    sha256_update(&ctx, tempkey, 32);
    sha256_update(&ctx, &od[0], 4);
    sha256_update(&ctx, zeros, 8);
    sha256_update(&ctx, &od[4], 3);

    uint8_t sn8 = 0xEE;
    sha256_update(&ctx, &sn8, 1);
    sha256_update(&ctx, &od[7], 4);

    uint8_t sn01[2] = { 0x01, 0x23 };
    sha256_update(&ctx, sn01, 2);
    sha256_update(&ctx, &od[11], 2);

    // format the request body
    struct {
        uint8_t		ch3[32];		// not actually used, but has to be there
        uint8_t		resp[32];
        uint8_t		od[13];
    } req;

    // content doesn't matter, but nice and visible:
    memcpy(req.ch3, copyright_msg, 32);

    sha256_final(&ctx, req.resp);
    memcpy(req.od, od, 13);

    // Give our answer to the chip.  The 0x01 means that TempKey holds
    // the second 32 byte value. First 32 byte value is in key slot 1 (pairing secret).
    se_write(OP_CheckMac, 0x01, keynum, (uint8_t *)&req, sizeof(req));
    rc = se_read1();
    se_sleep();
    if (rc != 0) {
        // did it work?! No.
        if (rc == SE_CHECKMAC_FAIL)
            ERR("CM fail");				// typical case: our hashs don't match
        else
            ERRV(rc, "CheckMac");
        return -2;
    }

    return 0;
}

// Check the chip produces a hash over various things the same way we would
// meaning that we both know the shared secret and the state of stuff in
// the 508a is what we expect.
//
int se_checkmac_hard(
    uint8_t keynum,
    const uint8_t *secret
)
{
    int rc;
    uint8_t     digest[32];

    rc = se_gendig_slot(keynum, secret, digest);
    if (rc < 0)
        return -1;

    // NOTE: we use this sometimes when we know the value is wrong, like
    // checking for blank pin codes... so not a huge error/security issue
    // if wrong here.
    if (!se_is_correct_tempkey(digest))
        return -2;

    return 0;
}

int se_encrypted_write32(
    int data_slot,
    int blk,
    int write_kn,
    const uint8_t *write_key,
    const uint8_t *data)
{
    int rc;
    uint8_t digest[32];

    rc = se_pair_unlock();
    if (rc < 0)
        return -1;

    // generate a hash over shared secret and rng
    rc = se_gendig_slot(write_kn, write_key, digest);
    if (rc < 0)
        return -1;

    // encrypt the data to be written, and append an authenticating MAC
    uint8_t body[32 + 32];

    for (int i=0; i<32; i++) {
        body[i] = data[i] ^ digest[i];
    }

    // make auth-mac to go with
    //	SHA-256(TempKey, Opcode, Param1, Param2, SN<8>, SN<0:1>, <25 bytes of zeros>, PlainTextData)
    //	msg = (dig
    //	    + ustruct.pack('<bbH', OP.Write, args['p1'], args['p2'])
    //	    + b'\xee\x01\x23'
    //	    + (b'\0'*25)
    //	    + new_value)
    //	assert len(msg) == 32+1+1+2+1+2+25+32
    //
    SHA256_CTX ctx;
    sha256_init(&ctx);

    uint8_t p1 = 0x80|2;        // 32 bytes into a data slot
    uint8_t p2_lsb = (data_slot << 3);
    uint8_t p2_msb = blk;

    uint8_t args[7] = { OP_Write, p1, p2_lsb, p2_msb, 0xEE, 0x01, 0x23 };
    uint8_t zeros[25] = { 0 };

    sha256_update(&ctx, digest, 32);
    sha256_update(&ctx, args, sizeof(args));
    sha256_update(&ctx, zeros, sizeof(zeros));
    sha256_update(&ctx, data, 32);

    sha256_final(&ctx, &body[32]);

    se_write(OP_Write, p1, (p2_msb << 8) | p2_lsb, body, sizeof(body));
    rc = se_read1();
    se_sleep();
    if (rc != 0)
        return -1;

    return 0;
}

int se_encrypted_write(
    int data_slot,
    int write_kn,
    const uint8_t *write_key,
    const uint8_t *data,
    int len
)
{
    for (int blk=0; blk<3 && len>0; blk++, len-=32) {
        int here = MIN(32, len);

        // be nice and don't read past end of input buffer
        uint8_t     tmp[32] = { 0 };
        memcpy(tmp, data+(32*blk), here);

        int rv = se_encrypted_write32(data_slot, blk, write_kn, write_key, tmp);
        if (rv < 0)
            return -1;
    }

    return 0;
}

void se_dump_stats(void)
{
    uint32_t tmp;

    tmp = crc_errors;
    tmp += not_ready_n;
    tmp += short_error;
    tmp += len_error;
    tmp += len_error_two;
    tmp += ln_retry;
    tmp += retry_out;
    tmp += rxne;
    tmp += rtof;
    tmp += notrxne;
    if (tmp > 0)
        tmp = 0;
}

#define SE_BAUDRATE 230400U

void se_setup(void)
{
   /*
    * MY_UART is pointer to USART_Typedef struct
    */
    GPIO_InitTypeDef gpiosetup = {0};
    uint32_t uartdiv;
    uint32_t uart_clock_prescaler = 0;

    /* Calculate the baud rate divisor */
    uartdiv = (uint16_t)(UART_DIV_SAMPLING16(HAL_RCC_GetPCLK1Freq(), SE_BAUDRATE, uart_clock_prescaler));

#ifdef DEV_STATS
    memset(&stats, 0, sizeof(stats));
#endif
    // configure pin A0 to be AF8_UART4, PULL_NONE
    // configure pin D15 to be INPUT, PULL_NONE, OD for output
    gpiosetup.Pin = GPIO_PIN_15;
    gpiosetup.Mode = GPIO_MODE_INPUT;
    gpiosetup.Pull = GPIO_NOPULL;
    gpiosetup.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(GPIOD, &gpiosetup);

    gpiosetup.Pin = GPIO_PIN_0;
    gpiosetup.Mode = GPIO_MODE_AF_OD;
    gpiosetup.Pull = GPIO_NOPULL;
    gpiosetup.Speed = GPIO_SPEED_FREQ_MEDIUM;
    gpiosetup.Alternate = GPIO_AF8_UART4;
    HAL_GPIO_Init(GPIOA, &gpiosetup);

    // enable clock to that part of chip
    __HAL_RCC_UART4_CLK_ENABLE();

    // copy config values from a running system, setup by mpy code
    // - except disable all interrupts
    // - mpy code will have to clean this up, see ...reinit() member func
    //
    // For max clock error insensitivity:
    // OVER8==0, ONEBIT=1

    // disable UART so some other bits can be set (only while disabled)

    MY_UART->CR1 = 0;
    MY_UART->CR1 = 0x1000002d & ~(0
                                  | USART_CR1_PEIE
                                  | USART_CR1_TXEIE
                                  | USART_CR1_TCIE
                                  | USART_CR1_RXNEIE
                                  | USART_CR1_IDLEIE
                                  | USART_CR1_OVER8
                                  | USART_CR1_UE);

    MY_UART->RTOR = 24;                  // timeout in bit periods: 3 chars or so
    MY_UART->CR2 = USART_CR2_RTOEN;      // rx timeout enable
    MY_UART->CR3 = USART_CR3_HDSEL | USART_CR3_ONEBIT;
    MY_UART->BRR = uartdiv; // 0x00000052;  // Value from HAL calcualtion above  for 230400 bps

    // clear rx timeout flag
    MY_UART->ICR = USART_ICR_RTOCF;

    // finally enable UART
    MY_UART->CR1 |= USART_CR1_UE;
}
