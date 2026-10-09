// SPDX-License-Identifier: MPL-2.0
// PS5CEMU-HAR: Cemu's emulated USB devices (usb_devices.h). Figures are put on and taken off as
// Cemu's Emulated USB Devices window does it (EmulatedUSBDeviceFrame), so this file keeps Cemu's
// MPL-2.0 licence.

#include "usb_devices.h"
#include "lang.h"
#include "paths.h"
#include "../ps5/log.h"

#include "Cafe/OS/libs/nsyshid/Dimensions.h"
#include "Cafe/OS/libs/nsyshid/Infinity.h"
#include "Cafe/OS/libs/nsyshid/Skylander.h"
#include "Common/FileStream.h"
#include "config/CemuConfig.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <memory>
#include <mutex>

namespace fs = std::filesystem;

namespace ps5usb
{
	using ps5lang::Tr;
	using ps5lang::TrF;
	using ps5lang::TrMark;

	namespace
	{
		struct SlotState
		{
			std::string figure;
			int portalSlot = -1; // the Skylanders portal's own slot the figure took
		};
		struct Request
		{
			Device device;
			size_t slot;
			std::string figure;
		};

		std::mutex s_mutex;
		std::vector<Request> s_requests;				   // under s_mutex
		std::array<std::vector<SlotState>, 3> s_slots;	   // under s_mutex
		std::array<std::vector<std::string>, 3> s_figures; // under s_mutex
		std::array<bool, 3> s_plugged{};				   // under s_mutex
		std::string s_error;							   // under s_mutex

		size_t Index(Device device) { return (size_t)device; }

		// Each device's slots in the menu: the Skylanders portal's first four (two players, and a
		// trap or magic item beside them), and every place on the Infinity Base and the Dimensions
		// Toypad, named as Cemu's window names them
		const std::vector<std::string>& Labels(Device device)
		{
			// tr: the places on the toys' portals that a figure goes on
			static const std::vector<std::string> skylanders = {TrMark("Slot 1"), TrMark("Slot 2"), TrMark("Slot 3"), TrMark("Slot 4")};
			static const std::vector<std::string> infinity = {TrMark("Play set / power disc"), TrMark("Power disc 2"), TrMark("Power disc 3"),
				TrMark("Player 1"), TrMark("Player 1 ability 1"), TrMark("Player 1 ability 2"), TrMark("Player 2"), TrMark("Player 2 ability 1"),
				TrMark("Player 2 ability 2")};
			static const std::vector<std::string> dimensions = {TrMark("Left pad 1"), TrMark("Centre pad"), TrMark("Right pad 1"),
				TrMark("Left pad 2"), TrMark("Left pad 3"), TrMark("Right pad 2"), TrMark("Right pad 3")};
			return device == Device::Skylanders ? skylanders : device == Device::Infinity ? infinity : dimensions;
		}

		// the Toypad's pad of each slot: Cemu's AddDimensionPanel(pad, index) (1 the centre, 2 the left, 3 the right)
		constexpr uint8 kDimensionPads[] = {2, 1, 3, 2, 2, 3, 3};

		ConfigValue<bool>& Switch(Device device)
		{
			auto& usb = GetConfig().emulated_usb_devices;
			return device == Device::Skylanders ? usb.emulate_skylander_portal :
				   device == Device::Infinity	? usb.emulate_infinity_base :
												  usb.emulate_dimensions_toypad;
		}

		std::vector<SlotState>& SlotsOf(Device device) // under s_mutex
		{
			auto& slots = s_slots[Index(device)];
			slots.resize(Labels(device).size());
			return slots;
		}

		// The figure off its slot (the main thread's)
		void Remove(Device device, size_t slot, SlotState& state)
		{
			if (state.figure.empty())
				return;
			switch (device)
			{
			case Device::Skylanders:
				if (state.portalSlot >= 0)
					nsyshid::g_skyportal.RemoveSkylander((uint8)state.portalSlot);
				break;
			case Device::Infinity: nsyshid::g_infinitybase.RemoveFigure((uint8)slot); break;
			case Device::Dimensions: nsyshid::g_dimensionstoypad.RemoveFigure(kDimensionPads[slot], (uint8)slot, true); break;
			}
			ps5log::Line("[usb] {}: {} off {}", Name(device), state.figure, Labels(device)[slot]);
			state = {};
		}

		// A dump's first bytes, the figure's data; false when the file is shorter
		template<size_t N>
		bool Read(FileStream& file, std::array<uint8, N>& data)
		{
			return file.readData(data.data(), (uint32)data.size()) == data.size();
		}

