# 🧊 Ice Audio Engine

一个为性能、控制力和模块化而生的现代C++20音频框架。

Ice Audio是一个**模块化的工具箱** 🧰提供构建简单音效播放器/复杂实时音频处理图等各种应用的*高性能*/*实时*/*高精度*的底层组件

本框架的核心设计哲学是：
*   **极致性能**: 所有热点路径都经过精心设计，以实现最大缓存亲和性、最小化内存分配和SIMD友好性。
*   **生命周期**: 可以完全控制所有核心服务（如线程池、后端上下文）的创建和销毁顺序，杜绝死锁和资源泄漏。
*   **高度解耦**: 核心逻辑与具体实现（如FFmpeg解码、SDL3播放）完全分离。
*   **强大控制**: 可以按需组装、配置和扩展。

## 核心概念

框架有以下几个核心概念

| 图标 | 概念 | 描述 |
| :--- | :--- | :--- |
| 🎶 | **音频点图** | 整个引擎基于一个由`IAudioNode`构成的“拉取式”处理图。数据从源头(`SourceNode`)流经效果器(`IEffectNode`)和混音器(均属于IAudioNode)，最终被接收器(`IReceiver`)拉取并输出 |
| 📦 | **异步管理** | 音频文件的加载和解码是昂贵的操作。框架通过`AudioPool`和内部的`ThreadPool`，以异步、非阻塞的方式在后台加载数据，并支持**完全缓存**和**流式播放**两种策略|
| 🔌 | **后端抽象** | 音频解码和播放与具体的第三方库完全解耦。通过`IDecoderFactory`和`IReceiver`接口，可以替换或增加新的后端（如从SDL3切换到OpenAL），而无需改动任何核心逻辑 |
| 🎛️ | **实时安全** | 所有可能在实时音频线程（热路径）中调用的函数，都经过精心设计，避免内存分配、锁争用、文件I/O等任何可能导致不确定延迟的操作 |

## 组件工具箱 🧰

可以直接使用的核心组件:

| 组件名 | 所属概念 | 核心职责 |
| :--- | :--- | :--- |
| `ice::ThreadPool` | 核心服务 | 通用的线程池，用于执行异步任务（如`CachyDecoder`的后台解码）|
| `ice::AudioPool` | 资源管理 | 高效缓存`AudioTrack`资源，避免重复加载。处理并发请求，并协调异步加载流程 |
| `ice::AudioTrack` | 资源管理 | 音频文件的逻辑表示。包含元数据（格式、时长），并持有一个解码策略（`IDecoder`）|
| `ice::SourceNode` | 音频图节点 | 音频处理图的起点。代表一个可播放的音源实例，管理着播放位置、循环、音量等状态|
| `ice::IEffectNode` | 音频图节点 | 所有效果器的基类。定义了效果器链的结构|
| `ice::GraphicEqualizer` | 效果器 | 一个多频段图形均衡器，用于调整音色|
| `ice::TimeStretcher` | 效果器 | 一个变速不变调效果器|
| `ice::Compressor` | 效果器 | 一个动态范围压缩器，用于控制音量动态|
| `ice::IReceiver` | 后端抽象 | 所有输出端的基类，如音频播放或文件写入器|
| `ice::SDLPlayer` | 接收器实现| 一个具体的接收器，负责将最终的音频数据通过SDL3 API发送到声卡|
| `ice::AudioBuffer` | 核心工具 | 一个高性能的音频数据容器，内部使用单一、对齐的连续内存块，为SIMD优化做了准备|


## 核心接口参考 📜

框架中的全部抽象接口

#### `IAudioNode` - 音频处理图的基础
```cpp
class IAudioNode {
public:
    // 从此节点拉取一块音频数据，填充到buffer中
    virtual void process(AudioBuffer& buffer) = 0;
};
```

#### `IEffectNode` - 效果器的基类-可以在此接口扩展其他效果器
```cpp
class IEffectNode : public IAudioNode {
public:
    // 设置效果器的上游输入节点
    void set_inputnode(std::shared_ptr<IAudioNode> input);
protected:
    // 派生类需实现的效果处理
    virtual void apply_effect(AudioBuffer& output, const AudioBuffer& input) = 0;
};
```

#### `IReceiver` - 所有输出端的接口-可以直接是实现为sdl播放器或是实现为openal的音源
```cpp
class IReceiver {
public:
    // 打开后端设备或文件
    virtual bool open() = 0;
    // 关闭后端
    virtual void close() = 0;
    
    // 开始拉取数据并播放/写入
    virtual bool start() = 0;
    // 停止拉取
    virtual void stop() = 0;

    // 查询运行状态
    virtual bool is_running() const = 0;

    // 设置要从中拉取数据的最终节点
    void set_source(std::shared_ptr<IAudioNode> source);
};
```

