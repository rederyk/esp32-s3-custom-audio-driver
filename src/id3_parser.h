#pragma once

#include <Arduino.h>
#include <LittleFS.h>

struct Metadata {
    String title;
    String artist;
    String album;
    bool cover_present = false;
    String genre;
    String track;
    String year;
    String comment;
    String custom;
};

class Id3Parser {
public:
    bool parse(const char *path, Metadata &out);

private:
    static void clear_metadata(Metadata &meta);
    static String trim_id3_string(const uint8_t *data, size_t len);
    static uint32_t parse_be32(const uint8_t *b);
    static uint32_t parse_synchsafe32(const uint8_t *b);
    static String decode_id3_text(const uint8_t *buf, size_t len);
    static String decode_id3_text_payload(uint8_t encoding, const uint8_t *data, size_t len);
    static bool read_id3v1(File &f, Metadata &out);
    static bool read_id3v2(File &f, Metadata &out);
};
