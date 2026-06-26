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
extern "C" {
using LPALSOFTLOGCALLBACK = void(ALC_APIENTRY*)(void* userptr, char level,
                                                const char* message,
                                                int         length) noexcept;
void ALC_APIENTRY alsoft_set_log_callback(LPALSOFTLOGCALLBACK callback,
                                          void*               userptr) noexcept;
}
#endif

#ifndef AL_FORMAT_MONO_FLOAT32
#    define AL_FORMAT_MONO_FLOAT32 0x10010
#endif

#ifndef AL_FORMAT_STEREO_FLOAT32
#    define AL_FORMAT_STEREO_FLOAT32 0x10011
#endif

#ifndef ALC_ALL_DEVICES_SPECIFIER
#    define ALC_ALL_DEVICES_SPECIFIER 0x1013
#endif

namespace ice
{
namespace
{
constexpr size_t   kOpenALBufferCount       = 8;
constexpr uint32_t kOpenALMinBufferFrames   = 2048;
constexpr size_t   kOpenALSoftLogBufferSize = 4096;

std::mutex                                 g_openALSoftLogMutex;
std::array<char, kOpenALSoftLogBufferSize> g_openALSoftLogBuffer{};
size_t                                     g_openALSoftLogLength{ 0 };
bool                                       g_openALSoftLogInstalled{ false };

/// @brief OpenAL 空间化参数缓存。
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

/// @brief Set an environment variable when the caller did not provide one.
/// @param name Environment variable name.
/// @param value Environment variable value.
void setEnvironmentDefault(const char* name, const char* value)
{
    const char* currentValue = std::getenv(name);
    if ( currentValue && *currentValue ) {
        return;
    }

#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

/// @brief Configure OpenAL Soft defaults before its first real initialization.
void configureOpenALSoftEnvironment()
{
#ifdef _WIN32
    setEnvironmentDefault("ALSOFT_DRIVERS", "winmm,dsound,wasapi");
#endif
    setEnvironmentDefault("ALSOFT_LOGLEVEL", "2");
}

/// @brief Raise the OpenAL feeder thread priority on Windows.
/// @warning Low-frequency thread setup only; do not add per-buffer logging or
/// blocking calls here.
void configureOpenALThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

/// @brief Append a raw text fragment to the captured OpenAL Soft log buffer.
/// @param text Text fragment.
/// @param length Text fragment length.
void appendOpenALSoftLogText(const char* text, size_t length) noexcept
{
    if ( !text || length == 0 ||
         g_openALSoftLogLength >= kOpenALSoftLogBufferSize - 1 ) {
        return;
    }

    const size_t available =
        kOpenALSoftLogBufferSize - 1 - g_openALSoftLogLength;
    const size_t copyLength = std::min(length, available);
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
void openALSoftLogCallback(void* userptr, char level, const char* message,
                           int length) noexcept
{
    (void)userptr;
    if ( !message || length <= 0 ) {
        return;
    }

    std::lock_guard lock(g_openALSoftLogMutex);
    if ( g_openALSoftLogLength != 0 ) {
        appendOpenALSoftLogText("\n", 1);
    }

    appendOpenALSoftLogText("[", 1);
    appendOpenALSoftLogText(&level, 1);
    appendOpenALSoftLogText("] ", 2);
    appendOpenALSoftLogText(message, static_cast<size_t>(length));
}
#endif

/// @brief 安装 OpenAL Soft 日志回调；动态 OpenAL 不引用非稳定导出符号。
void installOpenALSoftLogCallback()
{
    std::lock_guard lock(g_openALSoftLogMutex);
    if ( g_openALSoftLogInstalled ) {
        return;
    }

#if defined(ICE_OPENALSOFT_STATIC_LINKAGE)
    alsoft_set_log_callback(openALSoftLogCallback, nullptr);
#endif
    g_openALSoftLogInstalled = true;
}

/// @brief Clear captured OpenAL Soft log messages.
void resetOpenALSoftLog()
{
    std::lock_guard lock(g_openALSoftLogMutex);
    g_openALSoftLogLength    = 0;
    g_openALSoftLogBuffer[0] = '\0';
}

/// @brief Get a snapshot of captured OpenAL Soft log messages.
/// @return Captured log text.
std::string getOpenALSoftLogSnapshot()
{
    std::lock_guard lock(g_openALSoftLogMutex);
    return std::string(g_openALSoftLogBuffer.data(), g_openALSoftLogLength);
}

/// @brief Append captured OpenAL Soft log messages to an error string.
/// @param errorMessage Destination error message.
void appendOpenALSoftLogDetail(std::string& errorMessage)
{
    const std::string log = getOpenALSoftLogSnapshot();
    if ( log.empty() ) {
        return;
    }

    errorMessage += " OpenAL Soft log: ";
    errorMessage += log;
}

/// @brief Convert an ALC error code to a readable name.
/// @param error ALC error code.
/// @return Static readable error name.
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

/// @brief Convert an AL error code to a readable name.
/// @param error AL error code.
/// @return Static readable error name.
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

/// @brief Build a readable ALC failure message.
/// @param operation Failed OpenAL operation name.
/// @param device Device used to query the latest ALC error.
/// @return Readable failure message.
std::string formatALCErrorMessage(const char* operation, ALCdevice* device)
{
    const ALCenum error = alcGetError(device);
    if ( error == ALC_NO_ERROR ) {
        return fmt::format("OpenAL {} failed.", operation);
    }

    return fmt::format("OpenAL {} failed: {} (0x{:x}).",
                       operation,
                       getALCErrorName(error),
                       static_cast<int>(error));
}

/// @brief Check and report the latest AL error.
/// @param operation Failed OpenAL operation name.
/// @param lastError Optional destination for caller-side diagnostics.
/// @return true when no AL error is pending.
bool checkALError(const char* operation, std::string* lastError = nullptr)
{
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
        *lastError = message;
    }
    return false;
}

/// @brief 将 OpenAL 多字符串设备列表转成 vector。
std::vector<ALAudioDeviceInfo> parseDeviceList(const ALCchar* deviceList)
{
    std::vector<ALAudioDeviceInfo> devices;
    if ( !deviceList ) {
        return devices;
    }

    const ALCchar* current = deviceList;
    while ( *current != '\0' ) {
        devices.push_back({ std::string(current) });
        current += std::strlen(current) + 1;
    }
    return devices;
}

/// @brief 选择 OpenAL buffer 格式。
ALenum selectALFormat(uint16_t channels, bool useFloatFormat)
{
    if ( useFloatFormat ) {
        return channels == 1 ? AL_FORMAT_MONO_FLOAT32
                             : AL_FORMAT_STEREO_FLOAT32;
    }
    return channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
}

/// @brief 将浮点采样转换为 int16。
std::int16_t floatToInt16(float sample)
{
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    return static_cast<std::int16_t>(std::lrint(
        clamped *
        static_cast<float>(std::numeric_limits<std::int16_t>::max())));
}

/// @brief 归一化方向向量，零向量会回退到 OpenAL 默认前方。
std::array<float, 3> normalizeDirection(float x, float y, float z)
{
    const float lengthSq = x * x + y * y + z * z;
    if ( lengthSq <= 0.000001f ) {
        return { 0.0f, 0.0f, -1.0f };
    }

    const float invLength = 1.0f / std::sqrt(lengthSq);
    return { x * invLength, y * invLength, z * invLength };
}
}  // namespace

/// @brief OpenAL 后端运行时句柄。
class ALBackend
{
public:
    /// @brief OpenAL 设备句柄。
    ALCdevice* device{ nullptr };

