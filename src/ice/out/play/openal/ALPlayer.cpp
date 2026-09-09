#include <ice/out/play/openal/ALPlayer.hpp>

#include <ice/core/IAudioNode.hpp>
#include <ice/manage/AudioBuffer.hpp>

#include <SDL3/SDL_thread.h>
#include <al.h>
#include <alc.h>
#include <alext.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

#if defined(ICE_OPENALSOFT_STATIC_LINKAGE)
// 日志钩子属于静态 OpenAL Soft 包的额外入口，并非通用 OpenAL ABI。
// 只在匹配的静态链接配置下声明，避免动态库缺少该符号而加载失败。
extern "C" {
using LPALSOFTLOGCALLBACK = void(ALC_APIENTRY*)(void* userptr, char level,
                                                const char* message,
                                                int         length) noexcept;
void ALC_APIENTRY alsoft_set_log_callback(LPALSOFTLOGCALLBACK callback,
                                          void*               userptr) noexcept;
}
#endif

#ifndef AL_FORMAT_MONO_FLOAT32
// 兼容缺少扩展枚举声明的头文件；真正使用前仍需运行时扩展检测。
#    define AL_FORMAT_MONO_FLOAT32 0x10010
#endif

#ifndef AL_FORMAT_STEREO_FLOAT32
#    define AL_FORMAT_STEREO_FLOAT32 0x10011
#endif

#ifndef ALC_ALL_DEVICES_SPECIFIER
// 仅补枚举值不代表驱动支持完整枚举，list_devices 先检查扩展能力。
#    define ALC_ALL_DEVICES_SPECIFIER 0x1013
#endif

namespace ice
{
namespace
{
/// @brief 固定循环队列容量；每个槽在播放完成后才能重新填充。
constexpr size_t kOpenALBufferCount = 8;
/// @brief 单块最小帧数，用于抵御供数线程调度抖动，并非交互等待窗口。
constexpr uint32_t kOpenALMinBufferFrames = 2048;
/// @brief 进程级诊断文本容量，包含最后一个字符串终止符。
constexpr size_t kOpenALSoftLogBufferSize = 4096;

/// @brief 串行化日志回调写入、清空与诊断快照读取。
std::mutex g_openALSoftLogMutex;
/// @brief 固定日志存储；容量耗尽后丢弃后续片段，不在回调中扩容。
std::array<char, kOpenALSoftLogBufferSize> g_openALSoftLogBuffer{};
/// @brief 已存文本字节数，不包含终止符；受日志互斥量保护。
size_t g_openALSoftLogLength{ 0 };
/// @brief 记录安装流程是否执行，不表示动态库具备日志钩子。
bool g_openALSoftLogInstalled{ false };

/// @brief OpenAL 空间化参数缓存。
/// 保存控制侧请求值，方向归一化推迟到应用阶段，避免改变配置原值。
/// 整组参数在后端互斥量下替换，供数线程只消费独立的输出模式开关。
struct ALSpatialState {
    /// @brief 声源方向 X 分量。
    float directionX{ 0.0f };

    /// @brief 声源方向 Y 分量。
    float directionY{ 0.0f };

    /// @brief 声源方向 Z 分量。
    float directionZ{ -1.0f };

    /// @brief 声源距离。
    float distance{ 1.0f };

    /// @brief 参考距离。
    float referenceDistance{ 1.0f };

    /// @brief 最大距离。
    float maxDistance{ 100.0f };

