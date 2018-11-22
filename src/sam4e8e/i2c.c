// SAM4 I2C Port
//
// Copyright (C) 2018  Florian Heilmann <Florian.Heilmann@gmx.net>
// Copyright (C) 2018  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "gpio.h"
#include "internal.h"
#include "command.h" // shutdown
#include "sched.h" // sched_shutdown
#include "autoconf.h" // CONFIG_CLOCK_FREQ
#include "board/misc.h" //timer_from_us

void
i2c_init(Twi *p_twi, uint32_t rate)
{
    uint32_t twi_id = (p_twi == TWI0) ? ID_TWI0 : ID_TWI1;
    if ((PMC->PMC_PCSR0 & (1u << twi_id)) == 0) {
        PMC->PMC_PCER0 = 1 << twi_id;
    }
    if (p_twi == TWI0) {
        gpio_set_peripheral(TWI0_SCL_BANK, TWI0_SCL_PIN, TWI0_SCL_PERIPH, 0);
        gpio_set_peripheral(TWI0_SDA_BANK, TWI0_SDA_PIN, TWI0_SDA_PERIPH, 0);
    } else {
        gpio_set_peripheral(TWI1_SCL_BANK, TWI1_SCL_PIN, TWI1_SCL_PERIPH, 0);
        gpio_set_peripheral(TWI1_SDA_BANK, TWI1_SDA_PIN, TWI1_SDA_PERIPH, 0);
    }
    p_twi->TWI_IDR = 0xFFFFFFFF;
    (void)p_twi->TWI_SR;
    p_twi->TWI_CR = TWI_CR_SWRST;
    (void)p_twi->TWI_RHR;
    p_twi->TWI_CR = TWI_CR_MSDIS;
    p_twi->TWI_CR = TWI_CR_SVDIS;
    p_twi->TWI_CR = TWI_CR_MSEN;

    uint32_t cldiv = 0;
    uint32_t chdiv = 0;
    uint32_t ckdiv = 0;

    cldiv = CONFIG_CLOCK_FREQ / ((rate > 384000 ? 384000 : rate) * 2) - 4;

    while((cldiv > 255) && (ckdiv < 7)) {
        ckdiv++;
        cldiv /= 2;
    }

    if (rate > 348000) {
        chdiv = CONFIG_CLOCK_FREQ / ((2 * rate - 384000) * 2) - 4;
        while((chdiv > 255) && (ckdiv < 7)) {
            ckdiv++;
            chdiv /= 2;
        }
    } else {
        chdiv = cldiv;
    }
    p_twi->TWI_CWGR = TWI_CWGR_CLDIV(cldiv) | \
                      TWI_CWGR_CHDIV(chdiv) | \
                      TWI_CWGR_CKDIV(ckdiv);
}

uint32_t
addr_to_u32(uint8_t addr_len, uint8_t *addr)
{
    uint32_t address = addr[0];
    if (addr_len > 1) {
        address <<= 8;
        address |= addr[1];
    }
    if (addr_len > 2) {
        address <<= 8;
        address |= addr[2];
    }
    if (addr_len > 3) {
        shutdown("Addresses larger than 3 bytes are not supported");
    }
    return address;
}

struct i2c_config
i2c_setup(uint32_t bus, uint32_t rate, uint8_t addr)
{
    if ((bus > 1) | (rate > 400000))
        shutdown("Invalid i2c_setup parameters!");
    Twi *p_twi = (bus == 0) ? TWI0 : TWI1;
    i2c_init(p_twi, rate);
    return (struct i2c_config){ .twi=p_twi, .addr=addr};
}

void
i2c_write(struct i2c_config config,
                uint8_t write_len, uint8_t *write)
{
    Twi *p_twi = config.twi;
    uint32_t status;
    uint32_t bytes_to_send = write_len;
    p_twi->TWI_MMR = TWI_MMR_DADR(config.addr);
    for(;;) {
        status = p_twi->TWI_SR;
        if (status & TWI_SR_NACK)
            shutdown("I2C NACK error encountered!");
        if (!(status & TWI_SR_TXRDY))
            continue;
        if (!bytes_to_send)
            break;
        p_twi->TWI_THR = *write++;
        bytes_to_send--;
    }
    p_twi->TWI_CR = TWI_CR_STOP;
    while(!(p_twi->TWI_SR& TWI_SR_TXCOMP)) {
    }
}

static void
i2c_wait(Twi* p_twi, uint32_t bit, uint32_t timeout)
{
    for (;;) {
        uint32_t flags = p_twi->TWI_SR;
        if (flags & bit)
            break;
        if (!timer_is_before(timer_read_time(), timeout))
            shutdown("I2C timeout occured");
    }
}

void
i2c_read(struct i2c_config config,
              uint8_t reg_len, uint8_t *reg,
              uint8_t read_len, uint8_t *read)
{
    Twi *p_twi = config.twi;
    uint32_t status;
    uint32_t bytes_to_send=read_len;
    uint8_t stop = 0;
    p_twi->TWI_MMR = 0;
    p_twi->TWI_MMR = TWI_MMR_MREAD | TWI_MMR_DADR(config.addr) |
                     ((reg_len << TWI_MMR_IADRSZ_Pos) &
                     TWI_MMR_IADRSZ_Msk);
    p_twi->TWI_IADR = 0;
    p_twi->TWI_IADR = addr_to_u32(reg_len, reg);
    if (bytes_to_send == 1) {
        p_twi->TWI_CR = TWI_CR_START | TWI_CR_STOP;
        stop = 1;
    } else {
        p_twi->TWI_CR = TWI_CR_START;
        stop = 0;
    }
    while (bytes_to_send > 0) {
        status = p_twi->TWI_SR;
        if (status & TWI_SR_NACK) {
            shutdown("I2C NACK error encountered!");
        }
        if (bytes_to_send == 1 && !stop) {
            p_twi->TWI_CR = TWI_CR_STOP;
            stop = 1;
        }
        i2c_wait(p_twi, TWI_SR_RXRDY, timer_read_time() + timer_from_us(5000));
        if (!(status & TWI_SR_RXRDY)) {
            continue;
        }
        *read++ = p_twi->TWI_RHR;
        bytes_to_send--;
    }
    while (!(p_twi->TWI_SR & TWI_SR_TXCOMP)) {}
    (void)p_twi->TWI_SR;
}