    /// @brief OpenAL 上下文句柄。
    ALCcontext* context{ nullptr };

    /// @brief OpenAL 声源句柄。
    ALuint source{ 0 };

    /// @brief OpenAL 队列 buffer 句柄。
    std::array<ALuint, kOpenALBufferCount> buffers{};

    /// @brief buffer 句柄是否已创建。
    bool buffersReady{ false };

    /// @brief 是否支持 float32 buffer 扩展。
    bool floatFormatSupported{ false };

    /// @brief 空间化参数缓存。
    ALSpatialState spatialState;

    /// @brief 保护 OpenAL 上下文和源访问。
    std::mutex alMutex;
};

std::atomic<bool> ALPlayer::al_inited{ false };

ALPlayer::ALPlayer(const AudioDataFormat& format)
    : IReceiver(format)
    , m_playFormat(format)
    , m_backend(std::make_unique<ALBackend>())
{
    m_buffer.resize(m_playFormat,
                    std::max<uint32_t>(ICEConfig::default_buffer_size,
                                       kOpenALMinBufferFrames));
}

ALPlayer::~ALPlayer()
{
    close();
}

bool ALPlayer::init_backend()
{
    configureOpenALSoftEnvironment();
    installOpenALSoftLogCallback();
    al_inited.store(true);
    return true;
}

void ALPlayer::quit_backend()
{
    al_inited.store(false);
}

std::vector<ALAudioDeviceInfo> ALPlayer::list_devices()
{
    const bool enumerateAll =
        alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT") == AL_TRUE;
    const bool enumerateDefault =
        alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT") == AL_TRUE;

    std::vector<ALAudioDeviceInfo> devices;
    if ( enumerateAll ) {
        devices =
            parseDeviceList(alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER));
    } else if ( enumerateDefault ) {
        devices = parseDeviceList(alcGetString(nullptr, ALC_DEVICE_SPECIFIER));
    }

