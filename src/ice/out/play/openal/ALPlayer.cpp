#include <ice/out/play/openal/ALPlayer.hpp>

#include <al.h>
#include <alc.h>
#include <fmt/base.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

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

/// @brief 消费当前 AL 错误，并在失败时输出操作诊断。
/// @param operation 用于定位失败阶段的操作名。
/// @param lastError 可选的控制侧错误存储；供数路径未传入该参数。
/// @return 没有待处理 AL 错误时返回 true。
/// @warning 每次排队后也会调用；成功路径不格式化，失败路径仍有分配和输出。
/// 错误路径的执行时间受字符串分配及输出端影响，不满足无阻塞实时约束。
bool checkALError(const char* operation, std::string* lastError = nullptr)
{
    // alGetError 消费错误状态；同一操作不应再依靠第二次查询恢复原错误。
    const ALenum error = alGetError();
    if ( error == AL_NO_ERROR ) {
        return true;
    }

    const std::string message = fmt::format("OpenAL {} failed: {} (0x{:x}).",
                                            operation,
                                            getALErrorName(error),
                                            static_cast<int>(error));
    fmt::print("{}\n", message);
    if ( lastError ) {
        // 只有显式提供存储的控制路径更新错误引用，避免供数直接写该成员。
        *lastError = message;
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

/// @brief 将浮点采样转换为 int16。
/// @pre 输入为有限音频采样；此处不承担 NaN 等上游异常数据的修复。
/// 对称缩放使用正端最大值，负满幅对应 -32767 而非 -32768。
/// @return 单个有符号采样；输出字节布局由上传处的整型数组提供。
std::int16_t floatToInt16(float sample)
{
    // 裁剪避免超过有符号 16 位范围；lrint 沿用当前浮点舍入模式。
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
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
    const float lengthSq = x * x + y * y + z * z;
    if ( lengthSq <= 0.000001f ) {
        // 默认朝向与监听者前方一致，零方向不会导致除零或任意偏转。
        return { 0.0f, 0.0f, -1.0f };
    }

    const float invLength = 1.0f / std::sqrt(lengthSq);
    return { x * invLength, y * invLength, z * invLength };
}
}  // namespace

/// @brief OpenAL 后端运行时句柄。
/// 由 ALPlayer 独占拥有；句柄需在供数线程退出后按依赖次序释放。
/// 该结构仅保存状态，实际释放由 close 负责，不在成员析构中隐式执行。
class ALBackend
{
public:
    /// @brief OpenAL 设备句柄。
    /// 在上下文创建失败的早期分支直接释放，成功后交由 close 回收。
    ALCdevice* device{ nullptr };

    /// @brief OpenAL 上下文句柄。
    /// AL 对象在该上下文中创建，销毁时必须先释放源与缓冲。
    ALCcontext* context{ nullptr };

    /// @brief OpenAL 声源句柄。
    /// 一个源承载整个混音图，空间化作用于最终输出而非各图节点。
    ALuint source{ 0 };

    /// @brief OpenAL 队列 buffer 句柄。
    /// 句柄数组生命周期跨越多次 start/stop，不随每次退队重新创建。
    std::array<ALuint, kOpenALBufferCount> buffers{};

    /// @brief buffer 句柄是否已创建。
    bool buffersReady{ false };

    /// @brief 是否支持 float32 buffer 扩展。
    /// open 时写入，供数期间只读；换设备必须先结束旧线程。
    bool floatFormatSupported{ false };

    /// @brief 空间化参数缓存。
    ALSpatialState spatialState;

    /// @brief 串行化供数线程与控制侧的源操作和空间参数缓存访问。
    /// @warning 供数循环逐块持锁；控制侧不得在锁内等待播放线程退出。
    /// 锁不覆盖完整 open 生命周期，外部仍须串行执行设备管理操作。
    std::mutex alMutex;
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
    m_buffer.resize(m_playFormat,
                    std::max<uint32_t>(ICEConfig::default_buffer_size,
                                       kOpenALMinBufferFrames));
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
/// 排队失败只走即时输出，未必更新此字符串。
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
bool ALPlayer::open(std::string_view deviceName)
{
    // 每次尝试独立记录结果；清日志只影响诊断，不重置全局初始化标记。
    m_lastError.clear();
    m_openedDeviceName.clear();
    resetOpenALSoftLog();
    // 旧设备 close 仍可能产生库日志，因此捕获范围是本次切换而非纯打开调用。

    if ( m_backend->device || m_backend->context ) {
        // 重新打开也是完整的生命周期切换，不能复用旧设备中的 AL 句柄。
        close();
    }

    // string_view 不保证零结尾，复制后再传给 C 接口，并保持到调用完成。
    std::string deviceNameStorage(deviceName);
    const char* openName =
        deviceNameStorage.empty() ? nullptr : deviceNameStorage.c_str();

    m_backend->device = alcOpenDevice(openName);
    if ( !m_backend->device ) {
        // 没有可查询的设备句柄，用请求名称与库日志保留初始化阶段的原因。
        m_lastError = fmt::format(
            "OpenAL device open failed: {}.",
            deviceNameStorage.empty() ? "default device" : deviceNameStorage);
        appendOpenALSoftLogDetail(m_lastError);
        fmt::print("{}\n", m_lastError);
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
        fmt::print("{}\n", m_lastError);
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
        fmt::print("{}\n", m_lastError);
        alcCloseDevice(m_backend->device);
        m_backend->device = nullptr;
        return false;
    }

    if ( alcMakeContextCurrent(m_backend->context) != ALC_TRUE ) {
        // 尚未创建 AL 对象，先销毁上下文再关闭设备即可完成本阶段回滚。
        m_lastError =
            formatALCErrorMessage("make context current", m_backend->device);
        appendOpenALSoftLogDetail(m_lastError);
        fmt::print("{}\n", m_lastError);
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
    applySpatialState();

    if ( !checkALError("open", &m_lastError) ) {
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

    std::scoped_lock lock(m_backend->alMutex);
    if ( !m_backend->context ) {
        return;
    }

    // 以下操作仍需要有效上下文；不得在停止线程前先销毁 context。
    alcMakeContextCurrent(m_backend->context);
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

    // 先解除当前上下文再销毁，设备必须活到其上下文释放以后。
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(m_backend->context);
    alcCloseDevice(m_backend->device);
    m_backend->context = nullptr;
    m_backend->device  = nullptr;
    m_openedDeviceName.clear();
    // 空设备名表示已关闭；诊断保留使失败回滚后仍可向上层解释原因。
}

/// @brief 在资源就绪且尚未运行时创建供数线程。
/// @return true 只代表线程已发起，不保证首批排队或实体播放成功。
/// @pre 控制侧串行调用；原子运行标记不能替代线程对象的生命周期锁。
/// 未绑定图也允许启动，供数路径会提交静音，便于先建设备再启用图节点。
bool ALPlayer::start()
{
    if ( m_running.load() ) {
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

    // 重启丢弃上次暂停和队列重建请求，首批队列会按当前空间开关填充。
    m_lastError.clear();
    m_running.store(true);
    m_paused.store(false);
    m_rebuildQueuedBuffers.store(false);
    m_audioThread = std::thread(&ALPlayer::audio_thread_loop, this);
    // 线程借用 this，调用方必须保持实例存在直到 stop 完成 join。
    return true;
}

/// @brief 请求供数线程退出，并等待其完成源停播及队列清理。
/// @warning 低频控制侧 join 可能等待当前图处理和驱动调用；不能自行 join。
/// stop 保留设备资源，后续可再次 start；释放资源需调用 close。
/// @pre 不与另一次 start/stop 并发执行，不从音频图 process 中反向调用。
void ALPlayer::stop()
{
    if ( !m_running.load() ) {
        // 正常生命周期要求前一次 stop 已负责 join；这里不额外探测线程状态。
        return;
    }

    // 退出请求由循环和回填内层读取；同时取消暂停，便于下次启动恢复初态。
    m_running.store(false);
    m_paused.store(false);

    if ( m_audioThread.joinable() ) {
        // 返回后后台不再触碰缓冲与句柄，close 才有安全回收的时机。
        m_audioThread.join();
    }
}

/// @brief 查询运行请求标记，不代表 OpenAL 当前源状态或可听输出。
/// @warning 可高频读取控制侧写入的原子值；false 不代表 join 已完成。
bool ALPlayer::is_running() const
{
    return m_running.load();
}

/// @brief 发布暂停请求并暂停设备源，保留已排队的音频块。
/// @warning 控制侧会等待 alMutex；并非与整个供数迭代原子切换。
/// 暂停不撤回已提交到设备的数据，也不对图节点执行定位或状态复位。
void ALPlayer::pause()
{
    if ( !m_running.load() || m_paused.load() ) {
        // 暂停请求仅对已运行且未暂停的实例有意义，不改变下一次启动策略。
        return;
    }

    // 先让下一轮供数停止推进，再串行发出源暂停；已进入的迭代仍可能继续。
    m_paused.store(true);
    std::scoped_lock lock(m_backend->alMutex);
    if ( m_backend->context && m_backend->source != 0 ) {
        alcMakeContextCurrent(m_backend->context);
        alSourcePause(m_backend->source);
    }
}

/// @brief 允许供数继续并请求设备播放原有队列。
/// @warning 控制侧会等待 alMutex；生命周期必须保持设备仍然打开。
/// 若暂停期间改变空间模式，恢复后的供数循环还需消费队列重建请求。
void ALPlayer::resume()
{
    if ( !m_running.load() || !m_paused.load() ) {
        // resume 不承担启动线程的职责，也不重新生成已经排队的数据。
        return;
    }

    m_paused.store(false);
    std::scoped_lock lock(m_backend->alMutex);
    // 保留设备队列继续播放，供数循环随后回收已处理槽或执行积压重建。
    if ( m_backend->context && m_backend->source != 0 ) {
        alcMakeContextCurrent(m_backend->context);
        alSourcePlay(m_backend->source);
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
/// @pre 方向和距离输入有限；生命周期管理与本次控制操作串行。
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
/// 本线程读取并消费重建请求，以跨线程传递状态，保留既有默认原子序。
/// @warning 现有循环含互斥锁与 1ms 音频供数休眠，并非无阻塞实时回调。
/// 休眠只在本专用线程发生；禁止在此增加文件访问、逐块日志或额外等待。
/// @pre 只能由 start 创建一次；设备资源须保持有效直到线程退出。
void ALPlayer::audio_thread_loop()
{
    configureOpenALThreadPriority();

    std::vector<float>        floatScratch;
    std::vector<std::int16_t> int16Scratch;
    // 两种格式都预留最坏输出容量；后续 resize 在块长固定时复用这些存储。
    // scratch 由线程独占，不把每块临时容器或数据指针交给控制线程持有。
    floatScratch.reserve(m_buffer.num_frames() *
                         std::max<uint16_t>(2, m_playFormat.channels));
    int16Scratch.reserve(m_buffer.num_frames() *
                         std::max<uint16_t>(2, m_playFormat.channels));
    // 即使当前设备只用一种样本类型，两种容量也预先准备，循环中只选一条分支。

    {
        std::scoped_lock lock(m_backend->alMutex);
        alcMakeContextCurrent(m_backend->context);
    }

    // 初始上下文绑定不等于线程独占设备，控制侧仍会通过 AL 锁调整源。
    // 上下文绑定没有实例间的全局锁，多播放器并发使用需另行保证上下文隔离。
    /// @brief 为全部有效槽重新拉取音频，然后请求播放。
    /// @pre 调用方已确保队列为空；初始化及格式重建共同使用此流程。
    // 当前流程未传播单槽失败，start 成功不能视为队列填充成功的证明。
    auto refillAllBuffers = [&]() {
        for ( const auto bufferId : m_backend->buffers ) {
            // 缓冲数组保留零作为未创建哨兵，不能将其提交为合法设备对象。
            if ( bufferId != 0 ) {
                queueAudioBuffer(bufferId, floatScratch, int16Scratch);
            }
        }

        std::scoped_lock lock(m_backend->alMutex);
        if ( m_backend->source != 0 ) {
            // 请求播放不等待设备实际消耗首帧，队列延迟由后端和块容量决定。
            alSourcePlay(m_backend->source);
        }
    };

    // 先填满队列获得调度余量；这会让音频图游标领先实际设备播放位置。
    refillAllBuffers();

    while ( m_running.load() ) {
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
            }
        }

        while ( processed > 0 && m_running.load() ) {
            // 每槽再次检查停止请求，避免退出时还无条件回填完整批次。
            ALuint bufferId = 0;
            // 退队拿到可重用 ID 后释放 AL 锁，再执行可能较重的图处理。
            {
                std::scoped_lock lock(m_backend->alMutex);
                if ( m_backend->source != 0 ) {
                    alSourceUnqueueBuffers(m_backend->source, 1, &bufferId);
                }
            }
            // 无有效返回 ID 时跳过本槽，既有循环没有重试或替代缓冲创建逻辑。

            if ( bufferId != 0 ) {
                // 图处理放在 AL 锁外，缩短控制侧调整声源时的互斥等待。
                queueAudioBuffer(bufferId, floatScratch, int16Scratch);
            }
            --processed;
            // 每轮只消费先前查询到的数量，不在内层持续查询形成无限追赶。
        }

        {
            std::scoped_lock lock(m_backend->alMutex);
            if ( m_backend->source != 0 ) {
                ALint state  = AL_STOPPED;
                ALint queued = 0;
                alGetSourcei(m_backend->source, AL_SOURCE_STATE, &state);
                alGetSourcei(m_backend->source, AL_BUFFERS_QUEUED, &queued);
                // 无排队数据时不反复启动空源；下一次成功填充才可能恢复播放。
                if ( state != AL_PLAYING && queued > 0 ) {
                    // 调度欠载可能令源停播；有数据时恢复，而不是重建音频图。
                    // 此分支未再次检查暂停请求，可能覆盖同轮控制侧的源暂停操作。
                    alSourcePlay(m_backend->source);
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
        alcMakeContextCurrent(nullptr);
    }
}

/// @brief 从图拉取平面浮点块，转为设备格式并上传到一个空闲队列槽。
/// @param bufferId 已从源退队或尚未排队的有效缓冲句柄。
/// @param floatScratch 当前供数线程独占的浮点交错存储。
/// @param int16Scratch 当前供数线程独占的整型交错存储。
/// @return 源存在且上传、排队未报告 AL 错误时为 true。
/// @warning 每个回收槽调用一次；读取控制侧模式原子值以选择提交格式。
/// 既有图处理锁与 AL 锁可能等待，错误输出可能分配，不能承诺硬实时。
/// @pre 上游保持构造时的格式和块容量，且只在停止拉取后替换 source。
/// 转换不做采样率变换；需要重采样时应由上游音频图完成。
bool ALPlayer::queueAudioBuffer(unsigned int               bufferId,
                                std::vector<float>&        floatScratch,
                                std::vector<std::int16_t>& int16Scratch)
{
    {
        std::scoped_lock<std::mutex> lock(m_sourceMutex);
        // 预先静音，空图或未写满的图处理不会泄漏上一个块的采样。
        // 基类 set_source 不取此锁，因此它不能保证运行中替换图的安全。
        m_buffer.clear();
        // 无 source 不是 EOF：仍提交一整块静音，直到控制侧显式 stop。
        if ( get_source() ) {
            // 返回共享指针引用，连续检查与调用不额外复制共享所有权。
            // 节点处理契约由图实现负责，本后端没有单独的失败码分支。
            get_source()->process(m_buffer);
        }
    }

    // 一次提交只读取一次模式，确保格式枚举、声道数和数据布局一致。
    // 只有非空间化的双声道输入保留立体声，其余输入统一平均下混。
    const auto frames        = m_buffer.num_frames();
    const auto inputChannels = m_buffer.num_channels();
    const bool spatialOutput = m_spatialOutputEnabled.load();
    const bool monoOutput    = spatialOutput || inputChannels != 2;
    // 多声道输入也会归约为 mono，当前后端没有环绕声直接透传分支。
    const uint16_t outputChannels = monoOutput ? 1 : 2;
    // 空间开关决定设备布局，不会改变上游图的平面格式或其混音声道数。
    const auto format =
        selectALFormat(outputChannels, m_backend->floatFormatSupported);

    // 图内使用每声道独立平面，OpenAL 接收逐帧交错数据，不能直接上传平面。
    const float* const* planarData = m_buffer.raw_ptrs();
    if ( m_backend->floatFormatSupported ) {
        // reserve 已在启动时执行；前提是图处理未放大块长或改写缓冲格式。
        floatScratch.resize(frames * outputChannels);
        if ( monoOutput ) {
            // 平均而非直接相加，避免声道数增加时同步放大输出幅度。
            // 空声道输入明确提交零，不对零做除法。
            for ( size_t i = 0; i < frames; ++i ) {
                float sum = 0.0f;
                for ( uint16_t ch = 0; ch < inputChannels; ++ch ) {
                    sum += planarData[ch][i];
                }
                // 等权平均不包含声道布局权重，多声道内容的专门混音应由上游完成。
                floatScratch[i] = inputChannels == 0
                                      ? 0.0f
                                      : sum / static_cast<float>(inputChannels);
            }
        } else {
            // 保持左、右顺序逐帧交错；浮点路径不做整型量化或额外限幅。
            for ( size_t i = 0; i < frames; ++i ) {
                floatScratch[i * 2 + 0] = planarData[0][i];
                floatScratch[i * 2 + 1] = planarData[1][i];
            }
        }

        std::scoped_lock lock(m_backend->alMutex);
        if ( m_backend->source == 0 ) {
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
        const ALuint alBufferId = static_cast<ALuint>(bufferId);
        // 复用已有 ID 入队而非创建新缓冲，固定队列避免每块对象分配。
        alSourceQueueBuffers(m_backend->source, 1, &alBufferId);
        // 当前检查覆盖上传与入队的累计错误，不精确区分两次调用中的失败点。
        return checkALError("queue float buffer");
    }

    // 不支持浮点扩展时仍使用同样的声道映射，只在最后转换采样表示。
    int16Scratch.resize(frames * outputChannels);
    if ( monoOutput ) {
        for ( size_t i = 0; i < frames; ++i ) {
            float sum = 0.0f;
            for ( uint16_t ch = 0; ch < inputChannels; ++ch ) {
                sum += planarData[ch][i];
            }
            // 先完成下混再裁剪量化，避免各声道提前限幅改变平均结果。
            const float sample = inputChannels == 0
                                     ? 0.0f
                                     : sum / static_cast<float>(inputChannels);
            int16Scratch[i]    = floatToInt16(sample);
        }
    } else {
        for ( size_t i = 0; i < frames; ++i ) {
            // 两个声道分别量化，不先合并左右，否则非空间模式会失去立体声。
            int16Scratch[i * 2 + 0] = floatToInt16(planarData[0][i]);
            int16Scratch[i * 2 + 1] = floatToInt16(planarData[1][i]);
        }
    }

    std::scoped_lock lock(m_backend->alMutex);
    if ( m_backend->source == 0 ) {
        return false;
    }
    // int16 与 float 路径都只在上传阶段持 AL 锁，不把采样转换包进锁区间。
    // 上传和入队放在同一 AL 锁区间，避免控制侧声源操作插入二者之间。
    alBufferData(
        bufferId,
        format,
        int16Scratch.data(),
        static_cast<ALsizei>(int16Scratch.size() * sizeof(std::int16_t)),
        static_cast<ALsizei>(m_playFormat.samplerate));
    const ALuint alBufferId = static_cast<ALuint>(bufferId);
    alSourceQueueBuffers(m_backend->source, 1, &alBufferId);
    // 返回 false 不回滚已经推进的图游标，调用方目前也不会重放本块。
    return checkALError("queue int16 buffer");
}

/// @brief 退回源当前持有的所有队列槽，保留缓冲对象供重用或删除。
/// @pre 调用方持有 alMutex，且已经停源，使全部排队缓冲可退队。
/// @warning 用于退出、关闭或格式重建，有限遍历固定队列而非等待播放结束。
/// 退队不调用图节点，不消耗新的音频帧，也不回退已预读的帧。
void ALPlayer::clearQueuedBuffers()
{
    if ( m_backend->source == 0 ) {
        return;
    }

    // 查询一次数量后逐个退队；此 helper 不自行停源或反复等待槽变为可用。
    ALint queued = 0;
    alGetSourcei(m_backend->source, AL_BUFFERS_QUEUED, &queued);
    // 正常情况下数量至多为固定槽数；持锁保证本实例不会并发继续入队。
    while ( queued > 0 ) {
        // 退队 ID 不需要移出数组；数组一直保存这组缓冲的所有权登记。
        ALuint bufferId = 0;
        alSourceUnqueueBuffers(m_backend->source, 1, &bufferId);
        --queued;
    }
}

/// @brief 把空间缓存映射到相对监听者的声源坐标与衰减参数。
/// 只调整源属性；mono/stereo 数据格式变化由供数线程另行清队列完成。
/// @warning 控制侧低频调用会获取 AL 锁，不得持有该锁重入本函数。
/// @pre 不与 open/close 并发；无设备时允许仅保留尚待应用的配置。
void ALPlayer::applySpatialState()
{
    std::scoped_lock lock(m_backend->alMutex);
    if ( !m_backend->context || m_backend->source == 0 ) {
        // 尚未打开设备时只保留先前写入的缓存，下一次 open 会重新应用。
        return;
    }

    alcMakeContextCurrent(m_backend->context);
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
}

}  // namespace ice
