/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2019-2023  The DOSBox Staging Team
 *  Copyright (C) 2002-2017  The DOSBox Team
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

#include "gameblaster.h"

#include "channel_names.h"
#include "pic.h"
#include "setup.h"

// The Game Blaster is nothing else than a rebranding of Creative's first PC
// sound card, the Creative Music System (C/MS).
void GameBlaster::Open(const int port_choice, const std::string &card_choice,
                       const std::string &filter_choice)
{
	Close();
	assert(!is_open);

	is_standalone_gameblaster = (card_choice == "gb");

	// Ports are filtered and corrected by the conf system, so we simply
	// assert here
	const std::vector<io_port_t> valid_gb_ports = {0x210, 0x220, 0x230, 0x240, 0x250, 0x260};
	const std::vector<io_port_t> valid_cms_ports = {0x220, 0x240, 0x260, 0x280, 0x2a0, 0x2c0, 0x2e0, 0x300};
	const auto valid_ports = is_standalone_gameblaster ? valid_gb_ports
	                                                   : valid_cms_ports;
	base_port = check_cast<io_port_t>(port_choice);
	assert(contains(valid_ports, base_port));

	// Create the two SAA1099 devices
	for (auto &d : devices) {
		d = std::make_unique<saa1099_device>("", nullptr, chip_clock, render_divisor);
		d->device_start();
	}

	// The Sound Blaster 1.0 included the SAA-1099 chips on-board for C/MS
	// compatibility, and the Sound Blaster 2.0 had sockets for them as
	// optional add-ons. Therefore, we always set up these handlers, even if
	// the card type isn't a Game Blaster.
	using namespace std::placeholders;
	const auto data_to_left = std::bind(&GameBlaster::WriteDataToLeftDevice, this, _1, _2, _3);
	const auto control_to_left = std::bind(&GameBlaster::WriteControlToLeftDevice, this, _1, _2, _3);
	const auto data_to_right = std::bind(&GameBlaster::WriteDataToRightDevice, this, _1, _2, _3);
	const auto control_to_right = std::bind(&GameBlaster::WriteControlToRightDevice, this, _1, _2, _3);

	write_handlers[0].Install(base_port, data_to_left, io_width_t::byte);
	write_handlers[1].Install(base_port + 1, control_to_left, io_width_t::byte);
	write_handlers[2].Install(base_port + 2, data_to_right, io_width_t::byte);
	write_handlers[3].Install(base_port + 3, control_to_right, io_width_t::byte);

	// However, the Creative Music System (C/MS) / Game Blaster cards came
	// with a dedicated chip on them that could be used for detection. So we
	// set up those handlers for this chip only if the card type is a Game
	// Blaster:
	if (is_standalone_gameblaster) {
		const auto read_from_detection_port = std::bind(&GameBlaster::ReadFromDetectionPort, this, _1, _2);
		const auto write_to_detection_port = std::bind(&GameBlaster::WriteToDetectionPort, this, _1, _2, _3);

		read_handler_for_detection.Install(base_port, read_from_detection_port, io_width_t::byte, 16);
		write_handler_for_detection.Install(base_port + 4,
		                                    write_to_detection_port,
		                                    io_width_t::byte,
		                                    12);
	}

	// Set up the mixer and level controls
	const auto audio_callback = std::bind(&GameBlaster::AudioCallback, this, _1);

	channel = MIXER_AddChannel(audio_callback,
	                           use_mixer_rate,
	                           ChannelName::Cms,
	                           {ChannelFeature::Sleep,
	                            ChannelFeature::Stereo,
	                            ChannelFeature::ReverbSend,
	                            ChannelFeature::ChorusSend,
	                            ChannelFeature::Synthesizer});

	// The filter parameters have been tweaked by analysing real hardware
	// recordings. The results are virtually indistinguishable from the
	// real thing by ear only.
	auto enable_filter = [&]() {
		constexpr auto Order        = 1;
		constexpr auto CutoffFreqHz = 6000;

		channel->ConfigureLowPassFilter(Order, CutoffFreqHz);
		channel->SetLowPassFilter(FilterState::On);
	};

	if (const auto maybe_bool = parse_bool_setting(filter_choice)) {
		if (*maybe_bool) {
			enable_filter();
		} else {
			channel->SetLowPassFilter(FilterState::Off);
		}
	} else if (!channel->TryParseAndSetCustomFilter(filter_choice)) {
		LOG_WARNING("CMS: Invalid 'cms_filter' setting: '%s', using 'on'",
		            filter_choice.c_str());

		set_section_property_value("sblaster", "cms_filter", "on");
		enable_filter();
	}

	// Calculate rates and ratio based on the mixer's rate
	const auto frame_rate_hz = channel->GetSampleRate();

	// Set up the resampler to convert from the render rate to the mixer's frame rate
	const auto max_freq = std::max(frame_rate_hz * 0.9 / 2, 8000.0);
	for (auto &r : resamplers)
		r.reset(reSIDfp::TwoPassSincResampler::create(render_rate_hz, frame_rate_hz, max_freq));

	LOG_MSG("CMS: Running on port %xh with two %0.3f MHz Phillips SAA-1099 chips",
	        base_port,
	        chip_clock / 1e6);

	assert(channel);
	assert(devices[0]);
	assert(devices[1]);
	assert(resamplers[0]);
	assert(resamplers[1]);

	is_open = true;
}