    if ( devices.empty() ) {
        const ALCchar* defaultDevice =
            alcGetString(nullptr, ALC_DEFAULT_DEVICE_SPECIFIER);
        if ( defaultDevice ) {
            devices.push_back({ std::string(defaultDevice) });
        }
    }

    return devices;
}

bool ALPlayer::open()
{
    if ( open({}) ) {
        return true;
    }

    const std::string defaultError = m_lastError;
    const auto        devices      = list_devices();
    std::string       fallbackErrors;

    for ( const auto& device : devices ) {
        if ( device.name.empty() ) {
            continue;
        }

        if ( open(device.name) ) {
            return true;
        }

        if ( !fallbackErrors.empty() ) {
            fallbackErrors += " ";
        }
        fallbackErrors += fmt::format("[{}: {}]", device.name, m_lastError);
    }

    m_lastError = defaultError;
    if ( !fallbackErrors.empty() ) {
        m_lastError += " Fallback devices also failed: ";
        m_lastError += fallbackErrors;
    }
    return false;
}

const std::string& ALPlayer::getLastError() const
{
    return m_lastError;
}

const std::string& ALPlayer::getOpenedDeviceName() const
{
    return m_openedDeviceName;
}

bool ALPlayer::open(std::string_view deviceName)
{
    m_lastError.clear();
    m_openedDeviceName.clear();
    resetOpenALSoftLog();

    if ( m_backend->device || m_backend->context ) {
        close();
    }

    std::string deviceNameStorage(deviceName);
    const char* openName =
        deviceNameStorage.empty() ? nullptr : deviceNameStorage.c_str();

    m_backend->device = alcOpenDevice(openName);
    if ( !m_backend->device ) {
        m_lastError = fmt::format(
            "OpenAL device open failed: {}.",
            deviceNameStorage.empty() ? "default device" : deviceNameStorage);
        appendOpenALSoftLogDetail(m_lastError);
        fmt::print("{}\n", m_lastError);
        return false;
    }

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

    const std::array<ALCint, 5> contextAttributes{ ALC_FREQUENCY,
                                                   static_cast<ALCint>(
                                                       m_playFormat.samplerate),
                                                   ALC_REFRESH,
                                                   60,
                                                   0 };
    m_backend->context =
        alcCreateContext(m_backend->device, contextAttributes.data());
    if ( !m_backend->context ) {
        m_lastError =
            formatALCErrorMessage("context creation", m_backend->device);
        appendOpenALSoftLogDetail(m_lastError);
        fmt::print("{}\n", m_lastError);
        alcCloseDevice(m_backend->device);
        m_backend->device = nullptr;
        return false;
    }

    if ( alcMakeContextCurrent(m_backend->context) != ALC_TRUE ) {
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

    alGetError();
    alGenSources(1, &m_backend->source);
    if ( !checkALError("alGenSources", &m_lastError) ) {
        appendOpenALSoftLogDetail(m_lastError);
        close();
        return false;
    }

    alGenBuffers(static_cast<ALsizei>(m_backend->buffers.size()),
                 m_backend->buffers.data());
    if ( !checkALError("alGenBuffers", &m_lastError) ) {
        appendOpenALSoftLogDetail(m_lastError);
        close();
        return false;
    }
    m_backend->buffersReady = true;

    m_backend->floatFormatSupported =
        alIsExtensionPresent("AL_EXT_FLOAT32") == AL_TRUE;

    const ALfloat orientation[] = { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    alListenerfv(AL_ORIENTATION, orientation);
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    applySpatialState();

    if ( !checkALError("open", &m_lastError) ) {
        appendOpenALSoftLogDetail(m_lastError);
        close();
        return false;
    }

    const ALCchar* openedDeviceName =
        alcGetString(m_backend->device, ALC_DEVICE_SPECIFIER);
    m_openedDeviceName =
        openedDeviceName ? openedDeviceName : deviceNameStorage;
    return true;
}

void ALPlayer::close()
{
    stop();

    std::scoped_lock lock(m_backend->alMutex);
    if ( !m_backend->context ) {
        return;
    }

    alcMakeContextCurrent(m_backend->context);
    if ( m_backend->source != 0 ) {
        alSourceStop(m_backend->source);
        clearQueuedBuffers();
        alDeleteSources(1, &m_backend->source);
        m_backend->source = 0;
    }

    if ( m_backend->buffersReady ) {
        alDeleteBuffers(static_cast<ALsizei>(m_backend->buffers.size()),
                        m_backend->buffers.data());
        m_backend->buffersReady = false;
        m_backend->buffers.fill(0);
    }

    alcMakeContextCurrent(nullptr);
    alcDestroyContext(m_backend->context);
    alcCloseDevice(m_backend->device);
    m_backend->context = nullptr;
    m_backend->device  = nullptr;
    m_openedDeviceName.clear();
}

bool ALPlayer::start()
{
    if ( m_running.load() ) {
        m_lastError = "OpenAL playback thread is already running.";
        return false;
    }

    if ( !m_backend->context || m_backend->source == 0 ||
         !m_backend->buffersReady ) {
        m_lastError =
            "OpenAL playback backend is not fully initialized before start.";
        return false;
    }

    m_lastError.clear();
    m_running.store(true);
    m_paused.store(false);
    m_rebuildQueuedBuffers.store(false);
    m_audioThread = std::thread(&ALPlayer::audio_thread_loop, this);
    return true;
}

void ALPlayer::stop()
{
    if ( !m_running.load() ) {
        return;
    }

    m_running.store(false);
    m_paused.store(false);

    if ( m_audioThread.joinable() ) {
        m_audioThread.join();
    }
}

bool ALPlayer::is_running() const
{
    return m_running.load();
}

void ALPlayer::pause()
{
    if ( !m_running.load() || m_paused.load() ) {
        return;
    }

    m_paused.store(true);
    std::scoped_lock lock(m_backend->alMutex);
    if ( m_backend->context && m_backend->source != 0 ) {
        alcMakeContextCurrent(m_backend->context);
        alSourcePause(m_backend->source);
    }
}

void ALPlayer::resume()
{
    if ( !m_running.load() || !m_paused.load() ) {
        return;
    }

    m_paused.store(false);
    std::scoped_lock lock(m_backend->alMutex);
    if ( m_backend->context && m_backend->source != 0 ) {
        alcMakeContextCurrent(m_backend->context);
        alSourcePlay(m_backend->source);
    }
}

void ALPlayer::set_spatial_output_enabled(bool enabled)
{
    const bool previous = m_spatialOutputEnabled.exchange(enabled);
    applySpatialState();
    if ( previous != enabled ) {
        m_rebuildQueuedBuffers.store(true);
    }
}

bool ALPlayer::is_spatial_output_enabled() const
{
    return m_spatialOutputEnabled.load();
}

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
        m_backend->spatialState.distance   = std::max(0.0f, distance);
        m_backend->spatialState.referenceDistance =
            std::max(0.001f, referenceDistance);
        m_backend->spatialState.maxDistance =
            std::max(m_backend->spatialState.referenceDistance, maxDistance);
        m_backend->spatialState.rolloffFactor = std::max(0.0f, rolloffFactor);
    }

    applySpatialState();
}

void ALPlayer::audio_thread_loop()
{
    configureOpenALThreadPriority();

    std::vector<float>        floatScratch;
    std::vector<std::int16_t> int16Scratch;
    floatScratch.reserve(m_buffer.num_frames() *
                         std::max<uint16_t>(2, m_playFormat.channels));
    int16Scratch.reserve(m_buffer.num_frames() *
                         std::max<uint16_t>(2, m_playFormat.channels));

    {
        std::scoped_lock lock(m_backend->alMutex);
        alcMakeContextCurrent(m_backend->context);
    }

    auto refillAllBuffers = [&]() {
        for ( const auto bufferId : m_backend->buffers ) {
            if ( bufferId != 0 ) {
                queueAudioBuffer(bufferId, floatScratch, int16Scratch);
            }
        }

        std::scoped_lock lock(m_backend->alMutex);
        if ( m_backend->source != 0 ) {
            alSourcePlay(m_backend->source);
        }
    };

    refillAllBuffers();

    while ( m_running.load() ) {
        if ( m_paused.load() ) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if ( m_rebuildQueuedBuffers.exchange(false) ) {
            {
                std::scoped_lock lock(m_backend->alMutex);
                if ( m_backend->source != 0 ) {
                    alSourceStop(m_backend->source);
                    clearQueuedBuffers();
                }
            }
            refillAllBuffers();
            continue;
        }

        ALint processed = 0;
        {
            std::scoped_lock lock(m_backend->alMutex);
            if ( m_backend->source != 0 ) {
                alGetSourcei(
                    m_backend->source, AL_BUFFERS_PROCESSED, &processed);
            }
        }

        while ( processed > 0 && m_running.load() ) {
            ALuint bufferId = 0;
            {
                std::scoped_lock lock(m_backend->alMutex);
                if ( m_backend->source != 0 ) {
                    alSourceUnqueueBuffers(m_backend->source, 1, &bufferId);
                }
            }

            if ( bufferId != 0 ) {
                queueAudioBuffer(bufferId, floatScratch, int16Scratch);
            }
            --processed;
        }

        {
            std::scoped_lock lock(m_backend->alMutex);
            if ( m_backend->source != 0 ) {
                ALint state  = AL_STOPPED;
                ALint queued = 0;
                alGetSourcei(m_backend->source, AL_SOURCE_STATE, &state);
                alGetSourcei(m_backend->source, AL_BUFFERS_QUEUED, &queued);
                if ( state != AL_PLAYING && queued > 0 ) {
                    alSourcePlay(m_backend->source);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    {
        std::scoped_lock lock(m_backend->alMutex);
        if ( m_backend->source != 0 ) {
            alSourceStop(m_backend->source);
            clearQueuedBuffers();
        }
        alcMakeContextCurrent(nullptr);
    }
}

bool ALPlayer::queueAudioBuffer(unsigned int               bufferId,
                                std::vector<float>&        floatScratch,
                                std::vector<std::int16_t>& int16Scratch)
{
    {
        std::scoped_lock<std::mutex> lock(m_sourceMutex);
        m_buffer.clear();
        if ( get_source() ) {
            get_source()->process(m_buffer);
        }
    }

    const auto     frames         = m_buffer.num_frames();
    const auto     inputChannels  = m_buffer.num_channels();
    const bool     spatialOutput  = m_spatialOutputEnabled.load();
    const bool     monoOutput     = spatialOutput || inputChannels != 2;
    const uint16_t outputChannels = monoOutput ? 1 : 2;
    const auto     format =
        selectALFormat(outputChannels, m_backend->floatFormatSupported);

    const float* const* planarData = m_buffer.raw_ptrs();
    if ( m_backend->floatFormatSupported ) {
        floatScratch.resize(frames * outputChannels);
        if ( monoOutput ) {
            for ( size_t i = 0; i < frames; ++i ) {
                float sum = 0.0f;
                for ( uint16_t ch = 0; ch < inputChannels; ++ch ) {
                    sum += planarData[ch][i];
                }
                floatScratch[i] = inputChannels == 0
                                      ? 0.0f
                                      : sum / static_cast<float>(inputChannels);
            }
        } else {
            for ( size_t i = 0; i < frames; ++i ) {
                floatScratch[i * 2 + 0] = planarData[0][i];
                floatScratch[i * 2 + 1] = planarData[1][i];
            }
        }

        std::scoped_lock lock(m_backend->alMutex);
        if ( m_backend->source == 0 ) {
            return false;
        }
        alBufferData(bufferId,
                     format,
                     floatScratch.data(),
                     static_cast<ALsizei>(floatScratch.size() * sizeof(float)),
                     static_cast<ALsizei>(m_playFormat.samplerate));
        const ALuint alBufferId = static_cast<ALuint>(bufferId);
        alSourceQueueBuffers(m_backend->source, 1, &alBufferId);
        return checkALError("queue float buffer");
    }

    int16Scratch.resize(frames * outputChannels);
    if ( monoOutput ) {
        for ( size_t i = 0; i < frames; ++i ) {
            float sum = 0.0f;
            for ( uint16_t ch = 0; ch < inputChannels; ++ch ) {
                sum += planarData[ch][i];
            }
            const float sample = inputChannels == 0
                                     ? 0.0f
                                     : sum / static_cast<float>(inputChannels);
            int16Scratch[i]    = floatToInt16(sample);
        }
    } else {
        for ( size_t i = 0; i < frames; ++i ) {
            int16Scratch[i * 2 + 0] = floatToInt16(planarData[0][i]);
            int16Scratch[i * 2 + 1] = floatToInt16(planarData[1][i]);
        }
    }

    std::scoped_lock lock(m_backend->alMutex);
    if ( m_backend->source == 0 ) {
        return false;
    }
    alBufferData(
        bufferId,
        format,
        int16Scratch.data(),
        static_cast<ALsizei>(int16Scratch.size() * sizeof(std::int16_t)),
        static_cast<ALsizei>(m_playFormat.samplerate));
    const ALuint alBufferId = static_cast<ALuint>(bufferId);
    alSourceQueueBuffers(m_backend->source, 1, &alBufferId);
    return checkALError("queue int16 buffer");
}

void ALPlayer::clearQueuedBuffers()
{
    if ( m_backend->source == 0 ) {
        return;
    }

    ALint queued = 0;
    alGetSourcei(m_backend->source, AL_BUFFERS_QUEUED, &queued);
    while ( queued > 0 ) {
        ALuint bufferId = 0;
        alSourceUnqueueBuffers(m_backend->source, 1, &bufferId);
        --queued;
    }
}

void ALPlayer::applySpatialState()
{
    std::scoped_lock lock(m_backend->alMutex);
    if ( !m_backend->context || m_backend->source == 0 ) {
        return;
    }

    alcMakeContextCurrent(m_backend->context);
    const bool spatialOutput = m_spatialOutputEnabled.load();
    const auto direction =
        normalizeDirection(m_backend->spatialState.directionX,
                           m_backend->spatialState.directionY,
                           m_backend->spatialState.directionZ);
    const float distance = std::max(0.0f, m_backend->spatialState.distance);

    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    alSourcei(m_backend->source, AL_SOURCE_RELATIVE, AL_TRUE);
    if ( spatialOutput ) {
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
    } else {
        alSource3f(m_backend->source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSource3f(m_backend->source, AL_DIRECTION, 0.0f, 0.0f, -1.0f);
        alSourcef(m_backend->source, AL_REFERENCE_DISTANCE, 1.0f);
        alSourcef(m_backend->source, AL_MAX_DISTANCE, 100.0f);
        alSourcef(m_backend->source, AL_ROLLOFF_FACTOR, 0.0f);
    }
}

}  // namespace ice
