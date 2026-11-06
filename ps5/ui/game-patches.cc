//
// XPSemu: game patches (.JMP files), applied as the disc is read
//
// Jay's Magic Patches (github.com/JayYardley/Xbox-Magic-Patches-by-Jay,
// www.jayxbox.com) change bytes of a game's default.xbe: at an offset, or
// found and replaced. Rather than editing the disc image, the bytes are
// swapped in as the emulated DVD drive reads it (block/raw-format.c calls
// xemu_raw_read_hook for every read of a raw image): the game sees a
// patched default.xbe, the file stays as it was, and a patch is a toggle.
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
#include <algorithm>
#include <atomic>
#include <ctype.h>
#include <filesystem>
#include <fstream>
#include <stdio.h>
#include <string.h>
#include <sstream>
#include <sys/uio.h>
#include <zlib.h>

#include "game-patches.hh"
#include "xog_ui.h"

extern "C" void (*xemu_raw_read_hook)(const char *filename, int64_t offset,
                                      int64_t bytes, struct iovec *iov,
                                      int niov);

static std::string Trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Hex digits (spaces allowed between) to bytes; false if not hex.
static bool ParseHex(const std::string &text, std::vector<uint8_t> *out)
{
    std::string digits;
    for (char c : text) {
        if (isxdigit((unsigned char)c)) {
            digits += c;
        } else if (c != ' ' && c != '\t') {
            return false;
        }
    }
    if (digits.empty() || digits.size() % 2) {
        return false;
    }
    out->clear();
    for (size_t i = 0; i < digits.size(); i += 2) {
        out->push_back((uint8_t)strtoul(digits.substr(i, 2).c_str(), NULL, 16));
    }
    return true;
}

//
// Parsing
//

