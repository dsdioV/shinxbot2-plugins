#include "nikohug.h"

#include "nikohug_assets.h"
#include "utils.h"

#include <Magick++.h>
#include <fmt/core.h>

#include <chrono>
#include <exception>
#include <fstream>
#include <string>

namespace {

// ---- 几何参数（与上游 qq_nikohugapi 保持一致）----
constexpr int kCanvas = 486;     // 画布边长
constexpr int kAvatarSize = 300; // 头像裁剪尺寸

// 上游 PIL 用 rotate(+10)（逆时针）。ImageMagick 的 rotate 正方向与之相反，
// 故取负值以得到同样的视觉结果（已逐像素对照验证）。
constexpr double kAvatarRotateDeg = -10.0;

constexpr int kAvatarX = -35; // 头像左上角坐标（可为负，允许出血）
constexpr int kAvatarY = 250;

// 上游 PIL expand 后的画布边长（Pillow 对 300x300 旋转 10° 的结果）
constexpr int kAvatarRotatedSize = 348;

const char *kAvatarUrlTemplate = "https://q1.qlogo.cn/g?b=qq&nk={}&s=640";

/// 标准 base64 解码。
std::string base64_decode(const std::string &in)
{
    auto val = [](const char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::string out;
    out.reserve(in.size() * 3 / 4);

    int buf = 0, bits = 0;
    for (const char c : in) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) continue; // 跳过换行/空白

        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

/// 把 base64 内容写入 path（若已存在则跳过）。
void ensure_file(const std::string &path, const char *const b64)
{
    if (fs::exists(path) && fs::file_size(path) > 0) {
        return; // 运维可自行替换素材
    }

    const std::string raw = base64_decode(b64);
    if (raw.empty()) {
        throw std::runtime_error("embedded overlay is empty: " + path);
    }

    fs::create_directories(fs::path(path).parent_path());
    std::ofstream of(path, std::ios::binary | std::ios::trunc);
    if (!of) {
        throw std::runtime_error("cannot write overlay: " + path);
    }
    of.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    of.close();
}

/// 把 avatar 转成「圆形 + 旋转」的 RGBA 图层。
Magick::Image make_avatar_layer(const std::string &avatar_path)
{
    Magick::Image avatar;
    avatar.read(avatar_path);
    avatar.resize(Magick::Geometry(kAvatarSize, kAvatarSize));
    avatar.alpha(true);

    // 圆形蒙版：白色实心圆 + DstIn，仅保留圆内像素。
    Magick::Image mask(Magick::Geometry(kAvatarSize, kAvatarSize),
                       Magick::Color(0, 0, 0, 0));
    mask.fillColor(Magick::Color("white"));
    mask.strokeColor(Magick::Color("none"));
    mask.draw(Magick::DrawableCircle(kAvatarSize / 2, kAvatarSize / 2,
                                     kAvatarSize / 2, 0));
    avatar.composite(mask, 0, 0, Magick::DstInCompositeOp);

    // ImageMagick 的 rotate 会自动扩张画布（约 350x350），效果等同 PIL 的
    // expand=True。注意：不要在此前手工补边，否则变换后画布尺寸变化会让
    // 合成时的左上角偏移错位——位置统一由 make_avatar_layer 的中心点换算。
    avatar.backgroundColor(Magick::Color(0, 0, 0, 0));
    avatar.filterType(Magick::CubicFilter);
    avatar.rotate(kAvatarRotateDeg);

    return avatar;
}

/// 叠加一张 Overlay，居中放置。
void composite_overlay(Magick::Image &canvas, const std::string &path)
{
    Magick::Image overlay(path);
    if (overlay.columns() == 0 || overlay.rows() == 0) {
        throw std::runtime_error("overlay is empty: " + path);
    }

    const int x = (kCanvas - static_cast<int>(overlay.columns())) / 2;
    const int y = (kCanvas - static_cast<int>(overlay.rows())) / 2;
    canvas.composite(overlay, x, y, Magick::OverCompositeOp);
}

} // namespace

userid_t nikohug::resolve_target(const std::string &message,
                                 const msg_meta &conf)
{
    // 1) 优先 [CQ:at,qq=xxx] —— 与 NGgame / img_fun 的做法一致。
    const size_t at = message.find("[CQ:at,qq=");
    if (at != std::string::npos) {
        const size_t end = message.find(']', at);
        if (end != std::string::npos) {
            const userid_t uid =
                extract_qq_from_at_segment(message.substr(at, end - at + 1));
            if (uid != 0) {
                return uid;
            }
        }
    }

    // 2) 其次取消息中第一段连续数字作为 QQ 号。
    std::string digits;
    for (const char c : message) {
        if (is_digit(c)) {
            digits += c;
        }
        else if (!digits.empty()) {
            break;
        }
    }
    if (digits.size() >= 5 && digits.size() <= 12) {
        const userid_t uid = my_string2uint64(digits);
        if (uid != 0) {
            return uid;
        }
    }

    // 3) 兜底：发送者自己。
    return conf.user_id;
}

void nikohug::materialize_overlays(std::string &out1, std::string &out2)
{
    const std::string dir = bot_resource_path(nullptr, "nikohug");
    out1 = dir + "/0.png";
    out2 = dir + "/1.png";

    ensure_file(out1, kNikohugOverlay0B64);
    ensure_file(out2, kNikohugOverlay1B64);
}

void nikohug::compose(const std::string &avatar_path,
                      const std::string &overlay1,
                      const std::string &overlay2,
                      const std::string &out_png)
{
    Magick::Image avatar = make_avatar_layer(avatar_path);

    // 上游把「旋转后 348x348」贴到 (-35, 250)，即内容中心在
    // (139, 424)。这里按中心点反推左上角，避免依赖具体的扩张尺寸。
    const int cx = kAvatarX + kAvatarRotatedSize / 2;
    const int cy = kAvatarY + kAvatarRotatedSize / 2;
    const int ax = cx - static_cast<int>(avatar.columns()) / 2;
    const int ay = cy - static_cast<int>(avatar.rows()) / 2;

    Magick::Image canvas(Magick::Geometry(kCanvas, kCanvas),
                         Magick::Color(0, 0, 0, 0));
    canvas.composite(avatar, ax, ay, Magick::OverCompositeOp);

    composite_overlay(canvas, overlay1);
    composite_overlay(canvas, overlay2);

    canvas.magick("PNG");
    canvas.write(out_png);
}

void nikohug::process(std::string message, const msg_meta &conf)
{
    Json::Value J;
    J["message_id"] = conf.message_id;
    conf.p->cq_send("mark_msg_as_read", J);

    const std::string download_dir = bot_resource_path(nullptr, "download");
    try {
        fs::create_directories(download_dir);
    }
    catch (...) {
    }

    const userid_t target = resolve_target(message, conf);

    std::string o1, o2;
    try {
        materialize_overlays(o1, o2);
    }
    catch (const std::exception &e) {
        conf.p->setlog(LOG::ERROR, std::string("nikohug: ") + e.what());
        conf.p->cq_send(std::string("叠加素材准备失败: ") + e.what(), conf);
        return;
    }

    const std::string stem =
        download_dir + "/nikohug_" + std::to_string(target);
    const std::string avatar_path = stem + "_avatar.png";
    const std::string out_png = stem + ".png";

    auto t0 = std::chrono::steady_clock::now();
    auto ms_since = [&t0]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
            .count();
    };

