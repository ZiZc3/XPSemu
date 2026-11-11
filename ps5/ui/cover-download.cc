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
#include "launchbox-box-index.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef __PROSPERO__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

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

// host's IPv4 addresses ("a,b"), asked of a public name server over UDP
// (patch 166): the PS5's resolver fails in this app (0x8041013d, every
// lookup), so the art for shortcut backgrounds never came. "" if none.
static std::string DnsAsk(const char *server, const char *host)
{
    uint8_t q[300];
    size_t n = 0;
    const uint16_t id = (uint16_t)(time(NULL) ^ 0x5a17);
    const uint8_t head[12] = { (uint8_t)(id >> 8), (uint8_t)id, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
    memcpy(q, head, 12);
    n = 12;
    for (const char *p = host; *p;) {               // labels
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63 || n + len + 6 > sizeof(q)) {
            return "";
        }
        q[n++] = (uint8_t)len;
        memcpy(q + n, p, len);
        n += len;
        p += len + (dot ? 1 : 0);
    }
    const uint8_t tail[5] = { 0, 0, 1, 0, 1 };      // end, type A, class IN
    memcpy(q + n, tail, 5);
    n += 5;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return "";
    }
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    inet_pton(AF_INET, server, &sa.sin_addr);
    uint8_t r[1500];
    ssize_t got = -1;
    if (sendto(fd, q, n, 0, (struct sockaddr *)&sa, sizeof(sa)) == (ssize_t)n) {
        got = recv(fd, r, sizeof(r), 0);
    }
    close(fd);
    if (got < 12 || r[0] != head[0] || r[1] != head[1] || (r[3] & 0x0f)) {
        return "";
    }
    size_t at = n;                                  // past the question (echoed)
    const int answers = (r[6] << 8) | r[7];
    std::string ips;
    for (int i = 0; i < answers && at < (size_t)got; i++) {
        if ((r[at] & 0xc0) == 0xc0) {               // name: a pointer...
            at += 2;
        } else {                                    // ...or labels
            while (at < (size_t)got && r[at]) {
                at += r[at] + 1;
            }
            at++;
        }
        if (at + 10 > (size_t)got) {
            break;
        }
        const int type = (r[at] << 8) | r[at + 1];
        const int len = (r[at + 8] << 8) | r[at + 9];
        at += 10;
        if (at + len > (size_t)got) {
            break;
        }
        if (type == 1 && len == 4) {
            char ip[20];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", r[at], r[at + 1], r[at + 2], r[at + 3]);
            ips += (ips.empty() ? "" : ",") + std::string(ip);
        }
        at += len;
    }
    return ips;
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
    // A lookup can time out once (a shortcut's background was lost to one):
    // three tries, 5 s each with the resolver's own retries (patch 165).
    uint32_t addr = 0; // Network order
    int rc = -1;
    for (int attempt = 0; attempt < 3 && (rc < 0 || !addr); attempt++) {
        int rid = sceNetResolverCreate("xpsemu_covers", pool, 0);
        if (rid < 0) {
            fprintf(stderr, "XPSemu: covers: resolver %#x (pool %#x)\n", rid, pool);
            return "";
        }
        addr = 0;
        rc = sceNetResolverStartNtoa(rid, host, &addr, 5 * 1000 * 1000, 2, 0);
        sceNetResolverDestroy(rid);
        if (rc < 0 || !addr) {
            fprintf(stderr, "XPSemu: covers: %s lookup %d failed (%#x)\n", host, attempt + 1, rc);
        }
    }
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

// The disc's region, from its names ("Thief - Deadly Shadows (Europe) (En,Fr,De,It)" -> "Europe"): the first
// region word in brackets; "" for USA / World discs (xdb's art is theirs already) or none known.
static std::string DiscRegion(const std::vector<std::string> &names)
{
    static const char *const kRegions[] = { "Europe", "Japan", "Germany", "France", "Spain", "Italy",
                                            "UK", "Australia", "Netherlands", "Sweden", "Korea", "Asia" };
    for (const auto &n : names) {
        for (size_t at = n.find('('); at != std::string::npos; at = n.find('(', at + 1)) {
            const size_t end = n.find(')', at);
            const std::string group = n.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
            if (group.find("USA") != std::string::npos || group.find("World") != std::string::npos) {
                return "";
            }
            for (const char *r : kRegions) {
                if (group.find(r) != std::string::npos) {
                    return r;
                }
            }
        }
    }
    return "";
}

