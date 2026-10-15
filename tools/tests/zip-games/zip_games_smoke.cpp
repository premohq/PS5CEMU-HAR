#include "zip_games.h"
#include <cassert>
#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
	if (argc != 3) return 2;
	const std::filesystem::path directory = argv[1];
	const std::string test = argv[2];
	std::vector<ps5emu::Game> games;
	ps5zip::AddArchives(directory, test == "ds" ? ps5zip::System::Nds : test == "3ds" ? ps5zip::System::N3ds : ps5zip::System::WiiU, games);
	std::cout << test << ": " << games.size() << " games\n";
	if (games.size() != 2) return 3;
	for (const auto& game : games)
	{
		ps5emu::Game extracted;
		std::string error;
		assert(ps5zip::IsArchiveGame(game));
		if (!ps5zip::Prepare(game, extracted, error)) { std::cerr << "prepare: " << error << "\n"; return 4; }
		if (!std::filesystem::exists(extracted.path)) return 5;
		if (ps5zip::TileKey(game) == game.path.string()) return 8;
		const auto before = std::filesystem::last_write_time(extracted.path);
		if (!ps5zip::Prepare(game, extracted, error)) return 6;
		if (std::filesystem::last_write_time(extracted.path) != before) return 7;
		std::cout << "OK " << extracted.path.string() << "\n";
	}
	return 0;
}
