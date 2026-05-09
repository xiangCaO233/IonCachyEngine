#include <cstring>
#include <ice/core/IAudioNode.hpp>
#include <ice/core/MixBus.hpp>
#include <vector>

namespace ice
{
void MixBus::process(AudioBuffer& buffer)
{
    // 先清空buffer
    buffer.clear();

    // 为了防止在调用 source->process() 时因死锁或长时间持有锁导致音频线程卡顿，
    // 我们先加锁拷贝一份 shared_ptr 列表，然后释放锁再依次处理
    std::vector<std::shared_ptr<IAudioNode>> sources_copy;
    {
        std::lock_guard<std::mutex> lock(sources_mutex);
        sources_copy.assign(sources.begin(), sources.end());
    }

    // 拉取全部来源数据
    for ( const auto& source : sources_copy ) {
        temp_buffer.clear();
        temp_buffer.resize(buffer.afmt, buffer.num_frames());
        temp_buffer.clear();
        // 拉取输入源的数据到缓冲
        source->process(temp_buffer);

        // 混合到上游请求的buffer中
        buffer += temp_buffer;
    }

    // 应用静音
    if ( m_muteLeft || m_muteRight ) {
        float** ptns = buffer.raw_ptrs();
        if ( ptns ) {
            if ( m_muteLeft && buffer.num_channels() > 0 ) {
                std::memset(ptns[0], 0, buffer.num_frames() * sizeof(float));
            }
            if ( m_muteRight && buffer.num_channels() > 1 ) {
                std::memset(ptns[1], 0, buffer.num_frames() * sizeof(float));
            }
        }
    }

    // 计算峰值响度 (Peak Level)
    float   maxL = 0.0f;
    float   maxR = 0.0f;
    float** data = buffer.raw_ptrs();
    if ( data ) {
        if ( buffer.num_channels() > 0 ) {
            for ( size_t i = 0; i < buffer.num_frames(); ++i ) {
                float v = std::abs(data[0][i]);
                if ( v > maxL ) maxL = v;
            }
        }
        if ( buffer.num_channels() > 1 ) {
            for ( size_t i = 0; i < buffer.num_frames(); ++i ) {
                float v = std::abs(data[1][i]);
                if ( v > maxR ) maxR = v;
            }
        } else if ( buffer.num_channels() > 0 ) {
            maxR = maxL;
        }
    }

    // 更新电平 (简单衰减算法，让 UI 显示更平滑)
    // 注意：这里是在音频线程，我们只需存入瞬时峰值或缓慢衰减
    float oldL = m_leftLevel.load();
    float oldR = m_rightLevel.load();

    // 如果新值大则瞬间上升，否则缓慢下降
    m_leftLevel.store(maxL > oldL ? maxL : oldL * 0.95f);
    m_rightLevel.store(maxR > oldR ? maxR : oldR * 0.95f);
}

void MixBus::add_source(std::shared_ptr<IAudioNode> src)
{
    std::lock_guard<std::mutex> lock(sources_mutex);
    sources.insert(src);
}

void MixBus::remove_source(std::shared_ptr<IAudioNode> src)
{
    std::lock_guard<std::mutex> lock(sources_mutex);
    sources.erase(src);
}
}  // namespace ice
