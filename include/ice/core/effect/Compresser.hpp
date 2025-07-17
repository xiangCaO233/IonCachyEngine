#ifndef ICE_COMPRESSER_HPP
#define ICE_COMPRESSER_HPP

#include <atomic>

#include "ice/config/config.hpp"
#include "ice/core/effect/IEffectNode.hpp"
namespace ice {

class Compressor : public IEffectNode {
   public:
    // 控制接口
    // 阈值 (dB)
    inline void set_threshold_db(float db) {
        threshold_db.store(db);
        needs_update = true;
    }
    // 压缩比 (e.g., 4.0 for 4:1)
    inline void set_ratio(float vratio) {
        ratio.store(vratio);
        needs_update = true;
    }
    // 启动时间 (毫秒)
    inline void set_attack_ms(float ms) {
        attack_ms.store(ms);
        needs_update = true;
    }
    // 释放时间 (毫秒)
    inline void set_release_ms(float ms) {
        release_ms.store(ms);
        needs_update = true;
    }
    // 补充增益 (dB)
    inline void set_makeup_gain_db(float db) {
        makeup_gain_db.store(db);
        needs_update = true;
    }

   protected:
    // 应用压缩器效果
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

   private:
    // 内部状态和参数
    std::atomic<float> threshold_db{0.0f};
    std::atomic<float> ratio{1.0f};
    std::atomic<float> attack_ms{5.0f};
    std::atomic<float> release_ms{100.0f};
    std::atomic<float> makeup_gain_db{0.0f};

    // 包络跟随器 (Envelope Follower) 的状态
    // 这个状态必须在 process 调用之间保持
    // 每个声道都需要自己的状态
    std::vector<double> envelope_db;

    // 根据 ms 计算出的,用于平滑的系数
    double attack_coeff = 0.0;
    double release_coeff = 0.0;
    double makeup_gain_linear = 1.0;

    double sample_rate{
        static_cast<double>(ICEConfig::internal_format.samplerate)};
    // 系数是否需要重新计算
    bool needs_update = true;
};
}  // namespace ice

#endif  // ICE_COMPRESSER_HPP