static bool ParseText(const std::string &path, const char *text, PatchFile *out)
{
    std::istringstream in(text);
    *out = PatchFile();
    out->path = path;
    out->file_name = std::filesystem::path(path).filename().string();

    PatchGroup *group = NULL;
    std::vector<uint8_t> pending_find; // A replace record's first line
    auto current = [&]() -> PatchGroup & {
        if (!group) {
            out->groups.push_back(PatchGroup());
            out->groups.back().name = "Patch";
            group = &out->groups.back();
        }
        return *group;
    };
    auto unsupported = [&](const char *why) {
        if (out->unsupported.empty()) {
            out->unsupported = why;
        }
    };

    std::string line;
    bool in_header = true;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        size_t eq = line.find('=');
        std::string key = eq == std::string::npos ? "" :
                                                    Trim(line.substr(0, eq));
        if (in_header && line[0] != '#' &&
            (key == "system" || key == "game-title" || key == "region" ||
             key == "version" || key == "author" || key == "notes")) {
            std::string value = Trim(line.substr(eq + 1));
            if (key == "game-title") out->title = value;
            else if (key == "region") out->region = value;
            else if (key == "version") out->version = value;
            else if (key == "author") out->author = value;
            else if (key == "notes") out->notes = value;
            continue;
        }
        if (line[0] == '#') {
            if (line.find("Magic Patcher") != std::string::npos &&
                out->groups.empty() && !group) {
                continue; // The first header line
            }
            in_header = false;
            out->groups.push_back(PatchGroup());
            group = &out->groups.back();
            std::string name = Trim(line.substr(1));
            size_t off = name.find("[-]");
            if (off != std::string::npos) {
                group->default_on = false;
                name = Trim(name.erase(off, 3));
            }
            group->name = name.empty() ? "Patch" : name;
            pending_find.clear();
            continue;
        }
        in_header = false;

        size_t colon = line.find(':');
        if (colon != std::string::npos) { // offset:bytes, or APPEND:bytes
            std::string left = Trim(line.substr(0, colon));
            std::string upper = left;
            std::transform(upper.begin(), upper.end(), upper.begin(),
                           ::toupper);
            PatchRecord r;
            if (upper == "APPEND") {
                unsupported("Adds new code to the game (APPEND), which "
                            "XPSemu can't do yet");
                continue;
            }
            if (upper.rfind("0X", 0) == 0) {
                upper = upper.substr(2);
            }
            char *end = NULL;
            unsigned long offset = strtoul(upper.c_str(), &end, 16);
            if (upper.empty() || *end ||
                !ParseHex(line.substr(colon + 1), &r.data)) {
                unsupported("Has lines XPSemu can't read");
                continue;
            }
            r.offset = (uint32_t)offset;
            current().records.push_back(r);
            continue;
        }

        std::vector<uint8_t> bytes; // Find, then replace, on two lines
        if (!ParseHex(line, &bytes)) {
            unsupported("Has lines XPSemu can't read");
            continue;
        }
        if (pending_find.empty()) {
            pending_find = bytes;
        } else {
            PatchRecord r;
            r.replace = true;
            r.find = pending_find;
            r.data = bytes;
            pending_find.clear();
            if (r.find.size() == r.data.size()) {
                current().records.push_back(r);
            } else {
                unsupported("Changes the game's size, which XPSemu can't "
                            "do yet");
            }
        }
    }

    // "4D530064 (MS-100) {F86CFFDB} {E2FCC0E4}": the title ID, and the
    // CRC32 of each default.xbe (one per release) it's made for.
    std::string v = out->version;
    auto hex_id = [](const std::string &text, uint32_t *id) {
        std::vector<uint8_t> b;
        if (text.size() < 8 || !ParseHex(text.substr(0, 8), &b) ||
            b.size() != 4) {
            return false;
        }
        *id = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
        return true;
    };
    // "545400A4 (TT-164), 54540095 (TT-149)": one title ID per release.
    for (size_t start = 0; start <= v.size();) {
        size_t comma = v.find(',', start);
        std::string part =
            Trim(v.substr(start, comma == std::string::npos ? std::string::npos :
                                                            comma - start));
        uint32_t id;
        if (hex_id(part, &id)) {
            out->title_ids.push_back(id);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (!out->title_ids.empty()) {
        out->title_id = out->title_ids[0];
    }
    for (size_t brace = v.find('{'); brace != std::string::npos;
         brace = v.find('{', brace + 1)) {
        std::vector<uint8_t> crc;
        size_t close = v.find('}', brace);
        if (close != std::string::npos &&
            ParseHex(v.substr(brace + 1, close - brace - 1), &crc) &&
            crc.size() == 4) {
            out->crcs.push_back((uint32_t)crc[0] << 24 | crc[1] << 16 |
                                crc[2] << 8 | crc[3]);
        }
    }

    // HTML line breaks in the notes (the Patch Hub shows them as HTML).
    for (size_t br; (br = out->notes.find("<br>")) != std::string::npos;) {
        out->notes.replace(br, 4, " ");
    }
    out->notes = Trim(out->notes);

    // Comments with the same name are one option (patches split over
    // many records often repeat it), in the order they first appear.
    std::vector<PatchGroup> merged;
    for (auto &g : out->groups) {
        auto it = std::find_if(merged.begin(), merged.end(),
                               [&](const PatchGroup &m) {
                                   return m.name == g.name;
                               });
        if (it == merged.end()) {
            merged.push_back(g);
        } else {
            it->records.insert(it->records.end(), g.records.begin(),
                               g.records.end());
            it->default_on = it->default_on && g.default_on;
        }
    }
    out->groups.clear();
    for (auto &g : merged) {
        if (out->title_ids.size() > 1) {
            for (uint32_t id : out->title_ids) {
                char text[16];
                snprintf(text, sizeof(text), "%08X", id);
                std::string upper = g.name;
                std::transform(upper.begin(), upper.end(), upper.begin(),
                               ::toupper);
                if (upper.find(text) != std::string::npos) {
                    g.title_id = id;
                }
            }
        }
        if (g.records.empty()) {
            continue; // Only APPEND (the file says unsupported), or empty
        }
        // Replacing bytes with the same bytes changes nothing: a note.
        g.info = std::all_of(g.records.begin(), g.records.end(),
                             [](const PatchRecord &r) {
                                 return r.replace && r.find == r.data;
                             });
        out->groups.push_back(g);
    }
    return !out->groups.empty();
}

//
// The patch files
//

static std::vector<PatchFile> g_files;

// The widescreen and 60 FPS patches, in the app (tools/make_auto_patches.py).
static const struct AutoPatch {
    const char *kind, *path, *text;
} kAutoPatches[] = {
#include "patches-auto.inc"
};
static std::vector<const char *> g_kinds;   // beside g_files

void PatchesRescan()
{
    if (!g_files.empty()) {
        return;
    }
    for (const AutoPatch &a : kAutoPatches) {
        PatchFile file;
        if (ParseText(a.path, a.text, &file)) {
            g_files.push_back(std::move(file));
            g_kinds.push_back(a.kind);
        }
    }
    xog_ui_log("patches in the app: %u", (unsigned)g_files.size());
}

std::vector<const PatchFile *> PatchesFor(uint32_t title_id)
{
    std::vector<const PatchFile *> out;
    for (const auto &f : g_files) {
        if (title_id && std::find(f.title_ids.begin(), f.title_ids.end(),
                                  title_id) != f.title_ids.end()) {
            out.push_back(&f);
        }
    }
    return out;
}

//
// Choices
//

std::string PatchGroupId(const PatchFile &file, const PatchGroup &group)
{
    return file.file_name + "|" + group.name;
}

static std::string ChoicesPath(const std::string &key)
{
    return std::string(xemu_settings_get_base_path()) + "game-settings/" +
           key + ".patches";
}

PatchChoices PatchChoicesLoad(const std::string &key)
{
    PatchChoices choices;
    std::ifstream in(ChoicesPath(key));
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.rfind('=');
        if (eq != std::string::npos) {
            choices[line.substr(0, eq)] = line.substr(eq + 1) == "1";
        }
    }
    return choices;
}

