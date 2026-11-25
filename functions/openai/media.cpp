#include "gpt3_5.h"
#include "cqscan.h"
#include "openai_internal.h"

#include <Magick++.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// 读文件开头若干字节，用来嗅探图片格式。失败返回空串。
std::string read_binary_prefix(const std::string &path, size_t n)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::string buf(n, '\0');
    in.read(&buf[0], static_cast<std::streamsize>(n));
    buf.resize(static_cast<size_t>(in.gcount()));
    return buf;
}

/// 按魔术字节判断图片格式，返回 ImageMagick 认识的格式名。
std::string sniff_image_magick(const std::string &head)
{
    auto starts = [&head](const char *magic, size_t len) {
        return head.size() >= len && head.compare(0, len, magic, len) == 0;
    };
    if (starts("\xFF\xD8\xFF", 3)) return "JPEG";
    if (starts("\x89PNG\r\n\x1A\n", 8)) return "PNG";
    if (starts("GIF87a", 6) || starts("GIF89a", 6)) return "GIF";
    if (starts("BM", 2)) return "BMP";
    if (head.size() >= 12 && starts("RIFF", 4) && head.compare(8, 4, "WEBP") == 0)
        return "WEBP";
    if (starts("II*\x00", 4) || starts("MM\x00*", 4)) return "TIFF";
    return "";
}

void scan_for_image_urls(const Json::Value &node,
                         std::vector<std::string> &urls, int depth)
{
    if (depth > 8) return;

    if (node.isArray()) {
        for (const auto &item : node) {
            scan_for_image_urls(item, urls, depth + 1);
        }
        return;
    }
    if (!node.isObject()) return;

    const Json::Value &data = node["data"];
    if (data.isObject()) {
        std::string url = openai_internal::json_to_string(data["url"]);
        const bool looks_image =
            !url.empty() &&
            (data.isMember("file") || data.isMember("file_size") ||
             data.get("sub_type", Json::Value()).isInt());
        if (looks_image) {
            if (std::find(urls.begin(), urls.end(), url) == urls.end()) {
                urls.push_back(url);
            }
        }
    }

    for (const char *key : {"message", "content", "data", "messages"}) {
        if (node.isMember(key)) {
            scan_for_image_urls(node[key], urls, depth + 1);
        }
    }
}

} // namespace

std::string gpt3_5::media_dir() const
{
    return bot_config_path(nullptr, "gpt3_5/media");
}

