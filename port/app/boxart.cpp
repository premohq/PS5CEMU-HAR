// SPDX-License-Identifier: GPL-3.0-or-later
#include "boxart.h"
#include "paths.h"
#include "../ps5/log.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <png.h>
#include <sys/socket.h>
#include <turbojpeg.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;

// The PS5's network library (libSceNet): a title starts it itself, where a payload finds it started
// by the system, and its resolver is what libc's getaddrinfo asks too
extern "C"
{
	int sceNetInit(void);
	int sceNetPoolCreate(const char* name, int size, int flags);
	int sceNetPoolDestroy(int pool);
	int sceNetResolverCreate(const char* name, int pool, int flags);
	int sceNetResolverStartNtoa(int resolver, const char* host, uint32_t* address, int timeoutUs, int retries, int flags);
	int sceNetResolverDestroy(int resolver);
}

namespace ps5boxart
{
	namespace
	{
		constexpr size_t kMaxDownload = 8 * 1024 * 1024;
		constexpr const char* kHost = "art.gametdb.com";
		constexpr int kMaxDecoded = 8192; // a side, before scaling

		std::mutex s_mutex;
		std::condition_variable s_wake;
		std::deque<std::pair<System, std::string>> s_queue; // under s_mutex
		std::set<std::string> s_queued;						 // folder/ID, under s_mutex
		bool s_started = false;
		std::atomic<uint32_t> s_arrivals{0};
		std::atomic<bool> s_enabled{true};
		std::atomic<bool> s_stopped{false}; // for good: a game is starting
		std::atomic<bool> s_busy{false};	// a cover is being fetched
		std::atomic<int> s_answered{-1};	// Answered()

		const char* Folder(System system)
		{
			return system == System::WiiU ? "wiiu" : system == System::Nds ? "ds" : "3ds";
		}

		std::string Base(System system, const std::string& id)
		{
			return fmt::format("{}/boxart/{}/{}", ps5paths::kCovers, Folder(system), id);
		}

		bool Valid(const std::string& id)
		{
			if (id.size() != 4 && id.size() != 6)
				return false;
			return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
		}

		// GameTDB's region folders to try, the game's own first: the ID's fourth letter is its region
		std::vector<std::string> Regions(const std::string& id)
		{
			std::string own;
			switch (id[3])
			{
			case 'E': own = "US"; break;
			case 'J': own = "JA"; break;
			case 'K': own = "KO"; break;
			case 'D': own = "DE"; break;
			case 'F': own = "FR"; break;
			case 'S': own = "ES"; break;
			case 'I': own = "IT"; break;
			case 'H': own = "NL"; break;
			case 'U': own = "AU"; break;
			case 'R': own = "RU"; break;
			case 'C':
			case 'W':
			case 'Z': own = "ZH"; break;
			default: own = "EN"; break;
			}
			std::vector<std::string> regions{own};
			for (const char* other : {"US", "EN", "JA", "OTHER"})
				if (own != other)
					regions.emplace_back(other);
			return regions;
		}

		size_t Receive(char* data, size_t size, size_t count, void* user)
		{
			auto* out = static_cast<std::vector<uint8_t>*>(user);
			const size_t bytes = size * count;
			if (out->size() + bytes > kMaxDownload)
				return 0; // too big for a cover: stop
			out->insert(out->end(), data, data + bytes);
			return bytes;
		}

		// The HTTP status, or -1 when there was no answer (no network)
		long Get(CURL* curl, const std::string& url, std::vector<uint8_t>& out)
		{
			out.clear();
			char error[CURL_ERROR_SIZE] = {};
			curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &Receive);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
			curl_easy_setopt(curl, CURLOPT_USERAGENT, "PS5CEMU-HAR");
			const CURLcode result = curl_easy_perform(curl);
			if (result != CURLE_OK)
			{
				ps5log::Line("[boxart] {}: curl error {} ({})", url, (int)result, error[0] ? error : curl_easy_strerror(result));
				return -1;
			}
			long status = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
			return status;
		}

