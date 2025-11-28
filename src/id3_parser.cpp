// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#include "id3_parser.h"

#include "data_source.h" // Aggiunto per IDataSource
#include <cstring>

static constexpr size_t kMaxTextFrameRead = 512;

void Id3Parser::clear_metadata(Metadata &meta) {
    meta.title = "";
    meta.artist = "";
    meta.album = "";
    meta.cover_present = false;
    meta.genre = "";
    meta.track = "";
    meta.year = "";
    meta.comment = "";
    meta.custom = "";
}

String Id3Parser::trim_id3_string(const uint8_t *data, size_t len) {
    while (len > 0 && (data[len - 1] == 0 || data[len - 1] == ' ')) {
        len--;
    }
    size_t start = 0;
    while (start < len && (data[start] == 0 || data[start] == ' ')) {
        start++;
    }
    String out;
    for (size_t i = start; i < len; ++i) {
        char c = static_cast<char>(data[i]);
        if (c == '\0') {
            break;
        }
        out += c;
    }
    return out;
}

uint32_t Id3Parser::parse_be32(const uint8_t *b) {
    return (static_cast<uint32_t>(b[0]) << 24) |
           (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) |
           static_cast<uint32_t>(b[3]);
}

uint32_t Id3Parser::parse_synchsafe32(const uint8_t *b) {
    return ((b[0] & 0x7F) << 21) |
           ((b[1] & 0x7F) << 14) |
           ((b[2] & 0x7F) << 7) |
           (b[3] & 0x7F);
}

String Id3Parser::decode_id3_text_payload(uint8_t encoding, const uint8_t *data, size_t data_len) {
    if (data_len == 0) {
        return "";
    }

    // ISO-8859-1 o UTF-8 (treat as ASCII/UTF-8)
    if (encoding == 0 || encoding == 3) {
        return trim_id3_string(data, data_len);
    }

    // UTF-16 con BOM o UTF-16BE senza BOM
    bool big_endian = (encoding == 2);
    if (encoding == 1 && data_len >= 2) {
        // BOM detection
        if (data[0] == 0xFF && data[1] == 0xFE) {
            big_endian = false;
            data += 2;
            data_len = (data_len >= 2) ? (data_len - 2) : 0;
        } else if (data[0] == 0xFE && data[1] == 0xFF) {
            big_endian = true;
            data += 2;
            data_len = (data_len >= 2) ? (data_len - 2) : 0;
        }
    }

    String out;
    for (size_t i = 0; i + 1 < data_len; i += 2) {
        uint16_t code = big_endian ? (static_cast<uint16_t>(data[i]) << 8) | data[i + 1]
                                   : (static_cast<uint16_t>(data[i + 1]) << 8) | data[i];
        if (code == 0x0000) {
            break;
        }
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else {
            // best-effort: skip non-ASCII to avoid heavy conversion on MCU
            out += '?';
        }
    }
    return out;
}

String Id3Parser::decode_id3_text(const uint8_t *buf, size_t len) {
    if (len == 0) {
        return "";
    }
    uint8_t encoding = buf[0];
    const uint8_t *data = buf + 1;
    size_t data_len = (len > 1) ? (len - 1) : 0;
    return decode_id3_text_payload(encoding, data, data_len);
}

bool Id3Parser::read_id3v1(IDataSource* source, Metadata &out) {
    const size_t kTagSize = 128;
    size_t file_size = source->size();
    if (file_size < kTagSize) {
        return false;
    }
    if (!source->seek(file_size - kTagSize)) {
        return false;
    }
    uint8_t buf[kTagSize];
    if (source->read(buf, kTagSize) != kTagSize) {
        return false;
    }
    if (memcmp(buf, "TAG", 3) != 0) { // Controlla il magic number "TAG"
        return false;
    }

    if (out.title.length() == 0) {
        out.title = trim_id3_string(buf + 3, 30);
    }
    if (out.artist.length() == 0) {
        out.artist = trim_id3_string(buf + 33, 30);
    }
    if (out.album.length() == 0) {
        out.album = trim_id3_string(buf + 63, 30);
    }
    if (out.year.length() == 0) {
        out.year = trim_id3_string(buf + 93, 4);
    }
    // Comment (30 bytes). In ID3v1.1 byte 125 is 0 and 126 holds track number.
    if (out.comment.length() == 0) {
        size_t comment_len = 30;
        if (buf[125] == 0) {
            comment_len = 28; // preserve track byte
        }
        out.comment = trim_id3_string(buf + 97, comment_len);
    }
    if (out.track.length() == 0 && buf[125] == 0 && buf[126] != 0) {
        out.track = String((int)buf[126]);
    }
    if (out.genre.length() == 0) {
        uint8_t genre_id = buf[127];
        out.genre = String("ID3v1#") + String((int)genre_id);
    }
    return out.title.length() || out.artist.length() || out.album.length();
}

