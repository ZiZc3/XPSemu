//
// XPSemu: dev builds: how stable XPSemu has been, from the crash journal.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "dev-report.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <sstream>

#ifndef DEV_REPORT_DIR // A PC test sets its own
#define DEV_REPORT_DIR "/data/xemu/dev"
#endif
#ifndef DEV_REPORT_SYMBOLS
#define DEV_REPORT_SYMBOLS { "/app0/dev-symbols.txt", \
                             "/data/homebrew/PPSA97358/dev-symbols.txt" }
#endif
#ifndef DEV_REPORT_BUILD
extern "C" const char *xemu_dev_build(void);
#define DEV_REPORT_BUILD xemu_dev_build()
#endif

namespace {

// This build's functions (ps5/build-dev.sh: llvm-nm of llvm-pie.elf), for
// crashes in this build. Offsets are from the eboot's load address.
struct Symbols {
    bool tried = false;
    std::vector<unsigned long long> addr;
    std::vector<unsigned int> name_at;
    std::string names;

    void Load()
    {
        if (tried) {
            return;
        }
        tried = true;
        for (const char *path : DEV_REPORT_SYMBOLS) {
            FILE *f = fopen(path, "r");
            if (!f) {
                continue;
            }
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                char *end;
                unsigned long long a = strtoull(line, &end, 16);
                if (end == line || *end != ' ') {
                    continue;
                }
                char *name = end + 1;
                name[strcspn(name, "\r\n")] = 0;
                addr.push_back(a);
                name_at.push_back((unsigned int)names.size());
                names.append(name);
                names.push_back(0);
            }
            fclose(f);
            break;
        }
    }

    // "function+0x12", or "" when unknown
    std::string Name(unsigned long long offset)
    {
        Load();
        auto it = std::upper_bound(addr.begin(), addr.end(), offset);
        if (it == addr.begin()) {
            return "";
        }
        size_t i = it - addr.begin() - 1;
        char buf[48];
        snprintf(buf, sizeof(buf), "+%#llx", offset - addr[i]);
        return std::string(&names[name_at[i]]) + buf;
    }
};

struct Crash {
    long long time;
    int sig;
    std::string where, build, game;
    unsigned long long offset, fault;
    std::vector<unsigned long long> stack;
};

struct Run {
    std::string title, name;
    long long start, end;
    char outcome; // 'o' ok, 'c' crashed, 's' ended without a word, 'f' froze
};

std::string Clock(long long t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string Hours(double h)
{
    char buf[32];
    if (h < 1) {
        snprintf(buf, sizeof(buf), "%.0f min", h * 60);
    } else {
        snprintf(buf, sizeof(buf), "%.1f h", h);
    }
    return buf;
}

const char *SignalName(int sig)
{
    switch (sig) {
    case 4: return "illegal instruction";
    case 6: return "abort";
    case 8: return "arithmetic";
    case 10: return "bus error";
    case 11: return "bad memory access";
    case 12: return "refused system call";
    default: return "signal";
    }
}

} // namespace

