//
// XPSemu: covers downloaded from xdb, the xemu project's Xbox title archive
// (github.com/xemu-project/xdb), by title ID, else from libretro-thumbnails
// (RetroArch's box art) by name
//
// xdb keeps each title in titles/<publisher>/<number>/: the title ID's top
// two bytes as letters (0x4D53 is "MS") and its low 16 bits in decimal, at
// least three digits (Fable, 4D5300D1, is titles/MS/209), with
// cover_front.jpg, cover_back.jpg and cover_spine.jpg. libretro names its
// pictures after the disc (Named_Boxarts/Halo 2 (USA).png): its list is
// built in (ps5/make-libretro-index.py) and matched by the game's names.
// Downloads are HTTPS (curl with mbedTLS on the PS5, with Mozilla's
// certificate list built in: there's no system store).
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
#include "cover-download.hh"
#include "cover-cacert.h"
#include "cover-libretro-index.h"
#include "launchbox-index.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

#ifdef __PROSPERO__
// The PS5's resolver (libSceNet; no SDK header). Plain imports (the PS5
// leaves weak ones unresolved); one the console didn't resolve is skipped.
extern "C" {
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetResolverCreate(const char *name, int memid, int flags);
int sceNetResolverStartNtoa(int rid, const char *host, uint32_t *addr,
                            int timeout, int retry, int flags);
int xemu_ps5_import_ok(const char *name); // ui/xemu-os-utils-ps5.c
int sceNetResolverDestroy(int rid);
}

// host's IPv4 address as text, or "".
static std::string Resolve(const char *host)
{
    static int pool = -1;
    if (!xemu_ps5_import_ok("sceNetInit") ||
        !xemu_ps5_import_ok("sceNetPoolCreate") ||
        !xemu_ps5_import_ok("sceNetResolverCreate") ||
        !xemu_ps5_import_ok("sceNetResolverStartNtoa") ||
        !xemu_ps5_import_ok("sceNetResolverDestroy")) {
        fprintf(stderr, "XPSemu: covers: no PS5 resolver\n");
        return "";
    }
    if (pool < 0) {
        sceNetInit(); // Fine if the app already has
        pool = sceNetPoolCreate("xpsemu_dns", 16 * 1024, 0);
    }
    int rid = sceNetResolverCreate("xpsemu_covers", pool, 0);
    if (rid < 0) {
        fprintf(stderr, "XPSemu: covers: resolver %#x (pool %#x)\n", rid, pool);
        return "";
    }
    uint32_t addr = 0; // Network order
    int rc = sceNetResolverStartNtoa(rid, host, &addr, 0, 0, 0);
    sceNetResolverDestroy(rid);
    if (rc < 0 || !addr) {
        fprintf(stderr, "XPSemu: covers: %s not resolved (%#x)\n", host, rc);
        return "";
    }
    const uint8_t *b = (const uint8_t *)&addr;
    char ip[20];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return ip;
}
#endif

static std::mutex g_lock;
static std::deque<CoverWant> g_queue;
static std::set<std::pair<uint32_t, int>> g_asked; // This run
static std::string g_dir;
static std::vector<CoverDownloaded> g_done;
static int g_failed;
static bool g_running;

static const char *const kXdbFile[] = { "cover_front.jpg", "cover_back.jpg",
                                        "cover_spine.jpg" };
static const char *const kSuffix[] = { ".jpg", ".back.jpg", ".spine.jpg" };

std::string CoverDownloadUrl(uint32_t title_id, CoverPart part)
{
    char path[64];
    snprintf(path, sizeof(path), "%c%c/%03u/", (char)(title_id >> 24),
             (char)(title_id >> 16), (unsigned)(title_id & 0xffff));
    return std::string("https://raw.githubusercontent.com/xemu-project/xdb/"
                       "main/titles/") + path + kXdbFile[part];
}

//
// libretro, by name
//

