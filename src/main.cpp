#include <SDL3/SDL_init.h>
#include <fmt/base.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ice/tool/AllocationTracker.hpp>
#include <memory>
#include <mutex>
#include <numbers>
#include <stop_token>
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

/// @brief 使用本机音频文件组装效果图并手动试听 SDL 输出。
/// @details 这是依赖硬编码路径与实体设备的演示，不是自动化通过/失败测试。
/// @return 加载或设备操作失败时为 false。
/// @warning 低频示例入口：执行文件访问、设备初始化和长时间
/// sleep，不能用于音频回调。
/// @warning 诊断线程在设备关闭前取消并回收，局部节点必须活过 join。
bool test()
{
    // 线程池先于音轨池构造、后于其析构，使局部池清理期间仍有工作线程对象。
    ice::ThreadPool thread_pool(8);
#ifdef __APPLE__
    // 路径仅适用于原开发机器，不从参数或测试资源目录选择输入。
    auto file1 = "/Users/2333xiang/Music/Tensions - チョコレーション.mp3";
    auto file2 =
        "/Users/2333xiang/Music/Neko Hacker,利香 - GHOST (feat. 利香).mp3";
#else
    // 非 Apple 分支也使用 Linux 绝对路径，Windows 上不能假定这些资源存在。
    auto file1 =
        "/home/xiang/Documents/music game maps/Mind Enhancement - "
        "PIKASONIC/Mind Enhancement - PIKASONIC.mp3";
    // 后续赋值覆盖前一候选，最终只加载最后一次指定的 file1。
    file1 =
        "/home/xiang/Documents/music game maps/Tensions - 3秒ルール/Tensions - "
        "3秒ルール.mp3";
    file1 =
        "/home/xiang/Documents/MusicMapRepo/osu/(SV)The Koxx - A Fool Moon "
        "Night/audio.mp3";
    auto file2 =
        "/home/xiang/Documents/MusicMapRepo/osu/1134062 LeaF - "
        "Mopemope/audio.mp3";
#endif
    // 相同路径按策略分别缓存；下面先请求默认策略，再显式取得流式音轨。
    ice::AudioPool audiopool;
    auto           track1Weak = audiopool.get_or_load(thread_pool, file1);
    // 默认 CACHY 与 STREAMING 对象独立保活；这里覆盖弱句柄，不改变旧对象策略。
    track1Weak = audiopool.get_or_load(
        thread_pool, file1, ice::CachingStrategy::STREAMING);
    // 从弱句柄取得强引用，使音源图创建前能检查轨道是否实际可用。
    auto track1 = track1Weak.lock();

    auto track2Weak = audiopool.get_or_load(thread_pool, file2);
    // 重复请求同一路径，演示音轨池的缓存查询入口。
    track2Weak  = audiopool.get_or_load(thread_pool, file2);
    auto track2 = track2Weak.lock();

    if ( !track1 || !track2 ) {
        // 设备尚未初始化，直接把加载失败交给入口报告。
        fmt::print("failed to load audio tracks\n");
        return false;
    }

    // 打印文件元数据只供人工核对，不表示实际输出设备采用了相同采样格式。
    fmt::print("frames:{},{}\n",
               track1->get_media_info().frame_count,
               track2->get_media_info().frame_count);
    fmt::print("channels:{},{}\n",
               track1->get_media_info().format.channels,
               track2->get_media_info().format.channels);
    fmt::print("samplerate:{},{}\n",
               track1->get_media_info().format.samplerate,
               track2->get_media_info().format.samplerate);

    // OpenAL 只用于枚举演示，本次真正输出选择 SDL，不创建 ALPlayer 播放实例。
    ice::ALPlayer::init_backend();
    auto ds = ice::ALPlayer::list_devices();
    std::ranges::for_each(ds, [](const ice::ALAudioDeviceInfo& device) {
        fmt::print("al devicename:{}\n", device.name);
    });

    // SDL 子系统初始化必须先于设备枚举与 player.open。
    if ( !ice::SDLPlayer::init_backend() ) return false;

    auto devices = ice::SDLPlayer::list_devices();
    std::ranges::for_each(devices, [](const auto& device) {
        fmt::print("deviceid:{},devicename:{}\n", device.id, device.name);
    });

    // 枚举列表仅展示，没有按索引选设备；后面的无参 open 使用后端默认设备。
    ice::SDLPlayer player;

    // 图节点在启动供数前创建，共享所有权负责让上游轨道随音源节点存活。
    auto source = std::make_shared<ice::SourceNode>(track1);
    source->play();
    // 第二音源虽进入播放状态，但下文未加入混音总线，不会因 play 就自动输出。
    auto source2 = std::make_shared<ice::SourceNode>(track2);
    source2->play();

    using namespace std::chrono_literals;

    // 循环音源不会依靠文件末尾结束供数，示例以主线程等待后显式 stop 收尾。
    source->setloop(true);

    auto stretcher = std::make_shared<ice::TimeStretcher>();

    // 请求原速仅设置参数；本入口没有显式调用 prepare 来建立变速节点预热状态。
    stretcher->set_playback_ratio(1.);

    stretcher->set_inputnode(source);

    // 请求零半音偏移，保留音高节点作为图连接演示，不在此校验声学结果。
    auto pitchalter = std::make_shared<ice::PitchAlter>();
    pitchalter->set_pitch_shift(0.);
    pitchalter->set_inputnode(stretcher);

    // 未接入图的 source2 循环标志不影响当前输出，仍会参与下文演示等待时长计算。
    source2->setloop(true);

    auto mixer = std::make_shared<ice::MixBus>();

    // 当前总线只有一条已连接支路：source → stretcher → pitchalter。
    mixer->add_source(pitchalter);

    // 固定十段中心频率用于示范控制接口，不按当前音频采样率重新生成频段。
    std::vector<double> freqs = { 31,   62,   125,  250,  500,
                                  1000, 2000, 4000, 8000, 16000 };
    auto                eq    = std::make_shared<ice::GraphicEqualizer>(freqs);

    // 实际写入前三段的 Q 为 q/2，后面的演示日志只打印 q，二者并不相同。
    const double q = std::numbers::sqrt3;

    eq->set_band_q_factor(0, q / 2.0);
    eq->set_band_q_factor(1, q / 2.0);
    eq->set_band_q_factor(2, q / 2.0);

    // 三个低频段同时提升，试听时需为叠加增益预留输出余量。
    eq->set_band_gain_db(0, 9);
    eq->set_band_gain_db(1, 9);
    eq->set_band_gain_db(2, 9);

    // 均衡器接在总线之后，其输出直接成为本次 player 的图根。
    eq->set_inputnode(mixer);

    // 压缩器虽持有 eq 输入，但未被 player 或其他消费节点引用为输出根。
    // 因此以下压缩参数在当前实际播放链路上不生效，不提供均衡提升后的峰值保护。
    auto compressor = std::make_shared<ice::Compressor>();
    compressor->set_inputnode(eq);
    // 负阈值示范 dB 参数接口，不根据输入电平估计最佳压缩阈值。
    compressor->set_threshold_db(-30.0f);

    // 压缩比与补偿增益独立配置，设置前者不会自动调整后者。
    compressor->set_ratio(4.f);

    // 较慢的包络上升保留起音瞬态，压缩器不会立即钳制峰值。
    compressor->set_attack_ms(50.0f);

    // 包络回落采用毫秒时值，与音轨速度独立。
    compressor->set_release_ms(150.0f);

    // 补偿值仅作接口示范；当前压缩器未连接，不能解释为实际输出已增加 6dB。
    compressor->set_makeup_gain_db(6.0f);

    // 在设备开始供数前绑定图根，运行中替换图需要另外遵守播放器同步约束。
    player.set_source(eq);

    // 未打开或未启动时不进入按音轨时长等待的阶段。
    if ( !player.open() || !player.start() ) {
        // 即使打开成功但启动失败，也先关闭流再退出 SDL 子系统。
        player.close();
        ice::SDLPlayer::quit_backend();
        return false;
    }

    // 等待两轨时长最大值只是演示计时，不是设备排空或所有节点完成的同步信号。
    auto total_time = std::max(source->total_time(), source2->total_time());

    // 单位输出按纳秒换算供人工阅读，不用于推进音频播放游标。
    fmt::print("total:\n");
    fmt::print("{}ns\n", total_time.count());
    fmt::print("{}us\n", total_time.count() / 1000.0);
    fmt::print("{}ms\n", total_time.count() / 1000.0 / 1000.0);
    fmt::print("{}s\n", total_time.count() / 1000.0 / 1000.0 / 1000.0);
    fmt::print("{}min\n", total_time.count() / 1000.0 / 1000.0 / 1000.0 / 60.0);

    /// @brief 预留的低频均衡器扫参演示，当前未调用。
    /// @warning 每轮阻塞 2.5 秒，不能放入音频回调或交互更新线程。
    auto eq_controll = [&]() {
        int count = 0;
        while ( count < 16 ) {
            // 先等待再写入，第一轮把原先的 9dB 改为 0dB，不是平滑衔接原值。
            std::this_thread::sleep_for(std::chrono::milliseconds(2500ms));

            // 三次 setter
            // 分别提交，不保证音频侧把三个频段当成一份原子快照接收。
            eq->set_band_gain_db(0, count * 3);
            eq->set_band_gain_db(1, count * 3);
            eq->set_band_gain_db(2, count * 3);

            // 日志显示请求值，不读取节点实际接收或钳制后的参数。
            fmt::print(
                "[q={}]band [31hz,62hz,125hz] gain:{}db\n", q, count * 3);

            ++count;
        }
    };

    /// @brief 预留的低频音高步进演示，当前未调用。
    /// @warning 每轮阻塞 7.5
    /// 秒，按引用使用图节点，执行期间必须保证节点与捕获变量存活。
    auto pitch_controll = [&]() {
        int count = 0;
        while ( count < 16 ) {
            // 离散改变音高，每次更新对应一个新的处理参数。
            std::this_thread::sleep_for(std::chrono::milliseconds(7500ms));
            pitchalter->set_pitch_shift(count * 1.5);
            fmt::print("pitch alt:{}semitones\n", count * 1.5);
            ++count;
        }
    };

    /// @brief 延迟读取一次变速诊断值，不参与音频供数。
    /// @warning 低频诊断线程借用局部节点，退出时先取消等待并 join 再关闭设备。
    std::jthread get_actual_play_ratio([&](std::stop_token stop) {
        // 延迟仅用于观察运行中读数，不是预热或首块处理完成的保证。
        // 停止令牌使短音轨结束时无需继续等待整个诊断间隔。
        // 等待锁只服务本线程的诊断定时，不持有图或设备的控制锁。
        std::mutex                  waitMutex;
        std::condition_variable_any wake;
        std::unique_lock            lock(waitMutex);
        wake.wait_for(lock, stop, 500ms, [] { return false; });
        if ( stop.stop_requested() ) return;
        fmt::print("actual playback ratio:{}\n",
                   stretcher->get_actual_playback_ratio());
    });

    // 主线程不监听停止事件，此阻塞不可用于产品内需要即时取消的播放控制流程。
    std::this_thread::sleep_for(total_time);

    // 先回收借用图节点的诊断线程，防止设备关闭与诊断读取交错。
    // 停止与超时可能同时发生，若读数已开始，仍须等它完成后再销毁资源。
    // join 仅位于手动示例退出路径，不能搬到音频回调。
    // jthread 的析构还负责提前退出时的回收，避免重新引入分离线程。
    get_actual_play_ratio.request_stop();
    get_actual_play_ratio.join();

    // 先结束供数再关闭设备，最后退出 SDL 子系统，避免后台继续访问已释放设备。
    player.stop();
    player.close();
    ice::SDLPlayer::quit_backend();
    return true;
}

/// @brief 运行手工播放演示并输出分配计数。
/// @param argc 当前不使用，不据命令行参数数量选择测试资源。
/// @param argv 当前不解析，输入路径固定在 test 内。
/// @return 已知加载或设备失败返回 1，正常完成演示返回 0。
/// 零退出码不证明设备排空、音质或实时安全性。
/// @warning 入口包含长时间等待和实体设备访问，不应作为无设备 CI 的回归用例。
int main(int argc, char* argv[])
{
    // 统计范围覆盖示例调用，诊断线程由 test 在返回前回收。
    ice::reset_allocation_counters();
    const bool completed = test();
    // 演示进程独占 SDL 生命周期；所有局部播放器销毁后由入口完成全局收尾。
    SDL_Quit();
    // 输出计数没有实时路径专属断言，也没有按阈值决定进程退出状态。
    ice::print_allocation_stats();
    // 保留失败退出状态；自动化实时断言仍由独立 CTest 提供。
    return completed ? 0 : 1;
}
