//
// XPSemu: covers downloaded from xdb, the xemu project's Xbox title archive
// (github.com/xemu-project/xdb), by title ID, else from libretro-thumbnails
// (RetroArch's box art) by name
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
#include <string>
#include <vector>

enum CoverPart {
    COVER_FRONT, // dir/<TITLEID>.jpg (or .png, from libretro)
    COVER_BACK,  // dir/<TITLEID>.back.jpg (xdb only)
    COVER_SPINE, // dir/<TITLEID>.spine.jpg (xdb only)
};

struct CoverWant {
    uint32_t title_id;
    std::vector<std::string> names; // The game's names, for libretro
    CoverPart part;
};

// Asks for these pictures. They download one after another on a thread of
// their own into dir; a front cover found nowhere is noted in
// dir/.not-found and not asked for again. Pictures already asked for in
// this run are skipped.
void CoverDownloadRequest(const std::vector<CoverWant> &wants,
                          const std::string &dir);

struct CoverDownloaded {
    uint32_t title_id;
    CoverPart part;
    std::string path;
};

// What downloaded since the last call. idle: nothing left to do; failed:
// downloads that failed (no network?) since the last call.
std::vector<CoverDownloaded> CoverDownloadTake(bool *idle, int *failed);

// Where xdb keeps a title's picture.
std::string CoverDownloadUrl(uint32_t title_id, CoverPart part = COVER_FRONT);

// A file from raw.githubusercontent.com (any kind; blocking: call it on a
// thread of its own). false: not there, or no network.
bool CoverHttpGet(const std::string &url, std::string *body);

// A JSON POST (e.g. to a local API); short timeouts. true: a 2xx reply.
bool CoverHttpPost(const std::string &url, const std::string &json,
                   std::string *body);

// The LaunchBox Games Database's background art of a game with these
// names (a URL), or "".
std::string CoverLaunchboxArt(const std::vector<std::string> &names);

// libretro's box art name for a game with these names, or "".
std::string CoverLibretroMatch(const std::vector<std::string> &names);