bool GameBlaster::MaybeRenderFrame(AudioFrame &frame)
{
	// Static containers set up once and reused
	static std::array<int16_t, 2> buf = {}; // left and right
	static int16_t *p_buf[]           = {&buf[0], &buf[1]};
	static device_sound_interface::sound_stream stream;

	// Accumulate the samples from both SAA-1099 devices
	devices[0]->sound_stream_update(stream, nullptr, p_buf, 1);
	int left_accum = buf[0];
	int right_accum = buf[1];

	devices[1]->sound_stream_update(stream, nullptr, p_buf, 1);
	left_accum += buf[0];
	right_accum += buf[1];

	// Increment our time datum up to which the device has rendered
	last_rendered_ms += ms_per_render;

	// Resample the limited frame
	const auto l_ready = resamplers[0]->input(left_accum);
	const auto r_ready = resamplers[1]->input(right_accum);
	assert(l_ready == r_ready);
	const auto frame_is_ready = l_ready && r_ready;

	// Get the frame from the resampler
	if (frame_is_ready) {
		frame.left  = static_cast<float>(resamplers[0]->output());
		frame.right = static_cast<float>(resamplers[1]->output());
	}
	return frame_is_ready;
}

void GameBlaster::RenderUpToNow()
{
	const auto now = PIC_FullIndex();

	// Wake up the channel and update the last rendered time datum.
	assert(channel);
	if (channel->WakeUp()) {
		last_rendered_ms = now;
		return;
	}
	// Keep rendering until we're current
	while (last_rendered_ms < now) {
		last_rendered_ms += ms_per_render;
		if (AudioFrame f = {}; MaybeRenderFrame(f))
			fifo.emplace(f);
	}
}

void GameBlaster::WriteDataToLeftDevice(io_port_t, io_val_t value, io_width_t)
{
	RenderUpToNow();
	devices[0]->data_w(0, 0, check_cast<uint8_t>(value));
}

void GameBlaster::WriteControlToLeftDevice(io_port_t, io_val_t value, io_width_t)
{
	RenderUpToNow();
	devices[0]->control_w(0, 0, check_cast<uint8_t>(value));
}

void GameBlaster::WriteDataToRightDevice(io_port_t, io_val_t value, io_width_t)
{
	RenderUpToNow();
	devices[1]->data_w(0, 0, check_cast<uint8_t>(value));
}

void GameBlaster::WriteControlToRightDevice(io_port_t, io_val_t value, io_width_t)
{
	RenderUpToNow();
	devices[1]->control_w(0, 0, check_cast<uint8_t>(value));
}

void GameBlaster::AudioCallback(const uint16_t requested_frames)
{
	assert(channel);

	//if (fifo.size())
	//	LOG_MSG("CMS: Queued %2lu cycle-accurate frames", fifo.size());

	auto frames_remaining = requested_frames;

	// First, add any frames we've queued since the last callback
	while (frames_remaining && fifo.size()) {
		channel->AddSamples_sfloat(1, &fifo.front()[0]);
		fifo.pop();
		--frames_remaining;
	}
	// If the queue's run dry, render the remainder and sync-up our time datum
	while (frames_remaining) {
		if (AudioFrame f = {}; MaybeRenderFrame(f)) {
			channel->AddSamples_sfloat(1, &f[0]);
		}
		--frames_remaining;
	}
	last_rendered_ms = PIC_FullIndex();
}

void GameBlaster::WriteToDetectionPort(io_port_t port, io_val_t value, io_width_t)
{
	switch (port - base_port) {
	case 0x6:
	case 0x7: cms_detect_register = check_cast<uint8_t>(value); break;
	}
}

uint8_t GameBlaster::ReadFromDetectionPort(io_port_t port, io_width_t) const
{
	uint8_t retval = 0xff;
	switch (port - base_port) {
	case 0x4:
		retval = 0x7f;
		break;
	case 0xa:
	case 0xb:
		retval = cms_detect_register;
		break;
	}
	return retval;
}

void GameBlaster::Close()
{
	if (!is_open)
		return;

	LOG_INFO("CMS: Shutting down");

	// Drop access to the IO ports
	for (auto &w : write_handlers)
		w.Uninstall();
	write_handler_for_detection.Uninstall();
	read_handler_for_detection.Uninstall();

	// Stop playback
	if (channel)
		channel->Enable(false);

	// Deregister the mixer channel and remove it
	assert(channel);
	MIXER_DeregisterChannel(channel);
	channel.reset();

	// Remove the SAA-1099 devices and resamplers
	devices[0].reset();
	devices[1].reset();
	resamplers[0].reset();
	resamplers[1].reset();

	is_open = false;
}

GameBlaster gameblaster;

void CMS_ShutDown([[maybe_unused]] Section* conf)
{
	gameblaster.Close();
}

void CMS_Init(Section* conf)
{
	assert(conf);

	Section_prop* section = static_cast<Section_prop*>(conf);
	gameblaster.Open(section->Get_hex("sbbase"),
	                 section->Get_string("sbtype"),
	                 section->Get_string("cms_filter"));

	constexpr auto changeable_at_runtime = true;
	section->AddDestroyFunction(&CMS_ShutDown, changeable_at_runtime);
}