    /// @brief 距离衰减倍率。
    float rolloffFactor{ 1.0f };
};

/// @brief 在未提供非空配置时设置进程环境默认值。
/// @param name 环境变量名。
/// @param value 后端首次初始化前使用的默认值。
/// @warning 仅在控制侧初始化期间调用；不得与环境配置写入并发执行。
void setEnvironmentDefault(const char* name, const char* value)
{
    // 非空的外部设置优先，允许使用者选择驱动或诊断级别。
    const char* currentValue = std::getenv(name);
    if ( currentValue && *currentValue ) {
        return;
    }

#ifdef _WIN32
    _putenv_s(name, value);
#else
    // POSIX 分支禁止覆盖已存在项，因此已存在的空值也会保留。
    setenv(name, value, 0);
#endif
}

/// @brief 在实际打开设备前准备 OpenAL Soft 驱动顺序与日志级别。
/// 环境设置不验证驱动可用性，设备失败仍由 open 的回退路径处理。
void configureOpenALSoftEnvironment()
{
#ifdef _WIN32
    // 驱动顺序影响默认打开尝试；外部显式 ALSOFT_DRIVERS 会优先保留。
    setEnvironmentDefault("ALSOFT_DRIVERS", "winmm,dsound,wasapi");
#endif
    setEnvironmentDefault("ALSOFT_LOGLEVEL", "2");
}

/// @brief 在 Windows 提升供数线程优先级以减少调度欠载。
/// @warning 每次启动线程只调用一次；禁止加入逐块日志或阻塞操作。
/// 设置失败时保留系统原优先级，不能据此承诺硬实时调度。
void configureOpenALThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

/// @brief 将日志片段截断追加到固定缓冲区。
/// @param text 可读取至少 length 字节的文本。
/// @param length 原始片段字节数，不要求末尾带零。
/// @pre 调用方已持有 g_openALSoftLogMutex。
/// @return 无返回值；容量不足时截断，调用方不应依赖完整日志持久化。
void appendOpenALSoftLogText(const char* text, size_t length) noexcept
{
    if ( !text || length == 0 ||
         g_openALSoftLogLength >= kOpenALSoftLogBufferSize - 1 ) {
        return;
    }

    // 始终留一个终止符位置；截断只影响诊断完整度，不影响播放数据。
    const size_t available =
        kOpenALSoftLogBufferSize - 1 - g_openALSoftLogLength;
    const size_t copyLength = std::min(length, available);
    // 长度按字节计算，不假定日志编码；满缓冲时可能截断多字节字符。
    std::memcpy(
        g_openALSoftLogBuffer.data() + g_openALSoftLogLength, text, copyLength);
    g_openALSoftLogLength += copyLength;
    g_openALSoftLogBuffer[g_openALSoftLogLength] = '\0';
}

#if defined(ICE_OPENALSOFT_STATIC_LINKAGE)
/// @brief 捕获 OpenAL Soft 日志回调消息。
/// @param userptr 未使用的回调用户指针。
/// @param level OpenAL Soft 日志级别代码。
/// @param message 日志消息文本。
/// @param length 日志消息长度。
/// @warning 由库内部线程调用；现有互斥量可能等待，不能视为无锁回调。
/// 不保存 message 指针，回调返回后无需依赖库端消息缓冲的有效期。
void openALSoftLogCallback(void* userptr, char level, const char* message,
                           int length) noexcept
{
    (void)userptr;
    if ( !message || length <= 0 ) {
        return;
    }

    // 回调可能来自不同设备或线程，整条消息必须在同一锁区间追加。
    std::lock_guard lock(g_openALSoftLogMutex);
    if ( g_openALSoftLogLength != 0 ) {
        // 分隔符也消耗有限容量，满缓冲后消息不会触发滚动搬移或堆分配。
        appendOpenALSoftLogText("\n", 1);
    }

    appendOpenALSoftLogText("[", 1);
    appendOpenALSoftLogText(&level, 1);
    appendOpenALSoftLogText("] ", 2);
    appendOpenALSoftLogText(message, static_cast<size_t>(length));
}
#endif

/// @brief 安装 OpenAL Soft 日志回调；动态 OpenAL 不引用非稳定导出符号。
/// 安装状态是进程级的，播放器关闭时不撤销回调或销毁日志存储。
/// @pre 安装入口不在日志回调内重入，避免递归获取非递归互斥量。
void installOpenALSoftLogCallback()
{
    std::lock_guard lock(g_openALSoftLogMutex);
    if ( g_openALSoftLogInstalled ) {
        return;
    }

#if defined(ICE_OPENALSOFT_STATIC_LINKAGE)
    alsoft_set_log_callback(openALSoftLogCallback, nullptr);
#endif
    // 动态构建同样结束安装流程，但不会通过此入口捕获库内部日志。
    g_openALSoftLogInstalled = true;
}

/// @brief 开始一次设备打开尝试前清空进程级诊断文本。
/// 多实例并发打开会共享此存储，快照不能严格归属于单个设备。
void resetOpenALSoftLog()
{
    std::lock_guard lock(g_openALSoftLogMutex);
    // 只重置有效长度；旧尾部字节不再属于文本，无需清空整个固定数组。
    g_openALSoftLogLength    = 0;
    g_openALSoftLogBuffer[0] = '\0';
}

/// @brief 在持锁期间复制日志，避免返回会被回调改写的借用视图。
/// @return 当前已捕获的文本，超过固定容量的部分不可恢复。
/// @warning 仅供低频诊断；复制字符串可能分配内存。
std::string getOpenALSoftLogSnapshot()
{
    std::lock_guard lock(g_openALSoftLogMutex);
    return std::string(g_openALSoftLogBuffer.data(), g_openALSoftLogLength);
}

/// @brief 在设备失败信息后附加当前库日志快照。
/// @param errorMessage 已包含操作原因的诊断字符串。
void appendOpenALSoftLogDetail(std::string& errorMessage)
{
    const std::string log = getOpenALSoftLogSnapshot();
    // 无捕获信息时保留原错误，动态链接场景正常可能一直走此分支。
    if ( log.empty() ) {
        return;
    }

    errorMessage += " OpenAL Soft log: ";
    errorMessage += log;
}

/// @brief 将设备或上下文错误映射为稳定的诊断名称。
/// @param error ALC 错误码；与声源操作的 AL 错误域分开处理。
/// @return 静态字符串，未知枚举保留通用名称供数值诊断补充。
/// 不在此查询设备，避免名称转换意外清除待处理错误。
const char* getALCErrorName(ALCenum error)
{
    switch ( error ) {
    case ALC_NO_ERROR: return "ALC_NO_ERROR";
    case ALC_INVALID_DEVICE: return "ALC_INVALID_DEVICE";
    case ALC_INVALID_CONTEXT: return "ALC_INVALID_CONTEXT";
    case ALC_INVALID_ENUM: return "ALC_INVALID_ENUM";
    case ALC_INVALID_VALUE: return "ALC_INVALID_VALUE";
    case ALC_OUT_OF_MEMORY: return "ALC_OUT_OF_MEMORY";
    default: return "ALC_UNKNOWN_ERROR";
    }
}

/// @brief 将当前上下文的声源或缓冲错误映射为诊断名称。
/// @param error AL 错误码，不能用于解释 ALC 设备错误。
/// @return 静态字符串，不转移存储所有权。
/// 操作名与数值码由上层组合，此映射无需依赖当前设备存活。
const char* getALErrorName(ALenum error)
{
    switch ( error ) {
    case AL_NO_ERROR: return "AL_NO_ERROR";
    case AL_INVALID_NAME: return "AL_INVALID_NAME";
    case AL_INVALID_ENUM: return "AL_INVALID_ENUM";
    case AL_INVALID_VALUE: return "AL_INVALID_VALUE";
    case AL_INVALID_OPERATION: return "AL_INVALID_OPERATION";
    case AL_OUT_OF_MEMORY: return "AL_OUT_OF_MEMORY";
    default: return "AL_UNKNOWN_ERROR";
    }
}

/// @brief 消费设备错误并生成带操作名的失败信息。
/// @param operation 已经失败的操作名称。
/// @param device 尚未关闭、可供查询错误的设备。
/// @return 即使设备没有记录错误码，也返回操作失败说明。
std::string formatALCErrorMessage(const char* operation, ALCdevice* device)
{
    // 必须在释放设备前查询；无错误码不推翻调用方观察到的失败返回值。
    const ALCenum error = alcGetError(device);
    if ( error == ALC_NO_ERROR ) {
        return fmt::format("OpenAL {} failed.", operation);
    }

    return fmt::format("OpenAL {} failed: {} (0x{:x}).",
                       operation,
                       getALCErrorName(error),
                       static_cast<int>(error));
}

/// @brief 在后端锁内保存首个供数错误，操作名只引用静态字符串。
struct ALPendingError {
    /// @brief 失败阶段；空指针表示没有等待控制侧处理的诊断。
    const char* m_operation{ nullptr };
    /// @brief 首次失败的原始 AL 错误码；AL_NO_ERROR 表示入口失败但无 AL
    /// 错误码。
    ALenum m_code{ AL_NO_ERROR };
};

/// @brief 消费 AL 错误，控制侧生成文本，供数侧只保存固定大小状态。
/// @param operation 静态存储期的操作名称，延迟诊断不能借用临时字符串。
/// @param lastError 控制侧文本存储；实时调用必须传入空指针。
/// @param pending 受后端锁保护的延迟错误，空值表示无需延迟诊断。
/// @return 无错误时返回 true，失败时返回 false 以结束当前操作。
/// @warning 每块调用；供数失败分支不分配、不输出，也不额外加锁。
/// @pre 传入 pending 时须持有对应后端的 alMutex，控制侧消费时使用同一锁。
/// 延迟记录只保存静态名称和错误码，不延长设备、源或队列对象的生命周期。
bool checkALError(const char* operation, std::string* lastError = nullptr,
                  ALPendingError* pending = nullptr)
{
    // 查询会消费错误，必须在这里保存，不能到 stop 时重新读取 AL 状态。
    const ALenum error = alGetError();
    if ( error == AL_NO_ERROR ) return true;
    if ( pending && !pending->m_operation ) {
        // 保留根因，停止或退队中的次生错误不能覆盖首次失败。
        pending->m_operation = operation;
        pending->m_code      = error;
    }
    if ( lastError ) {
        // 只有控制路径立即格式化；日志记录由调用方读取诊断后决定。
        *lastError = fmt::format("OpenAL {} failed: {} (0x{:x}).",
                                 operation,
                                 getALErrorName(error),
                                 static_cast<int>(error));
    }
    return false;
}

/// @brief 将 OpenAL 多字符串设备列表转成 vector。
/// @pre 输入来自设备枚举接口，以空字符串结束整个列表。
/// @return 拥有各设备名副本的列表，不保留库返回文本的指针。
/// 设备名按库返回的字节序列复制，不做本地化替换，保持可用于重新打开。
std::vector<ALAudioDeviceInfo> parseDeviceList(const ALCchar* deviceList)
{
    std::vector<ALAudioDeviceInfo> devices;
    // 接口无返回数据时提供空列表，让上层统一执行默认名称降级。
    if ( !deviceList ) {
        return devices;
    }

    // 各名称之间有单个零，最后的额外零表示列表结束，不能按单字符串解析。
    const ALCchar* current = deviceList;
    while ( *current != '\0' ) {
        devices.push_back({ std::string(current) });
        current += std::strlen(current) + 1;
    }
    return devices;
}

/// @brief 选择 OpenAL buffer 格式。
/// @pre channels 已由提交路径归约为 1 或 2，不直接接收多声道格式。
/// 浮点扩展按打开的上下文检测；无扩展时走有符号 16 位兼容路径。
ALenum selectALFormat(uint16_t channels, bool useFloatFormat)
{
    // 输出声道已经限定后，格式枚举才能与上传的样本宽度一一对应。
    if ( useFloatFormat ) {
        return channels == 1 ? AL_FORMAT_MONO_FLOAT32
                             : AL_FORMAT_STEREO_FLOAT32;
    }
    return channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
}

/// @brief 将非有限样本视为静音，有限浮点样本保留动态范围。
/// @warning 每样本调用，不分配、不记录日志，避免异常数据污染整个设备队列。
float sanitizeSample(float sample)
{
    // NaN 无法可靠裁剪，正负无穷也不应转换成持续满幅输出。
    return std::isfinite(sample) ? sample : 0.0f;
}

/// @brief 将浮点采样转换为 int16。
/// 非有限输入先静音，保证 lrint 仅接收有界有限值。
/// 对称缩放使用正端最大值，负满幅对应 -32767 而非 -32768。
/// @return 单个有符号采样；输出字节布局由上传处的整型数组提供。
std::int16_t floatToInt16(float sample)
{
    // 裁剪避免超过有符号 16 位范围；lrint 沿用当前浮点舍入模式。
    const float clamped = std::clamp(sanitizeSample(sample), -1.0f, 1.0f);
    return static_cast<std::int16_t>(std::lrint(
        clamped *
        static_cast<float>(std::numeric_limits<std::int16_t>::max())));
}

/// @brief 归一化方向向量，零向量会回退到 OpenAL 默认前方。
/// 长度平方阈值避免极短向量被倒数放大；距离由独立参数决定。
/// @pre 输入分量有限，调用方负责配置数据有效性。
/// @return 按值返回三分量数组，不借用缓存或引入共享所有权。
std::array<float, 3> normalizeDirection(float x, float y, float z)
{
    // 有限 float 的平方和可能超出 float；先提升再相乘，double 可容纳整个范围。
    const double lengthSq = static_cast<double>(x) * x +
                            static_cast<double>(y) * y +
                            static_cast<double>(z) * z;
    if ( lengthSq <= 0.000001f ) {
        // 默认朝向与监听者前方一致，零方向不会导致除零或任意偏转。
        return { 0.0f, 0.0f, -1.0f };
    }

    // 归一化结果有界；乘法仍在 double 中完成，避免巨大方向的倒数提前丢失精度。
    const double invLength = 1.0 / std::sqrt(lengthSq);
    return { static_cast<float>(x * invLength),
             static_cast<float>(y * invLength),
             static_cast<float>(z * invLength) };
}
}  // namespace

/// @brief OpenAL 后端运行时句柄。
/// 由 ALPlayer 独占拥有；句柄需在供数线程退出后按依赖次序释放。
/// 该结构仅保存状态，实际释放由 close 负责，不在成员析构中隐式执行。
class ALBackend
{
public:
    /// @brief 供数错误暂存，读写均在 alMutex 内，控制侧停止后转成文本。
    /// 不用于发布其他资源，不增加供数路径原子操作或动态存储。
    ALPendingError m_pendingError;

    /// @brief OpenAL 设备句柄。
    /// 在上下文创建失败的早期分支直接释放，成功后交由 close 回收。
    ALCdevice* device{ nullptr };

    /// @brief OpenAL 上下文句柄。
    /// AL 对象在该上下文中创建，销毁时必须先释放源与缓冲。
    ALCcontext* context{ nullptr };

    /// @brief 设备打开时解析的线程绑定入口，不修改进程级当前上下文。
    PFNALCSETTHREADCONTEXTPROC setThreadContext{ nullptr };
    /// @brief 查询调用线程原有绑定，供控制操作结束时恢复。
    PFNALCGETTHREADCONTEXTPROC getThreadContext{ nullptr };

