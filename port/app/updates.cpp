// SPDX-License-Identifier: GPL-3.0-or-later
#include "updates.h"
#include "boxart.h"
#include "pack_updates.h"
#include "lang.h"
#include "paths.h"
#include "../ps5/kernel.h"
#include "../ps5/log.h"

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <zip.h>

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

// The PS5's network library, as app/boxart.cpp uses it: getaddrinfo finds nothing on the console,
// so each host's address comes from the PS5's resolver and is given to curl
extern "C"
{
	int sceNetInit(void);
	int sceNetPoolCreate(const char* name, int size, int flags);
	int sceNetPoolDestroy(int pool);
	int sceNetResolverCreate(const char* name, int pool, int flags);
	int sceNetResolverStartNtoa(int resolver, const char* host, uint32_t* address, int timeoutUs, int retries, int flags);
	int sceNetResolverDestroy(int resolver);
}

namespace fs = std::filesystem;

namespace ps5update
{
	namespace
	{
		constexpr const char* kLatest = "https://api.github.com/repos/premohq/PS5CEMU-HAR/releases/latest";
		// the releases branch: each release's ZIP (PS5CEMU-HAR-<tag>.zip, the PPSA99360 folder) and
		// SHA256SUMS for all of them
		constexpr const char* kFiles = "https://raw.githubusercontent.com/premohq/PS5CEMU-HAR/releases/";
		constexpr const char* kHosts[] = {"api.github.com", "raw.githubusercontent.com"};
		constexpr uint64_t kMaxDownload = 512ull << 20;

		std::mutex s_mutex;
		Status s_status;		  // under s_mutex
		bool s_dismissed = false; // under s_mutex
		std::thread s_thread;
		std::atomic<bool> s_stop{false};

		void Set(Status::State state, const std::string& message = {})
		{
			std::lock_guard lock(s_mutex);
			s_status.state = state;
			if (!message.empty() || state == Status::State::Failed)
				s_status.message = message;
		}

		void Fail(const std::string& message)
		{
			ps5log::Line("[update] {}", message);
			Set(Status::State::Failed, message);
		}

		// "2.0.0d", or "v2.0.0d": numbers, then a letter (none is before "a")
		using Version = std::tuple<int, int, int, int>;
		bool Parse(std::string text, Version& out)
		{
			if (!text.empty() && (text[0] == 'v' || text[0] == 'V'))
				text.erase(0, 1);
			int major = 0, minor = 0, patch = 0, used = 0;
			if (std::sscanf(text.c_str(), "%d.%d.%d%n", &major, &minor, &patch, &used) != 3)
				return false;
			const std::string rest = text.substr(used);
			const int letter = rest.empty() ? 0 : std::tolower((unsigned char)rest[0]) - 'a' + 1;
			out = {major, minor, patch, letter};
			return true;
		}

		// "host:443:address" for each host the PS5's resolver finds
		curl_slist* Resolve()
		{
			curl_slist* list = nullptr;
			const int pool = sceNetPoolCreate("ps5cemu-update", 16 * 1024, 0);
			if (pool < 0)
				return nullptr;
			for (const char* host : kHosts)
			{
				uint32_t address = 0;
				const int resolver = sceNetResolverCreate("ps5cemu-update", pool, 0);
				const int result = resolver >= 0 ? sceNetResolverStartNtoa(resolver, host, &address, 0, 0, 0) : resolver;
				if (resolver >= 0)
					sceNetResolverDestroy(resolver);
				if (result < 0 || address == 0)
					continue;
				char text[INET_ADDRSTRLEN] = {};
				inet_ntop(AF_INET, &address, text, sizeof(text));
				list = curl_slist_append(list, fmt::format("{}:443:{}", host, text).c_str());
			}
			sceNetPoolDestroy(pool);
			return list;
		}

		int Progress(void*, curl_off_t total, curl_off_t received, curl_off_t, curl_off_t)
		{
			{
				std::lock_guard lock(s_mutex);
				s_status.total = total > 0 ? (uint64_t)total : s_status.total;
				s_status.received = (uint64_t)received;
			}
			return s_stop ? 1 : 0; // a game is starting: cut short
		}

