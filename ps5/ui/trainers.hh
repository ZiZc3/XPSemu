//
// XPSemu: trainers (.etm cheat modules) for the dashboard and the quick menu (patch 246).
// Our own code. GPL-3.0-or-later.
//
// Folders: /data/xpsemu/trainers and xpsemu/trainers on USB drives, sub-folders too.
// The index is made in the background and cached in /data/xpsemu/trainers-index.txt
// (a file read again only when its size or date changed).
// A game's choice: /data/xpsemu/trainers/<TITLEID>.txt (file=, on=, opts=).
//
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct TrainerEntry {
    std::string path, name, author;
    std::vector<uint32_t> ids;
    std::vector<std::string> options;
};

struct TrainerChoice {
    std::string file;   // the trainer's path
    bool on = false;
    std::string opts;   // '0'/'1' per option
};

// Starts a background scan (unless one ran in the last 20 s, or force).
void TrainersScan(bool force = false);
bool TrainersReady();
// Bumped by each finished scan: lists made from the index can be made again.
unsigned TrainersGeneration();
// The trainers made for this title ID, by name.
std::vector<TrainerEntry> TrainersFor(uint32_t title_id);

TrainerChoice TrainerChoiceLoad(const std::string &key);
void TrainerChoiceSave(const std::string &key, const TrainerChoice &c);
// The start's lines (trainer=, trainer_opts=) for a game, or "" when it has none on.
std::string TrainerStartLines(const std::string &key, uint32_t title_id);
// One trainer file read now (the quick menu's): false when it can't be used.
bool TrainerReadOne(const std::string &path, TrainerEntry *out);
