#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <filesystem>
#include "rvlmutex.h"
#include "utils.h"
#include "titles.h"

#define THREAD_STACK_SIZE 16384

void GamesDatabase::handleDirEntryForScan(const std::filesystem::directory_entry& dir_entry, std::unordered_map<std::string, GameContainer*>& lookup, std::vector<GameContainer>& newGames, std::vector<std::string>& verifiedPaths) {
    if ((scanningForDirectories && !dir_entry.is_directory())
        || (!scanningForDirectories && !dir_entry.is_regular_file())
        || dir_entry.path().filename().string()[0] == '.') {
        return;
    }

    if (!scanningForDirectories && dir_entry.path().extension().string() != targetNameOrExtension) {
        return;
    }

    std::string current_path = dir_entry.path().string();
    if (scanningForDirectories) {
        current_path += "/" + targetNameOrExtension;
    }
    auto it = lookup.find(current_path);

    struct stat result;
    time_t fileLastModified = 0;
    if (stat(current_path.c_str(), &result) == 0) {
        fileLastModified = result.st_mtime;
    }

    //Check if the file is already in the database and if it has not been modified.
    if (it != lookup.end() && it->second->lastModified == fileLastModified) {
        //No need to read all the informations again.
        verifiedPaths.push_back(current_path);
    } else {
        // New file found! Read ID/Title from disk
        GameContainer new_entry = createGameContainer(fileLastModified, current_path); 
        newGames.push_back(new_entry);
    }
}

/*
    Reads a TSV cache file and populates the games vector
*/
bool GamesDatabase::readCache() {
    std::ifstream file(cachePath);
    if (!file.is_open()) return false;

    bool dummyExists = fileExists(DUMMY_COVER_PATH);
    
    std::string line;

    while (std::getline(file, line)) {
        //Remove \r if found
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        //Skip empty lines
        if (line.empty() || line.find_first_not_of(" \t") == std::string::npos) {
            continue; 
        }

        //Parse
        std::stringstream ss(line);
        std::string gameIDString, name, path, coverPath, lastModifiedStr;

        //Check if we can successfully read all 3 columns
        if (std::getline(ss, gameIDString, '\t') &&
            std::getline(ss, name, '\t') &&
            std::getline(ss, path, '\t') &&
            std::getline(ss, coverPath, '\t') &&
            std::getline(ss, lastModifiedStr, '\t')) {
            u32 gameID = *(u32*)gameIDString.c_str();
            std::string configPath = std::string(CONFIG_PATH) + "/" + gameIDString + ".cfg";
            if (!fileExists(coverPath)) {
                if (dummyExists) {
                    coverPath = DUMMY_COVER_PATH;
                } else {
                    coverPath = "";
                }
            }
            GameContainer container(std::stoll(lastModifiedStr), name, path, coverPath, configPath, gameIDString, gameID);
            addMetadataToGameContainer(container);
            games.push_back(container);
        }
    }

    return true;
}

/*
    Writes the games vector to a TSV cache file
*/
bool GamesDatabase::writeCache() {
    std::ofstream file(cachePath);
    if (!file.is_open()) return false;

    for (GameContainer& game : games) {
        file << game.gameIDString << "\t" << game.name << "\t" << game.path << "\t" << game.coverPath << "\t" << game.lastModified << "\n";
    }
    
    return true;
}

