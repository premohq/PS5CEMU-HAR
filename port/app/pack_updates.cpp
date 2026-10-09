// SPDX-License-Identifier: GPL-3.0-or-later
#include "pack_updates.h"
#include "lang.h"
#include "paths.h"
#include "../ps5/log.h"

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <zip.h>

#include <arpa/inet.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

// The PS5's network library, as app/updates.cpp uses it: getaddrinfo finds nothing on the console,
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

namespace ps5packs
{
	namespace
	{
		constexpr const char* kLatest = "https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest";
		// GitHub's answer, the release's file, and where github.com sends its download
		constexpr const char* kHosts[] = {"api.github.com", "github.com", "release-assets.githubusercontent.com",
			"objects.githubusercontent.com"};
		constexpr uint64_t kMaxDownload = 256ull << 20;

		std::mutex s_mutex;
		Status s_status; // under s_mutex
		std::thread s_thread;
		std::atomic<bool> s_stop{false};

		fs::path PacksFolder()
		{
			return fs::path(ps5paths::kRoot) / "graphicPacks" / "downloadedGraphicPacks";
		}

		void Set(Status::State state, const std::string& message = {})
		{
			std::lock_guard lock(s_mutex);
			s_status.state = state;
			if (!message.empty() || state == Status::State::Failed)
				s_status.message = message;
		}

