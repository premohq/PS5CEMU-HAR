// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: what melonDS's core asks of its frontend (melonDS::Platform, Platform.h), on the PS5:
// files through the C library (local files are melonDS's data folder's, kRoot), threads, semaphores
// and mutexes from the standard library, the clock, the game's save handed to the game's loop
// (core.cpp), the microphone as noise while R3 is held (input.cpp), and its log in the boot log. The
// DS's wireless (local play, the internet), its cameras, the DSi's AAC decoder and the Slot-2 add-ons
// are not there: what they read comes back empty.

#include "frontend.h"
#include "melonds.h"
#include "../ps5/log.h"

#include "Platform.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>

namespace melonDS::Platform
{
	namespace
	{
		const auto s_start = std::chrono::steady_clock::now();

		bool Exists(const std::string& path)
		{
			std::error_code ec;
			return std::filesystem::is_regular_file(path, ec);
		}

		// fopen's mode for melonDS's, as its own frontend chooses it
		std::string ModeString(FileMode mode, bool exists)
		{
			char access = 'w';
			if (mode & FileMode::Append)
				access = 'a';
			else if (!(mode & FileMode::Write))
				access = 'r';
			else if (mode & FileMode::NoCreate)
				access = 'r';
			else if ((mode & FileMode::Preserve) && exists)
				access = 'r';
			std::string text(1, access);
			if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite)
				text += '+';
			if (!(mode & FileMode::Text))
				text += 'b';
			return text;
		}

		FILE* Std(FileHandle* file)
		{
			return reinterpret_cast<FILE*>(file);
		}

		struct ThreadHandle
		{
			std::thread thread;
		};

		struct SemaphoreHandle
		{
			std::mutex mutex;
			std::condition_variable posted;
			int count = 0;
		};