    try {
        download(fmt::format(kAvatarUrlTemplate, target), download_dir,
                 "nikohug_" + std::to_string(target) + "_avatar.png");
        const auto t_dl = ms_since();
        compose(avatar_path, o1, o2, out_png);
        conf.p->setlog(LOG::INFO,
                       fmt::format("nikohug timing: download={}ms compose={}ms",
                                   t_dl, ms_since() - t_dl));
    }
    catch (const std::exception &e) {
        conf.p->setlog(LOG::ERROR, std::string("nikohug: ") + e.what());
        conf.p->cq_send("生成失败，可能是头像获取不到（QQ 号写错或非好友）", conf);
        std::error_code ec;
        fs::remove(avatar_path, ec);
        return;
    }

    const auto t_send_begin = ms_since();
    conf.p->cq_send(fmt::format("[CQ:image,file=file://{},id=40000]",
                                fs::absolute(fs::path(out_png)).string()),
                    conf);
    conf.p->setlog(LOG::INFO,
                   fmt::format("nikohug timing: send_call={}ms total_before_send={}ms",
                               ms_since() - t_send_begin, t_send_begin));
    conf.p->setlog(LOG::INFO,
                   fmt::format("nikohug at g{} by {} -> {}", conf.group_id,
                               conf.user_id, target));

    std::error_code ec;
    fs::remove(avatar_path, ec);
    fs::remove(out_png, ec);
}

bool nikohug::check(std::string message, const msg_meta &conf)
{
    (void)conf;
    return cmd_match_prefix(message, {"nikohug"});
}

std::string nikohug::help()
{
    return "Niko 抱抱。nikohug / nikohug <QQ号> / nikohug @某人";
}

DECLARE_FACTORY_FUNCTIONS(nikohug)