		CURL* Session(curl_slist* resolve, const char* url)
		{
			CURL* curl = curl_easy_init();
			if (!curl)
				return nullptr;
			static const std::string ca = ps5paths::Assets() + "/cacert.pem"; // the console has no store of its own
			curl_easy_setopt(curl, CURLOPT_URL, url);
			if (resolve)
				curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
			curl_easy_setopt(curl, CURLOPT_USERAGENT, "PS5CEMU-HAR/" PS5CEMU_VERSION);
			curl_easy_setopt(curl, CURLOPT_CAINFO, ca.c_str());
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
			return curl;
		}

		size_t Collect(char* data, size_t size, size_t count, void* out)
		{
			auto* body = static_cast<std::string*>(out);
			if (body->size() > 1024 * 1024)
				return 0; // a release's details and the sums are a few kilobytes
			body->append(data, size * count);
			return size * count;
		}

		// A small text from GitHub; false when it did not come
		bool Fetch(curl_slist* resolve, const std::string& url, std::string& body, long timeout, const char* accept = nullptr)
		{
			CURL* curl = Session(resolve, url.c_str());
			if (!curl)
				return false;
			curl_slist* headers = accept ? curl_slist_append(nullptr, accept) : nullptr;
			if (headers)
				curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
			const CURLcode result = curl_easy_perform(curl);
			long http = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
			curl_easy_cleanup(curl);
			curl_slist_free_all(headers);
			if (result != CURLE_OK || http != 200)
				ps5log::Line("[update] {}: {} (HTTP {})", url, s_stop ? "stopped for a game" : curl_easy_strerror(result), http);
			return result == CURLE_OK && http == 200;
		}

		struct Download
		{
			std::ofstream file;
			uint64_t written = 0;
		};

		size_t Write(char* data, size_t size, size_t count, void* user)
		{
			auto* download = static_cast<Download*>(user);
			const size_t bytes = size * count;
			if (download->written + bytes > kMaxDownload)
				return 0;
			download->file.write(data, (std::streamsize)bytes);
			download->written += bytes;
			return download->file ? bytes : 0;
		}

		// SHA-256 (FIPS 180-4), to check the download against the release's SHA256SUMS
		class Sha256
		{
		public:
			void Add(const uint8_t* data, size_t size)
			{
				m_length += size;
				while (size > 0)
				{
					const size_t take = std::min(size, 64 - m_used);
					std::memcpy(m_block.data() + m_used, data, take);
					m_used += take, data += take, size -= take;
					if (m_used == 64)
					{
						Compress(m_block.data());
						m_used = 0;
					}
				}
			}

			std::string Hex()
			{
				const uint64_t bits = m_length * 8;
				const uint8_t one = 0x80, zero = 0;
				Add(&one, 1);
				while (m_used != 56)
					Add(&zero, 1);
				for (int i = 7; i >= 0; i--)
				{
					const uint8_t byte = (uint8_t)(bits >> (i * 8));
					Add(&byte, 1);
				}
				std::string hex;
				for (const uint32_t word : m_state)
					hex += fmt::format("{:08x}", word);
				return hex;
			}

		private:
			static uint32_t Rotate(uint32_t value, int by) { return (value >> by) | (value << (32 - by)); }

			void Compress(const uint8_t* block)
			{
				static constexpr uint32_t kRounds[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
					0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1,
					0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
					0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354,
					0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585,
					0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee,
					0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
				uint32_t w[64];
				for (int i = 0; i < 16; i++)
					w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
				for (int i = 16; i < 64; i++)
				{
					const uint32_t s0 = Rotate(w[i - 15], 7) ^ Rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
					const uint32_t s1 = Rotate(w[i - 2], 17) ^ Rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
					w[i] = w[i - 16] + s0 + w[i - 7] + s1;
				}
				uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4], f = m_state[5], g = m_state[6],
						 h = m_state[7];
				for (int i = 0; i < 64; i++)
				{
					const uint32_t t1 = h + (Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25)) + ((e & f) ^ (~e & g)) + kRounds[i] + w[i];
					const uint32_t t2 = (Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
					h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
				}
				m_state[0] += a, m_state[1] += b, m_state[2] += c, m_state[3] += d;
				m_state[4] += e, m_state[5] += f, m_state[6] += g, m_state[7] += h;
			}

