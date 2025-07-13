#include <ice/tool/AllocationTracker.hpp>

#include "ice/manage/AudioPool.hpp"
#include "ice/thread/ThreadPool.hpp"

void test() {
    ice::ThreadPool thread_pool(8);
    auto file1 =
        "/home/xiang/Documents/music game maps/Mind Enhancement - "
        "PIKASONIC/Mind Enhancement - PIKASONIC.mp3";
    auto file2 =
        "/home/xiang/Documents/music game maps/osu/Akasha/Snare 3 - B.wav";
    // test
    ice::AudioPool audiopool(ice::CodecBackend::FFMPEG);
    auto track1 = audiopool.get_or_load(thread_pool, file1);
    track1 = audiopool.get_or_load(thread_pool, file1);
    auto track2 = audiopool.get_or_load(thread_pool, file2);
    track2 = audiopool.get_or_load(thread_pool, file2);
}

int main(int argc, char* argv[]) {
    ice::reset_allocation_counters();
    test();
    ice::print_allocation_stats();
    return 0;
}
