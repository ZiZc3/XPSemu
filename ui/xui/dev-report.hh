//
// XPSemu: dev builds: how stable XPSemu has been, from the crash journal
// (ui/xemu-dev-ps5.h). Only used when XPSEMU_DEV is 1.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <string>
#include <vector>

struct DevReport {
    int sessions = 0;   // XPSemu starts
    int runs = 0;       // games played to an end (the one running now not counted)
    int ok = 0;         // ...that ended normally (stopped, another game, quit)
    int crashed = 0, silent = 0, froze = 0;
    int dash_crashed = 0, dash_silent = 0; // no game running
    double hours = 0;   // game time
    std::vector<std::string> summary; // a few lines for the System page
};

// Reads /data/xemu/dev/journal.txt, writes /data/xemu/dev/report.txt (every
// crash with its functions, each game, each session) and returns the totals.
DevReport DevReportBuild();