// A name to compare by: its letters and digits in lower case, words only
// (no "the", "&" as "and", no trailing "xiso"), without what's in (..) [..].
static std::string NameKey(const std::string &name)
{
    std::vector<std::string> words;
    std::string word;
    int depth = 0;
    auto flush = [&]() {
        if (!word.empty() && word != "the") {
            words.push_back(word);
        }
        word.clear();
    };
    for (unsigned char c : name) {
        if (c == '(' || c == '[') {
            depth++;
            flush();
        } else if (c == ')' || c == ']') {
            depth = depth ? depth - 1 : 0;
        } else if (depth) {
            continue;
        } else if (c == '&') {
            flush();
            word = "and";
            flush();
        } else if (isalnum(c)) {
            word += (char)tolower(c);
        } else {
            flush();
        }
    }
    flush();
    if (!words.empty() && (words.back() == "xiso" || words.back() == "iso")) {
        words.pop_back();
    }
    std::string key;
    for (const auto &w : words) {
        key += w;
    }
    return key;
}

// Which of several pictures of a game: a release over a demo or beta, USA
// first, then the world, Europe, Japan.
static int ReleaseScore(const std::string &n)
{
    int score = n.find("(USA") != std::string::npos    ? 30 :
                n.find("(World") != std::string::npos  ? 25 :
                n.find("(Europe") != std::string::npos ? 20 :
                n.find("(Japan") != std::string::npos  ? 5 :
                                                         10;
    for (const char *bad : { "Beta", "Alpha", "Proto", "Demo", "Kiosk", "Sample",
                             "Disc 2", "Bonus" }) {
        if (n.find(bad) != std::string::npos) {
            score -= 50;
        }
    }
    if (n.find("(Rev") != std::string::npos || n.find("(v") != std::string::npos) {
        score -= 3;
    }
    return score;
}

std::string CoverLaunchboxArt(const std::vector<std::string> &names)
{
    std::set<std::string> keys;
    for (const auto &n : names) {
        std::string k = NameKey(n);
        if (k.size() >= 3) {
            keys.insert(k);
        }
    }
    for (const auto &entry : kLaunchboxXbox) {
        if (keys.count(NameKey(entry.name))) {
            return std::string("https://images.launchbox-app.com/") + entry.file;
        }
    }
    return "";
}

std::string CoverLibretroMatch(const std::vector<std::string> &names)
{
    std::set<std::string> keys;
    for (const auto &n : names) {
        std::string k = NameKey(n);
        if (k.size() >= 3) {
            keys.insert(k);
        }
    }
    std::string best;
    int best_score = -1000;
    for (const char *entry : kLibretroXbox) {
        std::string name = entry;
        if (!keys.count(NameKey(name))) {
            continue;
        }
        int score = ReleaseScore(name);
        if (score > best_score) {
            best_score = score;
            best = name;
        }
    }
    return best;
}

static std::string UrlEscape(const std::string &s)
{
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

//
// Downloading
//

static std::string NotFoundFile(const std::string &dir)
{
    return dir + "/.not-found";
}

static std::set<uint32_t> LoadNotFound(const std::string &dir)
{
    std::set<uint32_t> ids;
    FILE *f = fopen(NotFoundFile(dir).c_str(), "r");
    if (f) {
        unsigned id;
        while (fscanf(f, "%x", &id) == 1) {
            ids.insert(id);
        }
        fclose(f);
    }
    return ids;
}

static size_t Collect(void *ptr, size_t size, size_t n, void *out)
{
    auto *body = (std::string *)out;
    if (body->size() + size * n > 32 * 1024 * 1024) {
        return 0; // Not a cover
    }
    body->append((const char *)ptr, size * n);
    return size * n;
}

// One file: 1 got it, 0 not there, -1 failed (network). png: a PNG is
// wanted, else a JPEG; any: whatever it is.
static int Get(CURL *curl, const std::string &url, std::string *body,
               bool png, bool any = false)
{
    body->clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (res != CURLE_OK) {
        fprintf(stderr, "XPSemu: covers: %s\n", curl_easy_strerror(res));
        return -1;
    }
    if (code == 404) {
        return 0;
    }
    bool ok = any ? true :
              png ? body->size() > 8 && !body->compare(0, 4, "\x89PNG") :
                    body->size() > 3 && (unsigned char)(*body)[0] == 0xFF &&
                        (unsigned char)(*body)[1] == 0xD8;
    if (code != 200 || !ok) {
        fprintf(stderr, "XPSemu: covers: HTTP %ld, %zu bytes\n", code,
                body->size());
        return -1;
    }
    return 1;
}

static bool Save(const std::string &body, const std::string &path)
{
    std::string tmp = path + ".part";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "XPSemu: covers: can't write %s (%d)\n", tmp.c_str(),
                errno);
        return false;
    }
    bool ok = fwrite(body.data(), 1, body.size(), f) == body.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