Json::Value gpt3_5::store_image(const std::string &url, const msg_meta &conf)
{
    if (url.empty()) return Json::Value();

    const std::string dir = media_dir();
    const std::string tmp_name = generate_uuid();
    const fs::path tmp_path = fs::path(dir) / (tmp_name + ".img");
    try {
        fs::create_directories(dir);
        download(url, dir, tmp_name + ".img");
    }
    catch (const std::exception &e) {
        conf.p->setlog(LOG::WARNING,
                       std::string("openai: image download failed: ") +
                           e.what());
        return Json::Value();
    }
    catch (...) {
        conf.p->setlog(LOG::WARNING, "openai: image download failed");
        return Json::Value();
    }

    Json::Value part;
    try {
        {
            std::error_code ec;
            auto sz = fs::file_size(tmp_path, ec);
            if (ec || sz < 64) {
                conf.p->setlog(LOG::WARNING,
                               "openai: image download returned no usable data");
                fs::remove(tmp_path, ec);
                return Json::Value();
            }
        }

        Magick::Image img;
        {
            std::string head;
            try {
                head = read_binary_prefix(tmp_path.string(), 12);
            }
            catch (...) {
                head.clear();
            }
            const std::string sniffed = sniff_image_magick(head);
            if (!sniffed.empty()) {
                img.magick(sniffed);
            }
        }
        img.read(tmp_path.string());
        if (img.columns() > static_cast<size_t>(media_max_edge) ||
            img.rows() > static_cast<size_t>(media_max_edge)) {
            const double scale = std::min(
                static_cast<double>(media_max_edge) / img.columns(),
                static_cast<double>(media_max_edge) / img.rows());
            const size_t w = std::max<size_t>(
                1, static_cast<size_t>(img.columns() * scale));
            const size_t h = std::max<size_t>(
                1, static_cast<size_t>(img.rows() * scale));
            img.resize(Magick::Geometry(w, h, 0, 0));
        }

        Magick::Blob blob;
        img.magick("JPEG");
        img.strip();
        img.quality(media_quality);
        img.write(&blob);

        static const size_t media_max_bytes = 400 * 1024;
        static const int quality_floor = 55;
        int q = media_quality;
        while (blob.length() > media_max_bytes && q > quality_floor) {
            q -= 10;
            Magick::Blob smaller;
            img.quality(q);
            img.write(&smaller);
            blob = smaller;
        }
        conf.p->setlog(
            LOG::INFO,
            "openai: image stored " + std::to_string(blob.length() / 1024) +
                "KB (" + std::to_string(img.columns()) + "x" +
                std::to_string(img.rows()) + " q" + std::to_string(q) + ")");

        std::string bytes(static_cast<const char *>(blob.data()), blob.length());
        if (bytes.size() < 64) {
            conf.p->setlog(LOG::WARNING, "openai: converted image is empty");
            part = Json::Value();
        }
        else {
            std::string sha = openai_internal::sha256_hex(bytes);
            std::string fname = sha + ".jpg";
            fs::path final_path = fs::path(dir) / fname;
            if (!fs::exists(final_path)) {
                openai_internal::write_binary(final_path, bytes);
            }

            part["type"] = "image_ref";
            part["file"] = fname;
            part["mime"] = "image/jpeg";
        }
    }
    catch (const std::exception &e) {
        conf.p->setlog(LOG::WARNING,
                       std::string("openai: image convert failed: ") +
                           e.what());
        part = Json::Value();
    }
    catch (...) {
        part = Json::Value();
    }

    std::error_code ec;
    fs::remove(tmp_path, ec);
    return part;
}

void gpt3_5::collect_images_from_message(const Json::Value &msg,
                                         std::vector<std::string> &urls)
{
    if (msg.isArray() || msg.isObject()) {
        scan_for_image_urls(msg, urls, 0);
        return;
    }

    std::string s = msg.isString() ? msg.asString() : "";
    if (s.empty()) return;
    for (const auto &seg : cqscan::split(s)) {
        if (!seg.is_cq || seg.type != "image") continue;
        std::string url = cqscan::arg_value(seg.args, "url");
        if (url.empty()) continue;
        urls.push_back(cq_decode(url));
    }
}

