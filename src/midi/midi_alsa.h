/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2020-2024  The DOSBox Staging Team
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef DOSBOX_MIDI_ALSA_H
#define DOSBOX_MIDI_ALSA_H

#include "midi_device.h"

#if C_ALSA

#include <alsa/asoundlib.h>

struct AlsaAddress {
	int client = -1;
	int port   = -1;
};

class MidiDeviceAlsa final : public MidiDevice {
public:
	MidiDeviceAlsa(const char* conf);
	~MidiDeviceAlsa() override;

	// prevent copying
	MidiDeviceAlsa(const MidiDeviceAlsa&) = delete;
	// prevent assignment
	MidiDeviceAlsa& operator=(const MidiDeviceAlsa&) = delete;

	std::string GetName() const override
	{
		return MidiDeviceName::Alsa;
	}

	Type GetType() const override
	{
		return MidiDevice::Type::External;
	}

	void SendMidiMessage(const MidiMessage& msg) override;
	void SendSysExMessage(uint8_t* sysex, size_t len) override;

	AlsaAddress GetInputPortAddress();

private:
	snd_seq_event_t ev    = {};
	snd_seq_t* seq_handle = nullptr;

	// address of input port we're connected to
	AlsaAddress seq = {-1, -1};

	int output_port = 0;

	void send_event(int do_flush);
};

void ALSA_ListDevices(MidiDeviceAlsa* device, Program* caller);

#endif // C_ALSA

#endif
