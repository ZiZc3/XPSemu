//
// XPSemu: updates from the project's GitHub releases (patch 230)
//
// The latest release is asked of GitHub's API; its tag's first number
// ("v3.1", "Alpha-3.1", "Alpha 3.1") is the version. Its file for this app
// is the .zip named after the title (PPSA97358.zip). Installing: the zip to
// /data/xpsemu/update, checked, then its files written into the app's own
// folder, in place (patch 233): each one first as <file>.upd, and once all
// of them are written, each renamed over the old one. The folder itself is
// never moved (the PS5 knows the app by it: a new folder in its place was
// "can't run this app"), files the zip doesn't have are left alone, and no
// copy of the old version is kept (it's on GitHub).
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
#include "updater.hh"
#include "cover-download.hh"
#include "xog_title.h"
#include "xog_ui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define XOG_STR2(x) #x
#define XOG_STR(x) XOG_STR2(x)

namespace {

const char *const kApi = "https://api.github.com/repos/ZiZc3/XPSemu/releases/latest";
const std::string kApp = "/data/homebrew/" XOG_TITLE;
const std::string kWork = "/data/xpsemu/update";

std::mutex g_lock;
std::string g_status;           // under the button
std::string g_tag, g_url;       // the newer release, its file ("": none for this app)
double g_offer_until;           // "press again to install" holds until then
bool g_busy, g_quiet, g_restart;

double Now()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

void SetStatus(const std::string &text)
{
    std::lock_guard<std::mutex> guard(g_lock);
    g_status = text;
}

// The first run of numbers and dots in text: "Alpha-3.1" is {3, 1}.
std::vector<int> VersionOf(const std::string &text)
{
    std::vector<int> parts;
    size_t i = text.find_first_of("0123456789");
    while (i < text.size() && isdigit((unsigned char)text[i])) {
        int n = 0;
        while (i < text.size() && isdigit((unsigned char)text[i])) {
            n = std::min(n * 10 + (text[i] - '0'), 1000000);
            i++;
        }
        parts.push_back(n);
        if (i + 1 < text.size() && text[i] == '.' && isdigit((unsigned char)text[i + 1])) {
            i++;
        } else {
            break;
        }
    }
    return parts;
}

bool Newer(const std::vector<int> &a, const std::vector<int> &b)
{
    for (size_t i = 0; i < std::max(a.size(), b.size()); i++) {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) {
            return x > y;
        }
    }
    return false;
}

bool EndsWithNoCase(const std::string &text, const char *end)
{
    const size_t n = strlen(end);
    return text.size() >= n && !strcasecmp(text.c_str() + text.size() - n, end);
}

bool HasNoCase(const std::string &text, const char *part)
{
    std::string a = text, b = part;
    for (char &c : a) {
        c = (char)tolower((unsigned char)c);
    }
    for (char &c : b) {
        c = (char)tolower((unsigned char)c);
    }
    return a.find(b) != std::string::npos;
}

// The string value of "key" at or after from ("" if none); *next: past it.
std::string JsonField(const std::string &json, const char *key, size_t from, size_t *next)
{
    const std::string want = std::string("\"") + key + "\"";
    *next = std::string::npos;
    size_t at = json.find(want, from);
    if (at == std::string::npos) {
        return "";
    }
    *next = at + want.size();
    at = json.find_first_not_of(" \t\r\n", at + want.size());
    if (at == std::string::npos || json[at] != ':') {
        return "";
    }
    at = json.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string::npos || json[at] != '"') {
        return "";
    }
    std::string out;
    for (size_t i = at + 1; i < json.size(); i++) {
        const char c = json[i];
        if (c == '"') {
            *next = i + 1;
            return out;
        }
        if (c == '\\' && i + 1 < json.size()) {
            const char e = json[++i];
            if (e == 'u' && i + 4 < json.size()) {
                const unsigned v = (unsigned)strtoul(json.substr(i + 1, 4).c_str(), NULL, 16);
                out += v < 0x80 ? (char)v : '?';
                i += 4;
            } else {
                out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            }
            continue;
        }
        out += c;
    }
    return "";
}

