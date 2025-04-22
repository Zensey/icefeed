#include <pthread.h>
#include <sys/prctl.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

#ifdef DEBUG
#define DEBUG_MSG(str)                 \
    do {                               \
        std::cout << str << std::endl; \
    } while (false)
#else
#define DEBUG_MSG(str) \
    do {               \
    } while (false)
#endif

namespace fs = std::filesystem;

class ErrorWritePacket : public std::exception {};

class IcecastStreamer {
    std::string icecast_url;
    std::string music_dir;

    AVFormatContext *output_ctx = nullptr;
    AVStream *audio_stream = nullptr;
    AVDictionary *options = nullptr;

    // Tracks PTS between files 
    int64_t offset_pts = 0;
    std::chrono::time_point<std::chrono::system_clock> start_time;
    std::chrono::duration<long long, std::ratio<1, 1000000>> lag = {};

   public:
    IcecastStreamer(const std::string &url, const std::string &dir)
        : icecast_url(url), music_dir(dir) {
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
        for (const auto &entry : fs::directory_iterator(music_dir)) {
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

    void shuffle_playlist(std::vector<fs::path> &playlist) {
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
        av_dict_set(&options, "ice_name", "Icecast Stream", 0);
        av_dict_set(&options, "ice_genre", "Music", 0);

        if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open2(&output_ctx->pb, icecast_url.c_str(),
                           AVIO_FLAG_WRITE, nullptr, &options) < 0) {
                throw std::runtime_error("Could not open Icecast connection");
            }
        }
    }

    void stream_file(const fs::path &file) {
        AVFormatContext *input_ctx = nullptr;

        if (avformat_open_input(&input_ctx, file.string().c_str(), nullptr,
                                nullptr) < 0) {
            throw std::runtime_error("Could not open input file");
        }
        if (avformat_find_stream_info(input_ctx, nullptr) < 0) {
            avformat_close_input(&input_ctx);
            throw std::runtime_error("Failed to retrieve stream info");
        }
        int audio_stream_index = av_find_best_stream(
            input_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audio_stream_index < 0) {
            avformat_close_input(&input_ctx);
            throw std::runtime_error("No audio stream found");
        }

        AVStream *in_audio_stream = input_ctx->streams[audio_stream_index];
        AVRational input_time_base = in_audio_stream->time_base;

        if (!audio_stream) {
            audio_stream = avformat_new_stream(output_ctx, nullptr);
            if (!audio_stream) {
                avformat_close_input(&input_ctx);
                throw std::runtime_error("Failed to create output stream");
            }

            if (avcodec_parameters_copy(audio_stream->codecpar,
                                        in_audio_stream->codecpar) < 0) {
                avformat_close_input(&input_ctx);
                throw std::runtime_error("Failed to copy codec parameters");
            }

            if (avformat_write_header(output_ctx, &options) < 0) {
                avformat_close_input(&input_ctx);
                throw std::runtime_error("Failed to write header");
            }
        }

        AVPacket pkt;
        av_init_packet(&pkt);

        bool first_pkt = true;
        int64_t last_pts = 0;
        int64_t last_duration = 0;

        while (av_read_frame(input_ctx, &pkt) >= 0) {
            if (pkt.stream_index == audio_stream_index) {
                // Calculate sleep duration based on packet duration
                if (pkt.duration > 0) {
                    int64_t sleep_us = av_rescale_q(
                        pkt.duration, input_time_base, AV_TIME_BASE_Q);

                    auto diff_us = sleep_us - lag.count();
                    if (diff_us > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::microseconds(diff_us));
                    }
                }

                // Handle the FFMPEG-produced files quirk:
                // first n packets can have negative pts value
                if (first_pkt) {
                    first_pkt = false;
                    if (pkt.pts < 0) {
                        offset_pts -= pkt.pts;
                    }
                }
                pkt.pts = pkt.pts + offset_pts;
                pkt.dts = pkt.pts;
                last_pts = pkt.pts;
                last_duration = pkt.duration;

                int64_t t_track_us =
                    av_rescale_q(pkt.pts, input_time_base, AV_TIME_BASE_Q);

                DEBUG_MSG("pts:" << pkt.pts << "\t offs:" << offset_pts
                                 << "\t duration:" << pkt.duration
                                 << "\t t_track_us:" << t_track_us);

                pkt.stream_index = audio_stream->index;

                if (av_interleaved_write_frame(output_ctx, &pkt) < 0) {
                    av_packet_unref(&pkt);
                    avformat_close_input(&input_ctx);

                    throw ErrorWritePacket();
                }

                auto now = std::chrono::system_clock::now();
                lag = std::chrono::duration_cast<std::chrono::microseconds>(
                    now - start_time - std::chrono::microseconds(t_track_us));
            }
            av_packet_unref(&pkt);
        }
        offset_pts = last_pts + last_duration;
        avformat_close_input(&input_ctx);
    }

    void run() {
        start_time = std::chrono::system_clock::now();
        init_icecast_connection();

        while (true) {
            auto files = get_m4a_files();
            if (files.empty()) {
                std::cerr << "No M4A files found, waiting...\n";
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }

            shuffle_playlist(files);

            for (const auto &file : files) {
                std::cout << "Now playing: " << file.filename() << "\n";
                try {
                    stream_file(file);
                } catch (const ErrorWritePacket &e) {
                    throw e;
                } catch (const std::exception &e) {
                    std::cerr << "Error: " << e.what() << "\n";
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        }
    }
};

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <icecast_url> <music_directory>\n";
        return 1;
    }

    // Erace the URL from process title as it contains password
    std::string url = argv[1];
    memset(argv[1], 0, strlen(argv[1]));
    prctl(PR_SET_NAME, "");

    try {
        IcecastStreamer streamer(url, argv[2]);
        streamer.run();
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
