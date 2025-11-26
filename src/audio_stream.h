#pragma once

#include <memory>
#include <cstdint>
#include <cstddef>
#include "data_source.h"
#include "mp3_decoder.h"

class AudioStream {
public:
    AudioStream();
    ~AudioStream();

    // Takes ownership of the data source
    bool begin(std::unique_ptr<IDataSource> source);
    void end();

    // Reads PCM samples into buffer. Returns number of frames read.
    size_t read(int16_t* buffer, size_t frames_to_read);

    // Seeks to specific PCM frame index. Returns true on success.
    bool seek(uint64_t pcm_frame_index);

    // Getters
    uint32_t sample_rate() const { return decoder_.sample_rate(); }
    uint32_t channels() const { return decoder_.channels(); }
    uint64_t total_frames() const { return decoder_.total_frames(); }
    
    // Direct access to MP3 decoder if needed (e.g. for accessing internal buffers)
    // Ideally we would encapsulate this completely, but sticking to the plan's interface for now.
    // Actually, Mp3Decoder exposes buffers() which returns pointers to internal buffers.
    // The plan says "read(int16_t* buffer...)" which implies we copy or fill user provided buffer.
    // The current AudioPlayer uses decoder_.buffers().pcm directly to avoid copy if possible, 
    // but here we will follow the standard read interface which is cleaner.
    
    // Access underlying data source
    const IDataSource* data_source() const { return source_.get(); }

private:
    std::unique_ptr<IDataSource> source_;
    Mp3Decoder decoder_;
    bool initialized_ = false;
};
