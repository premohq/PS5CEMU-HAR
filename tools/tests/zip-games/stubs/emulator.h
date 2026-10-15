#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
namespace ps5emu { struct Game { uint64_t titleId=0; std::string name; std::filesystem::path path; std::string archiveMember; bool archiveFolder=false; std::string format; bool nds=false; }; }
