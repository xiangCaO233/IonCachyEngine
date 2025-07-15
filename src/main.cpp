#include <fmt/base.h>

#include <chrono>
#include <ice/tool/AllocationTracker.hpp>
#include <thread>

#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/AudioFormat.hpp"
#include "ice/manage/AudioPool.hpp"
#include "ice/thread/ThreadPool.hpp"

void test() {
    ice::ThreadPool thread_pool(8);
#ifdef __APPLE__
    auto file1 = "/Users/2333xiang/Music/Tensions - チョコレーション.mp3";
#else
    auto file1 =
        "/home/xiang/Documents/music game maps/Mind Enhancement - "
        "PIKASONIC/Mind Enhancement - PIKASONIC.mp3";
#endif  //__APPLE__
    auto file2 =
        "/home/xiang/Documents/music game maps/osu/Akasha/Snare 3 - B.wav";
    // test
    ice::AudioPool audiopool(ice::CodecBackend::FFMPEG);
    auto track1 = audiopool.get_or_load(thread_pool, file1);
    track1 = audiopool.get_or_load(thread_pool, file1);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    fmt::print("frames:{}\n", track1->num_frames());
    fmt::print("channels:{}\n", track1->native_format().channels);
    fmt::print("samplerate:{}\n", track1->native_format().samplerate);
    ice::AudioDataFormat format;
    ice::AudioBuffer buffer(format, 1024);
    track1->read(buffer, 3000000, 1024);

    // auto track2 = audiopool.get_or_load(thread_pool, file2);
    // track2 = audiopool.get_or_load(thread_pool, file2);
}

int main(int argc, char* argv[]) {
    ice::reset_allocation_counters();
    test();
    ice::print_allocation_stats();
    return 0;
}