    /// @brief OpenAL 声源句柄。
    /// 一个源承载整个混音图，空间化作用于最终输出而非各图节点。
    ALuint source{ 0 };

    /// @brief OpenAL 队列 buffer 句柄。
    /// 句柄数组生命周期跨越多次 start/stop，不随每次退队重新创建。
    std::array<ALuint, kOpenALBufferCount> buffers{};

    /// @brief 构造成功时确定的固定块长，不随上游对缓冲元数据的改写变化。
    /// 为零表示初始缓冲未准备成功，此时不能启动供数。
    size_t m_preparedFrames{ 0U };

    /// @brief 控制侧启动前准备的浮点交错存储，运行期间由供数线程独占。
    /// 停止后保留容量供重启复用，销毁后端前必须先回收供数线程。
    std::vector<float> floatScratch;
    /// @brief 与浮点路径同容量的整型备用存储，避免设备格式切换时临时分配。
    /// 控制侧只在旧线程退出后调整，两个存储不随单块转换创建或释放。
    std::vector<std::int16_t> int16Scratch;

    /// @brief buffer 句柄是否已创建。
    bool buffersReady{ false };

    /// @brief 是否支持 float32 buffer 扩展。
    /// open 时写入，供数期间只读；换设备必须先结束旧线程。
    bool floatFormatSupported{ false };

    /// @brief 当前空队列开始填充时确定的空间输出布局。
    /// 仅由供数线程读写，控制请求在清空队列后才更新，避免单批 mono/stereo
    /// 混用。
    bool queuedSpatialOutput{ false };

    /// @brief 空间化参数缓存。
    ALSpatialState spatialState;

    /// @brief 串行化供数线程与控制侧的源操作和空间参数缓存访问。
    /// @warning 供数循环逐块持锁；控制侧不得在锁内等待播放线程退出。
    /// 锁不覆盖完整 open 生命周期，外部仍须串行执行设备管理操作。
    std::mutex alMutex;
};

/// @brief 在当前线程临时绑定一个后端，作用域结束恢复此前线程绑定。
/// 不改变进程级绑定，多个播放器的供数线程可独立执行 AL 操作。
class ScopedALContext
{
public:
    /// @brief 保存线程原绑定后选择本次操作的上下文。
    /// @param backend 已解析线程扩展入口且仍存活的后端。
    /// @param closing 即将销毁目标时，不恢复指向该目标自身的旧绑定。
    /// @warning 控制操作或供数线程入口执行；不得在每个样本或每个槽反复构造。
    explicit ScopedALContext(ALBackend& backend, bool closing = false)
        : m_setContext(backend.setThreadContext)
    {
        // 只有扩展已就绪才读取旧绑定，失败对象不在析构时调用空入口。
        if ( !m_setContext || !backend.getThreadContext ) return;
        m_previous = backend.getThreadContext();
        // close 可能在 open 的错误回滚内嵌套调用，不能重新绑定即将销毁的目标。
        if ( closing && m_previous == backend.context ) m_previous = nullptr;
        m_bound = m_setContext(backend.context) == ALC_TRUE;
    }

    /// @brief 在目标资源销毁前恢复本线程原上下文，不触碰其他线程绑定。
    /// @warning 调用者保证原上下文在此作用域内存活，恢复不等待其他播放线程。
    ~ScopedALContext()
    {
        if ( m_bound ) m_setContext(m_previous);
    }

    /// @brief 禁止复制绑定守卫，避免重复恢复同一线程状态。
    ScopedALContext(const ScopedALContext&) = delete;
    /// @brief 禁止赋值，绑定与词法作用域保持一一对应。
    ScopedALContext& operator=(const ScopedALContext&) = delete;

    /// @brief 返回目标是否已成功绑定；失败时不得执行 AL 对象操作。
    explicit operator bool() const { return m_bound; }

private:
    /// @brief 借用扩展函数地址，其寿命覆盖所有后端实例。
    PFNALCSETTHREADCONTEXTPROC m_setContext{ nullptr };
    /// @brief 调用线程原有绑定，不拥有该上下文。
    ALCcontext* m_previous{ nullptr };
    /// @brief 只有成功绑定后析构才恢复，失败不改写原线程状态。
    bool m_bound{ false };
};

/// @brief 全局初始化标志只记录控制流程，不代表任意设备已打开。
std::atomic<bool> ALPlayer::al_inited{ false };

/// @brief 创建句柄容器并准备固定格式的图输出块，尚不访问设备。
/// @param format 播放期间保持稳定的平面音频格式。
/// @pre 采样率和声道数来自有效的引擎配置；构造不协商设备能力。
ALPlayer::ALPlayer(const AudioDataFormat& format)
    : IReceiver(format)
    , m_playFormat(format)
    , m_backend(std::make_unique<ALBackend>())
{
    // 分配放在构造阶段；块长由配置与下限共同决定，队列时长还乘以槽数。
    const size_t frames = std::max<uint32_t>(ICEConfig::default_buffer_size,
                                             kOpenALMinBufferFrames);
    // AL 上传使用有符号字节长度，按最坏的双声道 float 输出提前限制帧数。
    // 必须先拒绝再分配，避免异常配置触发巨额 PCM 申请或截断为负上传长度。
    if ( format.channels == 0 || format.samplerate == 0 ||
         format.samplerate >
             static_cast<uint32_t>(std::numeric_limits<ALsizei>::max()) ||
         format.samplerate >
             static_cast<uint32_t>(std::numeric_limits<ALCint>::max()) ||
         frames > static_cast<size_t>(std::numeric_limits<ALsizei>::max()) /
                      (2U * sizeof(float)) )
        return;
    // 同时覆盖上下文频率与上传频率的转换，零值不借用后端默认频率掩盖无效格式。
    if ( m_buffer.resize(m_playFormat, frames) )
        m_backend->m_preparedFrames = m_buffer.frame_capacity();
}

/// @brief 在成员销毁前结束线程，防止后台继续访问 this 和缓冲区。
/// @warning 析构包含 join，仅允许在低频控制侧执行。
ALPlayer::~ALPlayer()
{
    close();
}

/// @brief 准备全局默认值与日志钩子，不在此探测或打开实体设备。
/// @return 当前实现完成设置后返回 true，不能作为播放可用性判断。
/// @pre 调用方在创建播放实例前串行执行初始化。
bool ALPlayer::init_backend()
{
    configureOpenALSoftEnvironment();
    installOpenALSoftLogCallback();
    al_inited.store(true);
    return true;
}

/// @brief 清除初始化标记，实例资源仍须各自 close。
/// 日志安装状态和进程环境保留，重新初始化不会重复安装钩子。
void ALPlayer::quit_backend()
{
    al_inited.store(false);
}

/// @brief 根据枚举扩展能力收集设备名称，必要时降级为默认设备名。
/// @warning 控制侧低频调用，涉及设备接口与字符串分配。
/// @return 库提供的名称顺序；不排序、不去重，也不替调用方选择偏好设备。
std::vector<ALAudioDeviceInfo> ALPlayer::list_devices()
{
    const bool enumerateAll =
        alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT") == AL_TRUE;
    const bool enumerateDefault =
        alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT") == AL_TRUE;

    std::vector<ALAudioDeviceInfo> devices;
    // 优先使用更完整的输出设备枚举；旧扩展仅作为兼容路径。
    if ( enumerateAll ) {
        devices =
            parseDeviceList(alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER));
    } else if ( enumerateDefault ) {
        devices = parseDeviceList(alcGetString(nullptr, ALC_DEVICE_SPECIFIER));
    }

    // 无枚举支持或列表为空时，默认名称仍可供显式打开尝试。
    // 返回名称不代表设备一定能打开，调用方必须检查 open 的结果。
    if ( devices.empty() ) {
        const ALCchar* defaultDevice =
            alcGetString(nullptr, ALC_DEFAULT_DEVICE_SPECIFIER);
        if ( defaultDevice ) {
            devices.push_back({ std::string(defaultDevice) });
        }
    }

    return devices;
}

/// @brief 先尝试系统默认输出，再依次尝试枚举得到的具名设备。
/// @return 首次成功即返回 true；全部失败时保留默认及各回退诊断。
/// @warning 设备探测与初始化可能阻塞，只允许在低频控制流程执行。
/// @pre 上一次播放生命周期已由同一控制流程管理，无并发设备切换。
bool ALPlayer::open()
{
    if ( open({}) ) {
        // 空参数重载只尝试默认设备；此无参重载负责额外枚举回退。
        return true;
    }

    // 每次具名尝试都会覆盖错误，因此先保存默认设备失败原因。
    const std::string defaultError = m_lastError;
    const auto        devices      = list_devices();
    // 枚举失败不替换最初的打开错误，空列表会直接回到默认失败结果。
    std::string fallbackErrors;

    for ( const auto& device : devices ) {
        // 空名称会再次请求默认设备，不能当成新的回退目标。
        if ( device.name.empty() ) {
            continue;
        }

        if ( open(device.name) ) {
            // 成功重载已经保存实际设备名，无需再次打开或启动线程验证。
            return true;
        }

        if ( !fallbackErrors.empty() ) {
            fallbackErrors += " ";
        }
        fallbackErrors += fmt::format("[{}: {}]", device.name, m_lastError);
    }

    // 汇总全部失败以便定位驱动问题，不仅报告最后一个候选设备。
    m_lastError = defaultError;
    if ( !fallbackErrors.empty() ) {
        m_lastError += " Fallback devices also failed: ";
        m_lastError += fallbackErrors;
    }
    return false;
}

