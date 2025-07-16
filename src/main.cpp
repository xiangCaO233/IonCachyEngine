#include <fmt/base.h>

#include <chrono>
#include <ice/tool/AllocationTracker.hpp>
#include <thread>

#include "ice/config/config.hpp"
#include "ice/core/SourceNode.hpp"
#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/AudioFormat.hpp"
#include "ice/manage/AudioPool.hpp"
#include "ice/out/play/sdl/SDLPlayer.hpp"
#include "ice/thread/ThreadPool.hpp"

void test() {
    ice::ThreadPool thread_pool(8);
#ifdef __APPLE__
    auto file1 = "/Users/2333xiang/Music/Tensions - チョコレーション.mp3";
#else
    auto file1 =
        "/home/xiang/Documents/music game maps/Mind Enhancement - "
        "PIKASONIC/Mind Enhancement - PIKASONIC.mp3";
    file1 =
        "/home/xiang/Documents/music game maps/Tensions - 3秒ルール/Tensions - "
        "3秒ルール.mp3";
    file1 =
        "/home/xiang/Documents/music game maps/初音ミク 湊貴大 - 朧月/初音ミク "
        "湊貴大 - 朧月.mp3";
#endif  //__APPLE__
    auto file2 =
        "/home/xiang/Documents/music game maps/osu/Akasha/Snare 3 - B.wav";
    // test
    ice::AudioPool audiopool;
    auto track1 = audiopool.get_or_load(thread_pool, file1);
    track1 = audiopool.get_or_load(thread_pool, file1);
    // auto track2 = audiopool.get_or_load(thread_pool, file2);
    // track2 = audiopool.get_or_load(thread_pool, file2);

    // 获取音频轨道信息
    fmt::print("frames:{}\n", track1->num_frames());
    fmt::print("channels:{}\n", track1->native_format().channels);
    fmt::print("samplerate:{}\n", track1->native_format().samplerate);

    // ice::AudioDataFormat format;
    // ice::AudioBuffer buffer(format, 1024);
    // track1->read(buffer, 3000000, 1024);

    ice::SDLPlayer::init_backend();
    auto devices = ice::SDLPlayer::list_devices();
    for (const auto& device : devices) {
        fmt::print("deviceid:{},devicename:{}\n", device.id, device.name);
    }
    // auto selected_device = devices[0].id;

    ice::SDLPlayer player;

    auto source = std::make_shared<ice::SourceNode>(track1);
    // auto source2 = std::make_shared<ice::SourceNode>(track2);

    using namespace std::chrono_literals;

    // source->set_playpos(1min + 100us);

    source->setloop(true);

    // auto stretcher = std::make_shared<Stretcher>(source ,1.2 ,0.8);
    // auto mixer = std::make_shared<Mixer>();
    // mixer.add_source({stretcher ,source2 });

    player.set_source(source);
    // player.set_source(mixer);

    player.open(23);
    // player.open(selected_device);
    player.start();

    auto total_time = source->total_time();

    fmt::print("total:\n");
    fmt::print("{}ns\n", total_time.count());
    fmt::print("{}us\n", total_time.count() / 1000.0);
    fmt::print("{}ms\n", total_time.count() / 1000.0 / 1000.0);
    fmt::print("{}s\n", total_time.count() / 1000.0 / 1000.0 / 1000.0);
    fmt::print("{}min\n", total_time.count() / 1000.0 / 1000.0 / 1000.0 / 60.0);

    std::this_thread::sleep_for(total_time);

    player.stop();
    player.close();
    ice::SDLPlayer::quit_backend();
}

int main(int argc, char* argv[]) {
    ice::reset_allocation_counters();
    test();
    ice::print_allocation_stats();
    return 0;
}
