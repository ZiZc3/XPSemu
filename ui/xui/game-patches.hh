//
// XPSemu: game patches (.JMP files), applied as the disc is read
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
#include <map>
#include <stdint.h>
#include <string>
#include <vector>

// One change to default.xbe: bytes at an offset, or bytes found and
// replaced (same length).
struct PatchRecord {
    bool replace = false;
    uint32_t offset = 0;             // For offset records
    std::vector<uint8_t> find, data; // find: for replace records
};

// A named option in a patch file: its "#comment", and its records (the
// records of several comments with the same name are one option).
struct PatchGroup {
    std::string name;
    bool default_on = true; // "[-]" in its comment: off until turned on
    bool info = false;      // Changes nothing (a note, e.g. a link)
    // The one release it's for, when the file is for several and the
    // option's name says which ("[PAL 00000001 - 545400A4]"), else 0.
    uint32_t title_id = 0;
    std::vector<PatchRecord> records;
};

// A Jay's Magic Patch (.JMP) file: seven header lines, then the groups.
struct PatchFile {
    std::string path, file_name;
    std::string title, region, version, author, notes;
    uint32_t title_id = 0;           // The first of title_ids
    std::vector<uint32_t> title_ids; // "version=545400A4 (TT-164), 5454..."
    std::vector<uint32_t> crcs; // default.xbe CRC32s it's for (any; none:
                                // not said)
    // Why none of it can apply, or "": records XPSemu can't do (APPEND
    // adds code to the end of the XBE), which the rest may rely on.
    std::string unsupported;
    std::vector<PatchGroup> groups;
};

bool PatchFileParse(const std::string &path, PatchFile *out);

// The patch files in /data/xemu/patches (and folders in it), read again
// by PatchesRescan (when the dashboard opens).
void PatchesRescan();
std::vector<const PatchFile *> PatchesFor(uint32_t title_id);

// Which groups a game has turned on: game-settings/<key>.patches, lines
// "<file name>|<group name>=0/1"; groups not in it are at their default.
typedef std::map<std::string, bool> PatchChoices;
PatchChoices PatchChoicesLoad(const std::string &key);
void PatchChoicesSave(const std::string &key, const PatchChoices &choices);
bool PatchGroupOn(const PatchChoices &choices, const PatchFile &file,
                  const PatchGroup &group);
std::string PatchGroupId(const PatchFile &file, const PatchGroup &group);

// The CRC32 of a game's default.xbe (what patch files name on their
// version= line); false if it can't be read.
bool PatchXbeCrc(const std::string &iso_path, uint64_t xbe_offset,
                 uint32_t xbe_size, uint32_t *crc);

// Whether a group can apply to a game's default.xbe: "" if it can, else a
// short reason for the list ("Other version", "Not supported", "Not in
// this game", "Info"). The XBE is read from the disc image the first time
// (cached).
std::string PatchGroupProblem(const PatchFile &file, const PatchGroup &group,
                              const std::string &iso_path,
                              uint64_t xbe_offset, uint32_t xbe_size);

// True if a group is allowed only because its code was found (once) in a
// copy the file doesn't list: made for other releases, but it fits.
bool PatchGroupFoundInCopy(const PatchFile &file, const PatchGroup &group,
                           const std::string &iso_path, uint64_t xbe_offset,
                           uint32_t xbe_size);

// A game starts from iso_path, default.xbe at xbe_offset (xbe_size bytes)
// in it: its patches that are on are applied to what the Xbox reads of the
// disc from now on. Returns what happened, for the game log; *applied gets
// the names of the options applied.
std::string PatchesStart(const std::string &iso_path, uint64_t xbe_offset,
                         uint32_t xbe_size, uint32_t title_id,
                         const std::string &key,
                         std::vector<std::string> *applied);
// No patches (the Xbox dashboard, or no game).
void PatchesStop();
// Bytes of the disc the patches have changed as the Xbox read it, since
// PatchesStart (0: the game hasn't read a patched part yet).
uint64_t PatchesBytesApplied();
