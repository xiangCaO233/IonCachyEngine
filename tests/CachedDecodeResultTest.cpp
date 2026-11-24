#include "ice/config/config.hpp"
#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/AudioTrack.hpp"
#include "ice/manage/dec/IDecoderFactory.hpp"
#include "ice/manage/dec/IDecoderInstance.hpp"
#include "ice/manage/dec/MediaInfo.hpp"
#include "ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp"
#include "ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp"
#include "ice/thread/ThreadPool.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
using ice::AudioDataFormat;
using ice::AudioTrack;
using ice::CachedDecodeError;
using ice::CachingStrategy;
using Stage = CachedDecodeError::Stage;

/// @brief 负值模拟后端原始错误，确保缓存不会覆盖错误码。
constexpr int READ_ERROR = -123;

/// @brief 每个模拟媒体选择一个故障点，其余阶段保持可用。
enum class Mode {
    /// @brief 后端报告非零估值，但实际只交付一帧。
    Samples,
    /// @brief 初始探测正常，后台无法重新打开 IDecoderInstance。
    OpenFailure,
    /// @brief 已能建立解码实例，但后端返回不同采样率。
    InvalidFormat,
    /// @brief 同意探测和格式协商，拒绝顺序读取起点。
    SeekFailure,
    /// @brief 部分 PCM 已被缓存时报告不可恢复错误。
    ReadFailure,
    /// @brief 仅报告大于容量的帧数，用于验证调用方边界检查。
    InvalidReadCount,
    /// @brief 后台任务未正常返回结构化缓存结果。
    TaskFailure,
};

/// @brief 每个制品返回短有效块再排尾，用错误模式覆盖后台解码分支。
class TestDecoder final : public ice::IDecoderInstance
{
public:
    /// @brief 只有格式错误场景才改变采样率，避免误走读取分支。
    /// @param format 请求使用的采样率和通道布局。
    /// @param mode 当前解码实例模拟的输入和故障阶段。
    TestDecoder(AudioDataFormat format, Mode mode)
        : m_format(format), m_mode(mode)
    {
        if ( m_mode == Mode::InvalidFormat ) ++m_format.samplerate;
    }

    /// @brief 控制完整缓存的首帧准备；支持从头重试。
    /// @param frame 测试只接受顺序开始所需的绝对首帧位置。
    /// @return 首帧定位是否成功；定位失败场景在 PCM 缓存前退出。
    bool seek(size_t frame) override
    {
        m_reads = 0;
        return frame == 0 && m_mode != Mode::SeekFailure;
    }

    /// @brief 专门让末块仍有样本且报告错误，验证不会发布部分轨道。
    /// @param buffer 每个声道的 PCM 写入指针，模拟实例只使用各通道首槽。
    /// @param frames 调用方声明的目标容量。
    /// @return 本次读取的有效帧数；错误计数场景不写入目标。
    size_t read(float** buffer, size_t frames) override
    {
        ++m_reads;
        // 只伪造返回计数，避免测试自身在触发防御分支前越界。
        if ( m_mode == Mode::InvalidReadCount ) return frames + 1;
        // 正常序列包含有效帧、短块和零读取；错误序列在第二块提前结束。
        if ( m_reads > 1 && m_mode != Mode::ReadFailure ) return 0;
        for ( size_t channel = 0; channel < m_format.channels; ++channel )
            buffer[channel][0] = m_reads == 1 ? 0.25f : 0.5f;
        return 1;
    }

    /// @brief 设置短读块的致命错误，与普通零帧 EOF 严格区分。
    /// @return 后端模拟值与真实帧数独立，以验证切片长度不足以判断成功。
    int last_read_error() const noexcept override
    {
        return m_mode == Mode::ReadFailure && m_reads == 2 ? READ_ERROR : 0;
    }

    /// @brief 返回模拟后端可读取的 PCM 格式。
    /// @return 引用借用实例成员，与真实后端的生命周期一致。
    const AudioDataFormat& get_source_format() const override
    {
        return m_format;
    }

    /// @brief 故意提供不同的估算长度，成功长度应从 PCM 计数得出。
    /// @return 与实际音频刻意不一致的估值，以捕获错误的缓存长度来源。
    size_t get_source_total_frames() const override { return 20; }

private:
    /// @brief 构造时固定，测试线程不在读取期间重新配置它。
    /// 只有专门验证格式失败的模式与请求的采样率不同。
    AudioDataFormat m_format;
    /// @brief 模拟一个失败阶段，避免同一输入同时命中多个早退分支。
    Mode m_mode;
    /// @brief 表示被缓存线程实际拉取的块编号。
    size_t m_reads{};
};

/// @brief 探测先通过，之后才在解码线程触发真实的失败传播链。
class TestFactory final : public ice::IDecoderFactory
{
public:
    /// @brief 每份工厂只模拟一种行为，线程间没有可写共享状态。
    /// @param mode 只选择一次失败阶段，可由测试线程反复提交同一后端。
    explicit TestFactory(Mode mode) : m_mode(mode) {}