// libretro's picture of this game for that region (patch 315, issue #35 request): "(Europe)" alone best,
// then a group naming it ("(Europe, Australia)", "(USA, Europe)"); "" when it has none for the region.
static std::string CoverLibretroRegion(const std::vector<std::string> &names, const std::string &region)
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
        const size_t at = name.find('(');
        const size_t end = at == std::string::npos ? at : name.find(')', at);
        if (end == std::string::npos) {
            continue;
        }
        const std::string group = name.substr(at + 1, end - at - 1);
        if (group.find(region) == std::string::npos) {
            continue;
        }
        int score = ReleaseScore(name) + (group == region ? 100 : group.rfind(region, 0) == 0 ? 50 : 0);
        if (score > best_score) {
            best_score = score;
            best = name;
        }
    }
    return best;
}

// LaunchBox's box art of this game (patch 315), kind 'F' front, 'B' back, 'S' spine: the disc's region,
// else the nearest one (a European disc takes Europe, then the UK, Australia, Germany...). region "": an
// American or World disc, North America first, then whatever region has one. "" when none matches.
static std::string CoverLaunchboxBox(const std::vector<std::string> &names, const std::string &region, char kind)
{
    static const struct { const char *disc; const char *order[10]; } kOrder[] = {
        { "", { "North America", "United States", "World", "", "Canada", "Europe", "United Kingdom", "Australia", nullptr } },
        { "Europe", { "Europe", "United Kingdom", "Australia", "Oceania", "Germany", "France", "Spain", "Italy", nullptr } },
        { "UK", { "United Kingdom", "Europe", "Australia", "Oceania", "Germany", "France", nullptr } },
        { "Australia", { "Australia", "Oceania", "Europe", "United Kingdom", nullptr } },
        { "Germany", { "Germany", "Europe", "United Kingdom", "Australia", nullptr } },
        { "France", { "France", "Europe", "United Kingdom", "Australia", nullptr } },
        { "Spain", { "Spain", "Europe", "United Kingdom", "Australia", nullptr } },
        { "Italy", { "Italy", "Europe", "United Kingdom", "Australia", nullptr } },
        { "Netherlands", { "The Netherlands", "Europe", "United Kingdom", "Australia", nullptr } },
        { "Sweden", { "Europe", "United Kingdom", "Australia", nullptr } },
        { "Japan", { "Japan", "Asia", "Korea", nullptr } },
        { "Korea", { "Korea", "Asia", "Japan", nullptr } },
        { "Asia", { "Asia", "Japan", "Korea", nullptr } },
    };
    std::set<std::string> keys;
    for (const auto &n : names) {
        std::string k = NameKey(n);
        if (k.size() >= 3) {
            keys.insert(k);
        }
    }
    if (keys.empty()) {
        return "";
    }
    for (const auto &o : kOrder) {
        if (region != o.disc) {
            continue;
        }
        for (const char *want : o.order) {
            if (!want) {
                break;
            }
            for (const auto &e : kLaunchboxXboxBox) {
                if (e.kind == kind && !strcmp(e.region, want) && keys.count(NameKey(e.name))) {
                    return e.file;
                }
            }
        }
    }
    return "";
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

static CURL *NewHandle(struct curl_slist **resolve, const char *host);

// LaunchBox's picture (its own host: a handle of its own on the PS5). 1 got it (*png: it's a PNG), else 0.
static int LaunchboxGet(const std::string &file, std::string *body, bool *png)
{
    struct curl_slist *resolve = NULL;
    CURL *c = NewHandle(&resolve, "images.launchbox-app.com");
    int got = 0;
    if (c) {
        got = Get(c, "https://images.launchbox-app.com/" + file, body, false, true);
        curl_easy_cleanup(c);
    }
    curl_slist_free_all(resolve);
    *png = body->size() > 8 && !body->compare(0, 4, "\x89PNG");
    const bool jpg = body->size() > 3 && (unsigned char)(*body)[0] == 0xFF && (unsigned char)(*body)[1] == 0xD8;
    return got > 0 && (*png || jpg) ? 1 : 0;
}

// One picture: 1 saved, 0 found nowhere, -1 failed (network). Order (patch 315): a disc from another
// region than the USA takes its region's picture first (fronts: libretro then LaunchBox; backs and spines:
// LaunchBox); then xdb; then for a front libretro's best, for a back or spine LaunchBox's best.
static int Fetch(CURL *curl, const CoverWant &want, const std::string &dir,
                 std::string *saved)
{
    char id[16];
    snprintf(id, sizeof(id), "%08X", want.title_id);
    const char kind = want.part == COVER_FRONT ? 'F' : want.part == COVER_BACK ? 'B' : 'S';
    std::string body;
    int got = 0;
    bool png = false;
    std::string path, from;
    const std::string region = DiscRegion(want.names);
    if (!region.empty()) {
        if (want.part == COVER_FRONT) {
            const std::string name = CoverLibretroRegion(want.names, region);
            if (!name.empty()) {
                got = Get(curl,
                          "https://raw.githubusercontent.com/libretro-thumbnails/"
                          "Microsoft_-_Xbox/master/Named_Boxarts/" + UrlEscape(name) + ".png",
                          &body, true);
                got = got > 0 ? got : 0;    // not there or failed: the next way
                path = dir + "/" + id + ".png";
                from = "libretro, the disc's region (" + name + ")";
            }
        }
        const std::string lb = got > 0 ? std::string() : CoverLaunchboxBox(want.names, region, kind);
        if (!lb.empty() && (got = LaunchboxGet(lb, &body, &png)) > 0) {
            path = dir + "/" + id + (want.part == COVER_FRONT ? (png ? ".png" : ".jpg") : kSuffix[want.part]);
            from = "LaunchBox, the disc's region (" + lb + ")";
        }
        if (got <= 0) {
            fprintf(stderr, "XPSemu: cover %s %s: no %s one: the usual one\n", id, kXdbFile[want.part], region.c_str());
        }
    }
    if (got <= 0) {
        got = Get(curl, CoverDownloadUrl(want.title_id, want.part), &body, false);
        path = dir + "/" + id + kSuffix[want.part];
        from = "xdb";
    }
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
    } else if (got == 0) {
        // a back or spine xdb hasn't (most spines): LaunchBox's, North America first
        const std::string lb = CoverLaunchboxBox(want.names, "", kind);
        if (!lb.empty() && (got = LaunchboxGet(lb, &body, &png)) > 0) {
            path = dir + "/" + id + kSuffix[want.part];     // a PNG under .jpg: stb_image reads it by its bytes
            from = "LaunchBox (" + lb + ")";
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
        for (const char *server : { "1.1.1.1", "8.8.8.8" }) {
            if (ip.empty()) {
                ip = DnsAsk(server, host);
                fprintf(stderr, "XPSemu: covers: %s from %s: %s\n", host, server,
                        ip.empty() ? "no answer" : ip.c_str());
            }
        }
        if (ip.empty() && !strcmp(host, "images.launchbox-app.com")) {
            ip = "104.21.46.132,172.67.139.41";     // its addresses (Cloudflare), 2026-11
        }
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
    // Every folder on the way (issue #35): /data/xemu was only there for players who had Alpha 2, so on a
    // new 3.x install mkdir of /data/xemu/covers failed and no cover was ever saved.
    for (size_t at = dir.find('/', 1); ; at = dir.find('/', at + 1)) {
        mkdir(dir.substr(0, at).c_str(), 0777);
        if (at == std::string::npos) {
            break;
        }
    }
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

//
// The updater's GET (patch 230): files of any size, to disk, redirects
// followed by hand (GitHub sends a release's file to another host, and on
// the PS5 every host has to be looked up first).
//

struct FetchSink {
    CURL *curl;
    FILE *file;
    std::string *body;
    long long got, total;
    const std::function<void(long long, long long)> *progress;
};

static size_t FetchWrite(void *ptr, size_t size, size_t n, void *out)
{
    auto *sink = (FetchSink *)out;
    const size_t len = size * n;
    if (sink->file) {
        if (fwrite(ptr, 1, len, sink->file) != len) {
            return 0;
        }
    } else {
        if (sink->body->size() + len > 8 * 1024 * 1024) {
            return 0; // Not an answer this small reply should be
        }
        sink->body->append((const char *)ptr, len);
    }
    sink->got += (long long)len;
    if (sink->total <= 0) {
        curl_off_t length = -1;
        if (curl_easy_getinfo(sink->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                              &length) == CURLE_OK && length > 0) {
            sink->total = (long long)length;
        }
    }
    if (*sink->progress) {
        (*sink->progress)(sink->got, sink->total);
    }
    return len;
}

//
// Big files (patch 314): 8 connections, each its own byte range of the file, and big buffers
// (512 KB reads, a 4 MB socket receive buffer) - one connection with curl's 16 KB reads and the
// PS5's small default socket buffer crawled.
//

static int BigSocketBuffer(void *, curl_socket_t fd, curlsocktype)
{
    int size = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    return CURL_SOCKOPT_OK;
}

static void FastTransfer(CURL *curl)
{
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L * 1024);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, BigSocketBuffer);
}

static std::string HostOf(const std::string &url)
{
    size_t from = url.find("://");
    from = from == std::string::npos ? 0 : from + 3;
    return url.substr(from, url.find_first_of(":/?", from) - from);
}

static size_t RangesHeader(char *b, size_t size, size_t n, void *out)
{
    std::string h(b, size * n);
    for (char &c : h) {
        c = (char)tolower((unsigned char)c);
    }
    if (h.rfind("accept-ranges:", 0) == 0 && h.find("bytes") != std::string::npos) {
        *(bool *)out = true;
    }
    return size * n;
}

// The file's last address (redirects followed), its size, and whether it can be fetched in ranges.
static bool Probe(std::string *at, long long *length, bool *ranges, long connect_s)
{
    for (int hop = 0; hop < 6; hop++) {
        struct curl_slist *resolve = NULL, *headers = NULL;
        CURL *curl = NewHandle(&resolve, HostOf(*at).c_str());
        if (!curl) {
            curl_slist_free_all(resolve);
            return false;
        }
        if (HostOf(*at) == "api.github.com") {
            headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
        *ranges = false;
        curl_easy_setopt(curl, CURLOPT_URL, at->c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, RangesHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, ranges);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "XPSemu-Updater");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_s);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        const CURLcode res = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        std::string next;
        char *where = NULL;
        if (res == CURLE_OK && code >= 300 && code < 400 &&
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &where) == CURLE_OK && where) {
            next = where;
        }
        curl_off_t len = -1;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        curl_slist_free_all(resolve);
        if (res != CURLE_OK) {
            return false;
        }
        if (!next.empty()) {
            *at = next;
            continue;
        }
        *length = (long long)len;
        return code == 200;
    }
    return false;
}

