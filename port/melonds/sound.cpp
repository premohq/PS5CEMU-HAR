// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the DS's sound on the PS5's AudioOut (frontend.h). As Azahar's sink (port/azahar/sink.cpp)
// and Cemu's PS5AudioAPI: a thread feeds AudioOut 256-frame stereo grains at 48 kHz, and
// sceAudioOutOutput, blocking until the previous grain plays, paces it.
//
// melonDS resamples the DS's own sound to 48 kHz itself (its output rate, NDSArgs), into a buffer of
// about 40 ms the thread reads. The game runs at the rate the screens are shown, 59.94 frames a second,
// where a DS runs 59.83: melonDS is told to make its sound that much faster (SPU::SetOutputSkew, as its
// own frontend does for a frame rate it was asked for), and, by a little more or less, to keep its
// buffer half full, so the sound neither runs dry nor drops what it made.

#include "frontend.h"
#include "../ps5/log.h"

#include "NDS.h"
#include "SPU.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>

extern "C"
{
	int sceAudioOutInit(void);
	int sceAudioOutOpen(int userId, int type, int index, uint32_t length, uint32_t frequency, uint32_t format);
	int sceAudioOutClose(int handle);
	int sceAudioOutOutput(int handle, const void* samples);
	int sceAudioOutSetVolume(int handle, int flags, const int* volumes);
	void* scePthreadSelf();
	int scePthreadGetprio(void* thread, int* priority);
	int scePthreadSetprio(void* thread, int priority);
}

namespace ps5melonds::sound
{
	namespace
	{
		constexpr int kUserSystem = 0xff; // the port belongs to the system, not to one user
		constexpr int kPortMain = 0;
		constexpr uint32_t kGrain = 256; // frames per sceAudioOutOutput
		constexpr uint32_t kRate = 48000;
		constexpr uint32_t kFormatStereoS16 = 1;
		constexpr int kVolumeFlagsLeftRight = 3;
		constexpr int kVolume0dB = 32768;
		// as Azahar's sound thread: above the emulation's, so a busy CPU does not leave AudioOut without
		// its next grain (256 the highest, 767 the lowest, threads start at 700)
		constexpr int kAudioPriority = 384;
		// a DS's frame rate: its clock over the frame's cycles (355 dots of 6 cycles, 263 lines, twice)
		constexpr double kDsFrameRate = 33513982.0 / (2 * 6 * 263 * 355);
		// the buffer's fill kept here, in frames (melonDS's holds 2048; a frame adds about 800 at once)
		constexpr double kTargetFill = 1200;

		int s_port = -1;
		std::thread s_thread;
		std::atomic<bool> s_quit{false};
		std::atomic<int> s_volume{100};
		melonDS::NDS* s_nds = nullptr;
		double s_fill = kTargetFill; // the emulation's: the buffer's fill, averaged
		double s_skew = 1.0;

		void RaisePriority()
		{
			void* self = scePthreadSelf();
			int was = 0;
			scePthreadGetprio(self, &was);
			if (was > 0 && was <= kAudioPriority)
				return;
			const int result = scePthreadSetprio(self, kAudioPriority);
			ps5log::Line("[ds] sound thread priority {} -> {}{}", was, kAudioPriority,
				result == 0 ? std::string() : fmt::format(" refused ({:#x}): it stays", (uint32_t)result));
		}

		void Output()
		{
			RaisePriority();
			std::array<melonDS::s16, kGrain * 2> grain{};
			melonDS::s16 last[2] = {0, 0};
			while (!s_quit)
			{
				int got = s_nds->SPU.ReadOutput(grain.data(), (int)kGrain);
				if (got > 0)
				{
					last[0] = grain[(got - 1) * 2];
					last[1] = grain[(got - 1) * 2 + 1];
				}
				// not enough made yet (a slow frame): the last frame held, which does not click
				for (int frame = std::max(got, 0); frame < (int)kGrain; frame++)
				{
					grain[frame * 2] = last[0];
					grain[frame * 2 + 1] = last[1];
				}
				const int volume = s_volume;
				if (volume < 100)
					for (melonDS::s16& sample : grain)
						sample = (melonDS::s16)(sample * volume / 100);
				if (sceAudioOutOutput(s_port, grain.data()) < 0)
				{
					ps5log::Line("[ds] AudioOut output failed: the sound stops");
					return;
				}
			}
		}
	}

	void Start(melonDS::NDS& nds)
	{
		sceAudioOutInit(); // an error when the launcher has already: the port says
		s_port = sceAudioOutOpen(kUserSystem, kPortMain, 0, kGrain, kRate, kFormatStereoS16);
		if (s_port < 0)
		{
			ps5log::Line("[ds] AudioOut did not open ({:#x}): no sound", (uint32_t)s_port);
			return;
		}
		const std::array<int, 8> volumes{kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB};
		sceAudioOutSetVolume(s_port, kVolumeFlagsLeftRight, volumes.data());
		s_nds = &nds;
		s_fill = kTargetFill;
		s_skew = 1.0;
		s_quit = false;
		s_thread = std::thread(Output);
	}

	void Stop()
	{
		s_quit = true;
		if (s_thread.joinable())
			s_thread.join();
		if (s_port >= 0)
		{
			sceAudioOutOutput(s_port, nullptr); // wait for the last grain
			sceAudioOutClose(s_port);
		}
		s_port = -1;
		s_nds = nullptr;
	}

	void SetVolume(int percent)
	{
		s_volume = std::clamp(percent, 0, 100);
	}

	void Pace(melonDS::NDS& nds, double refreshHz, int speed)
	{
		// as many seconds of sound a second as the game makes: faster at a higher speed limit (its pitch
		// up with it), none the fill could follow when there is no limit
		double skew = refreshHz / kDsFrameRate * (speed > 0 ? speed / 100.0 : 1.0);
		if (speed == 100)
		{
			// a little more sound when the buffer runs low, a little less when it fills: half a percent
			// at most, which no ear hears
			s_fill = s_fill * 0.95 + nds.SPU.GetOutputSize() * 0.05;
			skew *= 1.0 + 0.005 * std::clamp((s_fill - kTargetFill) / kTargetFill, -1.0, 1.0);
		}
		if (std::abs(skew - s_skew) > 0.0001)
		{
			nds.SPU.SetOutputSkew(skew);
			s_skew = skew;
		}
	}
}
