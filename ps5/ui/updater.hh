//
// XPSemu: updates from the project's GitHub releases (patch 230)
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
#include <string>

// This app's version: the System page shows the text, updates compare the number.
#ifdef XOG_FAKE_VERSION     // a test build that plays an older version, to try updating (build.sh FAKE_VERSION=3.1)
#define XOG_VTXT2(x) #x
#define XOG_VTXT(x) XOG_VTXT2(x)
#define XOG_VERSION_TEXT "Alpha " XOG_VTXT(XOG_FAKE_VERSION)
#define XOG_VERSION_NUM XOG_FAKE_VERSION
#else
#define XOG_VERSION_TEXT "Alpha 3.5"
#define XOG_VERSION_NUM 3.5
#endif

// At startup (not back from a game): the quiet check (a notification when
// there's a newer version; nothing when there's no network), and what an
// update stopped halfway left (its download, .upd files) removed.
void UpdaterStartup();

// The System page's button: checks, or (pressed again while it offers an
// update) installs it. Both on a thread of their own.
void UpdaterPress();

// What the row shows under the button ("" nothing), and whether a check or an
// install is under way.
std::string UpdaterStatus(bool *busy);

// Installed: true once, when XPSemu should start again (on the UI thread).
bool UpdaterTakeRestart();
