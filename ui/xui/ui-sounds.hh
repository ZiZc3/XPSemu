//
// XPSemu: the dashboard's sounds
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#pragma once
#include <stdint.h>

enum UiSound {
    UI_SOUND_MOVE,   // The focus moves
    UI_SOUND_CHANGE, // A setting's value changes
    UI_SOUND_SELECT, // Into a page
    UI_SOUND_BACK,   // Back out of one
    UI_SOUND_OPEN,   // The dashboard opens
    UI_SOUND_LAUNCH, // A game (or the Xbox dashboard) starts
    UI_SOUND_ERROR,  // Can't do that
    UI_SOUND__COUNT
};

// Plays a sound (unless menu sounds are off). Each is made once, the first
// time: /data/xemu/sounds/<name>.wav if there is one (any WAV SDL reads),
// else XPSemu's own.
void UiSoundPlay(UiSound sound);

// Set by UiSoundPlay; the dashboard clears it before handling input.
extern bool g_ui_sound_played;

// Mixes the sounds playing into a block of 48 kHz stereo about to be output
// (by the audio thread, hw/xbox/mcpx/apu/monitor.c).
extern "C" void xemu_ps5_ui_sound_mix(int16_t *buf, int frames);