void gpt3_5::collect_forward_images(bot *p, const std::string &forward_id,
                                    std::vector<std::string> &urls, int depth)
{
    static thread_local std::set<std::string> visited;
    static thread_local int total_nodes = 0;
    if (depth == 0) {
        visited.clear();
        total_nodes = 0;
    }
    if (depth > 5) return;
    if (total_nodes >= 200) return;
    if (!visited.insert(forward_id).second) return;

    Json::Value param;
    param["id"] = forward_id;
    Json::Value info;
    try {
        info = string_to_json(p->cq_send("get_forward_msg", param));
    }
    catch (const std::string &e) {
        p->setlog(LOG::WARNING, "openai: get_forward_msg threw: " + e);
        return;
    }
    catch (...) {
        p->setlog(LOG::WARNING, "openai: get_forward_msg threw (unknown)");
        return;
    }

    if (openai_internal::json_to_int64(info["retcode"]) != 0 ||
        !info.isMember("data")) {
        p->setlog(LOG::WARNING,
                  "openai: forward retcode=" +
                      std::to_string(openai_internal::json_to_int64(
                          info["retcode"])) +
                      " has_data=" + std::to_string(info.isMember("data")) +
                      " body=" + info.toStyledString().substr(0, 300));
        return;
    }

    const Json::Value &data = info["data"];
    const Json::Value &messages =
        data.isMember("messages") ? data["messages"] : data;
    if (!messages.isArray()) {
        p->setlog(LOG::WARNING,
                  "openai: forward data is not array, keys=" +
                      Json::writeString(Json::StreamWriterBuilder(), data)
                          .substr(0, 200));
        return;
    }

    if (depth == 0) {
        if (!messages.empty()) {
            p->setlog(LOG::INFO,
                      "openai: forward sample node=" +
                          Json::writeString(Json::StreamWriterBuilder(),
                                            messages[0])
                              .substr(0, 400));
        }
        p->setlog(LOG::INFO,
                  "openai: forward nodes=" + std::to_string(messages.size()));
    }

    const size_t before = urls.size();
    for (const auto &m : messages) {
        if (!m.isObject()) continue;
        if (total_nodes >= 200) break;
        ++total_nodes;
        collect_images_from_message(m, urls);

        const Json::Value &body =
            m.isMember("message") ? m["message"] : m["content"];
        if (body.isArray()) {
            for (const auto &seg : body) {
                if (!seg.isObject()) continue;
                if (seg.get("type", "").asString() != "forward") continue;
                std::string nested = openai_internal::json_to_string(
                    seg["data"]["id"]);
                if (!nested.empty()) {
                    collect_forward_images(p, nested, urls, depth + 1);
                }
            }
        }
        else if (body.isString()) {
            const std::string s = body.asString();
            for (const auto &seg : cqscan::split(s)) {
                if (!seg.is_cq || seg.type != "forward") continue;
                const std::string id = cqscan::arg_value(seg.args, "id");
                if (!id.empty()) {
                    collect_forward_images(p, cq_decode(id), urls, depth + 1);
                }
            }
        }
    }
    if (depth == 0) {
        p->setlog(LOG::INFO,
                  "openai: forward images extracted=" +
                      std::to_string(urls.size() - before));
    }
}

Json::Value gpt3_5::build_user_content(const std::string &message,
                                       const msg_meta &conf, int64_t reply_id)
{
    if (!media_enabled) return Json::Value();

    struct Segment {
        bool is_image;
        std::string text;
    };
    std::vector<Segment> segs;

    for (const auto &cqseg : cqscan::split(message)) {
        if (!cqseg.is_cq || cqseg.type != "image") {
            if (!cqseg.text.empty()) segs.push_back({false, cqseg.text});
            continue;
        }
        std::string url = cq_decode(cqscan::arg_value(cqseg.args, "url"));
        segs.push_back({true, url});
    }

    std::vector<std::string> quoted_images;
    std::vector<std::string> forward_urls;
    if (reply_id != -1) {
        try {
            Json::Value param;
            param["message_id"] = reply_id;
            Json::Value info =
                string_to_json(conf.p->cq_send("get_msg", param));
            conf.p->setlog(
                LOG::INFO,
                "openai: quoted reply_id=" + std::to_string(reply_id) +
                    " retcode=" +
                        std::to_string(openai_internal::json_to_int64(
                            info["retcode"])) +
                    " body=" +
                    Json::writeString(Json::StreamWriterBuilder(), info)
                        .substr(0, 600));
            if (openai_internal::json_to_int64(info["retcode"]) == 0 &&
                info["data"].isMember("message")) {
                const Json::Value &quoted_msg = info["data"]["message"];
                collect_images_from_message(quoted_msg, quoted_images);
                conf.p->setlog(LOG::INFO,
                               "openai: quoted images found=" +
                                   std::to_string(quoted_images.size()));

                std::string quoted = messageArr_to_string(quoted_msg);
                for (const auto &cqseg : cqscan::split(quoted)) {
                    if (!cqseg.is_cq || cqseg.type != "forward") continue;
                    const std::string id = cqscan::arg_value(cqseg.args, "id");
                    if (!id.empty()) {
                        collect_forward_images(conf.p, cq_decode(id),
                                               forward_urls, 0);
                    }
                }
            }
        }
        catch (...) {
        }
    }

    for (const auto &cqseg : cqscan::split(message)) {
        if (!cqseg.is_cq || cqseg.type != "forward") continue;
        const std::string id = cqscan::arg_value(cqseg.args, "id");
        if (!id.empty()) {
            collect_forward_images(conf.p, cq_decode(id), forward_urls, 0);
        }
    }

    const bool has_inline_image =
        std::any_of(segs.begin(), segs.end(),
                    [](const Segment &s) { return s.is_image; });

    if (!has_inline_image && quoted_images.empty() && forward_urls.empty()) {
        return Json::Value();
    }

    Json::Value content(Json::arrayValue);
    for (const auto &s : segs) {
        if (s.is_image) {
            Json::Value part = store_image(s.text, conf);
            if (part.isNull()) continue;
            content.append(part);
        }
        else {
            std::string text = trim(s.text);
            if (text.empty()) continue;
            Json::Value part;
            part["type"] = "text";
            part["text"] = text;
            content.append(part);
        }
    }

    if (!quoted_images.empty()) {
        Json::Value note;
        note["type"] = "text";
        note["text"] = "（以下是引用消息里的图片）";
        content.append(note);
        for (const auto &url : quoted_images) {
            Json::Value part = store_image(url, conf);
            if (part.isNull()) continue;
            content.append(part);
        }
    }

    if (!forward_urls.empty()) {
        Json::Value note;
        note["type"] = "text";
        note["text"] = "（以下图片来自合并转发的聊天记录）";
        content.append(note);
        for (const auto &url : forward_urls) {
            Json::Value part = store_image(url, conf);
            if (part.isNull()) continue;
            content.append(part);
        }
    }

    if (content.empty()) return Json::Value();
    return content;
}

