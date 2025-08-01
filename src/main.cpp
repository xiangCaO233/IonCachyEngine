#include <fmt/base.h>

#include <algorithm>
#include <chrono>
#include <ice/tool/AllocationTracker.hpp>
#include <memory>
#include <numbers>
#include <thread>

#include "ice/core/MixBus.hpp"
#include "ice/core/SourceNode.hpp"
#include "ice/core/effect/Compresser.hpp"
#include "ice/core/effect/GraphicEqualizer.hpp"
#include "ice/core/effect/PitchAlter.hpp"
#include "ice/core/effect/TimeStretcher.hpp"
#include "ice/manage/AudioFormat.hpp"
#include "ice/manage/AudioPool.hpp"
#include "ice/manage/AudioTrack.hpp"
#include "ice/out/play/openal/ALPlayer.hpp"
#include "ice/out/play/sdl/SDLPlayer.hpp"
#include "ice/thread/ThreadPool.hpp"

void test() {
    ice::ThreadPool thread_pool(8);
#ifdef __APPLE__
    auto file1 = "/Users/2333xiang/Music/Tensions - チョコレーション.mp3";
    auto file2 =
        "/Users/2333xiang/Music/Neko Hacker,利香 - GHOST (feat. 利香).mp3";
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
    auto file2 =
        "/home/xiang/Documents/music game maps/osu/Akasha/Snare 3 - B.wav";
#endif  //__APPLE__
    // test
    ice::AudioPool audiopool;
    auto track1 = audiopool.get_or_load(thread_pool, file1);
    track1 = audiopool.get_or_load(thread_pool, file1,
                                   ice::CachingStrategy::STREAMING);

    auto track2 = audiopool.get_or_load(thread_pool, file2);
    track2 = audiopool.get_or_load(thread_pool, file2);

    // 获取音频轨道信息
    fmt::print("frames:{},{}\n", track1->get_media_info().frame_count,
               track2->get_media_info().frame_count);
    fmt::print("channels:{},{}\n", track1->get_media_info().format.channels,
               track2->get_media_info().format.channels);
    fmt::print("samplerate:{},{}\n", track1->get_media_info().format.samplerate,
               track2->get_media_info().format.samplerate);

    ice::ALPlayer::init_backend();
    auto ds = ice::ALPlayer::list_devices();
    std::ranges::for_each(ds, [](const ice::ALAudioDeviceInfo& device) {
        fmt::print("al devicename:{}\n", device.name);
    });

    ice::SDLPlayer::init_backend();

    auto devices = ice::SDLPlayer::list_devices();
    std::ranges::for_each(devices, [](const auto& device) {
        fmt::print("deviceid:{},devicename:{}\n", device.id, device.name);
    });

    // auto selected_device = devices[0].id;

    ice::SDLPlayer player;

    auto source = std::make_shared<ice::SourceNode>(track1);
    source->play();
    auto source2 = std::make_shared<ice::SourceNode>(track2);
    source2->play();

    using namespace std::chrono_literals;

    // source->set_playpos(1min + 100us);

    source->setloop(true);

    auto stretcher = std::make_shared<ice::TimeStretcher>();

    // 设置播放速度
    stretcher->set_playback_ratio(1.);

    stretcher->set_inputnode(source);

    auto pitchalter = std::make_shared<ice::PitchAlter>();
    pitchalter->set_pitch_shift(0.);
    pitchalter->set_inputnode(stretcher);

    source2->setloop(true);

    auto mixer = std::make_shared<ice::MixBus>();

    mixer->add_source(pitchalter);

    std::vector<double> freqs = {31,   62,   125,  250,  500,
                                 1000, 2000, 4000, 8000, 16000};
    auto eq = std::make_shared<ice::GraphicEqualizer>(freqs);

    const double q = std::numbers::sqrt3;

    eq->set_band_q_factor(0, q / 2.0);
    eq->set_band_q_factor(1, q / 2.0);
    eq->set_band_q_factor(2, q / 2.0);

    eq->set_band_gain_db(0, 9);
    eq->set_band_gain_db(1, 9);
    eq->set_band_gain_db(2, 9);

    // 启用均衡器
    eq->set_inputnode(mixer);

    // mixer->add_source(source2);

    // auto stretcher = std::make_shared<Stretcher>(source ,1.2 ,0.8);
    // auto mixer = std::make_shared<Mixer>();
    // mixer.add_source({stretcher ,source2 });
    auto compressor = std::make_shared<ice::Compressor>();
    compressor->set_inputnode(eq);
    // 为处理人声设置典型的参数
    // 设置一个相对较低的阈值，以便捕捉到大部分的演唱信号
    compressor->set_threshold_db(-30.0f);

    // 设置一个适中的压缩比，既能控制住大音量，又不会听起来太死板
    // 4:1 的压缩比
    compressor->set_ratio(4.f);

    // 设置一个较快的启动时间，以便能迅速对爆破音('p', 'b')做出反应
    compressor->set_attack_ms(50.0f);

    // 设置一个与音乐节奏相关的释放时间(beat / 2)，让声音听起来自然
    compressor->set_release_ms(150.0f);

    // 压缩了信号，整体音量变小，提回来
    compressor->set_makeup_gain_db(6.0f);

    player.set_source(eq);

    // player.set_source(mixer);

    player.open();
    // player.open(selected_device);
    player.start();

    auto total_time = std::max(source->total_time(), source2->total_time());

    fmt::print("total:\n");
    fmt::print("{}ns\n", total_time.count());
    fmt::print("{}us\n", total_time.count() / 1000.0);
    fmt::print("{}ms\n", total_time.count() / 1000.0 / 1000.0);
    fmt::print("{}s\n", total_time.count() / 1000.0 / 1000.0 / 1000.0);
    fmt::print("{}min\n", total_time.count() / 1000.0 / 1000.0 / 1000.0 / 60.0);

    // eq自动控制lambda
    auto eq_controll = [&]() {
        int count = 0;
        while (count < 16) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2500ms));

            eq->set_band_gain_db(0, count * 3);
            eq->set_band_gain_db(1, count * 3);
            eq->set_band_gain_db(2, count * 3);

            fmt::print("[q={}]band [31hz,62hz,125hz] gain:{}db\n", q,
                       count * 3);

            ++count;
        }
    };

    // pitch自动控制lambda
    auto pitch_controll = [&]() {
        int count = 0;
        while (count < 16) {
            std::this_thread::sleep_for(std::chrono::milliseconds(7500ms));
            pitchalter->set_pitch_shift(count * 1.5);
            fmt::print("pitch alt:{}semitones\n", count * 1.5);
            ++count;
        }
    };

    std::thread get_actual_play_ratio([&]() {
        // eq_controll();
        // pitch_controll();
        std::this_thread::sleep_for(std::chrono::milliseconds(500ms));
        fmt::print("actual playback ratio:{}\n",
                   stretcher->get_actual_playback_ratio());
    });
    get_actual_play_ratio.detach();

    std::this_thread::sleep_for(total_time);
    // player.join();

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
