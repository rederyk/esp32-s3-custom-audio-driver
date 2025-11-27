#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include "audio_player.h"
#include "logger.h"
#include "drivers/sd_card_driver.h"
#include <esp_heap_caps.h>
#include <memory>
#include "timeshift_manager.h"

// WiFi credentials - CONFIGURA QUI LE TUE CREDENZIALI
static const char *kWiFiSSID = "FASTWEB-2";
static const char *kWiFiPassword = "mailmioWIFIfaschifo";

static const char *kTestFilePath = "/sample-rich.mp3";
static const char *kSampleFilePath = "/fileWAV1MG.wav";
// Stream HTTP di test - Radio Paradise 128k MP3
static const char *kRadioStreamURL = "http://stream.radioparadise.com/mp3-128";

static AudioPlayer player;
static StorageMode preferred_storage_mode = StorageMode::SD_CARD;  // Default: SD card mode

// Auto-pause buffering settings (configurabile per connessioni diverse)
// Per connessioni veloci: impostare a 0 per disabilitare il margine
// Per connessioni lente: aumentare per evitare pause ripetute
static uint32_t auto_pause_delay_ms = 0;   // Delay prima di riprendere (0 = disabilitato)
static size_t auto_pause_min_chunks = 2;      // Chunk minimi prima di riprendere (0 = disabilitato)

void start_timeshift_radio() {
    // Stop any current playback first
    if (player.is_playing() || player.state() == PlayerState::PLAYING || player.state() == PlayerState::PAUSED) {
        LOG_INFO("Stopping current playback before starting timeshift...");
        player.stop();
        delay(500); // Give time to cleanup
    }

    // Create and configure timeshift manager
    auto* ts = new TimeshiftManager();

    // Set preferred storage mode BEFORE opening
    ts->setStorageMode(preferred_storage_mode);
    LOG_INFO("Starting timeshift in %s mode",
             preferred_storage_mode == StorageMode::PSRAM_ONLY ? "PSRAM" : "SD");

    if (!ts->open(kRadioStreamURL)) {
        LOG_ERROR("Failed to open timeshift stream URL");
        delete ts;
        return;
    }

    // Start download task
    if (!ts->start()) {
        LOG_ERROR("Failed to start timeshift download task");
        delete ts;
        return;
    }

    LOG_INFO("Timeshift download started, waiting for first chunk...");

    // Wait for at least one READY chunk before starting playback (max 10 seconds)
    const uint32_t MAX_WAIT_MS = 10000;
    uint32_t start_wait = millis();
    while (ts->buffered_bytes() == 0) {
        if (millis() - start_wait > MAX_WAIT_MS) {
            LOG_ERROR("Timeout waiting for first chunk to be ready");
            delete ts;
            return;
        }
        delay(100);

        // Log progress every second
        if ((millis() - start_wait) % 1000 == 0) {
            LOG_INFO("Waiting for chunks... (%u KB downloaded)",
                     (unsigned)(ts->total_downloaded_bytes() / 1024));
        }
    }

    LOG_INFO("First chunk ready! Starting playback...");

    // Register auto-pause callback BEFORE transferring ownership
    ts->set_auto_pause_callback([](bool should_pause) {
        player.set_pause(should_pause);
    });

    // Configure auto-pause buffering margin
    ts->set_auto_pause_margin(auto_pause_delay_ms, auto_pause_min_chunks);
    if (auto_pause_delay_ms == 0 && auto_pause_min_chunks == 0) {
        LOG_INFO("Auto-pause buffering: DISABLED (fast connection mode)");
    } else {
        LOG_INFO("Auto-pause buffering: %u ms delay, %u min chunks (slow connection mode)",
                 auto_pause_delay_ms, (unsigned)auto_pause_min_chunks);
    }

    // Transfer ownership to player
    player.select_source(std::unique_ptr<IDataSource>(ts));

    // Arm and start playback
    if (!player.arm_source()) {
        LOG_ERROR("Failed to arm timeshift source");
        return;
    }

    player.start();
    LOG_INFO("Timeshift radio playback started successfully!");
}

void set_preferred_storage_mode(StorageMode mode) {
    preferred_storage_mode = mode;
    LOG_INFO("Preferred timeshift storage mode set to: %s",
             mode == StorageMode::PSRAM_ONLY ? "PSRAM_ONLY (fast, ~2min buffer)" : "SD_CARD (slower, unlimited)");
    LOG_INFO("This will be used next time you start radio with 'r' command");
}

