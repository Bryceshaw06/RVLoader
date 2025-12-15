#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ctype.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <stdexcept>
#include <filesystem>
#include "titles.h"
#include "rvlmutex.h"
#include "utils.h"

#include <iostream>
#include <fstream>

#include "gcsave.h"

#define THREAD_STACK_SIZE 16384

#define GC_MAGIC    0xC2339F3D
#define CISO_MAGIC  0x4349534F

GamesDatabaseGC gcGamesDatabase;

GameContainer GamesDatabaseGC::createGameContainer(time_t lastModified, const std::string& path) {
    u32 magic;
    std::string gameId(7, '\0');
    u32 gameIdU32;
    std::string gameName(0x41, '\0');
    std::string coverPath;
    std::string configPath;

    GameContainer ret;
    
    std::ifstream ifs(path, std::ios::binary);

    if (!ifs.is_open())
        return ret;

    ifs.seekg(0x1C, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(&magic), sizeof(u32));
    if (magic == GC_MAGIC) {
        ifs.seekg(0x0, std::ios::beg);
        ifs.read(reinterpret_cast<char*>(&gameIdU32), sizeof(u32));
        ifs.seekg(0x0, std::ios::beg);
        ifs.read(gameId.data(), 6);
        gameId.resize(ifs.gcount());
    } else if (magic == CISO_MAGIC) {
        ifs.seekg(0x8000, std::ios::beg);
        ifs.read(reinterpret_cast<char*>(&gameIdU32), sizeof(u32));
        ifs.seekg(0x8000, std::ios::beg);
        ifs.read(gameId.data(), 6);
        gameId.resize(ifs.gcount());
    } else {
        ifs.close();
        return ret;
    }

    try {
        gameName = wiiTDB::getGameName(gameId);
    } catch (std::out_of_range& e) {
        if (magic == GC_MAGIC) {
            ifs.seekg(0x20, std::ios::beg);
            ifs.read(gameName.data(), 0x40);
            gameName.resize(ifs.gcount());
        } else if (magic == CISO_MAGIC) {
            ifs.seekg(0x8020, std::ios::beg);
            ifs.read(gameName.data(), 0x40);
            gameName.resize(ifs.gcount());
        }
    }

    ifs.close();

    configPath = std::string(CONFIG_PATH) + "/" + gameId + ".cfg";
    coverPath = std::string(COVER_PATH) + "/" + gameId + ".png";

    if (!fileExists(coverPath)) { //Check if gameID cover exists
        coverPath = DUMMY_COVER_PATH;
        if (!fileExists(coverPath)) { //Check if dummy exists
            coverPath = "";
        }
    }

    /*std::string savePath;
    gameId[4] = '\0';
    savePath = "/saves/" + std::string(gameId) + ".raw";*/
    //GCSave save(savePath);
    GCSave save;

    return GameContainer(lastModified, gameName, path, coverPath, configPath, "", gameId, gameIdU32, save);
}

void addGCGames() {
    gcGamesDatabase.startScanAndUpdateThread();
}
