#pragma once

#include "vf2_netplay.h"

// The select screen loads the arena textures in mode 7. Set the first
// versus match's stage in mode 6, before the game makes that choice.
inline bool select_arcade_versus_stage(model2a &board, int stage, bool &pending)
{
	constexpr uint32_t ladder = 0x54;
	auto &ram = const_cast<std::vector<u8> &>(board.workram());
	if (pending && board.game_mode() == 9) {
		if (ram[vf2net::VERSUS] == 0 && ram[vf2net::FIGHTS] == u8(stage))
			ram[vf2net::FIGHTS] = 0;
		pending = false;
	}
	if (stage <= 0 || board.game_mode() != 6 || ram[vf2net::FIGHTS] != 0 ||
	    ram[vf2net::STREAK] != 0 || ram[ladder] != 1)
		return false;
	ram[vf2net::FIGHTS] = ram[vf2net::STAGE] = u8(stage);
	pending = true;
	return true;
}