static void list_littlefs_files(const char *path)
{
    File root = LittleFS.open(path);
    if (!root)
    {
        LOG_ERROR("Impossibile aprire la directory: %s", path);
        return;
    }
    if (!root.isDirectory())
    {
        LOG_WARN("%s non e' una directory", path);
        root.close();
        return;
    }

    LOG_INFO("Contenuto di %s:", path);
    File entry = root.openNextFile();
    if (!entry)
    {
        LOG_INFO("(vuoto)");
    }
    while (entry)
    {
        const char *name = entry.name();
        if (entry.isDirectory())
        {
            LOG_INFO("DIR  %s", name);
        }
        else
        {
            LOG_INFO("FILE %s (%u bytes)", name, (unsigned)entry.size());
        }
        entry = root.openNextFile();
    }
    root.close();
}

static void list_sd_files(const char *path)
{
    auto &sd = SdCardDriver::getInstance();
    if (!sd.isMounted())
    {
        LOG_WARN("SD card not mounted. Cannot list files.");
        return;
    }

    LOG_INFO("Contenuto di SD:%s:", path);
    auto entries = sd.listDirectory(path);
    if (entries.empty())
    {
        LOG_INFO("(vuoto)");
    }
    for (const auto &entry : entries)
    {
        LOG_INFO("%s %s (%llu bytes)",
                 entry.isDirectory ? "DIR " : "FILE", entry.name.c_str(), entry.sizeBytes);
    }
}

static void select_source_path(const char *path)
{
    player.select_source(path);
    LOG_INFO("Source selected: %s", path);
}