// One picture: 1 saved, 0 found nowhere, -1 failed (network).
static int Fetch(CURL *curl, const CoverWant &want, const std::string &dir,
                 std::string *saved)
{
    char id[16];
    snprintf(id, sizeof(id), "%08X", want.title_id);
    std::string body;
    int got = Get(curl, CoverDownloadUrl(want.title_id, want.part), &body, false);
    std::string path = dir + "/" + id + kSuffix[want.part];
    std::string from = "xdb";
    if (got == 0 && want.part == COVER_FRONT) {
        std::string name = CoverLibretroMatch(want.names);
        if (!name.empty()) {
            got = Get(curl,
                      "https://raw.githubusercontent.com/libretro-thumbnails/"
                      "Microsoft_-_Xbox/master/Named_Boxarts/" +
                          UrlEscape(name) + ".png",
                      &body, true);
            path = dir + "/" + id + ".png";
            from = "libretro (" + name + ")";
        }
    }
    if (got <= 0) {
        fprintf(stderr, "XPSemu: cover %s %s: %s\n", id, kXdbFile[want.part],
                got ? "failed" : "found nowhere");
        return got;
    }
    if (!Save(body, path)) {
        return -1;
    }
    fprintf(stderr, "XPSemu: cover %s %s: from %s (%zu KB)\n", id,
            kXdbFile[want.part], from.c_str(), body.size() / 1024);
    *saved = path;
    return 1;
}

static CURL *NewHandle(struct curl_slist **resolve,
                       const char *host = "raw.githubusercontent.com");

static void Worker()
{
#ifdef __PROSPERO__
    struct curl_slist *resolve = NULL;
#endif
    CURL *curl = NewHandle(
#ifdef __PROSPERO__
        &resolve
#else
        NULL
#endif
    );
    int failed_in_a_row = 0;
    for (;;) {
        CoverWant want;
        std::string dir;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            // Offline: give up on this round (asked again next time).
            if (g_queue.empty() || !curl || failed_in_a_row >= 3) {
                g_failed += (int)g_queue.size();
                for (const auto &left : g_queue) {
                    g_asked.erase({ left.title_id, left.part });
                }
                g_queue.clear();
                g_running = false;
                break;
            }
            want = g_queue.front();
            g_queue.pop_front();
            dir = g_dir;
        }
        std::string saved;
        int got = Fetch(curl, want, dir, &saved);
        std::lock_guard<std::mutex> guard(g_lock);
        if (got > 0) {
            g_done.push_back({ want.title_id, want.part, saved });
            failed_in_a_row = 0;
        } else if (got == 0) {
            if (want.part == COVER_FRONT) {
                FILE *f = fopen(NotFoundFile(dir).c_str(), "a");
                if (f) {
                    fprintf(f, "%08X\n", want.title_id);
                    fclose(f);
                }
            }
            failed_in_a_row = 0;
        } else {
            g_failed++;
            g_asked.erase({ want.title_id, want.part }); // Again next time
            failed_in_a_row++;
        }
    }
    if (curl) {
        curl_easy_cleanup(curl);
    }
#ifdef __PROSPERO__
    curl_slist_free_all(resolve);
#endif
}