		// A figure onto its slot, as EmulatedUSBDeviceFrame's Load...Path do it (the main thread's);
		// the reason when it cannot be
		std::string Place(Device device, size_t slot, const std::string& figure, SlotState& state)
		{
			const fs::path path = fs::path(Folder(device)) / figure;
			// opened for writing too: the game keeps the figure's progress in it, as on the toy itself
			std::unique_ptr<FileStream> file(FileStream::openFile2(path, true));
			if (!file)
				return TrF("{0} could not be opened", figure);
			switch (device)
			{
			case Device::Skylanders:
			{
				std::array<uint8, nsyshid::SKY_FIGURE_SIZE> data{};
				if (!Read(*file, data))
					return TrF("{0} is too small for a Skylander", figure);
				const uint8 portalSlot = nsyshid::g_skyportal.LoadSkylander(data.data(), std::move(file));
				if (portalSlot >= nsyshid::MAX_SKYLANDERS)
					return Tr("The portal is full");
				state.portalSlot = portalSlot;
				break;
			}
			case Device::Infinity:
			{
				std::array<uint8, nsyshid::INF_FIGURE_SIZE> data{};
				if (!Read(*file, data))
					return TrF("{0} is too small for an Infinity figure", figure);
				nsyshid::g_infinitybase.LoadFigure(data, std::move(file), (uint8)slot);
				break;
			}
			case Device::Dimensions:
			{
				std::array<uint8, 0x2D * 0x04> data{};
				if (!Read(*file, data))
					return TrF("{0} is too small for a Dimensions figure", figure);
				nsyshid::g_dimensionstoypad.LoadFigure(data, std::move(file), kDimensionPads[slot], (uint8)slot);
				break;
			}
			}
			state.figure = figure;
			ps5log::Line("[usb] {}: {} on {}", Name(device), figure, Labels(device)[slot]);
			return {};
		}
	}

	const char* Name(Device device)
	{
		switch (device)
		{
		// tr: the toys' portals, by the names their makers gave them in the language
		case Device::Skylanders: return Tr("Skylanders Portal of Power");
		case Device::Infinity: return Tr("Disney Infinity Base");
		case Device::Dimensions: return Tr("LEGO Dimensions Toypad");
		}
		return "";
	}

	std::string Folder(Device device)
	{
		const char* name = device == Device::Skylanders ? "skylanders" : device == Device::Infinity ? "infinity" : "dimensions";
		return std::string(PS5CEMU_DATA) + "/figures/" + name;
	}

	bool Enabled(Device device)
	{
		return Switch(device).GetValue();
	}

	void SetEnabled(Device device, bool on)
	{
		Switch(device) = on;
		GetConfigHandle().Save();
		ps5log::Line("[usb] {}: {} (from the next game started)", Name(device), on ? "on" : "off");
	}

	bool Plugged(Device device)
	{
		std::lock_guard lock(s_mutex);
		return s_plugged[Index(device)];
	}

	std::vector<std::string> Figures(Device device)
	{
		std::lock_guard lock(s_mutex);
		return s_figures[Index(device)];
	}

	std::vector<Slot> Slots(Device device)
	{
		std::lock_guard lock(s_mutex);
		std::vector<Slot> slots;
		const auto& labels = Labels(device);
		const auto& states = SlotsOf(device);
		for (size_t i = 0; i < labels.size(); i++)
			slots.push_back({Tr(labels[i]), states[i].figure});
		return slots;
	}

	void RequestFigure(Device device, size_t slot, const std::string& figure)
	{
		std::lock_guard lock(s_mutex);
		s_requests.push_back({device, slot, figure});
	}

	void Refresh()
	{
		std::array<std::vector<std::string>, 3> found;
		for (Device device : kDevices)
		{
			std::error_code ec;
			const fs::path folder = Folder(device);
			fs::create_directories(folder, ec); // so it is there to put dumps in
			for (const auto& entry : fs::directory_iterator(folder, ec))
				if (entry.is_regular_file(ec))
					found[Index(device)].push_back(entry.path().filename().string());
			std::sort(found[Index(device)].begin(), found[Index(device)].end(), [](const std::string& a, const std::string& b) {
				return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
					[](char x, char y) { return std::tolower((unsigned char)x) < std::tolower((unsigned char)y); });
			});
		}
		std::lock_guard lock(s_mutex);
		s_figures = std::move(found);
	}

	void GameStarted()
	{
		std::lock_guard lock(s_mutex);
		for (Device device : kDevices)
			s_plugged[Index(device)] = Switch(device).GetValue();
	}

	void ServiceRequests()
	{
		std::vector<Request> requests;
		{
			std::lock_guard lock(s_mutex);
			requests.swap(s_requests);
		}
		for (const auto& request : requests)
		{
			std::string error;
			{
				std::lock_guard lock(s_mutex);
				auto& slots = SlotsOf(request.device);
				if (request.slot >= slots.size() || !s_plugged[Index(request.device)])
					continue;
				// the device's own lock is taken inside its calls; s_mutex only guards the slots' names
				SlotState state = slots[request.slot];
				Remove(request.device, request.slot, state);
				if (!request.figure.empty())
					error = Place(request.device, request.slot, request.figure, state);
				slots[request.slot] = state;
				s_error = error;
			}
			if (!error.empty())
				ps5log::Line("[usb] {}: {}", Name(request.device), error);
		}
	}

	std::string LastError()
	{
		std::lock_guard lock(s_mutex);
		return s_error;
	}
}