static void handle_command_string(String cmd)
{
    cmd.trim();
    if (cmd.length() == 0)
        return;
    char first_char = cmd.charAt(0);
    if (cmd.length() == 1)
    {
        switch (first_char)
        {
        case 'h':
        case 'H':
            LOG_INFO("=== COMANDI DISPONIBILI ===");
            LOG_INFO("PLAYBACK:");
            LOG_INFO("  r - Avvia radio streaming con timeshift (tutto in uno!)");
            LOG_INFO("  t - Riproduci test file (%s)", kTestFilePath);
            LOG_INFO("  s - Riproduci sample file (%s)", kSampleFilePath);
            LOG_INFO("  p - Play/Pause toggle");
            LOG_INFO("  q - Stop playback");
            LOG_INFO("");
            LOG_INFO("CONTROLLO:");
            LOG_INFO("  v## - Volume (es. v75 = 75%)");
            LOG_INFO("  [ - Seek indietro di 5 secondi");
            LOG_INFO("  ] - Seek avanti di 5 secondi");
            LOG_INFO("  s## - Seek assoluto a ## secondi (es. s30 = vai al secondo 30)");
            LOG_INFO("  i - Stato player");
            LOG_INFO("");
            LOG_INFO("FILE SYSTEM:");
            LOG_INFO("  d [path] - Lista file (es. 'd /' o 'd /sd/')");
            LOG_INFO("  f<path> - Seleziona file custom (es. f/song.mp3)");
            LOG_INFO("  x - Stato SD card");
            LOG_INFO("");
            LOG_INFO("TIMESHIFT STORAGE (set BEFORE starting radio):");
            LOG_INFO("  W - shoW preferred storage mode");
            LOG_INFO("  Z - set psRam mode preference (fast, ~2min buffer)");
            LOG_INFO("  C - set sd Card mode preference (slower, unlimited buffer)");
            LOG_INFO("");
            LOG_INFO("DEBUG:");
            LOG_INFO("  m - Memory stats");
            LOG_INFO("  h - Mostra questo help");
            break;
        case 'l':
        case 'L':
            if (player.is_playing() || player.state() == PlayerState::PLAYING || player.state() == PlayerState::PAUSED)
            {
                LOG_INFO("Loading new source: stopping current playback.");
                player.stop();
            }
            player.arm_source();
            break;
        case 'p':
        case 'P':
            if (player.state() == PlayerState::PAUSED)
            {
                player.toggle_pause();
            }
            else if (player.state() == PlayerState::PLAYING)
            {
                player.toggle_pause();
            }
            else
            {
                if (!player.data_source() || !player.data_source()->is_open())
                {
                    LOG_WARN("Nessun file armato. Usa 'l' per caricare prima di play.");
                    break;
                }
                player.start();
            }
            break;
        case 'q':
        case 'Q':
            player.stop();
            break;
        case 'd':
        case 'D':
            list_littlefs_files("/");
            break;
        case 't':
        case 'T':
            if (player.is_playing() || player.state() == PlayerState::PLAYING || player.state() == PlayerState::PAUSED) {
                player.stop();
                delay(300);
            }
            select_source_path(kTestFilePath);
            player.arm_source();
            player.start();
            break;
        case 's':
        case 'S':
            if (player.is_playing() || player.state() == PlayerState::PLAYING || player.state() == PlayerState::PAUSED) {
                player.stop();
                delay(300);
            }
            select_source_path(kSampleFilePath);
            player.arm_source();
            player.start();
            break;
        case 'r':
        case 'R':
            start_timeshift_radio();
            break;
        case 'i':
        case 'I':
            player.print_status();
            break;
        case 'm':
        case 'M':
            // Memory stats now printed in status; keep quick log
            LOG_INFO("Heap monitor -> start %u, min %u, current %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            break;
        case 'x':
        case 'X':
        {
            auto &sd = SdCardDriver::getInstance();
            LOG_INFO("--- SD Card Status ---");
            if (sd.isMounted()) {
                LOG_INFO("Status: Mounted");
                LOG_INFO("Card Type: %s", sd.cardTypeString().c_str());
                LOG_INFO("Size: %llu MB, Used: %llu MB", sd.totalBytes() / (1024 * 1024), sd.usedBytes() / (1024 * 1024));
            } else {
                LOG_INFO("Status: Not Mounted");
                LOG_INFO("Last Error: %s", sd.lastError().c_str());
            }
        }
            break;
        case 'w':
        case 'W':
            // Show preferred timeshift storage mode
            LOG_INFO("Preferred timeshift storage mode: %s",
                     preferred_storage_mode == StorageMode::PSRAM_ONLY ? "PSRAM_ONLY" : "SD_CARD");
            if (preferred_storage_mode == StorageMode::PSRAM_ONLY) {
                LOG_INFO("  - Fast access, ~2min buffer, 2MB PSRAM used");
            } else {
                LOG_INFO("  - Slower access, unlimited buffer, uses SD card");
            }
            LOG_INFO("Use 'Z' for PSRAM or 'C' for SD, then start radio with 'r'");
            break;
        case 'z':
        case 'Z':
            // Set PSRAM mode preference
            set_preferred_storage_mode(StorageMode::PSRAM_ONLY);
            break;
        case 'c':
        case 'C':
            // Set SD card mode preference
            set_preferred_storage_mode(StorageMode::SD_CARD);
            break;
        case '[':
            // Seek indietro di 5 secondi
            {
                uint32_t current_sec = player.current_position_sec();
                uint32_t total_sec = player.total_duration_sec();
                int target_sec = (int)current_sec - 5;

                if (target_sec < 0) {
                    target_sec = 0;
                }

                LOG_INFO("Seek -5 sec (from %u to %d sec)", current_sec, target_sec);
                player.request_seek(target_sec);
            }
            break;
        case ']':
            // Seek avanti di 5 secondi
            {
                uint32_t current_sec = player.current_position_sec();
                uint32_t total_sec = player.total_duration_sec();
                uint32_t target_sec = current_sec + 5;

                // Per timeshift: non superare il tempo disponibile
                if (total_sec > 0 && target_sec > total_sec) {
                    target_sec = total_sec;
                    LOG_WARN("Cannot seek beyond available time (%u sec)", total_sec);
                }

                LOG_INFO("Seek +5 sec (from %u to %u sec)", current_sec, target_sec);
                player.request_seek(target_sec);
            }
            break;
        default:
            LOG_WARN("Unknown command: %s. Type 'h' for help.", cmd.c_str());
            break;
        }
    }
    else
    {
        if (first_char == 'v')
        {
            int vol = cmd.substring(1).toInt();
            player.set_volume(vol);
        }
        else if (first_char == 's' || first_char == 'S')
        {
            int target_sec = cmd.substring(1).toInt();
            uint32_t total_sec = player.total_duration_sec();

            // Limita il seek al tempo disponibile con margine di sicurezza
            // Sottrai 10 secondi per evitare di seekare alla fine dell'ultimo chunk
            if (total_sec > 0 && target_sec > (int)total_sec) {
                LOG_WARN("Cannot seek to %d sec, max available is %u sec", target_sec, total_sec);
                // Seek a un chunk sicuro, non alla fine
                const int SAFETY_MARGIN_SEC = 10;
                target_sec = (total_sec > SAFETY_MARGIN_SEC) ? (total_sec - SAFETY_MARGIN_SEC) : 0;
                LOG_INFO("Adjusted seek to %d sec (safe position in earlier chunk)", target_sec);
            }

            if (target_sec < 0) {
                target_sec = 0;
            }

            uint32_t current_sec = player.current_position_sec();
            LOG_INFO("Absolute seek to %d sec (current: %u sec)", target_sec, current_sec);
            player.request_seek(target_sec);
        }
        else if (first_char == 'd' || first_char == 'D')
        {
            String path = cmd.substring(1);
            path.trim();
            if (path.length() == 0)
            {
                path = "/";
            }
            if (path.startsWith("/sd")) {
                list_sd_files(path.c_str());
            } else {
                if (path.charAt(0) != '/')
                {
                    path = "/" + path;
                }
                list_littlefs_files(path.c_str());
            }
        }
        else if (first_char == 'f' || first_char == 'F')
        {
            String new_path = cmd.substring(1);
            new_path.trim();
            if (new_path.length() == 0)
            {
                LOG_WARN("Invalid source path");
                return;
            }
            if (new_path.charAt(0) != '/' && strncmp(new_path.c_str(), "http", 4) != 0)
            {
                new_path = "/" + new_path;
            }
            player.select_source(new_path.c_str());
            LOG_INFO("Source selected: %s (use 'l' to load)", new_path.c_str());
        }
        else
        {
            LOG_WARN("Unknown command: %s. Type 'h' for help.", cmd.c_str());
        }
    }
}

void setup()
{
    Serial.begin(115200);
    delay(1000);
    LOG_INFO("=== BOOT: Audio Player Ready. Use serial commands: h for help ===");
    set_log_level(LogLevel::DEBUG); // Mostra tutte le chiamate do_seek

    if (!LittleFS.begin())
    {
        LOG_ERROR("LittleFS mount failed. Upload filesystem with 'pio run -t uploadfs'.");
        return;
    }

    // Inizializza SD Card
    LOG_INFO("Initializing SD card...");
    if (SdCardDriver::getInstance().begin()) {
        LOG_INFO("SD card mounted successfully.");
    } else {
        LOG_WARN("SD card mount failed: %s", SdCardDriver::getInstance().lastError().c_str());
    }

    // Inizializza WiFi se configurato (necessario per stream HTTP)
    if (strcmp(kWiFiSSID, "YOUR_WIFI_SSID") != 0)
    {
        LOG_INFO("Connecting to WiFi: %s", kWiFiSSID);
        WiFi.mode(WIFI_STA);
        WiFi.begin(kWiFiSSID, kWiFiPassword);

        int timeout = 20; // 10 secondi timeout
        while (WiFi.status() != WL_CONNECTED && timeout > 0)
        {
            delay(500);
            Serial.print(".");
            timeout--;
        }

        if (WiFi.status() == WL_CONNECTED)
        {
            LOG_INFO("WiFi connected! IP: %s", WiFi.localIP().toString().c_str());
        }
        else
        {
            LOG_WARN("WiFi connection failed (timeout). HTTP streaming will not work.");
        }
    }
    else
    {
        LOG_INFO("WiFi not configured. Set kWiFiSSID/kWiFiPassword for HTTP streaming.");
    }

    player.select_source(kTestFilePath);
    LOG_INFO("Setup completed.");
}

void loop()
{
    if (Serial.available() > 0)
    {
        String cmd = Serial.readStringUntil('\n');
        handle_command_string(cmd);
    }
    player.tick_housekeeping();
    static uint32_t last_log = 0;
    if (millis() - last_log > 5000)
    {
        last_log = millis();
        LOG_INFO("Uptime: %lu s, Heap Libero: %u bytes",
                 millis() / 1000,
                 esp_get_free_heap_size());
        
        if (player.state() == PlayerState::PLAYING)
        {
            // Usa la nuova interfaccia pulita di AudioPlayer
            uint32_t current_sec = player.current_position_sec();
            uint32_t total_sec = player.total_duration_sec();
            uint32_t current_min = current_sec / 60;
            uint32_t total_min = total_sec / 60;
            
            LOG_INFO("Progress: %02u:%02u / %02u:%02u",
                     current_min, current_sec % 60,
                     total_min, total_sec % 60);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // Cedi il controllo allo scheduler in modo più efficiente
}
