#pragma once

#include <dirent.h>      // opendir / readdir / closedir / dirent
#include <unistd.h>      // pid_t
#include <sys/types.h>
#include <fstream>       // std::ifstream
#include <string>
#include <algorithm>     // std::all_of
#include <cctype>        // ::isdigit

pid_t pid = 0;

inline pid_t getPid(const std::string& packageName) {
    DIR* dir = opendir("/proc");
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string pidStr(entry->d_name);
        if (!std::all_of(pidStr.begin(), pidStr.end(), ::isdigit)) continue;
        std::ifstream cmdFile(("/proc/" + pidStr + "/cmdline").c_str(),std::ios::in | std::ios::binary);
        if (!cmdFile) continue;

        std::string cmd;
        std::getline(cmdFile, cmd, '\0');
        if (cmd == packageName) {
            closedir(dir);
            return static_cast<pid_t>(std::stoi(pidStr));
        }
    }
    closedir(dir);
    return 0;
}