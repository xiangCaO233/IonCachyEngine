#include <fmt/base.h>
#include <fmt/printf.h>

#include <chrono>
#include <ice/tool/AllocationTracker.hpp>
#include <thread>

#include "ice/manage/AudioPool.hpp"

void test() {
    auto file1 =
        "/home/xiang/Documents/music game maps/Mind Enhancement - "
        "PIKASONIC/Mind Enhancement - PIKASONIC.mp3";
    auto file2 =
        "/home/xiang/Documents/music game maps/osu/Akasha/Snare 3 - B.wav";
    // test
    ice::AudioPool audiopool(ice::CodecBackend::FFMPEG);
    auto track1 = audiopool.get_or_load(file1);
    track1 = audiopool.get_or_load(file1);
    auto track2 = audiopool.get_or_load(file2);
    track2 = audiopool.get_or_load(file2);
}

int main(int argc, char* argv[]) {
    ice::reset_allocation_counters();
    test();
    ice::print_allocation_stats();
    return 0;
}