// A URL's file name ("%20" and the like turned back).
std::string FileName(const std::string &url)
{
    std::string name = url.substr(url.find_last_of('/') + 1), out;
    name = name.substr(0, name.find('?'));
    for (size_t i = 0; i < name.size(); i++) {
        if (name[i] == '%' && i + 2 < name.size() && isxdigit((unsigned char)name[i + 1]) &&
            isxdigit((unsigned char)name[i + 2])) {
            out += (char)strtoul(name.substr(i + 1, 2).c_str(), NULL, 16);
            i += 2;
        } else {
            out += name[i];
        }
    }
    return out;
}

struct Found {
    bool ok = false, newer = false;
    std::string tag, url, why;
};

Found CheckNow(long connect_s, long stall_s)
{
    Found f;
    std::string body;
    if (!CoverHttpFetch(kApi, "", &body, std::function<void(long long, long long)>(), connect_s, stall_s,
                        &f.why)) {
        return f;
    }
    size_t next;
    f.tag = JsonField(body, "tag_name", 0, &next);
    if (f.tag.empty()) {
        f.why = "no release in the reply";
        return f;
    }
    f.ok = true;
    const std::vector<int> theirs = VersionOf(f.tag), ours = VersionOf(XOG_STR(XOG_VERSION_NUM));
    f.newer = !theirs.empty() && Newer(theirs, ours);
    // Its files: a .zip named after this app's title
    size_t at = body.find("\"assets\"");
    while (at != std::string::npos) {
        const std::string url = JsonField(body, "browser_download_url", at, &at);
        const std::string name = FileName(url);
#ifdef XOG_FAKE_VERSION
        const char *const want = "PPSA97358";   // the download test: the release's own file
#else
        const char *const want = XOG_TITLE;
#endif
        if (!url.empty() && EndsWithNoCase(name, ".zip") && HasNoCase(name, want)) {
            f.url = url;
            break;
        }
    }
    return f;
}

uint32_t Rd16(const std::string &b, size_t o) { return (uint8_t)b[o] | (uint32_t)(uint8_t)b[o + 1] << 8; }
uint32_t Rd32(const std::string &b, size_t o) { return Rd16(b, o) | Rd16(b, o + 2) << 16; }