Json::Value gpt3_5::expand_content_for_api(const Json::Value &content,
                                           size_t *budget)
{
    if (content.isString()) return content;
    if (!content.isArray()) return content;

    Json::Value out(Json::arrayValue);
    for (const auto &part : content) {
        std::string type = openai_internal::json_to_string(part["type"]);
        if (type != "image_ref") {
            out.append(part);
            continue;
        }

        const std::string file = openai_internal::json_to_string(part["file"]);
        std::string mime = openai_internal::json_to_string(part["mime"]);
        if (mime.empty()) mime = "image/jpeg";
        fs::path path = fs::path(media_dir()) / file;
        if (file.empty() || !fs::exists(path)) {
            Json::Value fallback;
            fallback["type"] = "text";
            fallback["text"] = "[图片]";
            out.append(fallback);
            continue;
        }

        std::string bytes = openai_internal::read_binary(path);
        if (bytes.empty()) {
            Json::Value fallback;
            fallback["type"] = "text";
            fallback["text"] = "[图片]";
            out.append(fallback);
            continue;
        }

        const size_t encoded_size = bytes.size() / 3 * 4 + 8;
        if (budget != nullptr) {
            if (encoded_size > *budget) {
                Json::Value fallback;
                fallback["type"] = "text";
                fallback["text"] = "[图片]";
                out.append(fallback);
                continue;
            }
            *budget -= encoded_size;
        }

        Json::Value img;
        img["type"] = "image_url";
        img["image_url"]["url"] =
            "data:" + mime + ";base64," + openai_internal::b64_encode(bytes);
        out.append(img);
    }
    return out;
}

Json::Value gpt3_5::degrade_images(const Json::Value &content)
{
    if (!content.isArray()) return content;
    Json::Value out(Json::arrayValue);
    for (const auto &part : content) {
        if (part.get("type", "").asString() == "image_ref") {
            Json::Value fallback;
            fallback["type"] = "text";
            fallback["text"] = "[图片]";
            out.append(fallback);
        }
        else {
            out.append(part);
        }
    }
    return out;
}

std::string gpt3_5::content_to_text(const Json::Value &content)
{
    if (content.isString()) return content.asString();
    if (!content.isArray()) return "";

    std::string out;
    for (const auto &part : content) {
        std::string type = openai_internal::json_to_string(part["type"]);
        if (type == "text") {
            out += openai_internal::json_to_string(part["text"]);
        }
        else if (type == "image_ref" || type == "image_url") {
            out += "[图片]";
        }
    }
    return out;
}