void PatchChoicesSave(const std::string &key, const PatchChoices &choices)
{
    std::error_code ec;
    std::filesystem::create_directories(
        std::string(xemu_settings_get_base_path()) + "game-settings", ec);
    std::ofstream out(ChoicesPath(key), std::ios::trunc);
    for (const auto &[id, on] : choices) {
        out << id << "=" << (on ? 1 : 0) << "\n";
    }
}

bool PatchGroupOn(const PatchChoices &choices, const PatchFile &file,
                  const PatchGroup &group)
{
    auto it = choices.find(PatchGroupId(file, group));
    return it == choices.end() ? group.default_on : it->second;
}

//
// Checking: whether a group fits this game's default.xbe
//

// default.xbe of the last disc image asked about, read once.
extern "C" {
int rx_cci_open(const char *path);
bool rx_cci_name(const char *path);
bool rx_cci_read(int fd, void *buf, size_t len, uint64_t off);
void rx_cci_close(int fd);
}

static struct {
    std::string iso_path;
    uint64_t offset = 0;
    uint32_t size = 0;
    bool ok = false;
    std::vector<uint8_t> data;
    uint32_t crc = 0;
} g_xbe;

static bool ReadXbe(const std::string &iso_path, uint64_t offset,
                    uint32_t size)
{
    if (g_xbe.iso_path == iso_path && g_xbe.offset == offset &&
        g_xbe.size == size) {
        return g_xbe.ok;
    }
    g_xbe.iso_path = iso_path;
    g_xbe.offset = offset;
    g_xbe.size = size;
    g_xbe.ok = false;
    g_xbe.data.clear();
    if (rx_cci_name(iso_path.c_str())) {
        const int cci = rx_cci_open(iso_path.c_str());
        if (cci < 0 || !size) {
            if (cci >= 0) {
                rx_cci_close(cci);
            }
            return false;
        }
        g_xbe.data.resize(size);
        g_xbe.ok = rx_cci_read(cci, g_xbe.data.data(), size, offset);
        rx_cci_close(cci);
    } else {
        FILE *f = fopen(iso_path.c_str(), "rb");
        if (!f || !size) {
            if (f) {
                fclose(f);
            }
            return false;
        }
        g_xbe.data.resize(size);
        g_xbe.ok = fseeko(f, offset, SEEK_SET) == 0 &&
                   fread(g_xbe.data.data(), 1, size, f) == size;
        fclose(f);
    }
    if (g_xbe.ok) {
        g_xbe.crc = (uint32_t)crc32(0, g_xbe.data.data(), size);
    } else {
        g_xbe.data.clear();
    }
    return g_xbe.ok;
}

