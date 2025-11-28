// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include <Arduino.h>
#include "data_source.h" // Aggiunto per IDataSource

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
    bool parse(IDataSource* source, Metadata &out);

private:
    void clear_metadata(Metadata &meta);
    String trim_id3_string(const uint8_t *data, size_t len);
    uint32_t parse_be32(const uint8_t *b);
    uint32_t parse_synchsafe32(const uint8_t *b);
    String decode_id3_text(const uint8_t *buf, size_t len);
    String decode_id3_text_payload(uint8_t encoding, const uint8_t *data, size_t len);
    bool read_id3v1(IDataSource* source, Metadata &out);
    bool read_id3v2(IDataSource* source, Metadata &out);
};