		// "host:443:address" for each host the PS5's resolver finds
		curl_slist* Resolve()
		{
			curl_slist* list = nullptr;
			const int pool = sceNetPoolCreate("ps5cemu-packs", 16 * 1024, 0);
			if (pool < 0)
				return nullptr;
			for (const char* host : kHosts)
			{
				uint32_t address = 0;
				const int resolver = sceNetResolverCreate("ps5cemu-packs", pool, 0);
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

		CURL* Session(curl_slist* resolve, curl_slist* headers)
		{
			CURL* curl = curl_easy_init();
			if (!curl)
				return nullptr;
			static const std::string ca = ps5paths::Assets() + "/cacert.pem"; // the console has no store of its own
			if (resolve)
				curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
			if (headers)
				curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_USERAGENT, "PS5CEMU-HAR/" PS5CEMU_VERSION);
			curl_easy_setopt(curl, CURLOPT_CAINFO, ca.c_str());
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			return curl;
		}

		size_t Collect(char* data, size_t size, size_t count, void* out)
		{
			auto* body = static_cast<std::string*>(out);
			if (body->size() > 1024 * 1024)
				return 0; // a release's details are a few kilobytes
			body->append(data, size * count);
			return size * count;
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

		int Progress(void*, curl_off_t total, curl_off_t received, curl_off_t, curl_off_t)
		{
			{
				std::lock_guard lock(s_mutex);
				s_status.total = total > 0 ? (uint64_t)total : s_status.total;
				s_status.received = (uint64_t)received;
			}
			return s_stop ? 1 : 0;
		}

		// The release's packs unpacked into folder (made new): false, with a reason, when it is not
		// such a zip or a file cannot be written
		bool Unpack(const fs::path& archive, const fs::path& folder, std::string& error)
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
			int files = 0;
			for (zip_int64_t i = 0; ok && i < entries && !s_stop; i++)
			{
				const char* name = zip_get_name(zip, (zip_uint64_t)i, 0);
				if (!name)
					continue;
				const std::string entry = name;
				// nothing outside the folder, whatever the archive says
				if (entry.empty() || entry[0] == '/' || entry.find("..") != std::string::npos || entry.find('\\') != std::string::npos)
					continue;
				const fs::path path = folder / entry;
				std::error_code ec;
				if (entry.back() == '/')
				{
					fs::create_directories(path, ec);
					continue;
				}
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
				if (ok && !out)
				{
					error = ps5lang::TrF("{0} could not be written", entry);
					ok = false;
				}
				files++;
			}
			zip_close(zip);
			if (ok && s_stop)
			{
				error = ps5lang::Tr("stopped for a game");
				ok = false;
			}
			if (ok && files == 0)
			{
				error = ps5lang::Tr("the download holds no packs");
				ok = false;
			}
			return ok;
		}

		void Run()
		{
			sceNetInit();
			curl_global_init(CURL_GLOBAL_DEFAULT);
			curl_slist* resolve = Resolve();
			if (!resolve)
			{
				Set(Status::State::Failed, ps5lang::Tr("GitHub could not be found: is the PS5 online?"));
				return;
			}
			curl_slist* headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
			std::string body;
			CURL* curl = Session(resolve, headers);
			curl_easy_setopt(curl, CURLOPT_URL, kLatest);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
			CURLcode result = curl_easy_perform(curl);
			long http = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
			curl_easy_cleanup(curl);
			curl_slist_free_all(headers);
			rapidjson::Document json;
			std::string tag, url;
			if (result == CURLE_OK && http == 200 && !json.Parse(body.c_str()).HasParseError() && json.IsObject() &&
				json.HasMember("tag_name") && json["tag_name"].IsString() && json.HasMember("assets") && json["assets"].IsArray())
			{
				tag = json["tag_name"].GetString();
				for (const auto& asset : json["assets"].GetArray())
					if (asset.IsObject() && asset.HasMember("browser_download_url") && asset["browser_download_url"].IsString())
					{
						const std::string candidate = asset["browser_download_url"].GetString();
						if (candidate.size() > 4 && candidate.compare(candidate.size() - 4, 4, ".zip") == 0)
							url = candidate;
					}
			}
			if (tag.empty() || url.empty())
			{
				curl_slist_free_all(resolve);
				ps5log::Line("[packs] no release from GitHub ({}, HTTP {})", curl_easy_strerror(result), http);
				Set(Status::State::Failed, ps5lang::Tr("GitHub did not answer with a release"));
				return;
			}
			{
				std::lock_guard lock(s_mutex);
				s_status.version = tag;
			}
			const std::string installed = InstalledVersion();
			if (!Newer(tag, installed))
			{
				curl_slist_free_all(resolve);
				ps5log::Line("[packs] {} is the latest; {} installed", tag, installed);
				Set(Status::State::UpToDate);
				return;
			}

			ps5log::Line("[packs] downloading {} ({} installed) from {}", tag, installed.empty() ? "none" : installed, url);
			Set(Status::State::Downloading);
			const fs::path archive = fs::path(ps5paths::kCache) / "graphicPacks-download.zip";
			std::error_code ec;
			fs::create_directories(archive.parent_path(), ec);
			Download download;
			download.file.open(archive, std::ios::binary | std::ios::trunc);
			curl = Session(resolve, nullptr);
			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Write);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
			result = curl_easy_perform(curl);
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
			curl_easy_cleanup(curl);
			curl_slist_free_all(resolve);
			download.file.close();
			if (result != CURLE_OK || http != 200 || download.written == 0)
			{
				fs::remove(archive, ec);
				ps5log::Line("[packs] download of {} failed ({}, HTTP {})", tag, s_stop ? "stopped for a game" : curl_easy_strerror(result), http);
				Set(Status::State::Failed, s_stop ? ps5lang::Tr("Stopped for a game") : ps5lang::Tr("The download did not finish"));
				return;
			}

			// unpacked beside the installed packs, which make way only once it is whole
			Set(Status::State::Installing);
			const fs::path target = PacksFolder();
			const fs::path fresh = target.string() + ".new", old = target.string() + ".old";
			fs::remove_all(fresh, ec);
			std::string error;
			if (!Unpack(archive, fresh, error))
			{
				fs::remove_all(fresh, ec);
				fs::remove(archive, ec);
				ps5log::Line("[packs] {} could not be installed: {}", tag, error);
				Set(Status::State::Failed, ps5lang::TrF("It could not be installed: {0}", error));
				return;
			}
			std::ofstream(fresh / "version.txt") << tag;
			fs::remove_all(old, ec);
			fs::rename(target, old, ec); // none yet is fine
			fs::rename(fresh, target, ec);
			if (ec)
			{
				fs::rename(old, target, ec); // put the packs back as they were
				ps5log::Line("[packs] {} could not take the old packs' place", tag);
				Set(Status::State::Failed, ps5lang::Tr("The new packs could not take the old ones' place"));
				return;
			}
			fs::remove_all(old, ec);
			fs::remove(archive, ec);
			ps5log::Line("[packs] {} installed ({} MB)", tag, download.written >> 20);
			Set(Status::State::Done);
		}
	}

	std::string InstalledVersion()
	{
		std::ifstream file(PacksFolder() / "version.txt");
		std::string version;
		std::getline(file, version);
		return version;
	}

	bool Newer(const std::string& candidate, const std::string& than)
	{
		auto number = [](const std::string& text) {
			long value = -1;
			for (size_t i = 0; i < text.size(); i++)
				if (std::isdigit((unsigned char)text[i]))
				{
					value = std::strtol(text.c_str() + i, nullptr, 10);
					break;
				}
			return value;
		};
		return number(candidate) > number(than);
	}

	void Start()
	{
		{
			std::lock_guard lock(s_mutex);
			if (s_status.state == Status::State::Checking || s_status.state == Status::State::Downloading ||
				s_status.state == Status::State::Installing)
				return;
			s_status = {};
			s_status.state = Status::State::Checking;
		}
		if (s_thread.joinable())
			s_thread.join();
		s_stop = false;
		s_thread = std::thread(Run);
	}

	Status GetStatus()
	{
		std::lock_guard lock(s_mutex);
		return s_status;
	}

	void Stop()
	{
		s_stop = true;
		if (s_thread.joinable())
			s_thread.join();
	}
}
