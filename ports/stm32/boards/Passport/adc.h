// SPDX-FileCopyrightText: 2020 Foundation Devices, Inc.  <hello@foundationdevices.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright 2020 - Foundation Devices Inc.
//

#ifndef _ADC_H_
#define _ADC_H_

extern HAL_StatusTypeDef adc3_init(void);
extern HAL_StatusTypeDef adc2_init(void);
extern HAL_StatusTypeDef read_boardrev(uint16_t * board_rev);
extern HAL_StatusTypeDef read_powermon(uint16_t * current, uint16_t * voltage);
extern void enable_noise(void);
extern void disable_noise(void);
extern HAL_StatusTypeDef read_noise_inputs(uint32_t * noise1, uint32_t * noise2);

#endif //_ADC_H_