bool Id3Parser::read_id3v2(IDataSource* source, Metadata &out) {
    if (!source->seek(0)) {
        return false;
    }
    uint8_t header[10];
    if (source->read(header, sizeof(header)) != sizeof(header)) {
        return false;
    }
    if (memcmp(header, "ID3", 3) != 0) {
        return false;
    }
    uint8_t version_major = header[3];
    uint8_t flags = header[5];
    uint32_t tag_size = parse_synchsafe32(&header[6]);
    uint32_t tag_end = 10 + tag_size; // L'header non è incluso nella dimensione
    if (tag_end > source->size()) {
        tag_end = source->size();
    }

    if (flags & 0x40) { // extended header
        uint8_t ext_header[4];
        if (source->read(ext_header, sizeof(ext_header)) != sizeof(ext_header)) {
            return false;
        }
        uint32_t ext_size = (version_major == 4) ? parse_synchsafe32(ext_header) : parse_be32(ext_header);
        if (version_major == 3 && ext_size >= 4) {
            ext_size -= 4; // v2.3 size includes these 4 bytes
        }
        source->seek(source->tell() + ext_size);
    }

    while (source->tell() + 10 <= tag_end) {
        uint8_t frame_hdr[10];
        if (source->read(frame_hdr, sizeof(frame_hdr)) != sizeof(frame_hdr)) {
            break;
        }
        if (frame_hdr[0] == 0) {
            break; // padding
        }

        char id[5];
        memcpy(id, frame_hdr, 4);
        id[4] = '\0';
        uint32_t frame_size = (version_major == 4) ? parse_synchsafe32(&frame_hdr[4]) : parse_be32(&frame_hdr[4]);
        if (frame_size == 0) {
            break;
        }
        uint32_t frame_end = source->tell() + frame_size;
        bool handled = false;

        if (strcmp(id, "TIT2") == 0 || strcmp(id, "TPE1") == 0 || strcmp(id, "TALB") == 0 ||
            strcmp(id, "TCON") == 0 || strcmp(id, "TRCK") == 0 || strcmp(id, "TDRC") == 0 || strcmp(id, "TYER") == 0) {
            size_t to_read = frame_size;
            if (to_read > kMaxTextFrameRead) {
                to_read = kMaxTextFrameRead; // limit to keep stack small
            }
            uint8_t buf[kMaxTextFrameRead];
            size_t n = source->read(buf, to_read);
            if (n > 0) {
                String value = decode_id3_text(buf, n);
                if (strcmp(id, "TIT2") == 0 && out.title.length() == 0 && value.length()) {
                    out.title = value;
                } else if (strcmp(id, "TPE1") == 0 && out.artist.length() == 0 && value.length()) {
                    out.artist = value;
                } else if (strcmp(id, "TALB") == 0 && out.album.length() == 0 && value.length()) {
                    out.album = value;
                } else if (strcmp(id, "TCON") == 0 && out.genre.length() == 0 && value.length()) {
                    out.genre = value;
                } else if (strcmp(id, "TRCK") == 0 && out.track.length() == 0 && value.length()) {
                    out.track = value;
                } else if ((strcmp(id, "TDRC") == 0 || strcmp(id, "TYER") == 0) && out.year.length() == 0 && value.length()) {
                    out.year = value;
                }
                handled = true;
            }
            if (n < frame_size) {
                source->seek(frame_end);
            }
        } else if (strcmp(id, "APIC") == 0) {
            // Do not read the whole image, just mark presence.
            out.cover_present = true;
            source->seek(frame_end);
            handled = true;
        } else if (strcmp(id, "COMM") == 0) {
            size_t to_read = frame_size;
            if (to_read > kMaxTextFrameRead) {
                to_read = kMaxTextFrameRead;
            }
            uint8_t buf[kMaxTextFrameRead];
            size_t n = source->read(buf, to_read);
            if (n > 4) {
                uint8_t encoding = buf[0];
                size_t pos = 4; // skip encoding + 3-byte lang
                if (encoding == 1 || encoding == 2) {
                    while (pos + 1 < n) {
                        if (buf[pos] == 0 && buf[pos + 1] == 0) { pos += 2; break; }
                        pos += 2;
                    }
                } else {
                    while (pos < n && buf[pos] != 0) { pos++; }
                    if (pos < n) { pos++; }
                }
                size_t text_len = (pos < n) ? (n - pos) : 0;
                String value = decode_id3_text_payload(encoding, buf + pos, text_len);
                if (out.custom.length() == 0 && value.length()) {
                    out.custom = value;
                }
                handled = true;
            }
            if (n < frame_size) {
                source->seek(frame_end);
            }
        }

        if (!handled) {
            source->seek(frame_end);
        }
        if (source->tell() < frame_end) {
            source->seek(frame_end);
        }

        if (out.title.length() && out.artist.length() && out.album.length()) {
            break;
        }
        if (source->tell() >= tag_end) {
            break;
        }
    }

    return out.title.length() || out.artist.length() || out.album.length();
}

bool Id3Parser::parse(IDataSource* source, Metadata &out) {
    clear_metadata(out);
    if (!source || !source->is_open() || !source->is_seekable()) {
        return false;
    }

    bool found = read_id3v2(source, out);
    // Fall back or fill missing fields with ID3v1 if present.
    read_id3v1(source, out);
    return found || out.title.length() || out.artist.length() || out.album.length();
}
