// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#ifndef OPENESPAUDIO_USE_EXTERNAL_SD_DRIVER

#include "drivers/sd_card_driver.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <algorithm>
#include <cstring>
#include <FS.h>

#include "logger.h"

namespace {
constexpr const char* MOUNT_POINT = "/sdcard";

constexpr gpio_num_t SD_CLK = GPIO_NUM_38;
constexpr gpio_num_t SD_CMD = GPIO_NUM_40;
constexpr gpio_num_t SD_D0 = GPIO_NUM_39;
constexpr gpio_num_t SD_D1 = GPIO_NUM_41;
constexpr gpio_num_t SD_D2 = GPIO_NUM_48;
constexpr gpio_num_t SD_D3 = GPIO_NUM_47;

// Funzione per abilitare i pull-up interni
void configure_pullups() {
    const gpio_num_t pins[] = {SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3};
    for (gpio_num_t pin : pins) {
        gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    }
}
} // namespace

SdCardDriver& SdCardDriver::getInstance() {
    static SdCardDriver instance;
    return instance;
}

bool SdCardDriver::begin() {
    if (mounted_) {
        refreshStats();
        return true;
    }

    // Abilita i pull-up interni prima di qualsiasi altra operazione
    configure_pullups();

    if (!pins_configured_) {
        // NOTA: setPins è deprecato e non fa nulla, ma lo lasciamo per chiarezza
        // La configurazione dei pin avviene tramite i menuconfig o, in questo caso,
        // è gestita internamente dalla libreria SD_MMC che usa i pin di default
        // se non diversamente specificato. L'abilitazione dei pull-up è la vera modifica.
        if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3)) {
            last_error_ = "Pin remap failed";
            openespaudio::log_message(openespaudio::LogLevel::ERROR, "[SD] Failed to configure SD_MMC pins (setPins is deprecated)\n");
        }
        pins_configured_ = true;
    }

    auto mountAttempt = [&](bool one_bit_mode) -> bool {
        // Ultimo tentativo: allineamo la chiamata a quella dell'esempio ufficiale,
        // usando il 5° parametro (ddr_mode_retries = 5) e una freq di 20MHz.
        bool ok = SD_MMC.begin(MOUNT_POINT, one_bit_mode, false, 20000, 5);
        if (!ok) {
            SD_MMC.end();
        }
        return ok;
    };

    if (!mountAttempt(false)) {
        openespaudio::log_message(openespaudio::LogLevel::WARN, "[SD] 4-line init failed, retrying in 1-bit mode\n");
        if (!mountAttempt(true)) {
            last_error_ = "Mount failed";
            mounted_ = false;
            openespaudio::log_message(openespaudio::LogLevel::WARN, "[SD] SD card mount failed - insert or re-seat card\n");
            return false;
        }
    }

    mounted_ = true;
    last_error_.clear();
    card_type_ = SD_MMC.cardType();
    updateCardTypeString();
    refreshStats();
    openespaudio::log_message(openespaudio::LogLevel::INFO, "[SD] Card mounted (%s)\n", card_type_str_.c_str());
    return true;
}

bool SdCardDriver::refreshStats() {
    if (!mounted_) {
        return false;
    }
    total_bytes_ = SD_MMC.totalBytes();
    used_bytes_ = SD_MMC.usedBytes();
    return true;
}

void SdCardDriver::updateCardTypeString() {
    switch (card_type_) {
    case CARD_NONE:
        card_type_str_ = "None";
        break;
    case CARD_MMC:
        card_type_str_ = "MMC";
        break;
    case CARD_SD:
        card_type_str_ = "SDSC";
        break;
    case CARD_SDHC:
        card_type_str_ = "SDHC/SDXC";
        break;
    default:
        card_type_str_ = "Unknown";
        break;
    }
}

std::vector<SdCardEntry> SdCardDriver::listDirectory(const char* path, size_t max_entries) {
    std::vector<SdCardEntry> entries;
    if (!mounted_) {
        last_error_ = "Card not mounted";
        return entries;
    }

    const char* target = (path && path[0]) ? path : "/";
    File dir = SD_MMC.open(target);
    if (!dir || !dir.isDirectory()) {
        last_error_ = "Invalid path";
        return entries;
    }

    while (true) {
        File entry = dir.openNextFile();
        if (!entry) {
            break;
        }

        SdCardEntry info;
        const char* entry_name = entry.name();
        if (entry_name) {
            info.name = entry_name;
            if (!info.name.empty() && info.name.front() == '/') {
                info.name.erase(info.name.begin());
            }
        }
        if (info.name.empty()) {
            entry.close();
            continue;
        }
        info.isDirectory = entry.isDirectory();
        info.sizeBytes = info.isDirectory ? 0 : entry.size();
        entries.push_back(std::move(info));
        entry.close();

        if (entries.size() >= max_entries) {
            break;
        }
    }
    dir.close();

    std::sort(entries.begin(), entries.end(), [](const SdCardEntry& a, const SdCardEntry& b) {
        if (a.isDirectory != b.isDirectory) {
            return a.isDirectory && !b.isDirectory;
        }
        return a.name < b.name;
    });
    return entries;
}

bool SdCardDriver::formatCard() {
    if (!ensureMounted()) {
        return false;
    }

    openespaudio::log_message(openespaudio::LogLevel::WARN, "[SD] Formatting card (deleting all files)\n");
    bool ok = deleteRecursive("/");
    if (!ok) {
        last_error_ = "Failed to delete some entries";
        openespaudio::log_message(openespaudio::LogLevel::ERROR, "[SD] Format failed\n");
        return false;
    }
    refreshStats();
    openespaudio::log_message(openespaudio::LogLevel::INFO, "[SD] Format completed\n");
    return true;
}

bool SdCardDriver::ensureMounted() {
    if (mounted_) {
        return true;
    }
    return begin();
}

std::string SdCardDriver::buildChildPath(const char* parent, const char* child) const {
    std::string result;
    if (child && child[0] == '/') {
        result = child;
        return result;
    }
    if (!parent || parent[0] == '\0' || (parent[0] == '/' && parent[1] == '\0')) {
        result = "/";
    } else {
        result = parent;
        if (result.back() != '/') {
            result.push_back('/');
        }
    }
    if (child) {
        result += child;
    }
    return result;
}

bool SdCardDriver::deleteRecursive(const char* path) {
    if (!path) {
        return false;
    }

    File entry = SD_MMC.open(path);
    if (!entry) {
        return strcmp(path, "/") == 0;
    }

    bool success = true;
    if (entry.isDirectory()) {
        while (true) {
            File child = entry.openNextFile();
            if (!child) {
                break;
            }
            std::string child_path = buildChildPath(path, child.name());
            bool child_ok = true;
            if (child.isDirectory()) {
                child.close();
                child_ok = deleteRecursive(child_path.c_str());
                if (child_ok) {
                    child_ok = SD_MMC.rmdir(child_path.c_str());
                }
            } else {
                child.close();
                child_ok = SD_MMC.remove(child_path.c_str());
            }
            success = success && child_ok;
        }
        entry.close();
        if (strcmp(path, "/") != 0) {
            success = SD_MMC.rmdir(path) && success;
        }
    } else {
        entry.close();
        success = SD_MMC.remove(path);
    }
    return success;
}

#endif // OPENESPAUDIO_USE_EXTERNAL_SD_DRIVER
