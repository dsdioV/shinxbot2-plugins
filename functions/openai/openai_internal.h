#pragma once

#include "utils.h"

#include <jsoncpp/json/json.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace openai_internal {

/// 把可能混进来的 SSE（"data: {...}\ndata: [DONE]"）解析成 JSON。
///
/// 为什么需要：中转（new-api）在上游超时时会把上游的**流式原文**直接透传，
/// 即使请求是 stream=false。
inline Json::Value parse_api_response(const std::string &raw)
{
    if (raw.empty()) return Json::Value();

    {
        Json::Value root;
        Json::Reader re;
        if (re.parse(raw, root)) return root;
    }

    Json::Value last_with_choices;
    size_t pos = 0;
    bool saw_any = false;
    while (pos < raw.size()) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();
        std::string line = raw.substr(pos, eol - pos);
        pos = eol + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();

        const std::string prefix = "data:";
        if (line.size() < prefix.size() ||
            line.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        std::string payload = line.substr(prefix.size());
        while (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
        if (payload.empty() || payload == "[DONE]") continue;

        Json::Value chunk;
        Json::Reader re;
        if (!re.parse(payload, chunk)) continue;
        saw_any = true;
        if (chunk.isMember("choices") && chunk["choices"].isArray() &&
            !chunk["choices"].empty()) {
            last_with_choices = chunk;
        }
        else if (last_with_choices.isNull() && chunk.isMember("choices")) {
            last_with_choices = chunk;
        }
    }

    if (!last_with_choices.isNull()) return last_with_choices;
    if (saw_any) {
        Json::Value v;
        v["__sse_empty__"] = true;
        return v;
    }
    return Json::Value();
}

/// 标准 base64 编码。
inline std::string b64_encode(const std::string &in)
{
    static const char *tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                     (static_cast<unsigned char>(in[i + 1]) << 8) |
                     static_cast<unsigned char>(in[i + 2]);
        out += tbl[(v >> 18) & 0x3f];
        out += tbl[(v >> 12) & 0x3f];
        out += tbl[(v >> 6) & 0x3f];
        out += tbl[v & 0x3f];
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += tbl[(v >> 18) & 0x3f];
        out += tbl[(v >> 12) & 0x3f];
        out += "==";
    }
    else if (i + 2 == in.size()) {
        unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                     (static_cast<unsigned char>(in[i + 1]) << 8);
        out += tbl[(v >> 18) & 0x3f];
        out += tbl[(v >> 12) & 0x3f];
        out += tbl[(v >> 6) & 0x3f];
        out += '=';
    }
    return out;
}

/// 二进制安全的读文件。不能用框架的 readfile —— 它用 getline 逐行拼 "\n"。
inline std::string read_binary(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// 二进制安全的写文件。
inline bool write_binary(const std::filesystem::path &path, const std::string &data)
{
    std::ofstream f(path, std::ios::out | std::ios::binary);
    if (!f.is_open()) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return f.good();
}

/// SHA-256（十六进制）。只用于图片内容寻址去重，不是安全用途。
inline std::string sha256_hex(const std::string &data)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
    std::vector<unsigned char> msg(data.begin(), data.end());
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<unsigned char>((bit_len >> (i * 8)) & 0xff));
    }

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[off + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 =
                rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 =
                rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    static const char *hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
        for (int j = 7; j >= 0; --j) {
            out.push_back(hex[(h[i] >> (j * 4)) & 0xf]);
        }
    }
    return out;
}

/// 从 chat.completion 的 message.content 里取纯文本。
///
/// 为什么需要：部分上游（Mistral 开启 reasoning_effort 后、Anthropic 系
/// 中转）返回的 content 不再是字符串，而是 content blocks 数组：
///   [{"type":"thinking",...},{"type":"text","text":"答案"}]
/// jsoncpp 对数组调用 asString() 会返回空串，于是正文被判为空，bot 就会
/// 误报"空返回/只输出了思考"。这里统一提取所有 text 块并拼接。
inline std::string message_content_text(const Json::Value &content)
{
    if (content.isString()) return content.asString();
    if (!content.isArray()) return std::string();

    std::string out;
    for (const auto &part : content) {
        if (part.isString()) {
            out += part.asString();
            continue;
        }
        if (!part.isObject()) continue;
        const std::string type = part.get("type", "text").asString();
        if (type == "text") {
            std::string t = part.get("text", "").asString();
            if (t.empty() && part.isMember("content")) {
                t = message_content_text(part["content"]);
            }
            out += t;
        }
    }
    return out;
}

inline int64_t estimate_text_tokens(const std::string &s)
{
    int64_t ascii = 0, cjk = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ascii++;
            i++;
        }
        else {
            int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            cjk++;
            i += len;
        }
    }
    return ascii * 28 / 100 + cjk * 3 / 2;
}

inline int64_t content_tokens(const Json::Value &content)
{
    static const int64_t kImageTokens = 1100;
    if (content.isString()) return estimate_text_tokens(content.asString());
    if (!content.isArray()) return 0;

    int64_t total = 0;
    for (const auto &part : content) {
        std::string type = part.get("type", "").asString();
        if (type == "text") {
            total += estimate_text_tokens(part.get("text", "").asString());
        }
        else if (type == "image_ref" || type == "image_url") {
            total += kImageTokens;
        }
    }
    return total;
}

inline int64_t getlength(const Json::Value &J)
{
    int64_t l = 0;
    for (const auto &msg : J) {
        l += content_tokens(msg["content"]);
    }
    return l;
}

inline bool isASCII(const std::string &s)
{
    return !std::any_of(s.begin(), s.end(), [](char c) {
        return static_cast<unsigned char>(c) > 127;
    });
}

inline bool json_array_starts_with(const Json::Value &value,
                                   const Json::Value &prefix)
{
    if (!value.isArray() || !prefix.isArray()) return false;
    if (value.size() < prefix.size()) return false;
    for (Json::ArrayIndex i = 0; i < prefix.size(); ++i) {
        if (value[i] != prefix[i]) return false;
    }
    return true;
}

inline bool extract_leading_reply_id(const std::string &message,
                                     int64_t &reply_id)
{
    if (!starts_with(message, "[CQ:reply,id=")) return false;

    size_t id_start = 13;
    size_t id_end = message.find_first_of(",]", id_start);
    if (id_end == std::string::npos || id_end <= id_start) return false;

    try {
        reply_id = std::stoll(message.substr(id_start, id_end - id_start));
        return true;
    }
    catch (...) {
        reply_id = -1;
        return false;
    }
}

inline void strip_leading_reply_segment(std::string &message, int64_t &reply_id)
{
    if (!extract_leading_reply_id(message, reply_id)) return;

    size_t pos = message.find(']');
    if (pos != std::string::npos) {
        message = trim(message.substr(pos + 1));
    }
}

inline void send_forward_text(const std::string &content, const msg_meta &conf,
                              const std::string &node_name)
{
    if (conf.p == nullptr) return;

    Json::Value J;
    Json::Value node;
    node["type"] = "node";
    node["data"]["name"] = node_name;
    node["data"]["uin"] = std::to_string(conf.p->get_botqq());
    node["data"]["content"] = string_to_messageArr(content);
    J["messages"].append(node);

    if (conf.message_type == "group") {
        J["group_id"] = conf.group_id;
        conf.p->cq_send("send_group_forward_msg", J);
    }
    else {
        J["user_id"] = conf.user_id;
        conf.p->cq_send("send_private_forward_msg", J);
    }
}

} // namespace openai_internal