// Where each record of a group goes in default.xbe; false (with the
// reason) if one doesn't fit.
static bool Locate(const PatchGroup &g, std::vector<uint32_t> *at,
                   std::string *why)
{
    for (const auto &r : g.records) {
        uint32_t pos = r.offset;
        if (r.replace) {
            auto it = std::search(g_xbe.data.begin(), g_xbe.data.end(),
                                  r.find.begin(), r.find.end());
            if (it == g_xbe.data.end()) {
                *why = "Not in this game";
                return false;
            }
            pos = it - g_xbe.data.begin();
        }
        if ((uint64_t)pos + r.data.size() > g_xbe.size) {
            *why = "Not in this game";
            return false;
        }
        at->push_back(pos);
    }
    return true;
}

bool PatchXbeCrc(const std::string &iso_path, uint64_t xbe_offset,
                 uint32_t xbe_size, uint32_t *crc)
{
    if (!ReadXbe(iso_path, xbe_offset, xbe_size)) {
        return false;
    }
    *crc = g_xbe.crc;
    return true;
}

// A copy the file doesn't list can still take a group that only finds and
// replaces code, when each piece of code is in it exactly once: releases
// move code around, but the same code is the same fix. (Bytes at fixed
// offsets need the listed copy.)
static uint32_t XbeTitleId()
{
    const auto &x = g_xbe.data;
    if (x.size() < 0x120) {
        return 0;
    }
    uint32_t base, cert;
    memcpy(&base, &x[0x104], 4);
    memcpy(&cert, &x[0x118], 4);
    if (cert < base || cert - base + 12 > x.size()) {
        return 0;
    }
    uint32_t id;
    memcpy(&id, &x[cert - base + 8], 4);
    return id;
}

static bool FoundOnce(const PatchGroup &group)
{
    if (group.records.empty()) {
        return false;
    }
    for (const auto &r : group.records) {
        if (!r.replace) {
            return false;
        }
        auto first = std::search(g_xbe.data.begin(), g_xbe.data.end(),
                                 r.find.begin(), r.find.end());
        if (first == g_xbe.data.end() ||
            std::search(first + 1, g_xbe.data.end(), r.find.begin(),
                        r.find.end()) != g_xbe.data.end()) {
            return false;
        }
    }
    return true;
}

std::string PatchGroupProblem(const PatchFile &file, const PatchGroup &group,
                              const std::string &iso_path,
                              uint64_t xbe_offset, uint32_t xbe_size)
{
    if (group.info) {
        return "Info";
    }
    if (!file.unsupported.empty()) {
        return "Not supported";
    }
    if (!ReadXbe(iso_path, xbe_offset, xbe_size)) {
        return "Can't read the game";
    }
    if (group.title_id && group.title_id != XbeTitleId()) {
        return "Other version"; // For another of the file's releases
    }
    if (!file.crcs.empty() &&
        std::find(file.crcs.begin(), file.crcs.end(), g_xbe.crc) ==
            file.crcs.end() &&
        !FoundOnce(group)) {
        return "Other version";
    }
    std::vector<uint32_t> at;
    std::string why;
    return Locate(group, &at, &why) ? "" : why;
}

bool PatchGroupFoundInCopy(const PatchFile &file, const PatchGroup &group,
                           const std::string &iso_path, uint64_t xbe_offset,
                           uint32_t xbe_size)
{
    return !file.crcs.empty() && ReadXbe(iso_path, xbe_offset, xbe_size) &&
           std::find(file.crcs.begin(), file.crcs.end(), g_xbe.crc) ==
               file.crcs.end() &&
           FoundOnce(group);
}

//
// Applying: bytes at offsets of the disc image
//

struct PatchTable {
    std::string iso_path;
    struct Range {
        uint64_t start;
        std::vector<uint8_t> bytes;
    };
    std::vector<Range> ranges;
};

static std::atomic<const PatchTable *> g_table;
static std::atomic<uint64_t> g_bytes_applied;