		bool DecodeJpeg(const std::vector<uint8_t>& data, std::vector<uint8_t>& rgba, int& width, int& height)
		{
			tjhandle decoder = tjInitDecompress();
			if (!decoder)
				return false;
			int subsampling = 0, colourspace = 0;
			bool ok = tjDecompressHeader3(decoder, data.data(), (unsigned long)data.size(), &width, &height, &subsampling, &colourspace) == 0 &&
				width > 0 && height > 0 && width <= kMaxDecoded && height <= kMaxDecoded;
			if (ok)
			{
				rgba.resize((size_t)width * height * 4);
				ok = tjDecompress2(decoder, data.data(), (unsigned long)data.size(), rgba.data(), width, 0, height, TJPF_RGBA, TJFLAG_ACCURATEDCT) == 0;
			}
			tjDestroy(decoder);
			return ok;
		}

		bool DecodePng(const std::vector<uint8_t>& data, std::vector<uint8_t>& rgba, int& width, int& height)
		{
			png_image image{};
			image.version = PNG_IMAGE_VERSION;
			if (!png_image_begin_read_from_memory(&image, data.data(), data.size()))
				return false;
			if (image.width == 0 || image.height == 0 || image.width > kMaxDecoded || image.height > kMaxDecoded)
			{
				png_image_free(&image);
				return false;
			}
			image.format = PNG_FORMAT_RGBA;
			rgba.resize(PNG_IMAGE_SIZE(image));
			if (!png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr))
			{
				png_image_free(&image);
				return false;
			}
			width = (int)image.width;
			height = (int)image.height;
			return true;
		}

		// Scaled down to fit kMaxWidth x kMaxHeight, each pixel the average of those it covers
		std::vector<uint8_t> Fit(const std::vector<uint8_t>& rgba, int width, int height, int& outWidth, int& outHeight)
		{
			const double scale = std::min({1.0, (double)kMaxWidth / width, (double)kMaxHeight / height});
			outWidth = std::max(1, (int)(width * scale + 0.5));
			outHeight = std::max(1, (int)(height * scale + 0.5));
			if (outWidth == width && outHeight == height)
				return rgba;
			std::vector<uint8_t> out((size_t)outWidth * outHeight * 4);
			for (int y = 0; y < outHeight; y++)
			{
				const int y0 = (int)((int64_t)y * height / outHeight), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * height / outHeight));
				for (int x = 0; x < outWidth; x++)
				{
					const int x0 = (int)((int64_t)x * width / outWidth), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * width / outWidth));
					uint32_t sum[4] = {};
					for (int sy = y0; sy < y1; sy++)
						for (int sx = x0; sx < x1; sx++)
							for (int c = 0; c < 4; c++)
								sum[c] += rgba[((size_t)sy * width + sx) * 4 + c];
					const uint32_t count = (uint32_t)((y1 - y0) * (x1 - x0));
					for (int c = 0; c < 4; c++)
						out[((size_t)y * outWidth + x) * 4 + c] = (uint8_t)((sum[c] + count / 2) / count);
				}
			}
			return out;
		}

		// A 32-bit top-down TGA, as the launcher reads them (ui/images.cpp), written whole
		// or not at all
		bool WriteTga(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height)
		{
			std::vector<uint8_t> tga(18 + (size_t)width * height * 4);
			tga[2] = 2;
			tga[12] = width & 255, tga[13] = width >> 8, tga[14] = height & 255, tga[15] = height >> 8;
			tga[16] = 32, tga[17] = 0x28;
			for (size_t i = 0; i < (size_t)width * height; i++)
			{
				tga[18 + i * 4 + 0] = rgba[i * 4 + 2];
				tga[18 + i * 4 + 1] = rgba[i * 4 + 1];
				tga[18 + i * 4 + 2] = rgba[i * 4 + 0];
				tga[18 + i * 4 + 3] = rgba[i * 4 + 3];
			}
			const std::string temporary = path + ".tmp";
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				out.write((const char*)tga.data(), (std::streamsize)tga.size());
				if (!out)
					return false;
			}
			std::error_code ec;
			fs::rename(temporary, path, ec);
			return !ec;
		}

		// One game's cover: GameTDB's large one in each region, then its small one. False when there
		// is no network.
		bool FetchOne(CURL* curl, System system, const std::string& id)
		{
			const std::string base = Base(system, id);
			std::error_code ec;
			fs::create_directories(fs::path(base).parent_path(), ec);
			std::vector<uint8_t> data;
			for (const char* size : {"coverHQ", "cover"})
				for (const std::string& region : Regions(id))
				{
					const std::string url = fmt::format("http://art.gametdb.com/{}/{}/{}/{}.jpg", Folder(system), size, region, id);
					const long status = Get(curl, url, data);
					if (status < 0)
						return false;
					if (status != 200 || data.size() < 8)
						continue;
					std::vector<uint8_t> rgba;
					int width = 0, height = 0;
					const bool png = std::memcmp(data.data(), "\x89PNG", 4) == 0;
					if (!(png ? DecodePng(data, rgba, width, height) : DecodeJpeg(data, rgba, width, height)))
					{
						ps5log::Line("[boxart] {} is not an image GameTDB's cover should be", url);
						continue;
					}
					int fitWidth, fitHeight;
					const std::vector<uint8_t> fitted = Fit(rgba, width, height, fitWidth, fitHeight);
					if (WriteTga(base + ".tga", fitted, fitWidth, fitHeight))
					{
						s_arrivals++;
						ps5log::Line("[boxart] {} {}: {}x{} from {}", Folder(system), id, fitWidth, fitHeight, url);
					}
					return true;
				}
			// GameTDB has none: not asked for again
			std::ofstream(base + ".none").put('\n');
			ps5log::Line("[boxart] {} {}: GameTDB has no cover", Folder(system), id);
			return true;
		}

		// GameTDB's address through the PS5's resolver, as "host:80:address" for curl; empty when
		// it cannot be found
		std::string ResolveWithSystem()
		{
			const int pool = sceNetPoolCreate("ps5cemu-boxart", 16 * 1024, 0);
			if (pool < 0)
			{
				ps5log::Line("[boxart] sceNetPoolCreate failed: {:#x}", (uint32_t)pool);
				return {};
			}
			uint32_t address = 0;
			const int resolver = sceNetResolverCreate("ps5cemu-boxart", pool, 0);
			const int result = resolver >= 0 ? sceNetResolverStartNtoa(resolver, kHost, &address, 0, 0, 0) : resolver;
			if (resolver >= 0)
				sceNetResolverDestroy(resolver);
			sceNetPoolDestroy(pool);
			if (result < 0 || address == 0)
			{
				ps5log::Line("[boxart] the PS5's resolver could not find {}: {:#x}", kHost, (uint32_t)result);
				return {};
			}
			char text[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &address, text, sizeof(text));
			return fmt::format("{}:80:{}", kHost, text);
		}

		// The network, before the first download: libSceNet started, and GameTDB's address found by
		// libc's getaddrinfo (curl's way) or else by the PS5's resolver, which curl is then given.
		// Null when there is no way to GameTDB.
		curl_slist* PrepareNetwork()
		{
			const int net = sceNetInit();
			curl_global_init(CURL_GLOBAL_DEFAULT);
			addrinfo hints{};
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_STREAM;
			addrinfo* found = nullptr;
			const int lookup = getaddrinfo(kHost, "80", &hints, &found);
			if (lookup == 0 && found)
			{
				char text[INET_ADDRSTRLEN] = {};
				inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr, text, sizeof(text));
				ps5log::Line("[boxart] network ready (sceNetInit {:#x}): {} is {}", (uint32_t)net, kHost, text);
				freeaddrinfo(found);
				return nullptr;
			}
			ps5log::Line("[boxart] getaddrinfo({}) failed ({}, sceNetInit {:#x}): trying the PS5's resolver", kHost, lookup, (uint32_t)net);
			const std::string entry = ResolveWithSystem();
			if (entry.empty())
				return nullptr;
			ps5log::Line("[boxart] {} through the PS5's resolver", entry);
			return curl_slist_append(nullptr, entry.c_str());
		}

		void Worker()
		{
			curl_slist* resolved = PrepareNetwork();
			CURL* curl = curl_easy_init();
			if (curl && resolved)
				curl_easy_setopt(curl, CURLOPT_RESOLVE, resolved);
			for (;;)
			{
				std::pair<System, std::string> item;
				{
					std::unique_lock lock(s_mutex);
					s_wake.wait(lock, [] { return !s_queue.empty(); });
					item = std::move(s_queue.front());
					s_queue.pop_front();
				}
				if (!s_enabled || s_stopped || !curl || !Path(item.first, item.second).empty())
					continue;
				std::error_code ec;
				if (fs::exists(Base(item.first, item.second) + ".none", ec))
					continue;
				s_busy = true;
				const bool fetched = FetchOne(curl, item.first, item.second);
				s_busy = false;
				s_answered = fetched ? 1 : 0;
				if (!fetched)
				{
					// no network: the rest wait for the next start
					ps5log::Line("[boxart] GameTDB did not answer: no covers fetched until PS5CEMU-HAR starts again");
					std::lock_guard lock(s_mutex);
					s_queue.clear();
				}
			}
		}
	}

	std::string Path(System system, const std::string& id)
	{
		if (!Valid(id))
			return {};
		const std::string path = Base(system, id) + ".tga";
		std::error_code ec;
		return fs::exists(path, ec) ? path : std::string();
	}

	void Fetch(System system, const std::vector<std::string>& ids)
	{
		if (!s_enabled || s_stopped)
			return;
		std::lock_guard lock(s_mutex);
		size_t queued = 0;
		for (const std::string& id : ids)
		{
			if (!Valid(id) || !s_queued.insert(fmt::format("{}/{}", Folder(system), id)).second)
				continue;
			s_queue.emplace_back(system, id);
			queued++;
		}
		ps5log::Line("[boxart] {}: {} games have a box ID, {} new to look for", Folder(system), ids.size(), queued);
		if (!s_started && !s_queue.empty())
		{
			s_started = true;
			std::thread(Worker).detach();
		}
		s_wake.notify_one();
	}

	uint32_t Arrivals()
	{
		return s_arrivals;
	}

	int Answered()
	{
		return s_answered;
	}

	void SetEnabled(bool enabled)
	{
		s_enabled = enabled;
	}

	void Stop()
	{
		s_stopped = true;
		size_t dropped;
		{
			std::lock_guard lock(s_mutex);
			dropped = s_queue.size();
			s_queue.clear();
		}
		// a cover on its way is let finish (30 s at most, most take well under one), so no download
		// or file write runs beside the game; past 5 s the game starts anyway
		int waited = 0;
		for (; s_busy && waited < 250; waited++)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		ps5log::Line("[boxart] stopped for the game: {} covers left for the next start{}", dropped,
			s_busy ? "; one download still running" : waited ? fmt::format("; waited {} ms for one", waited * 20) : std::string());
	}

	bool ImageSize(const std::string& path, int& width, int& height)
	{
		uint8_t header[18];
		std::ifstream in(path, std::ios::binary);
		if (!in.read((char*)header, sizeof(header)))
			return false;
		width = header[12] | header[13] << 8;
		height = header[14] | header[15] << 8;
		return width > 0 && height > 0;
	}
}