void GamesDatabase::scanGames() {
    std::unordered_map<std::string, GameContainer*> lookup;
    std::vector<GameContainer> newGames;
    std::vector<std::string> verifiedPaths;

    //Return if the scan path does not exist or is not a directory
    if (!std::filesystem::exists(scanPath) || !std::filesystem::is_directory(scanPath)) {
        return;
    }

    //Build lookup table.
    //The block scope ensures the lock is released after the lookup table is built.
    {
        std::lock_guard<RVLMutex> lock(dbMutex);
        for (auto& game : games) {
            lookup[game.path] = &game;
        }
    }

    if (recursiveScan) {
        for (const auto& dir_entry : std::filesystem::recursive_directory_iterator(scanPath)) {
            handleDirEntryForScan(dir_entry, lookup, newGames, verifiedPaths);
        }
    } else {
        for (const auto& dir_entry : std::filesystem::directory_iterator(scanPath)) {
            handleDirEntryForScan(dir_entry, lookup, newGames, verifiedPaths);
        }
    }
    

    //Locks the database to update it
    std::lock_guard<RVLMutex> lock(dbMutex);
    
    // 1. Remove deleted games (in entries but not in verifiedPaths)
    // Convert verifiedPaths to a set for fast lookup during deletion checks
    std::unordered_set<std::string> verifiedSet(verifiedPaths.begin(), verifiedPaths.end());
    
    // Remove_if moves "to keep" items to front, returns iterator to "garbage"
    auto new_end = std::remove_if(games.begin(), games.end(),
        [&](const GameContainer& e) {
            // If path is NOT in verified set, delete it
            return verifiedSet.find(e.path) == verifiedSet.end();
        });
    games.erase(new_end, games.end());

    // 2. Add new games
    games.insert(games.end(), newGames.begin(), newGames.end());

    std::sort(games.begin(), games.end(), GameContainer::compare);

    writeCache();
}

void GamesDatabase::startScanAndUpdateThread() {
    readCache();
    scanAndUpdateThreadStack = (u8*)memalign(32, THREAD_STACK_SIZE);
    LWP_CreateThread(&scanAndUpdateThreadHandle, scanAndUpdateThread, (void*)this, scanAndUpdateThreadStack, THREAD_STACK_SIZE, 30);
}

void* GamesDatabase::scanAndUpdateThread(void* arg) {
    GamesDatabase* db = (GamesDatabase*)arg;
    db->scanGames();
    db->coversThreadStack = (u8*)memalign(32, THREAD_STACK_SIZE);
    LWP_CreateThread(&db->coversThreadHandle, loadCoversThread, (void*)db, db->coversThreadStack, THREAD_STACK_SIZE, 30);
    db->hasFinishedScanning.send();
    return NULL;
}

void* GamesDatabase::loadCoversThread(void* arg) {
    static const char gamesRegions[] = {'E', 'P', 'J'};
    GamesDatabase* db = (GamesDatabase*)arg;
    std::unordered_map<GameContainer*, std::string> coverMap;
    std::string tempID;
    std::string tempPath;

    bool dummyExists = fileExists(DUMMY_COVER_PATH);

    //Make a copy of the cover paths for each game
    {
        std::lock_guard<RVLMutex> lock(db->dbMutex);
        for (auto& game : db->games) {
            coverMap[&game] = game.gameIDString;
        }
    }

    //Check for covers
    for (auto& cover : coverMap) {
        tempID = cover.second;
        tempPath = std::string(COVER_PATH) + "/" + tempID + ".png";
        if (!fileExists(tempPath)) {
            //Try looking for a cover of the same game from a different region
            bool coverFound = false;
            for (u32 i = 0; i < sizeof(gamesRegions) && !coverFound; i++) {
                if (cover.second[3] == gamesRegions[i]) {
                    continue;
                }
                tempID[3] = gamesRegions[i];
                tempPath = std::string(COVER_PATH) + "/" + tempID + ".png";
                if (fileExists(tempPath)) {
                    coverFound = true;
                }
            }
            if (coverFound) {
                cover.second = tempID;
            } else {
                cover.second = "";
            }
        }
    }

    //Update the cover paths
    {
        std::lock_guard<RVLMutex> lock(db->dbMutex);
        for (auto& cover : coverMap) {
            GameContainer* game = cover.first;
            if (cover.second == "" && dummyExists) {
                game->coverPath  = DUMMY_COVER_PATH;
            } else if (cover.second != "") {
                game->coverPath  = std::string(COVER_PATH) + "/" + cover.second + ".png";
            } else {
                game->coverPath  = "";
            }
            if (game->image != NULL) {
                delete game->image;
                game->image = NULL; //This will force a reload of the cover.
            }
        }
        db->writeCache();
    }
    return NULL;
}

void lockTitlesDBs() {
    gcGamesDatabase.lock();
    wiiGamesDatabase.lock();
    vcGamesDatabase.lock();
    wiiChannelsDatabase.lock();
}

void unlockTitlesDBs() {
    gcGamesDatabase.unlock();
    wiiGamesDatabase.unlock();
    vcGamesDatabase.unlock();
    wiiChannelsDatabase.unlock();
}