## 用法示例 🚀

下面是一些典型的使用场景，展示了如何组装这些组件

### 1：简单的音频播放

基础用法：加载一个音频文件并播放
```cpp
#include <iostream>
#include <thread>
#include <chrono>

#include "ice/thread/ThreadPool.hpp"
#include "ice/manage/AudioPool.hpp"
#include "ice/core/SourceNode.hpp"
#include "ice/out/play/sdl/SDLPlayer.hpp"

int main() {
    try {
        // 创建线程池
        ice::ThreadPool thread_pool(8);

        // 创建资源池
        ice::AudioPool audio_pool(ice::CodecBackend::FFMPEG);

        // 加载一个音轨
        auto music_track = audio_pool.get_or_load(
            thread_pool, 
            "music.mp3"
        );
        if (!music_track) {
            std::cerr << "Failed to load track." << std::endl;
            return 1;
        }

        // 基于音轨创建音源节点
        auto music_source = std::make_shared<ice::SourceNode>(music_track);
        // 设置循环播放
        music_source->set_looping(true); 

        // 初始化sdl后端
        if(ice::SDLPlayer::init_backend()) {
            std::cerr << "Failed to init sdl3." << std::endl;
            return 1;
        }

        // 创建SDL播放器
        auto player = std::make_unique<ice::SDLPlayer>();
        // 打开默认音频设备
        if (!player->open()) { 
            std::cerr << "Failed to open audio device." << std::endl;
            return 1;
        }

        // 直接将音源连接到接收器
        player->set_source(music_source);

        // 开始播放并等待(或是分离线程去干别的)
        player->start();
        std::cout << "Playing 15 seconds..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(15));
        
        player->stop();
        player->close();
        ice::SDLPlayer::quit_backend();
        
    } catch (const std::exception& e) {
        std::cerr << "An error occurred: " << e.what() << std::endl;
        return 1;
    }

    return 0; 
}
```

### 2：使用效果器

在音源和接收器之间插入一个图形均衡器。

```cpp

// 创建一个10段图形均衡器
std::vector<double> freqs = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
auto graphic_eq = std::make_shared<ice::GraphicEqualizer>(freqs);

// 将音源连接到EQ
graphic_eq->set_inputnode(music_source);

// 调整EQ参数，做一个"V"形EQ，提升低频和高频
graphic_eq->set_band_gain_db(0, 4.0f); // 提升31Hz
graphic_eq->set_band_gain_db(1, 2.0f); // 提升62Hz
graphic_eq->set_band_gain_db(8, 2.0f); // 提升8kHz
graphic_eq->set_band_gain_db(9, 4.0f); // 提升16kHz

// 将EQ节点连接到接收器
player->set_source(graphic_eq);

```

### 3：使用混音器

在多个音源和接收器之间使用一个混音器融合音频

```cpp

// 创建一个混音器
std::shared_ptr<ice::MixBus> mixer = std::make_shared<ice::MixBus>();

// 添加混音器的音源
mixer->add_source(music_source1);
mixer->add_source(music_source2);
// ...

// 混音器作为一个IAudioNode
// 可以作为音源继续连接其他效果器或混音器
std::shared_ptr<ice::MixBus> mixer2 = std::make_shared<ice::MixBus>();
// graphic_eq->set_inputnode(mixer);
mixer2->add_source(graphic_eq);
mixer2->add_source(music_source3);

// ...然后在想要的结果处连接接收器
player.set_source(mixer2);

```

### 4：选择特定输出设备

```cpp
// 查询可用的设备
auto devices = ice::SDLPlayer::list_devices();
if (devices.empty()) {
    std::cerr << "No playback devices found." << std::endl;
    return 1;
}

std::cout << "Please select a device ID:\n";
for (const auto& dev : devices) {
    std::cout << "- ID: " << dev.id << ", Name: " << dev.name << std::endl;
}

uint32_t selected_id = 0;
std::cin >> selected_id; // 简单地从控制台读取选择

// 创建接收器并打开指定的设备
auto player = std::make_unique<ice::SDLPlayer>();
if (!player->open(selected_id)) {
    std::cerr << "Failed to open selected device, falling back to default." << std::endl;
    // 尝试打开默认设备
    if (!player->open()) { 
        return 1;
    }
}
```
