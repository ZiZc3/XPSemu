//
// XPSemu: trainers (.etm) for the dashboard and the quick menu (patch 246). See trainers.hh.
// Our own code. GPL-3.0-or-later.
//
#include "trainers.hh"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "native/core/trainer.h"
#include "xog_ui.h"

namespace {

const char *const kDir = "/data/xpsemu/trainers";
const char *const kCache = "/data/xpsemu/trainers-index.txt";

std::mutex g_lock;
std::vector<TrainerEntry> g_index;
bool g_ready = false, g_busy = false;
unsigned g_gen = 0;
time_t g_scanned_at = 0;

struct Found {
    std::string path;
    long long size, mtime;
};

struct Cached {
    long long size = -1, mtime = -1;
    bool ok = false;
    TrainerEntry e;
};

bool IsTrainerFile(const char *name)
{
    const size_t l = strlen(name);
    return l > 4 && !strcasecmp(name + l - 4, ".etm");
}

void Walk(const std::string &dir, int depth, std::vector<Found> &out)
{
    if (depth > 6) {
        return;
    }
    DIR *d = opendir(dir.c_str());
    if (!d) {
        return;
    }
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        const std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            Walk(p, depth + 1, out);
        } else if (IsTrainerFile(e->d_name) && st.st_size > 0 && st.st_size <= (off_t)NC_TRAINER_MAX_SIZE) {
            out.push_back({ p, (long long)st.st_size, (long long)st.st_mtime });
        }
    }
    closedir(d);
}

std::string Clean(std::string s)
{
    for (char &c : s) {
        if (c == '\t' || c == '\n' || c == '\r' || c == '\x1f') {
            c = ' ';
        }
    }
    return s;
}

std::vector<std::string> Split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep)) {
        out.push_back(cur);
    }
    return out;
}

std::map<std::string, Cached> LoadCache()
{
    std::map<std::string, Cached> m;
    std::ifstream in(kCache);
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string> f = Split(line, '\t');
        if (f.size() < 4) {
            continue;
        }
        Cached c;
        c.size = atoll(f[1].c_str());
        c.mtime = atoll(f[2].c_str());
        c.ok = f[3] == "1";
        c.e.path = f[0];
        if (c.ok && f.size() >= 7) {
            c.e.name = f[4];
            c.e.author = f[5];
            for (const std::string &id : Split(f[6], ',')) {
                if (!id.empty()) {
                    c.e.ids.push_back((uint32_t)strtoul(id.c_str(), nullptr, 16));
                }
            }
            if (f.size() >= 8) {
                c.e.options = Split(f[7], '\x1f');
            }
        }
        m[f[0]] = c;
    }
    return m;
}

void SaveCache(const std::map<std::string, Cached> &m)
{
    const std::string tmp = std::string(kCache) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (const auto &kv : m) {
            const Cached &c = kv.second;
            out << Clean(kv.first) << '\t' << c.size << '\t' << c.mtime << '\t' << (c.ok ? 1 : 0);
            if (c.ok) {
                out << '\t' << Clean(c.e.name) << '\t' << Clean(c.e.author) << '\t';
                for (size_t i = 0; i < c.e.ids.size(); i++) {
                    char id[16];
                    snprintf(id, sizeof(id), "%s%08X", i ? "," : "", c.e.ids[i]);
                    out << id;
                }
                out << '\t';
                for (size_t i = 0; i < c.e.options.size(); i++) {
                    out << (i ? "\x1f" : "") << Clean(c.e.options[i]);
                }
            }
            out << '\n';
        }
    }
    rename(tmp.c_str(), kCache);
}

bool ReadEntry(const std::string &path, TrainerEntry *e, std::string *why)
{
    std::unique_ptr<NcTrainerInfo> info(new NcTrainerInfo());
    char w[160] = "";
    if (!nc_trainer_read(path.c_str(), info.get(), w, sizeof(w))) {
        if (why) {
            *why = w;
        }
        return false;
    }
    e->path = path;
    e->name = info->name;
    e->author = info->author;
    e->ids.assign(info->title_ids, info->title_ids + info->nids);
    e->options.clear();
    for (unsigned i = 0; i < info->noptions; i++) {
        e->options.push_back(info->options[i]);
    }
    if (e->name.empty()) {
        const size_t slash = path.find_last_of('/');
        e->name = slash == std::string::npos ? path : path.substr(slash + 1);
    }
    return true;
}