bool WriteAll(int fd, const unsigned char *data, size_t len)
{
    while (len > 0) {
        const ssize_t n = write(fd, data, len);
        if (n <= 0) {
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

// Every file of the zip into dest, in place (a top folder all of them share left out): all
// written as <file>.upd (each listed in *upd), then each renamed over its old one. whole: false
// when the zip isn't a whole XPSemu (nothing written then). percent: as it goes.
bool Unzip(const std::string &zip, const std::string &dest, std::string *why, bool *whole,
           std::vector<std::string> *upd, const std::function<void(int)> &percent)
{
    *whole = true;
    FILE *f = fopen(zip.c_str(), "rb");
    if (!f) {
        *why = "can't open the download";
        return false;
    }
    std::unique_ptr<FILE, int (*)(FILE *)> closer(f, fclose);
    fseeko(f, 0, SEEK_END);
    const long long size = (long long)ftello(f);
    const size_t tail = (size_t)std::min<long long>(size, 22 + 65535);
    std::string end(tail, '\0');
    if (tail < 22 || fseeko(f, size - (long long)tail, SEEK_SET) != 0 || fread(&end[0], 1, tail, f) != tail) {
        *why = "not a zip";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = tail - 22 + 1; i-- > 0;) {
        if (Rd32(end, i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) {
        *why = "not a zip";
        return false;
    }
    const uint32_t count = Rd16(end, eocd + 10), cd_size = Rd32(end, eocd + 12), cd_at = Rd32(end, eocd + 16);
    if (count == 0xffff || cd_at == 0xffffffffu) {
        *why = "zip64 isn't handled";
        return false;
    }
    if ((long long)cd_at + cd_size > size || cd_size > (16u << 20)) {
        *why = "the zip is damaged";
        return false;
    }
    std::string cd(cd_size, '\0');
    if (fseeko(f, cd_at, SEEK_SET) != 0 || fread(&cd[0], 1, cd_size, f) != cd_size) {
        *why = "the zip is damaged";
        return false;
    }

    struct Entry {
        std::string name;
        uint32_t method, crc, csize, usize, local;
    };
    std::vector<Entry> entries;
    size_t at = 0;
    for (uint32_t n = 0; n < count; n++) {
        if (at + 46 > cd.size() || Rd32(cd, at) != 0x02014b50u) {
            *why = "the zip is damaged";
            return false;
        }
        Entry e;
        const uint32_t flags = Rd16(cd, at + 8), nlen = Rd16(cd, at + 28), xlen = Rd16(cd, at + 30),
                       clen = Rd16(cd, at + 32);
        e.method = Rd16(cd, at + 10);
        e.crc = Rd32(cd, at + 16);
        e.csize = Rd32(cd, at + 20);
        e.usize = Rd32(cd, at + 24);
        e.local = Rd32(cd, at + 42);
        if (at + 46 + nlen > cd.size()) {
            *why = "the zip is damaged";
            return false;
        }
        e.name = cd.substr(at + 46, nlen);
        at += 46 + nlen + xlen + clen;
        if (flags & 1) {
            *why = "the zip is locked";
            return false;
        }
        for (char &c : e.name) {    // zipped on Windows: "PPSA97358\eboot.bin"
            if (c == '\\') {
                c = '/';
            }
        }
        while (!e.name.empty() && e.name[0] == '/') {
            e.name.erase(0, 1);
        }
        if (("/" + e.name + "/").find("/../") != std::string::npos) {
            *why = "a file in the zip points outside it";
            return false;
        }
        if (!e.name.empty()) {
            entries.push_back(e);
        }
    }
    if (entries.empty()) {
        *why = "the zip is empty";
        return false;
    }
    // One folder holding it all ("PPSA97358/..."): its contents are the app's
    std::string top = entries[0].name.substr(0, entries[0].name.find('/') + 1);
    for (const Entry &e : entries) {
        if (top.size() < 2 || e.name.compare(0, top.size(), top) != 0) {
            top.clear();
            break;
        }
    }
    long long total = 0, done = 0;
    bool eboot = false, param = false;
    for (Entry &e : entries) {
        e.name.erase(0, top.size());
        total += e.csize;
        eboot = eboot || (e.name == "eboot.bin" && e.usize > (1u << 20));
        param = param || e.name == "sce_sys/param.json";
    }
    if (!eboot || !param) {
        *whole = false;
        *why = "no eboot.bin (over 1 MB) or sce_sys/param.json in the zip";
        return false;
    }

    std::error_code ec;
    std::vector<unsigned char> in(256 * 1024), out(256 * 1024);
    for (const Entry &e : entries) {
        if (e.name.empty()) {
            continue;
        }
        const std::string path = dest + "/" + e.name;
        if (e.name.back() == '/') {
            std::filesystem::create_directories(path, ec);
            continue;
        }
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::string local(30, '\0');
        if (fseeko(f, e.local, SEEK_SET) != 0 || fread(&local[0], 1, 30, f) != 30 ||
            Rd32(local, 0) != 0x04034b50u ||
            fseeko(f, (long long)e.local + 30 + Rd16(local, 26) + Rd16(local, 28), SEEK_SET) != 0) {
            *why = "the zip is damaged (" + e.name + ")";
            return false;
        }
        if (e.method != 0 && e.method != 8) {
            *why = "the zip packs " + e.name + " in a way that isn't handled";
            return false;
        }
        const int o = open((path + ".upd").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
        if (o < 0) {
            *why = "can't write " + path + ".upd (" + strerror(errno) + ")";
            return false;
        }
        upd->push_back(path + ".upd");
        uLong crc = crc32(0, NULL, 0);
        long long written = 0, left = e.csize;
        bool ok = true, ended = e.method == 0;
        z_stream zs = {};
        if (e.method == 8 && inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            ok = false;
        }
        while (ok && left > 0) {
            const size_t n = (size_t)std::min<long long>(left, (long long)in.size());
            if (fread(in.data(), 1, n, f) != n) {
                ok = false;
                break;
            }
            left -= (long long)n;
            done += (long long)n;
            if (e.method == 0) {
                crc = crc32(crc, in.data(), (uInt)n);
                written += (long long)n;
                ok = WriteAll(o, in.data(), n);
            } else {
                zs.next_in = in.data();
                zs.avail_in = (uInt)n;
                do {
                    zs.next_out = out.data();
                    zs.avail_out = (uInt)out.size();
                    const int r = inflate(&zs, Z_NO_FLUSH);
                    if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) {
                        ok = false;
                        break;
                    }
                    const size_t got = out.size() - zs.avail_out;
                    crc = crc32(crc, out.data(), (uInt)got);
                    written += (long long)got;
                    if (got && !WriteAll(o, out.data(), got)) {
                        ok = false;
                        break;
                    }
                    if (r == Z_STREAM_END) {
                        ended = true;
                        break;
                    }
                } while (zs.avail_out == 0);
            }
            if (percent && total > 0) {
                percent((int)(done * 100 / total));
            }
        }
        if (e.method == 8) {
            inflateEnd(&zs);
        }
        ok = fsync(o) == 0 && ok;   // on the disk before it replaces anything
        ok = close(o) == 0 && ok;
        if (!ok || !ended || written != e.usize || (uint32_t)crc != e.crc) {
            *why = "couldn't take out " + e.name;
            return false;
        }
    }
    // All written: each over its old one (the running eboot.bin too)
    for (const std::string &u : *upd) {
        const std::string path = u.substr(0, u.size() - 4);
        if (rename(u.c_str(), path.c_str()) != 0) {
            *why = "can't replace " + path + " (" + strerror(errno) + ")";
            return false;
        }
    }
    upd->clear();
    return true;
}

// .upd files an update left in the app's folder (stopped halfway), gone.
void RemoveLeftovers(const std::vector<std::string> *listed)
{
    std::error_code ec;
    if (listed) {
        for (const std::string &u : *listed) {
            unlink(u.c_str());
        }
    }
    std::vector<std::filesystem::path> found;
    for (auto it = std::filesystem::recursive_directory_iterator(kApp, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() > 4 && !name.compare(name.size() - 4, 4, ".upd") && it->is_regular_file(ec)) {
            found.push_back(it->path());
        }
    }
    for (const auto &p : found) {
        xog_ui_log("update: removed leftover %s", p.string().c_str());
        unlink(p.string().c_str());
    }
}

void Fail(const std::string &row, const std::string &log)
{
    xog_ui_log("update: failed: %s", log.c_str());
    std::error_code ec;
    std::filesystem::remove_all(kWork, ec);
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_status = row;
        g_busy = false;
    }
    xemu_queue_notification(("Update failed: " + row).c_str());
}

void Install(std::string tag, std::string url)
{
    xog_ui_log("update: installing %s from %s", tag.c_str(), url.c_str());
    std::error_code ec;
    // The app's own folder must take writes, or nothing can be put in it
    {
        const std::string probe = kApp + "/.update-test";
        FILE *f = fopen(probe.c_str(), "wb");
        bool ok = f && fwrite("ok", 1, 2, f) == 2;
        ok = (f ? fclose(f) == 0 : false) && ok;
        const std::string err = strerror(errno);
        unlink(probe.c_str());
        if (!ok) {
            Fail("Can't write the app's folder: update by FTP", "test write in " + kApp + ": " + err);
            return;
        }
    }

    std::filesystem::create_directories(kWork, ec);
    const std::string zip = kWork + "/update.zip";
    std::string why, unused;
    int shown = -1;
    SetStatus("Downloading... 0%");
    auto progress = [&shown](long long got, long long total) {
        const int p = total > 0 ? (int)(got * 100 / total) : (int)(got >> 20);
        if (p != shown) {
            shown = p;
            SetStatus(total > 0 ? "Downloading... " + std::to_string(p) + "%" :
                                  "Downloading... " + std::to_string(p) + " MB");
        }
    };
    const auto started = std::chrono::steady_clock::now();
    if (!CoverHttpFetch(url, zip, &unused, progress, 15, 30, &why)) {
        Fail("Download failed: try again", "download: " + why);
        return;
    }
    struct stat st;
    const long long bytes = stat(zip.c_str(), &st) == 0 ? (long long)st.st_size : -1LL;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    char speed[96];
    snprintf(speed, sizeof(speed), "%.1f MB in %.0f s (%.1f MB/s)", bytes / 1048576.0, secs,
             secs > 0 ? bytes / secs / 1048576.0 : 0.0);
    xog_ui_log("update: downloaded %lld bytes: %s", bytes, speed);
#ifdef XOG_FAKE_VERSION
    // The download test (FAKE_VERSION build): measured, not installed - the release isn't this app.
    std::filesystem::remove_all(kWork, ec);
    SetStatus(std::string("Download test done: ") + speed);
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_busy = false;
    }
    return;
#endif

    shown = -1;
    SetStatus("Installing... 0%");
    auto unpacked = [&shown](int p) {
        if (p != shown) {
            shown = p;
            SetStatus("Installing... " + std::to_string(p) + "%");
        }
    };
    std::vector<std::string> upd;
    bool whole = true;
    if (!Unzip(zip, kApp, &why, &whole, &upd, unpacked)) {
        RemoveLeftovers(&upd);
        Fail(whole ? why : "The download isn't a whole XPSemu", "install into " + kApp + ": " + why);
        return;
    }
    std::filesystem::remove_all(kWork, ec);
    xog_ui_log("update: %s installed, restarting", tag.c_str());
    SetStatus("Updated to " + tag + ": restarting");
    xemu_queue_notification(("Updated to " + tag + ": restarting").c_str());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::lock_guard<std::mutex> guard(g_lock);
    g_restart = true;
}

void CheckThread()
{
    const Found f = CheckNow(10, 15);
    std::lock_guard<std::mutex> guard(g_lock);
    const bool quiet = g_quiet;
    g_busy = false;
    g_quiet = false;
    if (!f.ok) {
        xog_ui_log("update: check failed: %s", f.why.c_str());
        if (!quiet) {
            g_status = "Can't check now: no network?";
        }
        return;
    }
    xog_ui_log("update: latest %s (%s), this is " XOG_VERSION_TEXT ", file %s", f.tag.c_str(),
               f.newer ? "newer" : "not newer", f.url.empty() ? "(none for " XOG_TITLE ")" : f.url.c_str());
    if (!f.newer) {
        if (!quiet) {
            g_status = "Up to date (" XOG_VERSION_TEXT ")";
        }
        return;
    }
    g_tag = f.tag;
    g_url = f.url;
    if (quiet) {
        g_status = "Update " + f.tag + " available";
        xemu_queue_notification(("XPSemu " + f.tag + " is available: System > Check for updates").c_str());
    } else if (f.url.empty()) {
        g_status = "Update " + f.tag + " available: no download for this app";
    } else {
        g_offer_until = Now() + 15;
        g_status = "Update " + f.tag + " available: press again to install";
    }
}

}  // namespace

// patch 237: the test app (PPSA97360) never updates: GitHub has the release
static bool TestApp()
{
#ifdef XOG_FAKE_VERSION
    return false;   // the download test build: it downloads (and doesn't install)
#else
    return std::string(XOG_TITLE) == "PPSA97360";
#endif
}

void UpdaterStartup()
{
    if (TestApp()) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_busy) {
            return;
        }
        g_busy = true;
        g_quiet = true;
    }
    std::thread([]() {
        std::error_code ec;
        std::filesystem::remove_all(kWork, ec);     // a download left behind
        RemoveLeftovers(nullptr);                   // an install stopped halfway
        CheckThread();
    }).detach();
}

void UpdaterPress()
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (TestApp()) {
        g_status = "The test app doesn't update";
        return;
    }
    if (g_busy) {
        if (g_quiet) {      // the startup check is under way: its answer shows
            g_quiet = false;
            g_status = "Checking...";
        }
        return;
    }
    g_busy = true;
    if (!g_url.empty() && Now() < g_offer_until) {
        g_offer_until = 0;
        g_status = "Starting the update...";
        std::thread(Install, g_tag, g_url).detach();
        return;
    }
    g_status = "Checking...";
    std::thread(CheckThread).detach();
}

std::string UpdaterStatus(bool *busy)
{
    std::lock_guard<std::mutex> guard(g_lock);
    *busy = g_busy;
    if (!g_busy && !g_url.empty() && g_offer_until > 0 && Now() >= g_offer_until &&
        g_status.find("press again") != std::string::npos) {
        g_status = "Update " + g_tag + " available";
    }
    return g_quiet ? std::string() : g_status;
}

bool UpdaterTakeRestart()
{
    std::lock_guard<std::mutex> guard(g_lock);
    const bool now = g_restart;
    g_restart = false;
    return now;
}