/// @brief 借用最近的控制侧诊断；调用方不得与设备管理并发访问。
/// 供数错误由 stop 回收线程后转入文本，运行期间查询不强制等待或格式化。
/// @return 引用有效期受播放器生命周期约束，跨操作保留内容需自行复制。
/// 查询不会清除错误，允许上层多次读取同一失败原因。
const std::string& ALPlayer::getLastError() const
{
    return m_lastError;
}

/// @brief 借用当前成功打开的设备名；关闭或重新打开会改变该内容。
/// @return 播放器拥有的字符串引用，不转移设备或名称所有权。
/// 查询不重新枚举设备，因此热插拔后的可用性仍需重新打开验证。
const std::string& ALPlayer::getOpenedDeviceName() const
{
    return m_openedDeviceName;
}

/// @brief 按设备、上下文、声源、队列缓冲的顺序建立播放资源。
/// @param deviceName 空值选默认设备，非空值只尝试指定名称。
/// @return 资源全部准备就绪时为 true，此时还未启动供数线程。
/// @warning 可能先 stop/join 旧线程并调用阻塞设备接口，不得用于热路径。
/// 调用方须串行管理生命周期；内部局部锁不保护并发 open/close。
/// 失败信息保存在
/// getLastError，调用方决定日志目的地，后端不直接输出到标准输出。
/// 默认设备回退可能多次调用本入口，中间失败不应形成重复且误导性的终端消息。
bool ALPlayer::open(std::string_view deviceName)
{
    // C 接口会在首个空字符截断名称；无效请求必须在关闭旧设备之前拒绝。
    // 空 string_view 仍表示默认设备，含空字符的非空名称不能等同于默认请求。
    if ( deviceName.find('\0') != std::string_view::npos ) {
        m_lastError = "OpenAL device name contains an embedded null character.";
        return false;
    }

    // 输入可能借用本实例的设备名称或诊断文本，清理成员前必须保存独立副本。
    // 同时提供 C 接口所需的末尾零字符，副本存活至设备打开及诊断结束。
    const std::string deviceNameStorage(deviceName);

    // 清日志只影响诊断，不重置全局初始化标记。
    m_openedDeviceName.clear();
    resetOpenALSoftLog();
    // 旧设备 close 仍可能产生库日志，因此捕获范围是本次切换而非纯打开调用。

    if ( m_backend->device || m_backend->context ) {
        // 重新打开也是完整的生命周期切换，不能复用旧设备中的 AL 句柄。
        close();
    }

    // 旧后端回收可能生成错误，必须在回收结束后才开始新打开操作的诊断区间。
    // 独立 close 仍保留关闭错误；重开成功不能携带上次退队失败的陈旧文本。
    m_lastError.clear();

    // 准备失败的实例没有合法块契约，不接触设备，也不依赖驱动偶然接受无效参数。
    if ( m_backend->m_preparedFrames == 0 ) {
        m_lastError = "OpenAL playback format or block size is invalid.";
        return false;
    }

    // 这里只从已拥有的副本借用地址，旧设备清理不再影响输入名称。
    const char* openName =
        deviceNameStorage.empty() ? nullptr : deviceNameStorage.c_str();

    m_backend->device = alcOpenDevice(openName);
    if ( !m_backend->device ) {
        // 没有可查询的设备句柄，用请求名称与库日志保留初始化阶段的原因。
        m_lastError = fmt::format(
            "OpenAL device open failed: {}.",
            deviceNameStorage.empty() ? "default device" : deviceNameStorage);
        appendOpenALSoftLogDetail(m_lastError);
        return false;
    }

    // 非空句柄仍可能伴随设备错误；此阶段尚无上下文，直接关闭设备。
    const ALCenum openError = alcGetError(m_backend->device);
    if ( openError != ALC_NO_ERROR ) {
        m_lastError = fmt::format(
            "OpenAL device open reported error for {}: {} "
            "(0x{:x}).",
            deviceNameStorage.empty() ? "default device" : deviceNameStorage,
            getALCErrorName(openError),
            static_cast<int>(openError));
        appendOpenALSoftLogDetail(m_lastError);
        alcCloseDevice(m_backend->device);
        m_backend->device = nullptr;
        return false;
    }

    // 本后端需要线程局部绑定，不能回退到会影响其他播放器的进程级状态。
    if ( alcIsExtensionPresent(m_backend->device,
                               "ALC_EXT_thread_local_context") != ALC_TRUE ) {
        m_lastError = "OpenAL thread-local context extension is required.";
        alcCloseDevice(m_backend->device);
        m_backend->device = nullptr;
        return false;
    }
    m_backend->setThreadContext = reinterpret_cast<PFNALCSETTHREADCONTEXTPROC>(
        alcGetProcAddress(m_backend->device, "alcSetThreadContext"));
    m_backend->getThreadContext = reinterpret_cast<PFNALCGETTHREADCONTEXTPROC>(
        alcGetProcAddress(m_backend->device, "alcGetThreadContext"));
    // 扩展声明与入口地址都必须有效，拒绝不完整驱动实现。
    if ( !m_backend->setThreadContext || !m_backend->getThreadContext ) {
        m_lastError =
            "OpenAL thread-local context entry points are unavailable.";
        alcCloseDevice(m_backend->device);
        m_backend->device = nullptr;
        return false;
    }

    // 属性表以零结束；请求的设备刷新率与本类供数轮询周期不是同一概念。
    // 上传数据的采样率仍使用 m_playFormat，不从刷新率推导块大小。
    const std::array<ALCint, 5> contextAttributes{ ALC_FREQUENCY,
                                                   static_cast<ALCint>(
                                                       m_playFormat.samplerate),
                                                   ALC_REFRESH,
                                                   60,
                                                   0 };
    m_backend->context =
        alcCreateContext(m_backend->device, contextAttributes.data());
    // 上下文创建是独立的失败阶段，不能只检查 alcOpenDevice 的结果。
    if ( !m_backend->context ) {
        // close 依赖存在的上下文，创建失败必须在本分支回收设备。
        m_lastError =
            formatALCErrorMessage("context creation", m_backend->device);
        appendOpenALSoftLogDetail(m_lastError);
        alcCloseDevice(m_backend->device);
        m_backend->device = nullptr;
        return false;
    }

    ScopedALContext currentContext(*m_backend);
    if ( !currentContext ) {
        // 尚未创建 AL 对象，先销毁上下文再关闭设备即可完成本阶段回滚。
        m_lastError =
            formatALCErrorMessage("make context current", m_backend->device);
        appendOpenALSoftLogDetail(m_lastError);
        alcDestroyContext(m_backend->context);
        alcCloseDevice(m_backend->device);
        m_backend->context = nullptr;
        m_backend->device  = nullptr;
        return false;
    }

    // 清除旧 AL 错误，后续逐阶段检查才能把失败归到正确资源操作。
    alGetError();
    alGenSources(1, &m_backend->source);
    if ( !checkALError("alGenSources", &m_lastError) ) {
        // 上下文已存在，统一 close 可以按当前有效句柄清理已完成阶段。
        appendOpenALSoftLogDetail(m_lastError);
        close();
        return false;
    }

    alGenBuffers(static_cast<ALsizei>(m_backend->buffers.size()),
                 m_backend->buffers.data());
    if ( !checkALError("alGenBuffers", &m_lastError) ) {
        // 此时 buffersReady 尚未置位，close 不进入整组缓冲显式删除分支。
        // 失败清理由源和上下文回收路径完成，数组不作为部分创建进度使用。
        appendOpenALSoftLogDetail(m_lastError);
        close();
        return false;
    }
    // 仅在整组创建检查通过后标记可释放，后续 start 也用它判定就绪。
    m_backend->buffersReady = true;

    // 以当前上下文能力为准，不能从编译期定义存在推断设备支持。
    m_backend->floatFormatSupported =
        alIsExtensionPresent("AL_EXT_FLOAT32") == AL_TRUE;

    // 监听者固定在原点、朝向 -Z；业务空间参数以相对此坐标系的声源描述。
    const ALfloat orientation[] = { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    alListenerfv(AL_ORIENTATION, orientation);
    // 不在此上传静音或预拉取图数据，实际队列由 start 后的线程准备。
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    // 先检查监听者初始化，避免空间状态内部消费错误后把 open 误判为成功。
    if ( !checkALError("open", &m_lastError) || !applySpatialState() ) {
        // 监听者或空间参数设置失败也使整个打开失败，避免返回半配置设备。
        appendOpenALSoftLogDetail(m_lastError);
        close();
        return false;
    }

    // 保存设备实际名称而非仅保存请求名，默认设备回退也能显示最终选择。
    const ALCchar* openedDeviceName =
        alcGetString(m_backend->device, ALC_DEVICE_SPECIFIER);
    m_openedDeviceName =
        openedDeviceName ? openedDeviceName : deviceNameStorage;
    return true;
}

/// @brief 停止供数后按声源、缓冲、上下文、设备的依赖次序释放。
/// @warning 低频控制侧操作，stop 含 join；不得从供数线程自身调用。
/// 保留 m_lastError，供打开失败后的调用方读取回滚前的原因。
/// @pre 实例的其他生命周期和空间设置调用已停止进入。
void ALPlayer::close()
{
    // join 必须先于 alMutex：线程退出时还要持该锁停源和退队。
    stop();

    std::unique_lock lock(m_backend->alMutex);
    if ( !m_backend->context ) {
        return;
    }

    {
        // 以下操作仍需要有效上下文；不得在停止线程前先销毁 context。
        ScopedALContext currentContext(*m_backend, true);
        if ( currentContext ) {
            if ( m_backend->source != 0 ) {
                alSourceStop(m_backend->source);
                clearQueuedBuffers();
                // 先退队再删除源，后续删除缓冲时不再有来自本源的队列引用。
                alDeleteSources(1, &m_backend->source);
                m_backend->source = 0;
            }

            // 声源已解除缓冲引用，才能删除缓冲对象；数组随后恢复未创建状态。
            if ( m_backend->buffersReady ) {
                alDeleteBuffers(static_cast<ALsizei>(m_backend->buffers.size()),
                                m_backend->buffers.data());
                m_backend->buffersReady = false;
                m_backend->buffers.fill(0);
            }
        } else if ( !m_backend->m_pendingError.m_operation ) {
            // 绑定失败不能继续调用依赖当前上下文的源与缓冲操作，保留明确阶段名。
            // 后续仍回收上下文和设备，不让诊断失败阻断既有的关闭生命周期。
            m_backend->m_pendingError = { "bind closing context", AL_NO_ERROR };
        }
        // 守卫先恢复线程绑定，随后才能销毁上下文。
    }
    alcDestroyContext(m_backend->context);
    alcCloseDevice(m_backend->device);
    m_backend->context      = nullptr;
    m_backend->device       = nullptr;
    m_backend->source       = 0;
    m_backend->buffersReady = false;
    m_backend->buffers.fill(0);
    m_openedDeviceName.clear();
    // 关闭期间的退队也可能记录错误，释放后端锁后再收集，避免递归锁与锁内格式化。
    lock.unlock();
    collectPendingError();
    // 空设备名表示已关闭；诊断保留使失败回滚后仍可向上层解释原因。
}

/// @brief 将 SDL 回调约定桥接到播放器的非静态供数循环。
struct ALPlayer::WorkerEntry {
    /// @brief 在线程生命周期内借用播放器，不转移对象所有权。
    /// @warning 每次 start 成功后执行一次，循环返回后控制侧仍须回收句柄。
    static int SDLCALL run(void* instance)
    {
        // start 已验证后端资源，close 先等待线程结束再释放借用对象。
        static_cast<ALPlayer*>(instance)->audio_thread_loop();
        return 0;
    }
};

/// @brief 在资源就绪且尚未运行时创建供数线程。
/// @return true 只代表线程已发起，不保证首批排队或实体播放成功。
/// @pre 控制侧串行调用；原子运行标记不能替代线程对象的生命周期锁。
/// @warning 故障后重启可能 join 旧供数线程，仅允许在低频控制侧调用。
/// 未绑定图也允许启动，供数路径会提交静音，便于先建设备再启用图节点。
bool ALPlayer::start()
{
    if ( m_running.load(std::memory_order_relaxed) ) {
        // 不允许覆盖尚需 join 的线程对象；重复启动以返回值告知控制侧。
        m_lastError = "OpenAL playback thread is already running.";
        return false;
    }

    if ( !m_backend->context || m_backend->source == 0 ||
         !m_backend->buffersReady ) {
        // 只有完整 open 后才允许线程访问句柄，不能由线程懒创建设备。
        m_lastError =
            "OpenAL playback backend is not fully initialized before start.";
        return false;
    }

    // 故障退出可能只发布停止状态，控制侧须先回收旧线程，才能覆盖线程对象。
    if ( m_audioThread ) {
        SDL_WaitThread(m_audioThread, nullptr);
        m_audioThread = nullptr;
    }
    // 旧图可能破坏缓冲并触发停止；重启不能把残留短块或失效平面当作新契约。
    // 固定块长来自初次准备，不能根据已被图改写的元数据重新推导。
    const size_t frames = m_backend->m_preparedFrames;
    if ( frames == 0 ) {
        m_lastError = "OpenAL audio buffer was not prepared.";
        return false;
    }
    bool validBuffer =
        m_buffer.afmt == m_playFormat && m_buffer.num_frames() == frames &&
        m_buffer.frame_capacity() == frames && m_buffer.raw_ptrs();
    for ( uint16_t channel = 0; validBuffer && channel < m_playFormat.channels;
          ++channel )
        validBuffer = m_buffer.raw_ptrs()[channel] != nullptr;
    if ( !validBuffer ) {
        // 重新创建存储和地址表，不能依赖同尺寸 resize 的快速返回修复损坏表项。
        // 仅在控制侧故障重启时分配，正常启停继续复用原块及交错容量。
        AudioBuffer prepared;
        if ( !prepared.resize(m_playFormat, frames) ) {
            m_lastError = "OpenAL audio buffer restoration failed.";
            return false;
        }
        m_buffer = std::move(prepared);
    }
    // 最坏输出为双声道，多声道输入也只会下混为单声道，无需按输入声道扩容。
    // 两种格式在发布运行状态前准备，分配失败不会发生在供数线程中。
    if ( frames > m_backend->floatScratch.max_size() / 2U ||
         frames > m_backend->int16Scratch.max_size() / 2U ) {
        m_lastError = "OpenAL scratch capacity exceeds supported size.";
        return false;
    }
    m_backend->floatScratch.reserve(frames * 2U);
    m_backend->int16Scratch.reserve(frames * 2U);
    // 重启丢弃上次暂停和队列重建请求，首批队列会按当前空间开关填充。
    // 旧线程已回收，控制侧串行启动，新的运行独立记录首个失败。
    {
        std::scoped_lock lock(m_backend->alMutex);
        m_backend->m_pendingError = {};
    }
    m_lastError.clear();
    m_running.store(true, std::memory_order_relaxed);
    m_paused.store(false);
    m_rebuildQueuedBuffers.store(false);
    m_audioThread = SDL_CreateThread(WorkerEntry::run, "ICE OpenAL", this);
    if ( !m_audioThread ) {
        // 没有后台线程会消费运行请求，必须回滚，允许控制侧重试或关闭设备。
        m_running.store(false, std::memory_order_relaxed);
        m_lastError = "OpenAL playback thread creation failed.";
        return false;
    }
    // 线程借用 this，调用方必须保持实例存在直到 stop 完成 join。
    return true;
}

/// @brief 请求供数线程退出，并等待其完成源停播及队列清理。
/// @warning 低频控制侧 join 可能等待当前图处理和驱动调用；不能自行 join。
/// stop 保留设备资源，后续可再次 start；释放资源需调用 close。
/// @pre 不与另一次 start/stop 并发执行，不从音频图 process 中反向调用。
void ALPlayer::stop()
{
    // 故障线程可先清运行标志，仍须检查句柄并回收，不能据标志提前返回。
    // 退出请求由循环和回填内层读取；同时取消暂停，便于下次启动恢复初态。
    m_running.store(false, std::memory_order_relaxed);
    m_paused.store(false);

    if ( m_audioThread ) {
        // 返回后后台不再触碰缓冲与句柄，close 才有安全回收的时机。
        SDL_WaitThread(m_audioThread, nullptr);
        // SDL 已释放线程对象，重复 stop 不能再次使用旧句柄。
        m_audioThread = nullptr;
    }
    collectPendingError();
}

/// @brief 将待处理错误转为控制侧文本，保留更早的明确失败原因。
/// @pre 供数线程已回收，且调用方未持有 alMutex；只在串行生命周期路径使用。
/// @warning stop 和 close 低频调用，格式化可能分配，不能从供数线程调用。
void ALPlayer::collectPendingError()
{
    // 记录只包含静态操作名和错误码，即使设备已关闭也无需再访问后端 API。
    ALPendingError pending;
    {
        std::scoped_lock lock(m_backend->alMutex);
        pending                   = m_backend->m_pendingError;
        m_backend->m_pendingError = {};
    }
    // 保留先前控制命令的明确失败，不让随后的供数退出覆盖调用方正在诊断的原因。
    // 暂存已清空，重复 stop 不重新格式化；文本保留至下一次启动的清理阶段。
    if ( pending.m_operation && m_lastError.empty() ) {
        // 格式化只发生在控制侧，不占用供数线程的错误退出时延。
        // 绑定失败只提供失败状态，不应把 AL_NO_ERROR 展示为实际驱动错误。
        if ( pending.m_code == AL_NO_ERROR ) {
            m_lastError = fmt::format("OpenAL {} failed.", pending.m_operation);
        } else {
            m_lastError = fmt::format("OpenAL {} failed: {} (0x{:x}).",
                                      pending.m_operation,
                                      getALErrorName(pending.m_code),
                                      static_cast<int>(pending.m_code));
        }
    }
}

/// @brief 查询运行请求标记，不代表 OpenAL 当前源状态或可听输出。
/// @warning 可高频读取控制侧或供数失败写入的 relaxed 原子值。
/// 跨线程查询不能使用普通 bool；false 只表示退出请求，不代表等待回收已完成。
bool ALPlayer::is_running() const
{
    return m_running.load(std::memory_order_relaxed);
}

/// @brief 发布暂停请求并暂停设备源，保留已排队的音频块。
/// @warning 控制侧会等待 alMutex；并非与整个供数迭代原子切换。
/// 暂停不撤回已提交到设备的数据，也不对图节点执行定位或状态复位。
void ALPlayer::pause()
{
    if ( !m_running.load(std::memory_order_relaxed) || m_paused.load() ) {
        // 暂停请求仅对已运行且未暂停的实例有意义，不改变下一次启动策略。
        return;
    }

    // 先让下一轮供数停止推进，再串行发出源暂停；已进入的迭代仍可能继续。
    m_paused.store(true);
    std::scoped_lock lock(m_backend->alMutex);
    if ( m_backend->context && m_backend->source != 0 ) {
        ScopedALContext currentContext(*m_backend);
        if ( !currentContext ) {
            m_lastError = "OpenAL thread context binding failed.";
            // 控制命令未能提交，结束本次供数，避免请求状态与设备状态长期分离。
            m_running.store(false, std::memory_order_relaxed);
            return;
        }
        alSourcePause(m_backend->source);
        // 必须在本线程消费错误，不能留给供数线程或后续不相关命令诊断。
        if ( !checkALError("pause playback", &m_lastError) ) {
            m_running.store(false, std::memory_order_relaxed);
        }
    }
}

/// @brief 允许供数继续并请求设备播放原有队列。
/// @warning 控制侧会等待 alMutex；生命周期必须保持设备仍然打开。
/// 若暂停期间改变空间模式，恢复后的供数循环还需消费队列重建请求。
void ALPlayer::resume()
{
    if ( !m_running.load(std::memory_order_relaxed) || !m_paused.load() ) {
        // resume 不承担启动线程的职责，也不重新生成已经排队的数据。
        return;
    }

    std::scoped_lock lock(m_backend->alMutex);
    // 等待锁期间供数线程可能已经失败，不应重新播放一个已结束的运行。
    if ( !m_running.load(std::memory_order_relaxed) ) return;
    // 保留设备队列继续播放，供数循环随后回收已处理槽或执行积压重建。
    if ( m_backend->context && m_backend->source != 0 ) {
        ScopedALContext currentContext(*m_backend);
        if ( !currentContext ) {
            m_lastError = "OpenAL thread context binding failed.";
            // 控制命令未能提交，结束本次供数，避免请求状态与设备状态长期分离。
            m_running.store(false, std::memory_order_relaxed);
            return;
        }
        alSourcePlay(m_backend->source);
        if ( !checkALError("resume playback", &m_lastError) ) {
            // 保留暂停请求并停止供数，错误后不允许后台自动重启声源。
            m_running.store(false, std::memory_order_relaxed);
            return;
        }
        // 成功提交后才允许供数继续；与自动播放检查共用 AL 锁。
        m_paused.store(false);
    }
}

/// @brief 更新空间参数，并在声道提交模式变化时请求供数侧重建队列。
/// 多次切换只保留最新开关；重建标志不是待执行事件计数。
/// @warning 控制侧写原子请求，供数侧读取并消费；实际队列切换异步发生。
/// @param enabled true 选择平均下混，false 仅对双声道输入保留立体声。
void ALPlayer::set_spatial_output_enabled(bool enabled)
{
    const bool previous = m_spatialOutputEnabled.exchange(enabled);
    // 即使开关未变也应用缓存；只有提交布局变化需要丢弃旧队列。
    applySpatialState();
    if ( previous != enabled ) {
        // 队列中可能仍是旧 mono/stereo 格式，不能仅修改声源空间属性。
        m_rebuildQueuedBuffers.store(true);
    }
}

/// @brief 查询请求的空间模式，不保证旧格式队列已被重建。
/// @warning 高频查询只读取控制侧发布的原子值，不查询驱动状态。
bool ALPlayer::is_spatial_output_enabled() const
{
    return m_spatialOutputEnabled.load();
}

/// @brief 整组替换空间缓存，归约距离边界后应用到已打开的声源。
/// 关闭设备时仍保存缓存，下一次 open 会应用这些参数。
/// @pre 生命周期管理与本次控制操作串行；非有限输入整组拒绝并保留旧状态。
/// @param directionX 相对监听者方向的 X 分量，允许使用未归一化向量。
/// @param directionY 相对监听者方向的 Y 分量。
/// @param directionZ 相对监听者方向的 Z 分量，默认前方为负 Z。
/// @param distance 声源距离，负值归约为零。
/// @param referenceDistance 衰减参考距离，归约为至少 0.001。
/// @param maxDistance 衰减上界距离，归约为不小于参考距离。
/// @param rolloffFactor 衰减倍率，负值归约为零。
void ALPlayer::set_spatial_parameters(float directionX, float directionY,
                                      float directionZ, float distance,
                                      float referenceDistance,
                                      float maxDistance, float rolloffFactor)
{
    // 整组先校验，不能让部分有限字段覆盖旧缓存后才发现其余字段不可用。
    // 非有限方向会污染归一化结果，无穷距离也无法提供有效的设备空间坐标。
    if ( !std::isfinite(directionX) || !std::isfinite(directionY) ||
         !std::isfinite(directionZ) || !std::isfinite(distance) ||
         !std::isfinite(referenceDistance) || !std::isfinite(maxDistance) ||
         !std::isfinite(rolloffFactor) ) {
        m_lastError = "OpenAL spatial parameters must be finite.";
        return;
    }
    {
        std::scoped_lock lock(m_backend->alMutex);
        m_backend->spatialState.directionX = directionX;
        m_backend->spatialState.directionY = directionY;
        m_backend->spatialState.directionZ = directionZ;
        // 距离非负、参考距离严格为正、最大距离不小于参考距离。
        // 这些约束保证后续距离衰减设置不会收到倒置的区间。
        m_backend->spatialState.distance = std::max(0.0f, distance);
        m_backend->spatialState.referenceDistance =
            std::max(0.001f, referenceDistance);
        m_backend->spatialState.maxDistance =
            std::max(m_backend->spatialState.referenceDistance, maxDistance);
        m_backend->spatialState.rolloffFactor = std::max(0.0f, rolloffFactor);
        // 这里只更新缓存，不调用需要再次获取 alMutex 的应用函数。
    }

    // applySpatialState 自行获取同一互斥量，必须在缓存写锁释放后调用。
    applySpatialState();
}

/// @brief 以固定槽队列供数，退回已消费缓冲并在欠载后重新启动声源。
/// @warning 播放期间持续轮询；控制线程写运行、暂停及模式原子标记，
/// 本线程读取并消费请求；运行标记使用 relaxed，只决定是否继续供数。
/// 线程创建发布准备状态，SDL_WaitThread 保证回收完成，运行标记不承担资源同步。
/// 暂停及重建请求的内存序另行保留，不能从运行标记推断这些请求已被处理。
/// @warning 现有循环含互斥锁与 1ms 音频供数休眠，并非无阻塞实时回调。
/// 休眠只在本专用线程发生；禁止在此增加文件访问、逐块日志或额外等待。
/// @pre 只能由 start 创建一次；设备资源须保持有效直到线程退出。
void ALPlayer::audio_thread_loop()
{
    configureOpenALThreadPriority();

    // 启动前已准备两种输出存储；线程只借用，不在入口分配或退出时回收容量。
    // 块契约检查限制后续 resize 长度，停止并等待结束后控制侧才可再次准备。
    auto& floatScratch = m_backend->floatScratch;
    auto& int16Scratch = m_backend->int16Scratch;

    ScopedALContext currentContext(*m_backend);
    if ( !currentContext ) {
        // 绑定失败时没有操作本实例 AL 对象，资源留给控制侧 close 回收。
        {
            std::scoped_lock lock(m_backend->alMutex);
            // 此失败来自绑定入口返回值，不能调用依赖当前上下文的 AL 错误查询。
            // 保存静态阶段名，由控制侧回收后格式化，线程入口不申请诊断字符串。
            if ( !m_backend->m_pendingError.m_operation )
                m_backend->m_pendingError = { "bind playback thread context",
                                              AL_NO_ERROR };
        }
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    // 初始上下文绑定不等于线程独占设备，控制侧仍会通过 AL 锁调整源。
    // 当前绑定只属于本供数线程，其他播放器或控制线程不会切换它。
    /// @brief 为全部有效槽重新拉取音频，然后请求播放。
    /// @pre 调用方已确保队列为空；初始化及格式重建共同使用此流程。
    // 单槽失败结束本次供数，统一出口负责停源退队；已经拉取的图进度不回滚。
    auto refillAllBuffers = [&]() {
        // 队列已清空，整批只接收一次模式快照；中途新请求留给下一轮重建。
        m_backend->queuedSpatialOutput = m_spatialOutputEnabled.load();
        for ( const auto bufferId : m_backend->buffers ) {
            // 缓冲数组保留零作为未创建哨兵，不能将其提交为合法设备对象。
            if ( bufferId != 0 ) {
                if ( !queueAudioBuffer(bufferId, floatScratch, int16Scratch) ) {
                    m_running.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        }

        std::scoped_lock lock(m_backend->alMutex);
        // 供数期间控制侧可能已暂停或停止，持同一 AL 锁复查后才允许播放。
        if ( m_backend->source != 0 &&
             m_running.load(std::memory_order_relaxed) && !m_paused.load() ) {
            // 请求播放不等待设备实际消耗首帧，队列延迟由后端和块容量决定。
            alSourcePlay(m_backend->source);
            // 启动失败同样走统一退出，不保留一个无法播放却持续拉取的线程。
            if ( !checkALError("start queued playback",
                               nullptr,
                               &m_backend->m_pendingError) )
                m_running.store(false, std::memory_order_relaxed);
        }
    };

    // 先填满队列获得调度余量；这会让音频图游标领先实际设备播放位置。
    refillAllBuffers();

    while ( m_running.load(std::memory_order_relaxed) ) {
        if ( m_paused.load() ) {
            // 保留队列和重建请求，恢复后再处理；暂停不是上游音频图的重置。
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if ( m_rebuildQueuedBuffers.exchange(false) ) {
            // 原子交换合并此前请求；交换之后的新请求会留给下一轮处理。
            // 停源后退队，丢弃尚未播放的旧块；这里不会倒退音频图游标。
            {
                std::scoped_lock lock(m_backend->alMutex);
                if ( m_backend->source != 0 ) {
                    // 停播使旧队列可整体退回；缓冲对象保留用于新模式上传。
                    alSourceStop(m_backend->source);
                    clearQueuedBuffers();
                }
            }
            // 清队列失败或控制侧停止后，不再向旧队列追加新模式缓冲。
            if ( !m_running.load(std::memory_order_relaxed) ) break;
            refillAllBuffers();
            // 重建本身完成整组填充，本轮无需继续执行旧 processed 快照逻辑。
            continue;
        }

        // 只回收设备已经消费的槽，仍在播放的缓冲不得覆盖。
        // 这是本次迭代的处理数量快照，新完成的槽留到下一轮回收。
        ALint processed = 0;
        {
            std::scoped_lock lock(m_backend->alMutex);
            if ( m_backend->source != 0 ) {
                alGetSourcei(
                    m_backend->source, AL_BUFFERS_PROCESSED, &processed);
                // 查询失败时默认的零值不是可信队列状态，立即停止而非继续轮询。
                if ( !checkALError("query processed buffers",
                                   nullptr,
                                   &m_backend->m_pendingError) ) {
                    m_running.store(false, std::memory_order_relaxed);
                    break;
                }
            }
        }

        while ( processed > 0 && m_running.load(std::memory_order_relaxed) ) {
            // 每槽再次检查停止请求，避免退出时还无条件回填完整批次。
            ALuint bufferId = 0;
            // 退队拿到可重用 ID 后释放 AL 锁，再执行可能较重的图处理。
            {
                std::scoped_lock lock(m_backend->alMutex);
                if ( m_backend->source != 0 ) {
                    alSourceUnqueueBuffers(m_backend->source, 1, &bufferId);
                    // 退队失败后不能复用可能无效的
                    // ID，也不能继续按旧快照消耗槽数。
                    if ( !checkALError("unqueue processed buffer",
                                       nullptr,
                                       &m_backend->m_pendingError) ) {
                        m_running.store(false, std::memory_order_relaxed);
                        break;
                    }
                }
            }
            // 无有效返回 ID 时跳过本槽，既有循环没有重试或替代缓冲创建逻辑。

            if ( bufferId != 0 ) {
                // 图处理放在 AL 锁外，缩短控制侧调整声源时的互斥等待。
                if ( !queueAudioBuffer(bufferId, floatScratch, int16Scratch) ) {
                    // 不丢失失败槽后继续假装运行，退出后允许控制侧显式重新启动。
                    m_running.store(false, std::memory_order_relaxed);
                    break;
                }
            }
            --processed;
            // 每轮只消费先前查询到的数量，不在内层持续查询形成无限追赶。
        }

        // 回填或退队已失败时直接进入清理，不再查询或操作故障源。
        if ( !m_running.load(std::memory_order_relaxed) ) break;
        {
            std::scoped_lock lock(m_backend->alMutex);
            if ( m_backend->source != 0 ) {
                ALint state  = AL_STOPPED;
                ALint queued = 0;
                alGetSourcei(m_backend->source, AL_SOURCE_STATE, &state);
                if ( !checkALError("query source state",
                                   nullptr,
                                   &m_backend->m_pendingError) ) {
                    m_running.store(false, std::memory_order_relaxed);
                    break;
                }
                alGetSourcei(m_backend->source, AL_BUFFERS_QUEUED, &queued);
                // 两个查询各自成功后才能用其结果判断是否需要自动恢复。
                if ( !checkALError("query queued buffers",
                                   nullptr,
                                   &m_backend->m_pendingError) ) {
                    m_running.store(false, std::memory_order_relaxed);
                    break;
                }
                // 无排队数据时不反复启动空源；下一次成功填充才可能恢复播放。
                if ( state != AL_PLAYING && queued > 0 &&
                     m_running.load(std::memory_order_relaxed) &&
                     !m_paused.load() ) {
                    // 调度欠载可能令源停播；有数据时恢复，而不是重建音频图。
                    // 暂停源和此处播放共用 AL
                    // 锁，复查请求后不覆盖已完成的暂停。 stop
                    // 也会清除暂停位，因此必须同时检查运行请求，避免退出前重播。
                    alSourcePlay(m_backend->source);
                    if ( !checkALError("resume queued playback",
                                       nullptr,
                                       &m_backend->m_pendingError) ) {
                        m_running.store(false, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        }

        // 供数线程的既有轮询退让，避免空转；不保证精确的 1ms 唤醒延迟。
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    {
        // 退出时清队列而不删除句柄；close 等 join 完成后才能释放它们。
        // 即使 stop 发生在暂停期间，这个统一出口也会执行源停止和退队。
        std::scoped_lock lock(m_backend->alMutex);
        if ( m_backend->source != 0 ) {
            alSourceStop(m_backend->source);
            clearQueuedBuffers();
        }
    }
}

/// @brief 从图拉取平面浮点块，转为设备格式并上传到一个空闲队列槽。
/// @param bufferId 已从源退队或尚未排队的有效缓冲句柄。
/// @param floatScratch 当前供数线程独占的浮点交错存储。
/// @param int16Scratch 当前供数线程独占的整型交错存储。
/// @return 运行未结束、源存在且上传及排队未报告 AL 错误时为 true。
/// @warning 每个回收槽调用一次；使用供数线程独占的队列模式快照选择提交格式。
/// 图内部同步与 AL 锁仍可能等待，失败只暂存诊断，不能承诺硬实时。
/// @warning 每块以 relaxed 检查控制侧或故障路径写入的运行请求，避免额外加锁。
/// 原子仅消除标记的数据竞争，图绑定和设备生命周期仍由外部串行控制保证。
/// @pre 上游保持构造时的格式和块容量，且只在停止拉取后替换 source。
/// 转换不做采样率变换；需要重采样时应由上游音频图完成。
bool ALPlayer::queueAudioBuffer(unsigned int               bufferId,
                                std::vector<float>&        floatScratch,
                                std::vector<std::int16_t>& int16Scratch)
{
    // 图只允许写入当前块样本，不能改变准备阶段确定的布局与长度。
    const size_t expectedFrames   = m_buffer.num_frames();
    const size_t expectedCapacity = m_buffer.frame_capacity();
    // 唯一供数线程借用稳定图绑定，控制侧必须等 stop 返回后才能替换 source。
    // 读端独自加锁无法同步基类的无锁写入，因此这里依靠既有生命周期契约。
    if ( !m_running.load(std::memory_order_relaxed) ) return false;
    // 预先静音，空图或未写满的图处理不会泄漏上一个块的采样。
    m_buffer.clear();
    // 无 source 不是 EOF：仍提交一整块静音，直到控制侧显式 stop。
    if ( get_source() ) {
        // 返回共享指针引用，不额外复制所有权；正在执行的图调用须自然返回。
        get_source()->process(m_buffer);
    }

    // 图处理期间控制命令可能失败或请求停止，丢弃本块而不继续转换和上传。
    if ( !m_running.load(std::memory_order_relaxed) ) return false;

    // 在索引声道或调整交错数组前拒绝图破坏的缓冲，不能上传错速或残缺块。
    // 固定块长保证后续 resize 复用启动容量，不因上游改大长度而在此扩容。
    if ( m_buffer.afmt != m_playFormat || expectedFrames == 0 ||
         m_buffer.num_frames() != expectedFrames ||
         m_buffer.frame_capacity() != expectedCapacity ||
         expectedFrames > expectedCapacity )
        return false;
    const float* const* planarData = m_buffer.raw_ptrs();
    if ( !planarData ) return false;
    // 全部平面先验证，避免已转换部分声道后才访问空指针。
    for ( uint16_t channel = 0; channel < m_playFormat.channels; ++channel )
        if ( !planarData[channel] ) return false;

    // 使用本队列固定模式，控制侧中途切换不能改变单个槽的声道布局。
    // 只有非空间化的双声道输入保留立体声，其余输入统一平均下混。
    const auto frames        = m_buffer.num_frames();
    const auto inputChannels = m_buffer.num_channels();
    const bool spatialOutput = m_backend->queuedSpatialOutput;
    const bool monoOutput    = spatialOutput || inputChannels != 2;
    // 多声道输入也会归约为 mono，当前后端没有环绕声直接透传分支。
    const uint16_t outputChannels = monoOutput ? 1 : 2;
    // 空间开关决定设备布局，不会改变上游图的平面格式或其混音声道数。
    const auto format =
        selectALFormat(outputChannels, m_backend->floatFormatSupported);

    // 图内使用每声道独立平面，OpenAL 接收逐帧交错数据，不能直接上传平面。
    if ( m_backend->floatFormatSupported ) {
        // reserve 已在启动时执行；前提是图处理未放大块长或改写缓冲格式。
        floatScratch.resize(frames * outputChannels);
        if ( monoOutput ) {
            // 平均而非直接相加，避免声道数增加时同步放大输出幅度。
            // double 累加可容纳全部有限 float 声道之和，避免先溢出再求平均。
            // 非有限声道按静音计入原声道数，保持其他声道的既有下混权重。
            // 空声道输入明确提交零，不对零做除法。
            for ( size_t i = 0; i < frames; ++i ) {
                double sum = 0.0;
                for ( uint16_t ch = 0; ch < inputChannels; ++ch ) {
                    sum +=
                        static_cast<double>(sanitizeSample(planarData[ch][i]));
                }
                // 等权平均不包含声道布局权重，多声道内容的专门混音应由上游完成。
                floatScratch[i] =
                    inputChannels == 0
                        ? 0.0f
                        : static_cast<float>(
                              sum / static_cast<double>(inputChannels));
            }
        } else {
            // 保持左、右顺序逐帧交错；浮点路径不做整型量化或额外限幅。
            for ( size_t i = 0; i < frames; ++i ) {
                floatScratch[i * 2 + 0] = sanitizeSample(planarData[0][i]);
                floatScratch[i * 2 + 1] = sanitizeSample(planarData[1][i]);
            }
        }

        std::scoped_lock lock(m_backend->alMutex);
        if ( !m_running.load(std::memory_order_relaxed) ||
             m_backend->source == 0 ) {
            // 没有可排队的源时终止提交，不将数据上传成功当成播放成功。
            return false;
        }
        // 字节长度来自交错样本数，采样率使用固定播放格式。
        // 上传接口复制数据，返回后 scratch 可用于下一个槽。
        alBufferData(bufferId,
                     format,
                     floatScratch.data(),
                     static_cast<ALsizei>(floatScratch.size() * sizeof(float)),
                     static_cast<ALsizei>(m_playFormat.samplerate));
        // 上传失败时不能把缓冲中的旧样本再次排队。
        if ( !checkALError(
                 "upload float buffer", nullptr, &m_backend->m_pendingError) )
            return false;
        const ALuint alBufferId = static_cast<ALuint>(bufferId);
        // 复用已有 ID 入队而非创建新缓冲，固定队列避免每块对象分配。
        alSourceQueueBuffers(m_backend->source, 1, &alBufferId);
        // 上传已经独立检查，此处只确认本次入队是否成功。
        return checkALError(
            "queue float buffer", nullptr, &m_backend->m_pendingError);
    }

    // 不支持浮点扩展时仍使用同样的声道映射，只在最后转换采样表示。
    // 先以 double 对净化样本求平均再量化，有限大数相消不能被中间溢出破坏。
    int16Scratch.resize(frames * outputChannels);
    if ( monoOutput ) {
        for ( size_t i = 0; i < frames; ++i ) {
            double sum = 0.0;
            for ( uint16_t ch = 0; ch < inputChannels; ++ch ) {
                sum += static_cast<double>(sanitizeSample(planarData[ch][i]));
            }
            // 先完成下混再裁剪量化，避免各声道提前限幅改变平均结果。
            const float sample =
                inputChannels == 0
                    ? 0.0f
                    : static_cast<float>(sum /
                                         static_cast<double>(inputChannels));
            int16Scratch[i] = floatToInt16(sample);
        }
    } else {
        for ( size_t i = 0; i < frames; ++i ) {
            // 两个声道分别量化，不先合并左右，否则非空间模式会失去立体声。
            int16Scratch[i * 2 + 0] = floatToInt16(planarData[0][i]);
            int16Scratch[i * 2 + 1] = floatToInt16(planarData[1][i]);
        }
    }

    std::scoped_lock lock(m_backend->alMutex);
    if ( !m_running.load(std::memory_order_relaxed) ||
         m_backend->source == 0 ) {
        return false;
    }
    // 等待 AL 锁期间也可能收到停止；与浮点路径保持同一提交边界。
    // int16 与 float 路径都只在上传阶段持 AL 锁，不把采样转换包进锁区间。
    // 上传和入队放在同一 AL 锁区间，避免控制侧声源操作插入二者之间。
    alBufferData(
        bufferId,
        format,
        int16Scratch.data(),
        static_cast<ALsizei>(int16Scratch.size() * sizeof(std::int16_t)),
        static_cast<ALsizei>(m_playFormat.samplerate));
    if ( !checkALError(
             "upload int16 buffer", nullptr, &m_backend->m_pendingError) )
        return false;
    const ALuint alBufferId = static_cast<ALuint>(bufferId);
    alSourceQueueBuffers(m_backend->source, 1, &alBufferId);
    // 返回 false 结束当前播放，不回滚已经推进的图游标或自动重放本块。
    return checkALError(
        "queue int16 buffer", nullptr, &m_backend->m_pendingError);
}

/// @brief 解除源的整个队列引用，保留缓冲对象供重用或删除。
/// @pre 调用方持有 alMutex，且已经请求停源；源处于初始或停止状态。
/// @warning 用于退出、关闭或格式重建，不等待缓冲播放完成。
/// 清队列不调用图节点，不消耗新的音频帧，也不回退已预读的帧。
void ALPlayer::clearQueuedBuffers()
{
    if ( m_backend->source == 0 ) {
        return;
    }

    // 首批入队期间退出时源仍可能处于 AL_INITIAL，stop 不会把它变为已播放。
    // 此时逐个 unqueue 会拒绝尚未处理的槽，必须直接解除整个队列。
    // AL_BUFFER 置空在 AL_INITIAL 与 AL_STOPPED 均合法，并重置源的队列类型。
    alSourcei(m_backend->source, AL_BUFFER, AL_NONE);
    if ( !checkALError(
             "clear source queue", nullptr, &m_backend->m_pendingError) ) {
        // 格式重建失败时禁止继续入队；退出路径则仍由 close 释放源与上下文。
        m_running.store(false, std::memory_order_relaxed);
    }
}

/// @brief 把空间缓存映射到相对监听者的声源坐标与衰减参数。
/// 只调整源属性；mono/stereo 数据格式变化由供数线程另行清队列完成。
/// @warning 控制侧低频调用会获取 AL 锁，不得持有该锁重入本函数。
/// @pre 不与 open/close 并发；无设备时允许仅保留尚待应用的配置。
/// @return 未打开设备或应用成功时为 true；失败保存诊断并结束本次供数。
bool ALPlayer::applySpatialState()
{
    std::scoped_lock lock(m_backend->alMutex);
    if ( !m_backend->context || m_backend->source == 0 ) {
        // 尚未打开设备时只保留先前写入的缓存，下一次 open 会重新应用。
        return true;
    }

    ScopedALContext currentContext(*m_backend);
    if ( !currentContext ) {
        m_lastError = "OpenAL thread context binding failed.";
        // 绑定失败不能依赖后续 AL 查询发现，直接向打开流程和运行状态传播。
        m_running.store(false, std::memory_order_relaxed);
        return false;
    }
    const bool spatialOutput = m_spatialOutputEnabled.load();
    // 缓存和设备调用共用锁，避免读到一组参数的半次更新。
    // 归一化把朝向和距离解耦，方向向量自身的长度不参与位置缩放。
    const auto direction =
        normalizeDirection(m_backend->spatialState.directionX,
                           m_backend->spatialState.directionY,
                           m_backend->spatialState.directionZ);
    const float distance = std::max(0.0f, m_backend->spatialState.distance);
    // 方向零值回退发生在 normalizeDirection，距离零值则保留为监听者位置。

    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    alSourcei(m_backend->source, AL_SOURCE_RELATIVE, AL_TRUE);
    // 位置相对监听者，不随世界坐标中的监听者平移而偏移。
    if ( spatialOutput ) {
        // 相对坐标以监听者为参照，距离沿指定方向展开为源的位置。
        // 参考和最大距离来自已归约缓存，衰减模型与 open 的初值一致。
        alSource3f(m_backend->source,
                   AL_POSITION,
                   direction[0] * distance,
                   direction[1] * distance,
                   direction[2] * distance);
        alSource3f(m_backend->source,
                   AL_DIRECTION,
                   direction[0],
                   direction[1],
                   direction[2]);
        alSourcef(m_backend->source,
                  AL_REFERENCE_DISTANCE,
                  m_backend->spatialState.referenceDistance);
        alSourcef(m_backend->source,
                  AL_MAX_DISTANCE,
                  m_backend->spatialState.maxDistance);
        alSourcef(m_backend->source,
                  AL_ROLLOFF_FACTOR,
                  m_backend->spatialState.rolloffFactor);
        // 未设置锥角等其他空间属性，本接口只承诺方向、距离与衰减参数映射。
    } else {
        // 非空间模式清除旧位置和距离衰减，避免关闭开关后仍残留音量变化。
        // 不覆盖缓存，重新开启时可恢复控制侧最后设置的空间参数。
        alSource3f(m_backend->source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSource3f(m_backend->source, AL_DIRECTION, 0.0f, 0.0f, -1.0f);
        alSourcef(m_backend->source, AL_REFERENCE_DISTANCE, 1.0f);
        alSourcef(m_backend->source, AL_MAX_DISTANCE, 100.0f);
        alSourcef(m_backend->source, AL_ROLLOFF_FACTOR, 0.0f);
        // 禁用衰减而非覆盖图增益，因此音量包络等上游处理继续生效。
    }
    // 控制线程必须就地消费整组设置的错误，不把它遗留给其他操作或供数线程。
    if ( !checkALError("apply spatial state", &m_lastError) ) {
        // 驱动可能只应用了部分属性；不承诺回滚，停止播放并保留请求供重开使用。
        m_running.store(false, std::memory_order_relaxed);
        return false;
    }
    return true;
}

}  // namespace ice