			std::array<uint32_t, 8> m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
			std::array<uint8_t, 64> m_block{};
			size_t m_used = 0;
			uint64_t m_length = 0;
		};

		std::string FileSha256(const fs::path& path)
		{
			std::ifstream file(path, std::ios::binary);
			Sha256 sha;
			std::vector<char> buffer(1 << 20);
			while (file)
			{
				file.read(buffer.data(), (std::streamsize)buffer.size());
				sha.Add(reinterpret_cast<const uint8_t*>(buffer.data()), (size_t)file.gcount());
			}
			return sha.Hex();
		}

		// The folder the app runs from, which the update replaces the files of: the one ShadowMountPlus
		// mounted it from (on a USB drive, say, with an older copy left in /data/homebrew), else the
		// usual install folder, else where the app was found. Empty for an app mounted from an image.
		fs::path AppFolder()
		{
			bool image = false;
			const std::string source = ps5paths::MountSource(&image);
			if (!source.empty())
				return source;
			if (image)
				return {};
			std::error_code ec;
			return fs::exists(ps5paths::kMountedEboot, ec) ? fs::path(ps5paths::kInstallDir) : fs::path(ps5paths::AppDir());
		}

		// The release's PPSA99360 folder unpacked into folder (made new); its files, relative to it
		bool Unpack(const fs::path& archive, const fs::path& folder, std::vector<fs::path>& files, std::string& error)
		{
			int code = 0;
			zip_t* zip = zip_open(archive.c_str(), ZIP_RDONLY, &code);
			if (!zip)
			{
				error = ps5lang::TrF("the download is not a ZIP ({0})", code);
				return false;
			}
			const zip_int64_t entries = zip_get_num_entries(zip, 0);
			std::vector<char> buffer(1 << 16);
			bool ok = entries > 0;
			for (zip_int64_t i = 0; ok && i < entries && !s_stop; i++)
			{
				const char* name = zip_get_name(zip, (zip_uint64_t)i, 0);
				if (!name)
					continue;
				std::string entry = name;
				// nothing outside the folder, whatever the archive says
				if (entry.empty() || entry[0] == '/' || entry.find("..") != std::string::npos || entry.find('\\') != std::string::npos)
					continue;
				if (entry.rfind(ps5paths::kTitleId + std::string("/"), 0) == 0)
					entry.erase(0, std::strlen(ps5paths::kTitleId) + 1);
				if (entry.empty() || entry.back() == '/')
					continue;
				const fs::path path = folder / entry;
				std::error_code ec;
				fs::create_directories(path.parent_path(), ec);
				zip_file_t* in = zip_fopen_index(zip, (zip_uint64_t)i, 0);
				std::ofstream out(path, std::ios::binary | std::ios::trunc);
				if (!in || !out)
				{
					error = ps5lang::TrF("{0} could not be unpacked", entry);
					ok = false;
				}
				for (zip_int64_t read; ok && (read = zip_fread(in, buffer.data(), buffer.size())) > 0;)
					out.write(buffer.data(), (std::streamsize)read);
				if (in)
					zip_fclose(in);
				out.close();
				if (ok && !out)
				{
					error = ps5lang::TrF("{0} could not be written (is /data full?)", entry);
					ok = false;
				}
				// readable, writable and runnable by all, as the app's files are when copied over
				fs::permissions(path, fs::perms::all, ec);
				files.push_back(entry);
			}
			zip_close(zip);
			if (ok && s_stop)
			{
				error = ps5lang::Tr("stopped for a game");
				ok = false;
			}
			if (ok && std::find(files.begin(), files.end(), fs::path("eboot.bin")) == files.end())
			{
				error = ps5lang::Tr("the download has no eboot.bin");
				ok = false;
			}
			return ok;
		}