		struct MutexHandle
		{
			std::mutex mutex;
		};
	}

	void SignalStop(StopReason reason, void*)
	{
		ps5melonds::Stopped((int)reason);
	}

	std::string GetLocalFilePath(const std::string& filename)
	{
		if (!filename.empty() && filename[0] == '/')
			return filename;
		return std::string(ps5melonds::kRoot) + "/" + filename;
	}

	FileHandle* OpenFile(const std::string& path, FileMode mode)
	{
		if ((mode & (FileMode::ReadWrite | FileMode::Append)) == FileMode::None)
		{
			Log(LogLevel::Error, "Attempted to open \"%s\" in neither read nor write mode (FileMode 0x%x)\n", path.c_str(), mode);
			return nullptr;
		}
		const bool exists = Exists(path);
		if ((mode & FileMode::NoCreate) && !exists)
			return nullptr;
		FILE* file = std::fopen(path.c_str(), ModeString(mode, exists).c_str());
		return reinterpret_cast<FileHandle*>(file);
	}

	FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
	{
		return OpenFile(GetLocalFilePath(path), mode);
	}

	bool FileExists(const std::string& name)
	{
		return Exists(name);
	}

	bool LocalFileExists(const std::string& name)
	{
		return Exists(GetLocalFilePath(name));
	}

	bool CheckFileWritable(const std::string& filepath)
	{
		FileHandle* file = OpenFile(filepath, Exists(filepath) ? FileMode::Append : FileMode::Write);
		if (!file)
			return false;
		CloseFile(file);
		return true;
	}

	bool CheckLocalFileWritable(const std::string& filepath)
	{
		return CheckFileWritable(GetLocalFilePath(filepath));
	}

	bool CloseFile(FileHandle* file)
	{
		return std::fclose(Std(file)) == 0;
	}

	bool IsEndOfFile(FileHandle* file)
	{
		return std::feof(Std(file)) != 0;
	}

	bool FileReadLine(char* str, int count, FileHandle* file)
	{
		return std::fgets(str, count, Std(file)) != nullptr;
	}

	u64 FilePosition(FileHandle* file)
	{
		return (u64)std::ftell(Std(file));
	}

	bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
	{
		const int whence = origin == FileSeekOrigin::Start ? SEEK_SET : origin == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;
		return std::fseek(Std(file), (long)offset, whence) == 0;
	}

	void FileRewind(FileHandle* file)
	{
		std::rewind(Std(file));
	}

	u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
	{
		return std::fread(data, size, count, Std(file));
	}

	bool FileFlush(FileHandle* file)
	{
		return std::fflush(Std(file)) == 0;
	}

	u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
	{
		return std::fwrite(data, size, count, Std(file));
	}

	u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
	{
		if (!fmt)
			return 0;
		va_list args;
		va_start(args, fmt);
		const int written = std::vfprintf(Std(file), fmt, args);
		va_end(args);
		return written < 0 ? 0 : (u64)written;
	}

	u64 FileLength(FileHandle* file)
	{
		FILE* stream = Std(file);
		const long position = std::ftell(stream);
		std::fseek(stream, 0, SEEK_END);
		const long length = std::ftell(stream);
		std::fseek(stream, position, SEEK_SET);
		return length < 0 ? 0 : (u64)length;
	}

	// melonDS's log in the boot log, its debug lines left out (they come by the thousand)
	void Log(LogLevel level, const char* fmt, ...)
	{
		if (!fmt || level == LogLevel::Debug)
			return;
		char text[1024];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(text, sizeof(text), fmt, args);
		va_end(args);
		size_t length = std::strlen(text);
		while (length && (text[length - 1] == '\n' || text[length - 1] == '\r'))
			text[--length] = 0;
		if (length)
			ps5log::Line("[melonds] {}{}", level == LogLevel::Error ? "error: " : level == LogLevel::Warn ? "warning: " : "", text);
	}

	Thread* Thread_Create(std::function<void()> func)
	{
		auto* handle = new ThreadHandle;
		handle->thread = std::thread(std::move(func));
		return reinterpret_cast<Thread*>(handle);
	}

	void Thread_Free(Thread* thread)
	{
		auto* handle = reinterpret_cast<ThreadHandle*>(thread);
		if (!handle)
			return;
		// melonDS waits for its threads before it frees them; one it did not is let go
		if (handle->thread.joinable())
			handle->thread.detach();
		delete handle;
	}

	void Thread_Wait(Thread* thread)
	{
		auto* handle = reinterpret_cast<ThreadHandle*>(thread);
		if (handle && handle->thread.joinable())
			handle->thread.join();
	}

	Semaphore* Semaphore_Create()
	{
		return reinterpret_cast<Semaphore*>(new SemaphoreHandle);
	}

	void Semaphore_Free(Semaphore* sema)
	{
		delete reinterpret_cast<SemaphoreHandle*>(sema);
	}

	void Semaphore_Reset(Semaphore* sema)
	{
		auto* handle = reinterpret_cast<SemaphoreHandle*>(sema);
		std::lock_guard lock(handle->mutex);
		handle->count = 0;
	}

	void Semaphore_Wait(Semaphore* sema)
	{
		auto* handle = reinterpret_cast<SemaphoreHandle*>(sema);
		std::unique_lock lock(handle->mutex);
		handle->posted.wait(lock, [handle] { return handle->count > 0; });
		handle->count--;
	}

	bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
	{
		auto* handle = reinterpret_cast<SemaphoreHandle*>(sema);
		std::unique_lock lock(handle->mutex);
		if (timeout_ms > 0)
			handle->posted.wait_for(lock, std::chrono::milliseconds(timeout_ms), [handle] { return handle->count > 0; });
		if (handle->count <= 0)
			return false;
		handle->count--;
		return true;
	}

	void Semaphore_Post(Semaphore* sema, int count)
	{
		auto* handle = reinterpret_cast<SemaphoreHandle*>(sema);
		{
			std::lock_guard lock(handle->mutex);
			handle->count += count;
		}
		if (count == 1)
			handle->posted.notify_one();
		else
			handle->posted.notify_all();
	}

	Mutex* Mutex_Create()
	{
		return reinterpret_cast<Mutex*>(new MutexHandle);
	}

	void Mutex_Free(Mutex* mutex)
	{
		delete reinterpret_cast<MutexHandle*>(mutex);
	}

	void Mutex_Lock(Mutex* mutex)
	{
		reinterpret_cast<MutexHandle*>(mutex)->mutex.lock();
	}

	void Mutex_Unlock(Mutex* mutex)
	{
		reinterpret_cast<MutexHandle*>(mutex)->mutex.unlock();
	}

	bool Mutex_TryLock(Mutex* mutex)
	{
		return reinterpret_cast<MutexHandle*>(mutex)->mutex.try_lock();
	}

	void Sleep(u64 usecs)
	{
		std::this_thread::sleep_for(std::chrono::microseconds(usecs));
	}

	u64 GetMSCount()
	{
		return (u64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_start).count();
	}

	u64 GetUSCount()
	{
		return (u64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - s_start).count();
	}

	void WriteNDSSave(const u8* savedata, u32 savelen, u32, u32, void*)
	{
		ps5melonds::SaveChanged(savedata, savelen);
	}

	void WriteGBASave(const u8*, u32, u32, u32, void*)
	{
		// no GBA cartridge in Slot-2
	}

	void WriteFirmware(const Firmware&, u32, u32, void*)
	{
		// the firmware's settings stay as the game started with them: the launcher's (core.cpp), or the
		// DS's own firmware.bin, which is not written to
	}

	void WriteDateTime(int, int, int, int, int, int, void*)
	{
		// the DS's clock is the PS5's, set at each start
	}

	// The DS's local wireless play: no other DS
	void MP_Begin(void*) {}
	void MP_End(void*) {}
	int MP_SendPacket(u8*, int, u64, void*) { return 0; }
	int MP_RecvPacket(u8*, u64*, void*) { return 0; }
	int MP_SendCmd(u8*, int, u64, void*) { return 0; }
	int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
	int MP_SendAck(u8*, int, u64, void*) { return 0; }
	int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
	u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }

	// The internet (Nintendo WFC): no network
	int Net_SendPacket(u8*, int, void*) { return 0; }
	int Net_RecvPacket(u8*, void*) { return 0; }

	// The DSi's cameras: black
	void Camera_Start(int, void*) {}
	void Camera_Stop(int, void*) {}
	void Camera_CaptureFrame(int, u32* frame, int width, int height, bool yuv, void*)
	{
		// black, in RGB or in YUV422 (Y 0, U and V at their middle)
		const u32 black = yuv ? 0x80008000 : 0xFF000000;
		const int words = yuv ? width * height / 2 : width * height;
		for (int i = 0; i < words; i++)
			frame[i] = black;
	}

	// The microphone: silence, or noise loud enough for the games that ask to be blown into, while R3
	// is held (input.cpp)
	void Mic_Start(void*) {}
	void Mic_Stop(void*) {}
	int Mic_ReadInput(s16* data, int maxlength, void*)
	{
		static std::minstd_rand noise(0x5D5);
		const bool blowing = ps5melonds::input::Blowing();
		for (int i = 0; i < maxlength; i++)
			data[i] = blowing ? (s16)((int)(noise() % 0x6000) - 0x3000) : 0;
		return maxlength;
	}

	// The DSi's AAC decoder (its DSP's, in HLE): none, as DS games never ask
	AACDecoder* AAC_Init() { return nullptr; }
	void AAC_DeInit(AACDecoder*) {}
	bool AAC_Configure(AACDecoder*, int, int) { return false; }
	bool AAC_DecodeFrame(AACDecoder*, const void*, int, void* output, int outputlen)
	{
		std::memset(output, 0, (size_t)outputlen);
		return false;
	}

	// Slot-2's add-ons (the Guitar Grip, the Rumble Pak, the Motion Pak): none
	bool Addon_KeyDown(KeyType, void*) { return false; }
	void Addon_RumbleStart(u32, void*) {}
	void Addon_RumbleStop(void*) {}
	float Addon_MotionQuery(MotionQueryType type, void*)
	{
		// lying still: 1 g along the axis out of the screen
		return type == MotionAccelerationZ ? 9.80665f : 0.0f;
	}

	// No libraries load on the PS5 (melonDS asks only for libpcap, its network)
	DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
	void DynamicLibrary_Unload(DynamicLibrary*) {}
	void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }
}
