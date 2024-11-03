// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2020-2022 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 * Copyright (C) 2022 Niklas Ekström <mail@niklasekstrom.nu>
 *
 * libgpiod bitbang driver added by Niklas Ekström <mail@niklasekstrom.nu> in 2022
 */

#include "libgpiodJtagBitbang.hpp"

#include <gpiod.h>
#include <stdio.h>
#include <string.h>

#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <stdexcept>


#include "display.hpp"

#define DEBUG 1

#ifdef DEBUG
#define display(...) \
	do { \
		if (_verbose) fprintf(stdout, __VA_ARGS__); \
	}while(0)
#else
#define display(...) do {}while(0)
#endif

LibgpiodJtagBitbang::LibgpiodJtagBitbang(
		const jtag_pins_conf_t *pin_conf,
		const std::string &dev, __attribute__((unused)) uint32_t clkHZ,
		int8_t verbose):_verbose(verbose>1)
{
	_tck_pin = pin_conf->tck_pin;
	_tms_pin = pin_conf->tms_pin;
	_tdi_pin = pin_conf->tdi_pin;
	_tdo_pin = pin_conf->tdo_pin;

	std::string chip_dev = dev;
	if (chip_dev.empty())
		chip_dev = "/dev/gpiochip0";

	display("libgpiod jtag bitbang driver, dev=%s, tck_pin=%d, tms_pin=%d, tdi_pin=%d, tdo_pin=%d\n",
		chip_dev.c_str(), _tck_pin, _tms_pin, _tdi_pin, _tdo_pin);

	if (chip_dev.length() < 14 || chip_dev.substr(0, 13) != "/dev/gpiochip") {
		display("Invalid gpio chip %s, should be /dev/gpiochipX\n", chip_dev.c_str());
		throw std::runtime_error("Invalid gpio chip\n");
	}

	/* Validate pins */
#ifdef GPIOD_APIV2
	const unsigned int pins[] = {_tck_pin, _tms_pin, _tdi_pin, _tdo_pin};
	in_pins[0] = _tdo_pin;
	out_pins[0] = _tdi_group[1] = _tms_group[1] = _tck_pin;
	out_pins[1] = _tms_group[0] = _tms_pin;
	out_pins[2] = _tdi_group[0] = _tdi_pin;
#else
	const int pins[] = {_tck_pin, _tms_pin, _tdi_pin, _tdo_pin};
#endif
	for (uint32_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		if (pins[i] < 0 || pins[i] >= 1000) {
			display("Pin %d is outside of valid range\n", pins[i]);
			throw std::runtime_error("A pin is outside of valid range\n");
		}

		for (uint32_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); j++) {
			if (pins[i] == pins[j]) {
				display("Two or more pins are assigned to the same pin number %d\n", pins[i]);
				throw std::runtime_error("Two or more pins are assigned to the same pin number\n");
			}
		}
	}

	_chip = gpiod_chip_open(chip_dev.c_str());
	if (!_chip) {
		display("Unable to open gpio chip %s\n", chip_dev.c_str());
		throw std::runtime_error("Unable to open gpio chip\n");
	}

#ifdef GPIOD_APIV2
	_in_req_cfg = gpiod_request_config_new();
	_out_req_cfg = gpiod_request_config_new();

	gpiod_request_config_set_consumer(_in_req_cfg, "ofl_in");
	gpiod_request_config_set_consumer(_out_req_cfg, "ofl_out");

	_out_settings = gpiod_line_settings_new();
	_in_settings = gpiod_line_settings_new();

	gpiod_line_settings_set_direction(
		_in_settings, GPIOD_LINE_DIRECTION_INPUT);
	gpiod_line_settings_set_direction(
		_out_settings, GPIOD_LINE_DIRECTION_OUTPUT);

	gpiod_line_settings_set_bias(
		_in_settings, GPIOD_LINE_BIAS_DISABLED);
	gpiod_line_settings_set_bias(
		_out_settings, GPIOD_LINE_BIAS_DISABLED);

	_out_line_cfg = gpiod_line_config_new();
	_in_line_cfg = gpiod_line_config_new();

	gpiod_line_config_add_line_settings(
		_out_line_cfg, _out_pins, 3, _out_settings);
	gpiod_line_config_add_line_settings(
		_in_line_cfg, _in_pins, 1, _in_settings);

	_out_request = gpiod_chip_request_lines(
		_chip, _out_req_cfg, _out_line_cfg);
	_in_request = gpiod_chip_request_lines(
		_chip, _in_req_cfg, _in_line_cfg);