		void RunCheck(curl_slist* resolve)
		{
			Set(Status::State::Checking);
			std::string body;
			rapidjson::Document json;
			if (!Fetch(resolve, kLatest, body, 10, "Accept: application/vnd.github+json") || json.Parse(body.c_str()).HasParseError() ||
				!json.IsObject() || !json.HasMember("tag_name") || !json["tag_name"].IsString())
			{
				// tr: why an update did not finish
				Fail(ps5lang::Tr("GitHub did not answer with a release"));
				return;
			}
			const std::string latest = json["tag_name"].GetString();
			Version mine, theirs;
			if (!Parse(PS5CEMU_VERSION, mine) || !Parse(latest, theirs))
			{
				Fail(ps5lang::TrF("The latest release's version ({0}) is not one this can read", latest));
				return;
			}
			{
				std::lock_guard lock(s_mutex);
				s_status.latest = latest;
			}
			if (theirs <= mine)
			{
				ps5log::Line("[update] {} is the latest release", Readable(PS5CEMU_VERSION));
				Set(Status::State::UpToDate);
				return;
			}
			ps5log::Line("[update] {} is out (this is {})", Readable(latest), Readable(PS5CEMU_VERSION));
			Set(Status::State::Available);
		}

		void RunInstall(curl_slist* resolve)
		{
			const std::string latest = GetStatus().latest;
			const std::string zipName = fmt::format("PS5CEMU-HAR-{}.zip", latest);
			// the release's checksum, from the sums beside it
			std::string sums, expected;
			if (!Fetch(resolve, std::string(kFiles) + "SHA256SUMS", sums, 20))
			{
				Fail(ps5lang::Tr("The release's checksums could not be downloaded"));
				return;
			}
			for (size_t start = 0; start < sums.size();)
			{
				size_t end = sums.find('\n', start);
				if (end == std::string::npos)
					end = sums.size();
				const std::string line = sums.substr(start, end - start);
				if (line.size() > 66 && line.find(zipName) != std::string::npos)
					expected = line.substr(0, 64);
				start = end + 1;
			}
			if (expected.size() != 64)
			{
				Fail(ps5lang::TrF("The releases branch has no checksum for {0}", zipName));
				return;
			}

			ps5log::Line("[update] downloading {}", zipName);
			Set(Status::State::Downloading);
			const fs::path archive = fs::path(ps5paths::kCache) / "app-update.zip";
			std::error_code ec;
			fs::create_directories(archive.parent_path(), ec);
			Download download;
			download.file.open(archive, std::ios::binary | std::ios::trunc);
			CURL* curl = Session(resolve, (std::string(kFiles) + zipName).c_str());
			if (!curl)
			{
				Fail(ps5lang::Tr("The download could not start"));
				return;
			}
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Write);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
			const CURLcode result = curl_easy_perform(curl);
			long http = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
			curl_easy_cleanup(curl);
			download.file.close();
			if (result != CURLE_OK || http != 200 || download.written == 0)
			{
				fs::remove(archive, ec);
				ps5log::Line("[update] the download of {} failed ({}, HTTP {})", zipName, s_stop ? "stopped for a game" : curl_easy_strerror(result), http);
				Set(Status::State::Failed, s_stop ? ps5lang::Tr("Stopped for a game") : ps5lang::Tr("The download did not finish: is the PS5 online?"));
				return;
			}

			Set(Status::State::Verifying);
			const std::string actual = FileSha256(archive);
			if (actual != expected)
			{
				fs::remove(archive, ec);
				Fail(ps5lang::TrF("The download is damaged (SHA-256 {0}, not {1})", actual.substr(0, 12), expected.substr(0, 12)));
				return;
			}

