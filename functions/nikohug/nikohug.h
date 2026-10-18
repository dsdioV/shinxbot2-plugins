#pragma once

#include "processable.h"

/**
 * nikohug: 生成 "Niko 抱抱" 图。
 *
 * 用法:
 *   nikohug            抱自己
 *   nikohug 114514     抱指定 QQ 号
 *   nikohug @某人       抱被 at 的人
 *
 * 图像合成完全在本地完成 (Magick++)，不依赖上游 HTTP API。
 * 叠加素材已内嵌进 .so，来源与许可见 README.md / LICENSE-ASSETS。
 */
class nikohug : public processable {
  private:
    /// 解析消息中的目标 QQ：@某人 > 纯数字 > 发送者自己。
    static userid_t resolve_target(const std::string &message,
                                   const msg_meta &conf);
    /// 把内嵌的 base64 叠加图落到 resource/nikohug/ 并返回本地路径。
    /// 已存在则直接复用，便于运维替换素材。
    static void materialize_overlays(std::string &out1, std::string &out2);
    /// 合成并写入 out_png。
    static void compose(const std::string &avatar_path,
                        const std::string &overlay1,
                        const std::string &overlay2,
                        const std::string &out_png);

  public:
    void process(std::string message, const msg_meta &conf) override;
    bool check(std::string message, const msg_meta &conf) override;
    std::string help() override;
};

DECLARE_FACTORY_FUNCTIONS_HEADER