#else
	_tdo_line = get_line(_tdo_pin, 0, GPIOD_LINE_REQUEST_DIRECTION_INPUT);
	_tdi_line = get_line(_tdi_pin, 0, GPIOD_LINE_REQUEST_DIRECTION_OUTPUT);
	_tck_line = get_line(_tck_pin, 0, GPIOD_LINE_REQUEST_DIRECTION_OUTPUT);
	_tms_line = get_line(_tms_pin, 1, GPIOD_LINE_REQUEST_DIRECTION_OUTPUT);
#endif

	_curr_tdi = 0;
	_curr_tck = 0;
	_curr_tms = 1;

	// FIXME: I'm unsure how this value should be set.
	// Maybe experiment, or think through what it should be.
	_clkHZ = 5000000;
}

LibgpiodJtagBitbang::~LibgpiodJtagBitbang()
{
#ifdef GPIOD_APIV2
	if (_out_request)
		gpiod_line_request_release(_out_request);
	if (_in_request)
		gpiod_line_request_release(_in_request);

	if (_out_line_cfg)
		gpiod_line_config_free(_out_line_cfg);
	if (_in_line_cfg)
		gpiod_line_config_free(_in_line_cfg);

	if (_out_settings)
		gpiod_line_settings_free(_out_settings);
	if (_in_settings)
		gpiod_line_settings_free(_in_settings);
#else
	if (_tms_line)
		gpiod_line_release(_tms_line);

	if (_tck_line)
		gpiod_line_release(_tck_line);

	if (_tdi_line)
		gpiod_line_release(_tdi_line);

	if (_tdo_line)
		gpiod_line_release(_tdo_line);
#endif

	if (_chip)
		gpiod_chip_close(_chip);
}

#ifndef GPIOD_APIV2
gpiod_line *LibgpiodJtagBitbang::get_line(unsigned int offset, int val, int dir)
{
	gpiod_line *line = gpiod_chip_get_line(_chip, offset);
	if (!line) {
		display("Unable to get gpio line %u\n", offset);
		throw std::runtime_error("Unable to get gpio line\n");
	}

	gpiod_line_request_config config = {
		.consumer = "openFPGALoader",
		.request_type = dir,
		.flags = 0,
	};

	int ret = gpiod_line_request(line, &config, val);
	if (ret < 0) {
		display("Error requesting gpio line %u\n", offset);
		throw std::runtime_error("Error requesting gpio line\n");
	}

	return line;
}
#endif

int LibgpiodJtagBitbang::update_pins(int tck, int tms, int tdi)
{
	if (tdi != _curr_tdi) {
#ifdef GPIOD_APIV2
		if (gpiod_line_request_set_value(_tdi_request, _tdi_pin,
			(tdi == 0) ? GPIOD_LINE_VALUE_INACTIVE :
				GPIOD_LINE_VALUE_ACTIVE) < 0)
#else
		if (gpiod_line_set_value(_tdi_line, tdi) < 0)
#endif
			display("Unable to set gpio pin tdi\n");
	}

	if (tms != _curr_tms) {
#ifdef GPIOD_APIV2
		if (gpiod_line_request_set_value(_tms_request, _tms_pin,
			(tms == 0) ? GPIOD_LINE_VALUE_INACTIVE :
				GPIOD_LINE_VALUE_ACTIVE) < 0)
#else
		if (gpiod_line_set_value(_tms_line, tms) < 0)
#endif
			display("Unable to set gpio pin tms\n");
	}

	if (tck != _curr_tck) {
#ifdef GPIOD_APIV2
		if (gpiod_line_request_set_value(_tck_request, _tck_pin,
			(tck == 0) ? GPIOD_LINE_VALUE_INACTIVE :
				GPIOD_LINE_VALUE_ACTIVE) < 0)
#else
		if (gpiod_line_set_value(_tck_line, tck) < 0)
#endif
			display("Unable to set gpio pin tck\n");
	}

	_curr_tdi = tdi;
	_curr_tms = tms;
	_curr_tck = tck;

	return 0;
}