			// unpacked beside the app's folder (the same drive, so each file then moves into place)
			Set(Status::State::Installing);
			const fs::path app = AppFolder();
			if (app.empty())
			{
				fs::remove(archive, ec);
				Fail(ps5lang::Tr("PS5CEMU-HAR runs from an image, whose files can't be replaced: install the new release by hand"));
				return;
			}
			ps5log::Line("[update] installing in {}", app.string());
			const fs::path staging = app.string() + ".update";
			fs::remove_all(staging, ec);
			std::vector<fs::path> files;
			std::string error;
			if (!Unpack(archive, staging, files, error))
			{
				fs::remove_all(staging, ec);
				fs::remove(archive, ec);
				Fail(ps5lang::TrF("It could not be unpacked: {0}", error));
				return;
			}
			// the program last, so that until then the app is the old one whole
			std::stable_partition(files.begin(), files.end(), [](const fs::path& file) { return file != "eboot.bin"; });
			for (const fs::path& file : files)
			{
				if (!fs::exists((app / file).parent_path(), ec))
				{
					fs::create_directories((app / file).parent_path(), ec);
					fs::permissions((app / file).parent_path(), fs::perms::all, ec);
				}
				fs::rename(staging / file, app / file, ec);
				if (ec)
				{
					Fail(ps5lang::TrF("{0} could not be replaced ({1}): copy the release's PPSA99360 folder over by hand", file.string(), ec.message()));
					return;
				}
			}
			fs::remove_all(staging, ec);
			fs::remove(archive, ec);
			ps5log::Line("[update] {} installed in {} ({} files, {} MB)", Readable(latest), app.string(), files.size(), download.written >> 20);
			Set(Status::State::Ready);
		}

		void Run(bool install)
		{
			sceNetInit();
			curl_global_init(CURL_GLOBAL_DEFAULT);
			curl_slist* resolve = Resolve();
			if (!resolve)
			{
				ps5log::Line("[update] GitHub could not be found: no update check");
				Set(Status::State::Failed, ps5lang::Tr("GitHub could not be found: is the PS5 online?"));
				return;
			}
			if (install)
				RunInstall(resolve);
			else
				RunCheck(resolve);
			curl_slist_free_all(resolve);
		}

		bool Busy(Status::State state)
		{
			return state == Status::State::Checking || state == Status::State::Downloading || state == Status::State::Verifying ||
				state == Status::State::Installing;
		}

		void Launch(bool install)
		{
			if (s_thread.joinable())
				s_thread.join();
			s_stop = false;
			s_thread = std::thread(Run, install);
		}
	}

	void Start()
	{
		{
			std::lock_guard lock(s_mutex);
			if (s_status.state != Status::State::Idle)
				return;
			s_status.state = Status::State::Checking;
		}
		Launch(false);
	}

	void Check()
	{
		{
			std::lock_guard lock(s_mutex);
			if (Busy(s_status.state) || s_status.state == Status::State::Ready)
				return;
			s_status = {};
			s_status.state = Status::State::Checking;
			s_dismissed = false;
		}
		Launch(false);
	}

	void Install()
	{
		{
			std::lock_guard lock(s_mutex);
			if (s_status.state != Status::State::Available)
				return;
			s_status.state = Status::State::Downloading;
			s_status.installing = true;
			s_status.received = s_status.total = 0;
		}
		Launch(true);
	}

	Status GetStatus()
	{
		std::lock_guard lock(s_mutex);
		return s_status;
	}

	bool Prompting()
	{
		std::lock_guard lock(s_mutex);
		if (s_status.installing)
			return true;
		return s_status.state == Status::State::Available && !s_dismissed;
	}

	void Dismiss()
	{
		std::lock_guard lock(s_mutex);
		if (Busy(s_status.state) || s_status.state == Status::State::Ready)
			return; // an install under way, or done, is not put off
		s_dismissed = true;
		s_status.installing = false;
	}

	void Restart()
	{
		// nothing else on the network or writing, as RestartToLibrary does
		ps5boxart::Stop();
		ps5packs::Stop();
		// the program just installed: where ShadowMountPlus mounted the app from, as AppFolder()
		const std::string eboot = (AppFolder().empty() ? fs::path(ps5paths::kInstallDir) : AppFolder()).string() + "/eboot.bin";
		const int result = sceSystemServiceLoadExec(eboot.c_str(), nullptr);
		// it does not come back when it works; allow for one that returns before ending the process
		if (result == 0)
			for (int i = 0; i < 100; i++)
				sceKernelUsleep(100000);
		ps5log::Line("[update] LoadExec({}) returned {:#x}", eboot, (uint32_t)result);
	}

	void Stop()
	{
		s_stop = true;
		if (s_thread.joinable())
			s_thread.join();
	}
}