DevReport DevReportBuild()
{
    DevReport r;
    std::vector<Run> runs;
    std::vector<Crash> crashes;
    Symbols symbols;
    std::string this_build = DEV_REPORT_BUILD;

    FILE *f = fopen(DEV_REPORT_DIR "/journal.txt", "r");
    std::string build = "unknown";
    bool session = false, game = false;
    Run cur{};
    long long last = 0;

    auto end_game = [&](long long t, char outcome) {
        if (game) {
            cur.end = std::max(t, cur.start);
            cur.outcome = outcome;
            runs.push_back(cur);
            game = false;
        }
    };

    if (f) {
        char line[2048];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char kind = line[0];
            long long t = 0;
            int used = 0;
            if (!kind || sscanf(line + 1, " %lld %n", &t, &used) < 1) {
                continue;
            }
            const char *rest = line + 1 + used;
            switch (kind) {
            case 'S':
                if (session) { // The one before never said goodbye
                    if (game) {
                        end_game(last, 's');
                    } else {
                        r.dash_silent++;
                    }
                }
                session = true;
                r.sessions++;
                build = rest[0] ? rest : "unknown";
                break;
            case 'G': {
                end_game(t, 'o');
                char title[16] = "";
                int n = 0;
                sscanf(rest, "%15s %n", title, &n);
                cur = Run{ title, rest + n, t, t, 'o' };
                game = true;
                break;
            }
            case 'E':
                end_game(t, 'o');
                break;
            case 'F':
                end_game(t, 'f');
                break;
            case 'W': // The whole app hung (the stress test's watchdog)
                if (game) {
                    end_game(t, 'f');
                } else {
                    r.dash_silent++;
                }
                session = false;
                break;
            case 'Q':
                end_game(t, 'o');
                session = false;
                break;
            case 'C': {
                Crash c{ t, 0, "", build, game ? cur.name : "" , 0, 0, {} };
                std::istringstream in(rest);
                std::string off, fault, s;
                in >> c.sig >> c.where >> off >> fault;
                c.offset = strtoull(off.c_str(), NULL, 16);
                c.fault = strtoull(fault.c_str(), NULL, 16);
                while (in >> s) {
                    c.stack.push_back(strtoull(s.c_str(), NULL, 16));
                }
                crashes.push_back(c);
                if (game) {
                    end_game(t, 'c');
                } else {
                    r.dash_crashed++;
                }
                session = false;
                break;
            }
            default: // H and anything newer
                break;
            }
            last = t;
        }
        fclose(f);
    }
    // The last session is this one, still running: its game isn't counted.

    // What a crash is, for grouping: its function in this build.
    auto cause = [&](const Crash &c) -> std::string {
        bool ours = c.build == this_build && !this_build.empty();
        auto fn = [&](unsigned long long o) {
            std::string n = ours ? symbols.Name(o) : "";
            if (n.empty()) {
                char buf[32];
                snprintf(buf, sizeof(buf), "eboot+%#llx", o);
                return std::string(buf);
            }
            return n.substr(0, n.find('+'));
        };
        if (c.where == "eboot") {
            return fn(c.offset);
        }
        std::string from = c.stack.empty() ? "?" : fn(c.stack[0]);
        return std::string(c.where == "jit" ? "Xbox code" : "the system") +
               ", from " + from;
    };

    std::map<std::string, int> causes;
    std::map<std::string, std::string> cause_games;
    for (const auto &c : crashes) {
        std::string k = cause(c);
        causes[k]++;
        if (!c.game.empty() &&
            cause_games[k].find(c.game) == std::string::npos) {
            cause_games[k] += (cause_games[k].empty() ? "" : ", ") + c.game;
        }
    }
    std::vector<std::pair<int, std::string>> top;
    for (const auto &c : causes) {
        top.push_back({ c.second, c.first });
    }
    std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });

    struct PerGame { double hours = 0; int runs = 0, ok = 0, c = 0, s = 0, f = 0; };
    std::map<std::string, PerGame> games;
    for (const auto &run : runs) {
        r.runs++;
        double h = (run.end - run.start) / 3600.0;
        r.hours += h;
        PerGame &g = games[run.name];
        g.hours += h;
        g.runs++;
        switch (run.outcome) {
        case 'o': r.ok++; g.ok++; break;
        case 'c': r.crashed++; g.c++; break;
        case 's': r.silent++; g.s++; break;
        case 'f': r.froze++; g.f++; break;
        }
    }

    char buf[256];
    int bad = r.crashed + r.silent + r.froze;
    if (r.runs) {
        snprintf(buf, sizeof(buf), "Stable: %.0f%% of %d game runs ended normally",
                 100.0 * r.ok / r.runs, r.runs);
    } else {
        snprintf(buf, sizeof(buf), "No game runs yet (sessions: %d)", r.sessions);
    }
    r.summary.push_back(buf);
    if (bad) {
        snprintf(buf, sizeof(buf), "Played %s: a problem every %s",
                 Hours(r.hours).c_str(), Hours(r.hours / bad).c_str());
    } else {
        snprintf(buf, sizeof(buf), "Played %s, no problems", Hours(r.hours).c_str());
    }
    r.summary.push_back(buf);
    snprintf(buf, sizeof(buf), "Crashed %d, froze %d, ended without a word %d",
             r.crashed, r.froze, r.silent);
    r.summary.push_back(buf);
    if (r.dash_crashed || r.dash_silent) {
        snprintf(buf, sizeof(buf), "In the dashboard: crashed %d, ended %d",
                 r.dash_crashed, r.dash_silent);
        r.summary.push_back(buf);
    }
    for (size_t i = 0; i < top.size() && i < 4; i++) {
        snprintf(buf, sizeof(buf), "%dx %s", top[i].first,
                 top[i].second.substr(0, 60).c_str());
        r.summary.push_back(buf);
    }

    // The whole report, to send.
    std::string out;
    out += "XPSemu stability report (dev build " + this_build + ")\n";
    out += "Made " + Clock(time(NULL)) + " from " DEV_REPORT_DIR "/journal.txt\n\n";
    for (const auto &l : r.summary) {
        out += l + "\n";
    }
    out += "\n\"Ended without a word\": the app was gone with no crash report: a GPU\n"
           "fault (the PS5 closes the app), a hang the PS5 ended, or XPSemu closed\n"
           "from the PS5's home screen.\n";

    out += "\n== Games ==\n";
    std::vector<std::pair<std::string, PerGame>> by_game(games.begin(), games.end());
    std::sort(by_game.begin(), by_game.end(), [](const auto &a, const auto &b) {
        return a.second.hours > b.second.hours;
    });
    for (const auto &g : by_game) {
        snprintf(buf, sizeof(buf),
                 "%-40.40s %8s  runs %3d  ok %3d  crashed %2d  froze %2d  "
                 "without a word %2d\n",
                 g.first.c_str(), Hours(g.second.hours).c_str(), g.second.runs,
                 g.second.ok, g.second.c, g.second.f, g.second.s);
        out += buf;
    }

    out += "\n== Crash causes ==\n";
    for (const auto &t : top) {
        snprintf(buf, sizeof(buf), "%3dx %s\n", t.first, t.second.c_str());
        out += buf;
        out += "      games: " + (cause_games[t.second].empty() ?
                                  std::string("(dashboard)") :
                                  cause_games[t.second]) + "\n";
    }

    out += "\n== Every crash ==\n";
    for (const auto &c : crashes) {
        bool ours = c.build == this_build && !this_build.empty();
        snprintf(buf, sizeof(buf),
                 "%s  %s  signal %d (%s) in %s+%#llx, address %#llx, build %s\n",
                 Clock(c.time).c_str(),
                 c.game.empty() ? "(dashboard)" : c.game.c_str(), c.sig,
                 SignalName(c.sig), c.where.c_str(), c.offset, c.fault,
                 c.build.c_str());
        out += buf;
        if (c.where == "eboot" && ours) {
            out += "      at   " + symbols.Name(c.offset) + "\n";
        }
        for (auto s : c.stack) {
            std::string n = ours ? symbols.Name(s) : "";
            snprintf(buf, sizeof(buf), "      from eboot+%#llx %s\n", s, n.c_str());
            out += buf;
        }
    }

    out += "\n== Game runs ==\n";
    for (const auto &run : runs) {
        const char *how = run.outcome == 'o' ? "ok" :
                          run.outcome == 'c' ? "CRASHED" :
                          run.outcome == 'f' ? "FROZE" : "ENDED WITHOUT A WORD";
        snprintf(buf, sizeof(buf), "%s  %-36.36s %8s  %s\n",
                 Clock(run.start).c_str(), run.name.c_str(),
                 Hours((run.end - run.start) / 3600.0).c_str(), how);
        out += buf;
    }

    FILE *o = fopen(DEV_REPORT_DIR "/report.txt.part", "w");
    if (o) {
        fwrite(out.data(), 1, out.size(), o);
        fclose(o);
        rename(DEV_REPORT_DIR "/report.txt.part", DEV_REPORT_DIR "/report.txt");
    }
    return r;
}