    /// @brief 始终允许创建音轨，使测试可以取得后台准备结果。
    /// @param result 交给 AudioTrack 的探测结果；后端仍要独立出具实际 PCM
    /// 格式。
    /// @return 探测固定成功，这里不能提前消费后台错误。
    bool probe(std::string_view, ice::MediaInfo& result) const override
    {
        // 先用全局配置建立有效的音轨元信息，格式不匹配留给实例报告。
        result.format = ice::ICEConfig::internal_format;
        return true;
    }

    /// @brief 在后台模拟打开失败、被放弃的 promise 或可读取的实例。
    /// @param format 探测通过后由缓存策略发出的目标 PCM 格式。
    /// @return 正常建立时移交独占解码器；打开失败返回空指针。
    std::unique_ptr<ice::IDecoderInstance> create_instance(
        std::string_view, const AudioDataFormat& format) const override
    {
        // 合同允许探测后文件消失，用空实例精确模拟这个竞态结果。
        if ( m_mode == Mode::OpenFailure ) return {};
        if ( m_mode == Mode::TaskFailure ) {
            // 标准库的 broken promise 从工作任务交付异常，不需测试手工抛出。
            std::future<void> broken;
            {
                std::promise<void> promise;
                broken = promise.get_future();
            }
            // 确认只在 promise 析构发布异常后提取；不会增加测试等待时间。
            broken.get();
        }
        return std::make_unique<TestDecoder>(format, m_mode);
    }

private:
    /// @brief 传给每次创建的独立实例，工厂无需维护状态。
    Mode m_mode;
};

/// @brief 用真实的缓存任务与 AudioTrack 查询检查指定错误及其稳定性。
/// @param pool 负责异步执行工厂和解码读取。
/// @param mode 控制本次缓存失败时机。
/// @param stage 非实时查询应返回的阶段。
/// @param backend_code 预期保留的后端原始诊断。
/// @return 帧接口和诊断均与失败类型一致时为 true。
bool check_failure(ice::ThreadPool& pool, Mode mode, Stage stage,
                   int backend_code = 0)
{
    const auto track = AudioTrack::create("virtual",
                                          pool,
                                          std::make_shared<TestFactory>(mode),
                                          CachingStrategy::CACHY);
    if ( !track ) return false;
    // 先走既有接口消费 future，再读取诊断，验证失败原因不会被帧数查询吞掉。
    // 失败转移对只认识帧数的调用方仍表现为零长度。
    if ( track->num_frames() != 0 ) return false;
    const auto status = track->cached_decode_result();
    if ( !status || status->has_value() || status->error().stage != stage ||
         status->error().backendCode != backend_code )
        return false;
    // 若上层忽略诊断，旧帧接口至少不能让缓存中一度存在的前缀通过。
    ice::AudioBuffer buffer;
    if ( !buffer.resize(ice::ICEConfig::internal_format, 1) ) return false;
    buffer.raw_ptrs()[0][0] = 0.75f;
    if ( track->read(buffer, 0, 1) != 0 || buffer.raw_ptrs()[0][0] != 0.75f )
        return false;

    // 检查同一失败重复可见，且仅任务异常携带可重用的原始异常指针。
    // 不执行重新抛出；比较实例身份即可发现原因被后续查询替换。
    const auto again = track->cached_decode_result();
    return again && !again->has_value() && again->error().stage == stage &&
           again->error().exception == status->error().exception &&
           static_cast<bool>(status->error().exception) ==
               (stage == Stage::BackgroundTask);
}

/// @brief 验证普通音轨仍返回真实帧数，且流式策略不报告完整缓存状态。
/// @param pool 接收完整缓存轨道的解码任务，流式由自己的预读策略管理。
/// @return 实际样本、帧数和两个策略各自的查询语义均保持时为 true。
bool check_legacy(ice::ThreadPool& pool)
{
    const auto factory = std::make_shared<TestFactory>(Mode::Samples);
    const auto cached =
        AudioTrack::create("virtual", pool, factory, CachingStrategy::CACHY);
    const auto streaming = AudioTrack::create(
        "virtual", pool, factory, CachingStrategy::STREAMING);
    if ( !cached || !streaming ) return false;
    const auto       status = cached->cached_decode_result();
    ice::AudioBuffer buffer;
    // 缓存有一帧，读取请求两个槽时只返回真实可读前缀。
    if ( !buffer.resize(ice::ICEConfig::internal_format, 2) ) return false;
    return status && status->has_value() && **status == 1 &&
           cached->num_frames() == 1 && cached->read(buffer, 0, 2) == 1 &&
           buffer.raw_ptrs()[0][0] == 0.25f &&
           !streaming->cached_decode_result();
}