// Copies bytes into an I/O vector at position pos (of the request).
// default.xbe's offset as the address the game has it at: its header, or one of its sections.
static bool XbeVa(uint32_t pos, uint32_t *va)
{
    const auto &x = g_xbe.data;
    uint32_t base, count, table, headers;
    if (x.size() < 0x124) {
        return false;
    }
    memcpy(&base, &x[0x104], 4);
    memcpy(&headers, &x[0x108], 4);
    memcpy(&count, &x[0x11C], 4);
    memcpy(&table, &x[0x120], 4);
    for (uint32_t i = 0; i < count; i++) {
        const uint64_t h = (uint64_t)table - base + i * 56ull;
        if (h + 56 > x.size()) {
            return false;
        }
        uint32_t vaddr, raw, size;
        memcpy(&vaddr, &x[h + 4], 4);
        memcpy(&raw, &x[h + 12], 4);
        memcpy(&size, &x[h + 16], 4);
        if (pos >= raw && pos - raw < size) {
            *va = vaddr + (pos - raw);
            return true;
        }
    }
    if (pos < headers) {
        *va = base + pos;
        return true;
    }
    return false;
}

std::string PatchesForStart(const std::string &iso_path, uint64_t xbe_offset, uint32_t xbe_size, uint32_t title_id,
                            bool widescreen, bool fps60, std::string *pokes, bool *widescreen_on)
{
    PatchesRescan();
    std::string log;
    char buf[320];
    bool done_wide = false, done_fps = false;
    for (size_t f = 0; f < g_files.size(); f++) {
        const PatchFile &file = g_files[f];
        if (!title_id || std::find(file.title_ids.begin(), file.title_ids.end(), title_id) == file.title_ids.end()) {
            continue;
        }
        // A patch can be both; it needs both switches on.
        const bool is_wide = strcmp(g_kinds[f], "60fps") != 0, is_fps = strcmp(g_kinds[f], "widescreen") != 0;
        // One of a kind for a game: two authors' patches of the same thing can fight.
        if ((is_wide && (!widescreen || done_wide)) || (is_fps && (!fps60 || done_fps))) {
            continue;
        }
        std::string lines;
        size_t changes = 0;
        for (const auto &g : file.groups) {
            if (g.info || !g.default_on) {
                continue;
            }
            std::string why = PatchGroupProblem(file, g, iso_path, xbe_offset, xbe_size);
            std::vector<uint32_t> at;
            if (why.empty()) {
                Locate(g, &at, &why);
            }
            if (!why.empty()) {
                snprintf(buf, sizeof(buf), "  %s: %s - skipped: %s\n", file.file_name.c_str(), g.name.c_str(), why.c_str());
                log += buf;
                continue;
            }
            // A group goes on whole or not at all.
            std::string group_lines;
            bool placed = true;
            for (size_t i = 0; i < at.size() && placed; i++) {
                const std::vector<uint8_t> &bytes = g.records[i].data;
                for (size_t o = 0; o < bytes.size(); o += 1024) {   // a line of the file holds this much
                    uint32_t va;
                    if (!XbeVa(at[i] + (uint32_t)o, &va)) {
                        placed = false;
                        break;
                    }
                    char head[32];
                    snprintf(head, sizeof(head), "poke=%08x:", va);
                    group_lines += head;
                    for (size_t k = o; k < bytes.size() && k < o + 1024; k++) {
                        snprintf(head, sizeof(head), "%02x", bytes[k]);
                        group_lines += head;
                    }
                    group_lines += "\n";
                }
            }
            if (!placed) {
                snprintf(buf, sizeof(buf), "  %s: %s - skipped: outside the game's sections\n", file.file_name.c_str(), g.name.c_str());
                log += buf;
                continue;
            }
            lines += group_lines;
            changes += at.size();
        }
        if (!changes) {
            continue;
        }
        *pokes += lines;
        done_wide = done_wide || is_wide;
        done_fps = done_fps || is_fps;
        if (is_wide) {
            *widescreen_on = true;
        }
        snprintf(buf, sizeof(buf), "  %s: on, %zu changes\n", file.file_name.c_str(), changes);
        log += buf;
    }
    return log;
}

// The Patch Store's page isn't reached any more; these keep its code linking until it goes.
void PatchesStop() {}

bool PatchFileParse(const std::string &path, PatchFile *out)
{
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    return ParseText(path, text.str().c_str(), out);
}