void Scan()
{
    const struct timespec t0 = [] { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t; }();
    std::map<std::string, Cached> cache = LoadCache(), next;
    std::vector<Found> files;
    mkdir(kDir, 0777);
    Walk(kDir, 0, files);
    for (const char *drive : { "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4", "/mnt/usb5",
                               "/mnt/usb6", "/mnt/usb7", "/mnt/ext0", "/mnt/ext1" }) {
        Walk(std::string(drive) + "/xpsemu/trainers", 0, files);
    }
    unsigned read = 0, bad = 0;
    bool changed = cache.size() != files.size();
    std::vector<TrainerEntry> index;
    for (const Found &f : files) {
        auto it = cache.find(f.path);
        Cached c;
        if (it != cache.end() && it->second.size == f.size && it->second.mtime == f.mtime) {
            c = it->second;
        } else {
            changed = true;
            read++;
            c.size = f.size;
            c.mtime = f.mtime;
            std::string why;
            c.ok = ReadEntry(f.path, &c.e, &why);
            c.e.path = f.path;
            if (!c.ok) {
                xog_ui_log("trainer %s refused: %s", f.path.c_str(), why.c_str());
            }
        }
        bad += !c.ok;
        if (c.ok) {
            index.push_back(c.e);
        }
        next[f.path] = c;
    }
    if (changed) {
        SaveCache(next);
    }
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    xog_ui_log("trainers: %u files (%u read now, %u refused) in %.0f ms", (unsigned)files.size(), read, bad,
               (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    std::lock_guard<std::mutex> guard(g_lock);
    g_index.swap(index);
    g_ready = true;
    g_busy = false;
    g_gen++;
    g_scanned_at = time(nullptr);
}

std::string ChoicePath(const std::string &key)
{
    return std::string(kDir) + "/" + key + ".txt";
}

} // namespace

void TrainersScan(bool force)
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_busy || (!force && g_ready && time(nullptr) - g_scanned_at < 20)) {
        return;
    }
    g_busy = true;
    std::thread(Scan).detach();
}

bool TrainersReady()
{
    std::lock_guard<std::mutex> guard(g_lock);
    return g_ready;
}

unsigned TrainersGeneration()
{
    std::lock_guard<std::mutex> guard(g_lock);
    return g_gen;
}

std::vector<TrainerEntry> TrainersFor(uint32_t title_id)
{
    std::vector<TrainerEntry> out;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        for (const TrainerEntry &e : g_index) {
            if (title_id && std::find(e.ids.begin(), e.ids.end(), title_id) != e.ids.end()) {
                out.push_back(e);
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const TrainerEntry &a, const TrainerEntry &b) {
        return a.name != b.name ? a.name < b.name : a.path < b.path;
    });
    return out;
}

TrainerChoice TrainerChoiceLoad(const std::string &key)
{
    TrainerChoice c;
    std::ifstream in(ChoicePath(key));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("file=", 0) == 0) {
            c.file = line.substr(5);
        } else if (line.rfind("on=", 0) == 0) {
            c.on = line.substr(3) == "1";
        } else if (line.rfind("opts=", 0) == 0) {
            c.opts = line.substr(5);
        }
    }
    return c;
}

void TrainerChoiceSave(const std::string &key, const TrainerChoice &c)
{
    if (key.empty()) {
        return;
    }
    mkdir(kDir, 0777);
    const std::string file = ChoicePath(key), tmp = file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "file=" << c.file << "\non=" << (c.on ? 1 : 0) << "\nopts=" << c.opts << "\n";
    }
    rename(tmp.c_str(), file.c_str());
}

bool TrainerReadOne(const std::string &path, TrainerEntry *out)
{
    return ReadEntry(path, out, nullptr);
}

std::string TrainerStartLines(const std::string &key, uint32_t title_id)
{
    const TrainerChoice c = TrainerChoiceLoad(key);
    if (!c.on || c.file.empty()) {
        return "";
    }
    TrainerEntry e;
    std::string why;
    if (!ReadEntry(c.file, &e, &why)) {
        xog_ui_log("trainer for %s not used: %s: %s", key.c_str(), c.file.c_str(), why.c_str());
        return "";
    }
    if (std::find(e.ids.begin(), e.ids.end(), title_id) == e.ids.end()) {
        xog_ui_log("trainer for %s not used: %s is for another game", key.c_str(), c.file.c_str());
        return "";
    }
    std::string bits = c.opts;
    bits.resize(e.options.size(), '0');
    for (char &ch : bits) {
        ch = ch == '1' ? '1' : '0';
    }
    return "trainer=" + c.file + "\ntrainer_opts=" + bits + "\n";
}
