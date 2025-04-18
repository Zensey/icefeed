#include <filesystem>
#include <iostream>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

namespace fs = std::filesystem;

class IcecastStreamer {
    std::string icecast_url;
    std::string username;
    std::string password;
    
    AVFormatContext* output_ctx = nullptr;
    AVStream* audio_stream = nullptr;
    AVDictionary* options = nullptr;
    
    // Timing control
    int64_t first_audio_pts = AV_NOPTS_VALUE;
    int64_t next_pts = 0; // Tracks the next PTS to use (strictly increasing)
    int64_t stream_start_time = AV_NOPTS_VALUE;
    
public:
    IcecastStreamer(const std::string& url, 
                   const std::string& user, 
                   const std::string& pass)
        : icecast_url(url), username(user), password(pass) {
        avformat_network_init();
    }
    
    ~IcecastStreamer() {
        if (output_ctx) {
            if (output_ctx->pb) {
                av_write_trailer(output_ctx);
                avio_closep(&output_ctx->pb);
            }
            avformat_free_context(output_ctx);
        }
        av_dict_free(&options);
        avformat_network_deinit();
    }
    
    std::vector<fs::path> get_m4a_files() {
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(".")) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".m4a" || ext == ".mp4") {
                    files.push_back(entry.path());
                }
            }
        }
        return files;
    }
    
    void shuffle_playlist(std::vector<fs::path>& playlist) {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(playlist.begin(), playlist.end(), g);
    }
    
    void init_icecast_connection() {
        if (avformat_alloc_output_context2(&output_ctx, nullptr, "adts", 
                                         icecast_url.c_str()) < 0) {
            throw std::runtime_error("Could not create output context");
        }

        av_dict_set(&options, "content_type", "audio/aac", 0);
        av_dict_set(&options, "ice_name", "C++ Icecast Stream", 0);
        av_dict_set(&options, "ice_genre", "Music", 0);
        std::string auth = username + ":" + password;
        av_dict_set(&options, "auth", auth.c_str(), 0);

        if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open2(&output_ctx->pb, icecast_url.c_str(), 
                         AVIO_FLAG_WRITE, nullptr, &options) < 0) {
                throw std::runtime_error("Could not open Icecast connection");
            }
        }
        
        stream_start_time = av_gettime();
    }
    
    void stream_file(const fs::path& file) {
        AVFormatContext* input_ctx = nullptr;
        
        if (avformat_open_input(&input_ctx, file.string().c_str(), nullptr, nullptr) < 0) {
            throw std::runtime_error("Could not open input file");
        }
        
        if (avformat_find_stream_info(input_ctx, nullptr) < 0) {
            avformat_close_input(&input_ctx);
            throw std::runtime_error("Failed to retrieve stream info");
        }
        
        int audio_stream_index = av_find_best_stream(input_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audio_stream_index < 0) {
            avformat_close_input(&input_ctx);
            throw std::runtime_error("No audio stream found");
        }
        
        AVStream* in_audio_stream = input_ctx->streams[audio_stream_index];
        AVRational input_time_base = in_audio_stream->time_base;
        
        if (!audio_stream) {
            audio_stream = avformat_new_stream(output_ctx, nullptr);
            if (!audio_stream) {
                avformat_close_input(&input_ctx);
                throw std::runtime_error("Failed to create output stream");
            }
            
            if (avcodec_parameters_copy(audio_stream->codecpar, in_audio_stream->codecpar) < 0) {
                avformat_close_input(&input_ctx);
                throw std::runtime_error("Failed to copy codec parameters");
            }
            
            if (avformat_write_header(output_ctx, &options) < 0) {
                avformat_close_input(&input_ctx);
                throw std::runtime_error("Failed to write header");
            }
            
            first_audio_pts = AV_NOPTS_VALUE;
        }
        
        AVPacket pkt;
        av_init_packet(&pkt);
        
        while (av_read_frame(input_ctx, &pkt) >= 0) {
            if (pkt.stream_index == audio_stream_index) {
                // Initialize first packet time if needed
                if (first_audio_pts == AV_NOPTS_VALUE && pkt.pts != AV_NOPTS_VALUE) {
                    first_audio_pts = pkt.pts;
                }
                
                // Calculate sleep duration based on packet duration
                if (pkt.duration > 0) {
                    int64_t sleep_us = av_rescale_q(pkt.duration, input_time_base, AV_TIME_BASE_Q);
                    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
                }
                
                // Assign strictly increasing PTS/DTS
                if (pkt.pts != AV_NOPTS_VALUE) {
                    if (first_audio_pts != AV_NOPTS_VALUE) {
                        // Calculate duration since first packet in stream timebase
                        int64_t pts_diff = pkt.pts - first_audio_pts;
                        // Convert to output timebase and add to our running counter
                        pkt.pts = next_pts + av_rescale_q(pts_diff, input_time_base, audio_stream->time_base);
                        pkt.dts = pkt.pts; // For audio, DTS should equal PTS
                    }
                } else {
                    // If no PTS, just use our running counter
                    pkt.pts = next_pts;
                    pkt.dts = next_pts;
                }
                
                // Ensure we always move forward by at least 1
                next_pts = pkt.pts + 1;
                
                pkt.stream_index = audio_stream->index;
                
                if (av_interleaved_write_frame(output_ctx, &pkt) < 0) {
                    av_packet_unref(&pkt);
                    avformat_close_input(&input_ctx);
                    throw std::runtime_error("Error writing packet");
                }
            }
            av_packet_unref(&pkt);
        }
        
        first_audio_pts = AV_NOPTS_VALUE; // Reset for next file
        avformat_close_input(&input_ctx);
    }
    
    void run() {
        init_icecast_connection();
        
        while (true) {
            auto files = get_m4a_files();
            if (files.empty()) {
                std::cerr << "No M4A files found, waiting...\n";
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }
            
            shuffle_playlist(files);
            
            for (const auto& file : files) {
                std::cout << "Now playing: " << file.filename() << "\n";
                try {
                    stream_file(file);
                } catch (const std::exception& e) {
                    std::cerr << "Error: " << e.what() << "\n";
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        }
    }
};

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <icecast_url> <username> <password>\n";
        return 1;
    }
    
    try {
        IcecastStreamer streamer(argv[1], argv[2], argv[3]);
        streamer.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