int LibgpiodJtagBitbang::read_tdo()
{
#ifdef GPIOD_APIV2
	gpiod_line_value req = gpiod_line_request_get_value(
		_tdo_request, _tdo_pin);
		if (req == GPIOD_LINE_VALUE_ERROR)
		{
		display("Error reading TDO line\n");
		throw std::runtime_error("Error reading TDO line\n");
	}
	return (req == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
#else
	return gpiod_line_get_value(_tdo_line);
#endif
}

int LibgpiodJtagBitbang::setClkFreq(__attribute__((unused)) uint32_t clkHZ)
{
	// FIXME: The assumption is that calling the gpiod_line_set_value
	// routine will limit the clock frequency to lower than what is specified.
	// This needs to be verified, and possibly artificial delays should be added.
	return 0;
}

int LibgpiodJtagBitbang::writeTMS(const uint8_t *tms_buf, uint32_t len,
		__attribute__((unused)) bool flush_buffer,
		__attribute__((unused)) uint8_t tdi)
{
	int tms;
	enum gpiod_line_value val[2] = {GPIOD_LINE_VALUE_INACTIVE, GPIOD_LINE_VALUE_INACTIVE};

	if (len == 0) // nothing -> stop
		return len;

	for (uint32_t i = 0; i < len; i++) {
		_curr_tms = ((tms_buf[i >> 3] & (1 << (i & 7))) ? 1 : 0);
		val[0] = (_curr_tms == 1) ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
		gpiod_line_set_values_subset(out_req, 2, _tms_group, val);
		gpiod_line_request_set_value(out_req, _tck_pin, GPIOD_LINE_VALUE_ACTIVE);
	}

	/* force TCK low */
	gpiod_line_set_value(out_req, _tck_pin, GPIOD_LINE_VALUE_INACTIVE);

	return len;
}

int LibgpiodJtagBitbang::writeTDI(const uint8_t *tx, uint8_t *rx, uint32_t len, bool end)
{
	enum gpiod_line_value val[4] = {0, 0, 0, 1};

	if (rx)
		memset(rx, 0, len / 8);

	for (uint32_t i = 0; i < len; i++) {
		if (tx)
			_curr_tdi = (tx[i >> 3] & (1 << (i & 7))) ? 1 : 0;

		/* When TMS needs to be also updated (because of end) */
		if (end && (i == len - 1)) {
			_curr_tms = 1;
			val[0] = _curr_tms;
			val[1] = _curr_tdi;
			val[2] = 0;
			/* Write all output GPIOs in one command */
			gpiod_line_set_values(out_req, val);
			/* Only update TCK (Rising edge) */
			gpiod_line_request_set_value(out_req, _tck_pin, GPIOD_LINE_VALUE_ACTIVE);
		} else {
			/* otherwise only update TDI + TCK */
			val[0] = val[2] = _curr_tdi;
			gpiod_line_request_set_values_subset(out_req, 2, tdi_group, val);
			gpiod_line_request_set_value(out_req, _tck_pin, GPIOD_LINE_VALUE_ACTIVE);
		}


		if (rx) {
			if (read_tdo() > 0)
				rx[i >> 3] |= 1 << (i & 7);
		}
	}

	/* Is it really required? First step is always to set TCK low
	 * maybe only a simple update at destructor to have coherent state
	 */
	gpiod_line_request_set_value(out_req, _tck_pin, GPIOD_LINE_VALUE_INACTIVE);

	return len;
}

int LibgpiodJtagBitbang::toggleClk(uint8_t tms, uint8_t tdi, uint32_t clk_len)
{
	for (uint32_t i = 0; i < clk_len; i++) {
		gpiod_line_request_set_value(out_req, _tck_pin, GPIOD_LINE_VALUE_INACTIVE);
		gpiod_line_request_set_value(out_req, _tck_pin, GPIOD_LINE_VALUE_ACTIVE);
	}

	gpiod_line_request_set_value(out_req, _tck_pin, GPIOD_LINE_VALUE_INACTIVE);

	return clk_len;
}