struct PartSink {
    int fd;
    long long at;
    std::atomic<long long> *all;
};

static size_t PartWrite(void *ptr, size_t size, size_t n, void *out)
{
    auto *sink = (PartSink *)out;
    const size_t len = size * n;
    if (pwrite(sink->fd, ptr, len, (off_t)sink->at) != (ssize_t)len) {
        return 0;
    }
    sink->at += (long long)len;
    sink->all->fetch_add((long long)len);
    return len;
}

static bool FetchParallel(const std::string &at, const std::string &path, long long total,
                          const std::function<void(long long, long long)> &progress,
                          long connect_s, long stall_s, std::string *why)
{
    const int kParts = 8;
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0 || ftruncate(fd, (off_t)total) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        *why = "can't write " + path;
        return false;
    }
    // The handles (and their name lookups) made here, one at a time; the transfers on threads.
    struct Part {
        CURL *curl;
        struct curl_slist *resolve;
        long long from, to;
        PartSink sink;
        bool ok;
    };
    std::atomic<long long> all{ 0 };
    std::atomic<int> done{ 0 };
    std::vector<Part> parts(kParts);
    const long long step = total / kParts;
    for (int i = 0; i < kParts; i++) {
        Part &p = parts[i];
        p.resolve = NULL;
        p.curl = NewHandle(&p.resolve, HostOf(at).c_str());
        p.from = step * i;
        p.to = i == kParts - 1 ? total - 1 : step * (i + 1) - 1;
        p.sink = { fd, p.from, &all };
        p.ok = false;
    }
    std::vector<std::thread> threads;
    for (Part &p : parts) {
        threads.emplace_back([&p, &at, &done, connect_s, stall_s]() {
            for (int attempt = 0; p.curl && attempt < 4 && p.sink.at <= p.to; attempt++) {
                const std::string range = std::to_string(p.sink.at) + "-" + std::to_string(p.to);
                curl_easy_setopt(p.curl, CURLOPT_URL, at.c_str());
                curl_easy_setopt(p.curl, CURLOPT_RANGE, range.c_str());
                curl_easy_setopt(p.curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(p.curl, CURLOPT_WRITEFUNCTION, PartWrite);
                curl_easy_setopt(p.curl, CURLOPT_WRITEDATA, &p.sink);
                curl_easy_setopt(p.curl, CURLOPT_USERAGENT, "XPSemu-Updater");
                curl_easy_setopt(p.curl, CURLOPT_CONNECTTIMEOUT, connect_s);
                curl_easy_setopt(p.curl, CURLOPT_TIMEOUT, 0L);
                curl_easy_setopt(p.curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
                curl_easy_setopt(p.curl, CURLOPT_LOW_SPEED_TIME, stall_s);
                FastTransfer(p.curl);
                const CURLcode res = curl_easy_perform(p.curl);
                long code = 0;
                curl_easy_getinfo(p.curl, CURLINFO_RESPONSE_CODE, &code);
                if (res == CURLE_OK && code != 206 && code != 200) {
                    break;  // refused: not worth asking again
                }
                if (res == CURLE_OK && code == 200 && p.from != 0) {
                    break;  // the whole file, not the range: can't use it here
                }
            }
            p.ok = p.sink.at > p.to;
            done.fetch_add(1);
        });
    }
    // Progress shown from here (the updater's thread) until every part's thread is done.
    while (done.load() < kParts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (progress) {
            progress(all.load(), total);
        }
    }
    for (std::thread &t : threads) {
        t.join();
    }
    if (progress) {
        progress(all.load(), total);
    }
    bool ok = true;
    for (Part &p : parts) {
        ok = ok && p.ok;
        if (p.curl) {
            curl_easy_cleanup(p.curl);
        }
        curl_slist_free_all(p.resolve);
    }
    ok = (close(fd) == 0) && ok;
    if (!ok) {
        *why = "cut short (" + std::to_string(all.load()) + " of " + std::to_string(total) + " bytes)";
    }
    return ok;
}

bool CoverHttpFetch(const std::string &url, const std::string &path,
                    std::string *body,
                    const std::function<void(long long, long long)> &progress,
                    long connect_s, long stall_s, std::string *why)
{
    std::string at = url;
    why->clear();
    if (!path.empty()) {
        std::string last = url;
        long long length = -1;
        bool ranges = false;
        if (Probe(&last, &length, &ranges, connect_s) && ranges && length >= (8LL << 20)) {
            const auto start = std::chrono::steady_clock::now();
            if (FetchParallel(last, path, length, progress, connect_s, stall_s, why)) {
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                fprintf(stderr, "XPSemu: download: %lld bytes in %.1f s (%.1f MB/s), 8 connections\n",
                        length, s, s > 0 ? length / s / 1048576.0 : 0.0);
                return true;
            }
            fprintf(stderr, "XPSemu: download: 8 connections failed (%s): one connection\n", why->c_str());
            why->clear();
        } else {
            fprintf(stderr, "XPSemu: download: one connection (size %lld, ranges %d)\n", length, (int)ranges);
        }
    }
    for (int hop = 0; hop < 6; hop++) {
        size_t from = at.find("://");
        from = from == std::string::npos ? 0 : from + 3;
        const std::string host = at.substr(from, at.find_first_of(":/?", from) - from);
        struct curl_slist *resolve = NULL, *headers = NULL;
        CURL *curl = NewHandle(&resolve, host.c_str());
        if (!curl) {
            *why = "no network";
            curl_slist_free_all(resolve);
            return false;
        }
        FetchSink sink = { curl, NULL, body, 0, 0, &progress };
        if (!path.empty()) {
            sink.file = fopen(path.c_str(), "wb");
            if (!sink.file) {
                *why = "can't write " + path;
                curl_easy_cleanup(curl);
                curl_slist_free_all(resolve);
                return false;
            }
        } else {
            body->clear();
        }
        if (host == "api.github.com") {
            headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
        curl_easy_setopt(curl, CURLOPT_URL, at.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FetchWrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "XPSemu-Updater");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_s);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);    // a big file takes what it takes...
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, stall_s); // ...while it moves
        if (!path.empty()) {
            FastTransfer(curl);     // big reads + socket buffer (patch 314)
        }
        const CURLcode res = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        std::string next;
        if (res == CURLE_OK && code >= 300 && code < 400) {
            char *where = NULL;
            if (curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &where) == CURLE_OK && where) {
                next = where;
            }
        }
        bool ok = true;
        if (sink.file) {
            ok = fclose(sink.file) == 0;
        }
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        curl_slist_free_all(resolve);
        if (res != CURLE_OK) {
            *why = curl_easy_strerror(res);
            return false;
        }
        if (!next.empty()) {
            at = next;
            continue;
        }
        if (code != 200) {
            *why = "HTTP " + std::to_string(code);
            return false;
        }
        if (!ok) {
            *why = "can't write " + path;
            return false;
        }
        if (sink.total > 0 && sink.got != sink.total) {
            *why = "cut short (" + std::to_string(sink.got) + " of " +
                   std::to_string(sink.total) + " bytes)";
            return false;
        }
        return true;
    }
    *why = "too many redirects";
    return false;
}