// A curl handle for raw.githubusercontent.com: certificates, timeouts,
// and on the PS5 the address (resolve: to free after the handle).
static CURL *NewHandle(struct curl_slist **resolve, const char *host)
{
    static bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    CURL *curl = curl_ready ? curl_easy_init() : NULL;
    (void)resolve;
    if (curl) {
        curl_blob ca = { (void *)kCoverCaCerts, sizeof(kCoverCaCerts) - 1,
                         CURL_BLOB_NOCOPY };
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "XPSemu");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // A thread of its own
    }
#ifdef __PROSPERO__
    // libc can't look names up in a PS5 app: the PS5's resolver gives the
    // address, else GitHub's (raw.githubusercontent.com's, long-standing).
    if (curl) {
        bool github = !strcmp(host, "raw.githubusercontent.com");
        std::string ip = Resolve(host);
        std::string ips = ip;
        if (github) {
            ips += std::string(ip.empty() ? "" : ",") +
                   "185.199.108.133,185.199.109.133,185.199.110.133,"
                   "185.199.111.133";
        }
        if (ips.empty()) {
            fprintf(stderr, "XPSemu: covers: can't find %s" "\n", host);
            return curl;
        }
        fprintf(stderr, "XPSemu: covers: %s at %s\n", host, ips.c_str());
        *resolve = curl_slist_append(NULL, (std::string(host) + ":443:" + ips).c_str());
        curl_easy_setopt(curl, CURLOPT_RESOLVE, *resolve);
    }
#endif
    return curl;
}

bool CoverHttpPost(const std::string &url, const std::string &json,
                   std::string *body)
{
    size_t from = url.find("://");
    from = from == std::string::npos ? 0 : from + 3;
    std::string host = url.substr(from, url.find_first_of(":/", from) - from);
    struct curl_slist *resolve = NULL, *headers = NULL;
    CURL *curl = NewHandle(&resolve, host.c_str());
    bool ok = false;
    if (curl) {
        headers = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
        long code = 0;
        ok = curl_easy_perform(curl) == CURLE_OK &&
             curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK &&
             code >= 200 && code < 300;
        curl_easy_cleanup(curl);
    }
    curl_slist_free_all(headers);
    curl_slist_free_all(resolve);
    return ok;
}

bool CoverHttpGet(const std::string &url, std::string *body)
{
    // The host: between "://" and the next "/".
    size_t from = url.find("://");
    from = from == std::string::npos ? 0 : from + 3;
    std::string host = url.substr(from, url.find('/', from) - from);
    struct curl_slist *resolve = NULL;
    CURL *curl = NewHandle(&resolve, host.c_str());
    bool ok = curl && Get(curl, url, body, false, true) > 0;
    if (curl) {
        curl_easy_cleanup(curl);
    }
    curl_slist_free_all(resolve);
    return ok;
}

void CoverDownloadRequest(const std::vector<CoverWant> &wants,
                          const std::string &dir)
{
    static bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curl_ready || wants.empty()) {
        return;
    }
    mkdir(dir.c_str(), 0777);
    std::set<uint32_t> none = LoadNotFound(dir);
    std::lock_guard<std::mutex> guard(g_lock);
    g_dir = dir;
    for (const auto &want : wants) {
        if (!want.title_id ||
            (want.part == COVER_FRONT && none.count(want.title_id))) {
            continue;
        }
        if (g_asked.insert({ want.title_id, want.part }).second) {
            // The viewer's pictures go first: someone is looking.
            if (want.part == COVER_FRONT) {
                g_queue.push_back(want);
            } else {
                g_queue.push_front(want);
            }
        }
    }
    if (!g_queue.empty() && !g_running) {
        g_running = true;
        std::thread(Worker).detach();
    }
}

std::vector<CoverDownloaded> CoverDownloadTake(bool *idle, int *failed)
{
    std::lock_guard<std::mutex> guard(g_lock);
    std::vector<CoverDownloaded> done;
    done.swap(g_done);
    *idle = !g_running;
    *failed = g_failed;
    g_failed = 0;
    return done;
}
