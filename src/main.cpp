#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include "audio_player.h"
#include "logger.h"
#include <esp_heap_caps.h>

// WiFi credentials - CONFIGURA QUI LE TUE CREDENZIALI
static const char *kWiFiSSID = "YOUR_WIFI_SSID";
static const char *kWiFiPassword = "YOUR_WIFI_PASSWORD";

static const char *kTestFilePath = "/sample-rich.mp3";
static const char *kSampleFilePath = "/audioontag.mp3";
// Stream HTTP di test - Radio Paradise 128k MP3
static const char *kRadioStreamURL = "http://stream.radioparadise.com/mp3-128";

static AudioPlayer player;

static void list_files(const char *path)
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
            LOG_INFO("Commands:");
            LOG_INFO("h - Help");
            LOG_INFO("t - Seleziona test file %s", kTestFilePath);
            LOG_INFO("s - Seleziona sample file %s", kSampleFilePath);
            LOG_INFO("r - Seleziona radio HTTP stream (Radio Paradise 128k)");
            LOG_INFO("l - Carica/verifica file selezionato (no play)");
            LOG_INFO("p - Play/Pause toggle");
            LOG_INFO("q - Stop playback");
            LOG_INFO("d - Lista i file in LittleFS (usa d/path per directory specifica)");
            LOG_INFO("m - Memory stats (start/min/current)");
            LOG_INFO("f<path> - Seleziona file custom (es. fmy.mp3)");
            LOG_INFO("i - Player status");
            LOG_INFO("v## - Set volume to ##% (e.g. v75)");
            LOG_INFO("s## - Seek to ## seconds (e.g. s123)");
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
                if (!player.file_armed())
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
            list_files("/");
            break;
        case 't':
        case 'T':
            select_source_path(kTestFilePath);
            break;
        case 's':
        case 'S':
            select_source_path(kSampleFilePath);
            break;
        case 'r':
        case 'R':
            select_source_path(kRadioStreamURL);
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
        else if (first_char == 's')
        {
            int seconds = cmd.substring(1).toInt();
            player.request_seek(seconds);
        }
        else if (first_char == 'd' || first_char == 'D')
        {
            String path = cmd.substring(1);
            path.trim();
            if (path.length() == 0)
            {
                path = "/";
            }
            if (path.charAt(0) != '/')
            {
                path = "/" + path;
            }
            list_files(path.c_str());
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
        if (player.ring_buffer_size() > 0)
        {
            size_t ring_used = player.ring_buffer_used();
            size_t ring_free = player.ring_buffer_size() - ring_used;
            LOG_INFO("Uptime: %lu s, Heap Libero: %u bytes, Audio ring: %u used / %u free",
                     millis() / 1000,
                     esp_get_free_heap_size(),
                     (unsigned)ring_used,
                     (unsigned)ring_free);
        }
        if (player.state() == PlayerState::PLAYING)
        {
            uint32_t current_sec = player.played_frames() / player.current_sample_rate();
            uint32_t total_sec = player.total_frames() / player.current_sample_rate();
            LOG_INFO("Progress: %u/%u seconds", current_sec, total_sec);
        }
    }
    delay(100);
}