/// @brief 明确生成两个独立样本，验证真实 FFmpeg 对原始空文件和合法空轨的区别。
/// @param pool 运行真实 FFmpeg 工厂产生的完整缓存任务。
/// @param test_dir 唯一用例工作目录，由 CTest 设在构建树中。
/// @return 无媒体输入被拒绝，且零帧 PCM 成功时为 true。
bool check_file_inputs(ice::ThreadPool&             pool,
                       const std::filesystem::path& test_dir)
{
    // 所有输入在运行时生成，避免依赖 LFS 下载和开发机媒体。
    std::error_code error;
    std::filesystem::create_directories(test_dir, error);
    if ( error ) return false;
    const auto empty = test_dir / "empty.bin";
    const auto valid = test_dir / "empty.wav";
    // RIFF/WAVE 的 PCM 格式块完整，数据块长度为零。与无头空文件不同，
    // 该文件提供有效声道、样本格式和采样率，可建立正常的解码器。
    // 小端格式由字节直接构造，避免测试在异构目标上依赖整数对象布局。
    constexpr std::array<unsigned char, 44> wave{
        'R', 'I', 'F', 'F', 36, 0, 0,   0,   'W', 'A', 'V', 'E', 'f', 'm', 't',
        ' ', 16,  0,   0,   0,  1, 0,   1,   0,   128, 187, 0,   0,   0,   119,
        1,   0,   2,   0,   16, 0, 'd', 'a', 't', 'a', 0,   0,   0,   0,
    };
    {
        // 关闭文件之后再交给 FFmpeg，避免测试误读未写入的格式头。
        std::ofstream raw(empty, std::ios::binary | std::ios::trunc);
        std::ofstream wav(valid, std::ios::binary | std::ios::trunc);
        wav.write(reinterpret_cast<const char*>(wave.data()), wave.size());
        if ( !raw || !wav ) return false;
    }

    // 后端文件接口接收 UTF-8，与宿主路径在 Windows 上的原生编码无关。
    /// @brief 保留平台转换后的 UTF-8 字节，不让窄字符本地编码改变测试路径。
    const auto to_utf8 = [](const std::filesystem::path& path) {
        const auto utf8 = path.u8string();
        return std::string(reinterpret_cast<const char*>(utf8.data()),
                           utf8.size());
    };
    // 探测会拒绝连格式头都没有的文件，此时不会创建后台任务。
    const auto factory = std::make_shared<ice::FFmpegDecoderFactory>();
    if ( AudioTrack::create(
             to_utf8(empty), pool, factory, CachingStrategy::CACHY) )
        return false;
    // 合法空 PCM 必须得到一个音轨句柄及有值的成功结果。
    const auto empty_track = AudioTrack::create(
        to_utf8(valid), pool, factory, CachingStrategy::CACHY);
    if ( !empty_track ) return false;
    // 既有接口也返回零，因此必须进一步检查可预备状态是否为成功。
    const auto status = empty_track->cached_decode_result();
    if ( !status || !status->has_value() || **status != 0 ) return false;

    // 查询属于本次 read；非法调用之后，正常排尾必须清掉旧错误码。
    // 直接创建第二份后端，仅测试读错误通道，不改动已完成的缓存。
    ice::FFmpegDecoderInstance instance(to_utf8(valid),
                                        ice::ICEConfig::internal_format);
    ice::AudioBuffer           buffer;
    return instance.isValid() &&
           buffer.resize(ice::ICEConfig::internal_format, 1) &&
           instance.read(nullptr, 1) == 0 && instance.last_read_error() < 0 &&
           instance.read(buffer.raw_ptrs(), 1) == 0 &&
           instance.last_read_error() == 0;
}
}  // namespace

/// @brief 从构建产物目录验证资源探测、完整缓存准备与旧帧数接口。
/// @param argc 命令包含唯一一个目标工作目录参数。
/// @param argv 工作目录由 CMake 提供，测试不改动源码。
/// @return 按失败的用例阶段返回不同退出码，便于定位错误。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    // 所有轨道句柄随用例销毁，任务池活到最后一个诊断查询完成。
    ice::ThreadPool pool(2);
    // 使用真实探测作为首个门槛，再逐个隔离可控缓存阶段。
    if ( !check_file_inputs(pool, argv[1]) ) return 2;
    if ( !check_legacy(pool) ) return 3;
    if ( !check_failure(pool, Mode::OpenFailure, Stage::Open) ) return 4;
    if ( !check_failure(pool, Mode::InvalidFormat, Stage::Format) ) return 5;
    if ( !check_failure(pool, Mode::SeekFailure, Stage::Seek) ) return 6;
    if ( !check_failure(pool, Mode::ReadFailure, Stage::Read, READ_ERROR) )
        return 7;
    if ( !check_failure(pool, Mode::InvalidReadCount, Stage::FrameCount) )
        return 8;
    // 异常分支最后执行，其提取方式与普通工作任务返回值不同。
    if ( !check_failure(pool, Mode::TaskFailure, Stage::BackgroundTask) )
        return 9;
    return 0;
}
