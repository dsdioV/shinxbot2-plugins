#include "gpt3_5.h"
#include "cqscan.h"
#include "utils.h"

#include <Magick++.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <ctime>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace {

/// 把可能混进来的 SSE（"data: {...}\ndata: [DONE]"）解析成 JSON。
///
/// 为什么需要：中转（new-api）在上游超时时会把上游的**流式原文**直接透传，
/// 即使请求是 stream=false。实测响应体是：
///     data: {"id":"",...,"choices":[],"usage":{...,"prompt_tokens":0,...}}
///     data: [DONE]
/// 这种文本直接丢给 Json::Reader 会失败，返回空 Value，上层就把
/// "解析失败" 误报成 "缺少 choices"。这里先尝试整体解析，失败则按行
/// 抽出所有 data: 负载，取最后一个带 choices 的 JSON 对象。
/// 解析不出来时返回空 Value（isNull()），让调用方走 err 路径。
Json::Value parse_api_response(const std::string &raw)
{
    if (raw.empty()) return Json::Value();

    // 正常情况：整段就是 JSON
    {
        Json::Value root;
        Json::Reader re;
        if (re.parse(raw, root)) return root;
    }

    // SSE：逐行取 "data:" 负载
    Json::Value last_with_choices;
    size_t pos = 0;
    bool saw_any = false;
    while (pos < raw.size()) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();
        std::string line = raw.substr(pos, eol - pos);
        pos = eol + 1;

        // 去掉行尾 \r
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
            // 只有空 choices 的收尾块：留着，好让上层能看出 tokens 用量
            last_with_choices = chunk;
        }
    }

    if (!last_with_choices.isNull()) return last_with_choices;
    // 流里一条可用的都没有：交给上层当格式异常处理，但要带上线索
    if (saw_any) {
        Json::Value v;
        v["__sse_empty__"] = true;
        return v;
    }
    return Json::Value();
}

/// 标准 base64 编码。自己实现是为了不引入额外依赖：
/// 插件仓的 lib/base64 是空 submodule，而 httplib 里那个在 detail 命名空间。
std::string b64_encode(const std::string &in)
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

/// 二进制安全的读文件。不能用框架的 readfile —— 它用 getline 逐行拼 "\n"，
/// 会把图片字节改坏。
std::string read_binary(const fs::path &path)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// 二进制安全的写文件（同理，不用框架的 writefile）。
bool write_binary(const fs::path &path, const std::string &data)
{
    std::ofstream f(path, std::ios::out | std::ios::binary);
    if (!f.is_open()) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return f.good();
}

/// SHA-256（十六进制）。只用于图片内容寻址去重，不是安全用途。
/// 自己实现是为了不给插件引入 OpenSSL 依赖。
std::string sha256_hex(const std::string &data)
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

} // namespace

/**
 * Overall API intro: https://platform.openai.com/docs/api-reference/chat/create
 * do post at https://api.openai.com/v1/chat/completions
 * model: gpt-3.5-turbo
 * upload:
 * {
 *  "model": "gpt-3.5-turbo",
 *  "messages": [{"role": "user", "content": "Hello!"}]
 * }
 * messages format: https://platform.openai.com/docs/guides/chat/introduction
 * temperature: 0~1
 * max_tokens: default inf(4096)
 *
 * ATTENTION!!!
 * In general, gpt-3.5-turbo-0301 does not pay strong attention to the system
 * message, and therefore important instructions are often better placed in a
 * user message.
 */

int MAX_TOKEN = 4000;
int MAX_REPLY = 1000;
int RED_LINE = 1000;
const int MAX_KEYS = 255;

std::mutex gptlock[MAX_KEYS];

gpt3_5::gpt3_5()
{
    const std::string openai_conf_path =
        bot_config_path(nullptr, "features/openai/openai.json");
    const std::string gpt_history_dir = bot_config_path(nullptr, "gpt3_5");

    if (!fs::exists(openai_conf_path)) {
        std::cout << "Please config your openai key in openai.json (and "
                     "restart) OR see openai_example.json"
                  << std::endl;
        std::ofstream of(openai_conf_path, std::ios::out);
        of << "{"
              "\"base_url\": \"https://api.openai.com\","
              "\"keys\": [\"\"],"
              "\"model\": \"gpt-3.5-turbo-1106\","
              "\"mode\": [\"default\"],"
              "\"default\": [{\"role\": \"system\", \"content\": \"You are a "
              "helpful assistant.\"}],"
              "\"black_list\": [\"股票\"],"
              "\"compress_summary_prompt\": \"\","
              "\"compress_recent_rounds\": 10,"
              "\"reply_on_at\": true,"
              "\"reply_on_keyword\": false,"
              "\"wake_keywords\": [\"bot\", \"猫猫\"],"
              "\"active_context_show_time\": true,"
              "\"active_context_show_user_id\": true,"
              "\"MAX_TOKEN\": 4000,"
              "\"MAX_REPLY\": 700,"
              "\"RED_LINE\": 1000"
              "}";
        of.close();
    }

    std::string ans = readfile(openai_conf_path);

    Json::Value res = string_to_json(ans);

    Json::ArrayIndex sz = res["keys"].size();
    if (sz > MAX_KEYS) sz = MAX_KEYS;
    for (Json::ArrayIndex i = 0; i < sz; ++i) {
        key.push_back(res["keys"][i].asString());
        is_lock.push_back(false);
    }

    sz = res["mode"].size();
    for (Json::ArrayIndex i = 0; i < sz; i++) {
        std::string tmp = res["mode"][i].asString();
        modes.push_back(tmp);
        mode_prompt[tmp] = res[tmp];
        if (i == 0) {
            default_prompt = tmp;
        }
    }

    parse_json_to_set(res["black_list"], black_list);

    MAX_TOKEN = res["MAX_TOKEN"].asInt();
    MAX_REPLY = res["MAX_REPLY"].asInt();
    RED_LINE = res["RED_LINE"].asInt();
    base_url = res.get("base_url", "https://api.openai.com").asString();
    model_name = res.get("model", "gpt-3.5-turbo").asString();
    compress_summary_prompt = res["compress_summary_prompt"].asString();
    compress_recent_rounds = res.get("compress_recent_rounds", 10).asInt();
    if (compress_recent_rounds < 1) compress_recent_rounds = 1;
    reply_on_at = res.get("reply_on_at", true).asBool();
    reply_on_keyword = res.get("reply_on_keyword", false).asBool();
    active_context_show_time = res.get("active_context_show_time", true).asBool();
    active_context_show_user_id = res.get("active_context_show_user_id", true).asBool();
    if (res.isMember("wake_keywords") && res["wake_keywords"].isArray()) {
        for (Json::ArrayIndex i = 0; i < res["wake_keywords"].size(); ++i) {
            std::string keyword = trim(res["wake_keywords"][i].asString());
            if (!keyword.empty()) wake_keywords.push_back(keyword);
        }
    }
    if (wake_keywords.empty()) {
        wake_keywords.push_back("vvbot");
        wake_keywords.push_back("vvm");
    }

    // 搜索工具：没有 key 就保持禁用，不影响其余功能
    search_api_key = res.get("search_api_key", "").asString();
    search_base_url =
        res.get("search_base_url", "https://api.deepseek.com").asString();
    search_enabled = res.get("search_enabled", false).asBool() &&
                     !trim(search_api_key).empty();
    search_count = res.get("search_count", 5).asInt();
    if (search_count < 1) search_count = 1;
    if (search_count > 20) search_count = 20;
    search_max_rounds = res.get("search_max_rounds", 3).asInt();
    if (search_max_rounds < 1) search_max_rounds = 1;
    if (search_max_rounds > 5) search_max_rounds = 5;

    // 工具调用兜底端点。newapi 中转会丢 tool_calls，检测到畸形响应时
    // 整轮改用这里重跑。默认复用搜索 key + DeepSeek 官方。
    tool_fallback_base_url = res.get("tool_fallback_base_url", "").asString();
    tool_fallback_model = res.get("tool_fallback_model", "").asString();
    tool_fallback_key = res.get("tool_fallback_key", "").asString();
    if (tool_fallback_base_url.empty()) tool_fallback_base_url = search_base_url;
    if (tool_fallback_model.empty()) tool_fallback_model = "deepseek-chat";
    if (tool_fallback_key.empty()) tool_fallback_key = search_api_key;

    is_open = true;
    is_debug = false;
    key_cycle = 0;

    if (fs::exists(gpt_history_dir)) {
        fs::path gpt_files = gpt_history_dir;
        fs::directory_iterator di(gpt_files);
        std::regex history_file_regex(R"(^(-?\d+)\.json$)");
        for (auto &entry : di) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::string filename = entry.path().filename().string();
            std::smatch match;
            if (!std::regex_match(filename, match, history_file_regex)) {
                continue;
            }
            int64_t id = std::stoll(match[1].str());
            Json::Value J = string_to_json(readfile(entry.path()));
            history[id] = J["history"];
            pre_default[id] = J["pre_prompt"].asString();
            if (pre_default[id].empty()) pre_default[id] = default_prompt;
        }
    }
}

void gpt3_5::save_file()
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    Json::Value J;
    J["base_url"] = base_url;
    J["model"] = model_name;
    for (const std::string &u : key)
        J["keys"].append(u);
    for (const std::string &u : modes)
        J["mode"].append(u);
    for (const std::string &u : wake_keywords)
        J["wake_keywords"].append(u);
    J["black_list"] = parse_set_to_json(black_list);
    J["compress_summary_prompt"] = compress_summary_prompt;
    J["compress_recent_rounds"] = compress_recent_rounds;
    J["reply_on_at"] = reply_on_at;
    J["reply_on_keyword"] = reply_on_keyword;
    J["active_context_show_time"] = active_context_show_time;
    J["active_context_show_user_id"] = active_context_show_user_id;
    J["MAX_TOKEN"] = MAX_TOKEN;
    J["MAX_REPLY"] = MAX_REPLY;
    J["RED_LINE"] = RED_LINE;
    // 搜索相关必须一起写回，否则任何一次 .ai.set 都会把它们抹掉
    J["search_api_key"] = search_api_key;
    J["search_base_url"] = search_base_url;
    J["search_enabled"] = search_enabled;
    J["search_count"] = search_count;
    J["search_max_rounds"] = search_max_rounds;
    J["tool_fallback_base_url"] = tool_fallback_base_url;
    J["tool_fallback_model"] = tool_fallback_model;
    J["tool_fallback_key"] = tool_fallback_key;
    for (const std::string &u : modes) {
        J[u] = mode_prompt[u];
    }
    writefile(bot_config_path(nullptr, "features/openai/openai.json"),
              J.toStyledString());
}

/// 本地 token 估算。只在拿不到 API 的 usage 时用（首轮、压缩后）。
///
/// 为什么不用旧的 `content.length() / 0.75`：
/// `std::string::length()` 返回**字节数**，一个汉字在 UTF-8 里占 3 字节，
/// 于是中文被算成 3/0.75 = 4 token/字，而真实值约 1.5。高估 2.7 倍，
/// 后果是压缩和裁剪触发得远比需要的早（4k 时代不易察觉）。
///
/// 这里按 Unicode 字符分类：ASCII ≈ 0.28 token/字符，非 ASCII ≈ 1.5 token/字符。
static int64_t estimate_text_tokens(const std::string &s)
{
    int64_t ascii = 0, cjk = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ascii++;
            i++;
        }
        else {
            // UTF-8 多字节：按字符数算，不当成 3 个 token
            int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            cjk++;
            i += len;
        }
    }
    return ascii * 28 / 100 + cjk * 3 / 2;
}

/// 一张图在主流视觉模型里的 token 开销。按 2048px 上限估算，取偏保守值。
static const int64_t kImageTokens = 1100;

/// 估算单条 content 的 token。图片按固定开销计，文字按字符类型加权。
static int64_t content_tokens(const Json::Value &content)
{
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

/// 估算整个 messages 数组。数组形态（多模态）必须走 parts 估算，
/// 否则 asString() 返回空串、估算归零，压缩和裁剪就永远不触发。
static int64_t getlength(const Json::Value &J)
{
    int64_t l = 0;
    for (const auto &msg : J) {
        l += content_tokens(msg["content"]);
    }
    return l;
}

// https://stackoverflow.com/a/48212993/17792535
bool isASCII(const std::string &s)
{
    return !std::any_of(s.begin(), s.end(), [](char c) {
        return static_cast<unsigned char>(c) > 127;
    });
}

static bool json_array_starts_with(const Json::Value &value,
                                   const Json::Value &prefix)
{
    if (!value.isArray() || !prefix.isArray()) return false;
    if (value.size() < prefix.size()) return false;
    for (Json::ArrayIndex i = 0; i < prefix.size(); ++i) {
        if (value[i] != prefix[i]) return false;
    }
    return true;
}

std::string gpt3_5::do_black(std::string message)
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    bool filtered = false;
    for (std::string u : black_list) {
        if (u.empty()) continue;
        if (message.find(u) == std::string::npos) continue;

        filtered = true;
        // Escape basic regex special chars
        std::string escaped_u;
        for (char c : u) {
            if (strchr(".^$*+?()[]{}\\|", c)) escaped_u += '\\';
            escaped_u += c;
        }
        std::regex black_regex;
        try {
            if (isASCII(u))
                black_regex = std::regex("\\b" + escaped_u + "\\b");
            else
                black_regex = std::regex(escaped_u);
            message = std::regex_replace(message, black_regex, "__");
        } catch (...) {
            // If regex still fails, fallback to simple find/replace if needed
            size_t pos = 0;
            while ((pos = message.find(u, pos)) != std::string::npos) {
                message.replace(pos, u.length(), "__");
                pos += 2;
            }
        }
    }
    if (filtered) {
        message += "\n(QAQ 响应被过滤了)";
    }
    return message;
}

size_t gpt3_5::get_avaliable_key()
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    size_t u;
    for (size_t i = 0; i < key.size(); i++) {
        if (!is_lock[u = (key_cycle + i) % key.size()]) {
            key_cycle = u;
            return u;
        }
    }
    return key_cycle;
}

bool gpt3_5::try_acquire_session(int64_t id, size_t keyid, const msg_meta &conf,
                                 bool ensure_default_prompt)
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    if (is_lock[keyid]) {
        conf.p->cq_send("请等待其他对话中输入的回复。", conf);
        return false;
    }
    if (!is_open) {
        conf.p->cq_send("已关闭。" + close_message, conf);
        return false;
    }
    if (ensure_default_prompt && history.find(id) == history.end()) {
        pre_default[id] = default_prompt;
    }
    is_lock[keyid] = true;
    return true;
}

void gpt3_5::release_session(size_t keyid)
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    is_lock[keyid] = false;
}

void gpt3_5::save_history(int64_t id)
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    Json::Value J;
    J["pre_prompt"] = pre_default[id];
    J["history"] = history[id];
    writefile(
        bot_config_path(nullptr, "gpt3_5/" + std::to_string(id) + ".json"),
        J.toStyledString());
}

void gpt3_5::fallback_trim_history(int64_t id, int rounds)
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    Json::Value ign;
    for (int j = 0; j < rounds; ++j) {
        if (history[id].size() > 0) history[id].removeIndex(0, &ign);
        if (history[id].size() > 0) history[id].removeIndex(0, &ign);
        if (history[id].size() == 0) break;
    }
}

bool gpt3_5::compress_history(int64_t id, size_t keyid, const msg_meta &conf,
                              std::string *error_message)
{
    Json::Value old_history;
    std::string current_mode;
    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        if (history.find(id) == history.end() || history[id].size() <= 2) {
            if (error_message) *error_message = "history too short to compress.";
            return false;
        }
        old_history = history[id];
        current_mode = pre_default[id];
    }

    Json::ArrayIndex total_sz = old_history.size();
    Json::ArrayIndex recent_keep = static_cast<Json::ArrayIndex>(compress_recent_rounds * 2);
    if (total_sz <= recent_keep) {
        if (error_message) *error_message = "history too short to compress.";
        return false;
    }

    Json::ArrayIndex split_idx = total_sz - recent_keep;

    Json::Value older_history(Json::arrayValue);
    Json::Value recent_history(Json::arrayValue);
    for (Json::ArrayIndex i = 0; i < split_idx; ++i) {
        older_history.append(old_history[i]);
    }
    for (Json::ArrayIndex i = split_idx; i < total_sz; ++i) {
        recent_history.append(old_history[i]);
    }

    if (older_history.empty()) {
        if (error_message) *error_message = "history too short to compress.";
        return false;
    }

    perform_archive(id, conf, true, true);

    Json::Value req;
    req["model"] = model_name;
    req["temperature"] = 0.3;
    req["max_tokens"] = MAX_REPLY;

    Json::Value messages(Json::arrayValue);
    Json::Value prompt_messages = mode_prompt[current_mode.empty() ? default_prompt : current_mode];
    for (Json::ArrayIndex i = 0; i < prompt_messages.size(); ++i) {
        messages.append(prompt_messages[i]);
    }
    for (Json::ArrayIndex i = 0; i < older_history.size(); ++i) {
        Json::Value entry = older_history[i];
        // 摘要是"读一遍旧对话再写总结"，不需要图片本身。
        // 直接把 parts 拍平成文字（图片变 [图片]），避免请求体塞满 base64。
        if (entry["content"].isArray()) {
            Json::Value flat;
            flat["role"] = entry["role"];
            flat["content"] = content_to_text(entry["content"]);
            messages.append(flat);
        }
        else {
            messages.append(entry);
        }
    }

    Json::Value user_msg;
    user_msg["role"] = "user";
    user_msg["content"] = compress_summary_prompt;
    messages.append(user_msg);
    req["messages"] = messages;

    Json::Value resp;
    try {
        resp = parse_api_response(
            do_post(base_url, "/v1/chat/completions", false, req,
                    {{"Content-Type", "application/json"},
                     {"Authorization", "Bearer " + key[keyid]}},
                    true));
    }
    catch (std::string e) {
        if (error_message) *error_message = e;
        return false;
    }
    catch (...) {
        if (error_message) *error_message = "http connection failed.";
        return false;
    }

    if (resp.isMember("error")) {
        if (error_message) *error_message = resp["error"]["message"].asString();
        return false;
    }
    if (!resp.isMember("choices") || !resp["choices"].isArray() ||
        resp["choices"].empty()) {
        if (error_message) *error_message = "summary response format invalid";
        return false;
    }

    std::string summary = trim(resp["choices"][0]["message"]["content"].asString());
    if (summary.empty()) {
        if (error_message) *error_message = "summary is empty";
        return false;
    }

    Json::Value new_history(Json::arrayValue);
    Json::Value summary_msg;
    summary_msg["role"] = "system";
    summary_msg["content"] = summary;
    new_history.append(summary_msg);
    for (Json::ArrayIndex i = 0; i < recent_history.size(); ++i) {
        new_history.append(recent_history[i]);
    }

    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        Json::Value tail(Json::arrayValue);
        if (!json_array_starts_with(history[id], old_history)) {
            if (error_message) *error_message = "history changed while compressing.";
            return false;
        }
        for (Json::ArrayIndex i = old_history.size(); i < history[id].size(); ++i) {
            tail.append(history[id][i]);
        }
        for (Json::ArrayIndex i = 0; i < tail.size(); ++i) {
            new_history.append(tail[i]);
        }
        history[id] = new_history;
        last_prompt_tokens[id] = 0;
        if (pre_default.find(id) == pre_default.end()) {
            pre_default[id] = current_mode.empty() ? default_prompt : current_mode;
        }
    }
    return true;
}


std::string gpt3_5::get_cached_nickname(const msg_meta &conf)
{
    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        auto it = nickname_cache.find(conf.user_id);
        if (it != nickname_cache.end()) return it->second;
    }

    std::string nickname = "Unknown";
    try {
        nickname = get_stranger_name(conf.p, conf.user_id);
    }
    catch (...) {
    }
    if (trim(nickname).empty()) nickname = std::to_string(conf.user_id);

    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        nickname_cache[conf.user_id] = nickname;
    }
    return nickname;
}

std::string gpt3_5::get_short_time()
{
    std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    now += 8 * 3600;
    tm tm_utc8_res = *std::gmtime(&now);
    char time_buf[32] = "??/?? ??:??";
    std::strftime(time_buf, sizeof(time_buf), "%m/%d %H:%M", &tm_utc8_res);
    return time_buf;
}

std::string gpt3_5::simplify_cq_codes(const std::string &message, userid_t botqq)
{
    // 原来这里是 static const std::regex cq_regex(R"(\[CQ:([^,\]]+)([^\]]*)\])")。
    // 这个模式在「不含 ',' 也不含 ']' 的超长连续段」上会让 libstdc++ 的
    // 回溯执行器无限深递归、耗尽栈并 SIGSEGV（实测 28KB 即可打死 8MB 栈，
    // 合并转发内联展开很容易到几十 KB）。改用线性扫描，无回溯、常数栈。
    const std::vector<cqscan::Segment> segs = cqscan::split(message);
    std::string result;
    for (const auto &seg : segs) {
        if (!seg.is_cq) {
            result += seg.text;
            continue;
        }
        const std::string &type = seg.type;
        const std::string &args = seg.args;
        if (type == "image") result += "[图片]";
        else if (type == "record") result += "[语音]";
        else if (type == "face") result += "[表情]";
        else if (type == "video") result += "[视频]";
        else if (type == "reply") result += "[回复]";
        else if (type == "forward") result += "[转发]";
        else if (type == "at") {
            bool is_bot = false;
            if (botqq != 0) {
                const std::string needle = "qq=" + std::to_string(botqq);
                is_bot = args.find(needle) != std::string::npos;
            }
            result += is_bot ? "[@BOT]" : "[@某人]";
        }
        else result += "[" + type + "]";
    }
    std::replace(result.begin(), result.end(), '\n', ' ');
    std::replace(result.begin(), result.end(), '\r', ' ');
    return trim(result);
}

std::string gpt3_5::strip_bot_at(const std::string &message, const msg_meta &conf)
{
    std::string result = message;
    userid_t botqq = conf.p->get_botqq();
    try {
        std::regex bot_at_regex("\\[CQ:at,qq=" + std::to_string(botqq) +
                                R"((?:,[^\]]*)?\])");
        result = std::regex_replace(result, bot_at_regex, "");
    }
    catch (...) {
    }
    return trim(result);
}

std::string gpt3_5::format_context_message(const std::string &message,
                                           const msg_meta &conf,
                                           const std::string &nickname,
                                           int64_t reply_id,
                                           bool include_quote)
{
    bool show_id = active_context_show_user_id;
    bool show_time = active_context_show_time;

    std::string prefix = "[";
    if (show_id) {
        prefix += std::to_string(conf.user_id) + " (" + nickname + ")";
    }
    else {
        prefix += nickname;
    }
    if (show_time) {
        prefix += ", " + get_short_time();
    }
    prefix += "] ";

    std::string body = simplify_cq_codes(message, conf.p->get_botqq());
    std::string quoted;
    if (include_quote && reply_id != -1) {
        quoted = "引用：" + get_quoted_content(conf.p, reply_id) + " ";
    }
    return trim(prefix + quoted + body);
}

bool gpt3_5::message_mentions_bot(const std::string &message, const msg_meta &conf)
{
    std::string bot_at = "[CQ:at,qq=" + std::to_string(conf.p->get_botqq());
    return message.find(bot_at) != std::string::npos;
}

bool gpt3_5::message_has_wake_keyword(const std::string &message)
{
    std::string lower_message = message;
    std::transform(lower_message.begin(), lower_message.end(), lower_message.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    for (std::string keyword : wake_keywords) {
        keyword = trim(keyword);
        if (keyword.empty()) continue;
        std::transform(keyword.begin(), keyword.end(), keyword.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower_message.find(keyword) != std::string::npos) return true;
    }
    return false;
}

static bool extract_leading_reply_id(const std::string &message,
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

static void strip_leading_reply_segment(std::string &message, int64_t &reply_id)
{
    if (!extract_leading_reply_id(message, reply_id)) return;

    size_t pos = message.find(']');
    if (pos != std::string::npos) {
        message = trim(message.substr(pos + 1));
    }
}

static void send_forward_text(const std::string &content, const msg_meta &conf,
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

std::string gpt3_5::get_quoted_content(const bot *p, int64_t reply_id, int depth)
{
    if (depth > 5) return "...(too deep)";

    Json::Value get_msg_param;
    get_msg_param["message_id"] = reply_id;
    Json::Value msg_info =
        string_to_json(p->cq_send("get_msg", get_msg_param));

    if (msg_info["retcode"].asInt() != 0 || !msg_info.isMember("data")) return "[Failed to fetch message]";

    Json::Value &msg_data = msg_info["data"];
    if (!msg_data.isMember("message")) return "[Empty message content]";

    std::string content = messageArr_to_string(msg_data["message"]);

    // Check for forward message with ID
    size_t fwd_pos = content.find("[CQ:forward,id=");
    if (fwd_pos != std::string::npos) {
        size_t id_start = fwd_pos + 15;
        size_t id_end = content.find_first_of(",]", id_start);
        if (id_end != std::string::npos && id_end > id_start) {
            std::string forward_id = content.substr(id_start, id_end - id_start);
            return expand_forward_content(p, forward_id, depth);
        }
    }

    // Check for forward message with raw content (JSON format)
    size_t fwd_cont_pos = content.find("[CQ:forward,content=");
    if (fwd_cont_pos != std::string::npos) {
        // 这个分支处理的是 NapCat 把转发树**内联展开**进 CQ 码的情况：
        //   [CQ:forward,content=&#91;{node},...,{node}&#93;]
        // 注意 jsoncpp 的 getMemberNames() 按**字母序**输出，所以
        // content 永远排在 id 前面 —— 上面按 "[CQ:forward,id=" 找永远
        // 找不到，实际都会落到这里。内联展开可能非常大（实测单条转发
        // 序列化后 160KB+），所以这里先做量级检查再解析。
        size_t cont_start = fwd_cont_pos + 20;
        size_t cont_end = content.find_last_of(']'); // Find the outermost CQ code closure
        if (cont_end != std::string::npos && cont_end > cont_start) {
            const size_t raw_len = cont_end - cont_start;
            // 内联展开超过这个体量就不再尝试解析：既没意义（模型也读不完），
            // 又能避免把巨大的畸形 JSON 丢给 jsoncpp 做无谓的深拷贝。
            static const size_t kMaxInlineForwardBytes = 256 * 1024;
            if (raw_len > kMaxInlineForwardBytes) {
                return "[转发记录过大，已省略]";
            }
            std::string raw_json = content.substr(cont_start, raw_len);
            // Decode potential HTML entities (like &#91; for [)
            // 用线性替换，别再走 regex_replace（大输入下同样有风险）
            raw_json = cqscan::replace_all(raw_json, "&#91;", "[");
            raw_json = cqscan::replace_all(raw_json, "&#93;", "]");
            raw_json = cqscan::replace_all(raw_json, "&#44;", ",");
            
            try {
                Json::Value fwd_data = string_to_json(raw_json);
                if (fwd_data.isArray()) {
                    // 这里原来用
                    //   static const std::regex cq_regex(R"(\[CQ:([^,\]]+)[^\]]*\])");
                    // 对每个节点做 sregex_iterator —— **这正是线上 SIGSEGV 的现场**。
                    // 内联展开的节点里含有几十 KB 不含 ',' / ']' 的连续段，
                    // libstdc++ 回溯执行器会无限深递归把栈打爆。
                    // 改成 cqscan::split 线性扫描。
                    std::string result = (depth == 0) ? "合并转发记录:" : "";
                    for (const auto &m : fwd_data) {
                        std::string msg_text = messageArr_to_string(m.isMember("message") ? m["message"] : m["content"]);
                        std::replace(msg_text.begin(), msg_text.end(), '\n', ' ');
                        std::replace(msg_text.begin(), msg_text.end(), '\r', ' ');

                        std::string cleaned;
                        for (const auto &seg : cqscan::split(msg_text)) {
                            if (!seg.is_cq) {
                                cleaned += seg.text;
                                continue;
                            }
                            const std::string &type = seg.type;
                            if (type == "image") cleaned += "[图片]";
                            else if (type == "record") cleaned += "[语音]";
                            else if (type == "face") cleaned += "[表情]";
                            else if (type == "at") cleaned += "[@某人]";
                            else cleaned += "[" + type + "]";
                        }

                        std::string nick = m["sender"]["nickname"].asString();
                        uint64_t uid = m["sender"]["user_id"].asUInt64();
                        if (uid != 0) result += "[" + nick + "(" + std::to_string(uid) + ")]：" + cleaned;
                        else result += "[" + nick + "]：" + cleaned;
                    }
                    return trim(result);
                }
            } catch (...) {}
        }
    }

    std::string nickname = "Unknown";
    if (msg_data.isMember("sender") && msg_data["sender"].isMember("nickname") && msg_data["sender"]["nickname"].isString()) {
        nickname = msg_data["sender"]["nickname"].asString();
    }

    uint64_t uid = 0;
    if (msg_data.isMember("sender") && msg_data["sender"].isMember("user_id")) {
        uid = msg_data["sender"]["user_id"].asUInt64();
    }

    content = simplify_cq_codes(content, p->get_botqq());
    if (uid != 0) {
        return "[" + nickname + "(" + std::to_string(uid) + ")]：" + content;
    }
    return "[" + nickname + "]：" + content;
}

std::string gpt3_5::expand_forward_content(const bot *p, const std::string &forward_id, int depth)
{
    forward_budget budget;
    return expand_forward_content(p, forward_id, depth, budget);
}

std::string gpt3_5::expand_forward_content(const bot *p, const std::string &forward_id,
                                           int depth, forward_budget &budget)
{
    // ── 展开预算 ──────────────────────────────────────────────────────
    // QQ 只允许 4 层嵌套，但每层节点数可以上百，逐层展开的调用次数是
    // 指数级的（2 → 200 → 2万 → 200万）。只卡层数没用，这里同时卡：
    //   层数、重复 id、节点总数、累计字节数。
    // 任一超限就停止继续展开，返回可读的截断说明，绝不让进程被拖死。
    static const int kMaxDepth = 5;      // 层数（比 QQ 的 4 层略宽，留余量）
    static const int kMaxNodes = 200;    // 整个展开过程计入的节点总数
    static const size_t kMaxBytes = 8192; // 展开文本的累计字节上限

    if (depth > kMaxDepth) return "[转发嵌套过深，已省略]";
    if (budget.nodes >= kMaxNodes) return "[转发内容过多，已省略]";
    if (budget.bytes >= kMaxBytes) return "[转发内容过长，已省略]";
    // 同一个转发 id 只展开一次：既防循环引用，也避免重复请求
    if (!budget.visited.insert(forward_id).second) return "";

    Json::Value get_fwd_param;
    get_fwd_param["id"] = forward_id;
    Json::Value fwd_info = string_to_json(
        p->cq_send("get_forward_msg", get_fwd_param));

    if (fwd_info["retcode"].asInt() != 0 || !fwd_info.isMember("data")) return "[Failed to fetch forward message]";

    Json::Value messages = fwd_info["data"].isMember("messages")
                               ? fwd_info["data"]["messages"]
                               : fwd_info["data"];
    std::string result = (depth == 0) ? "合并转发记录:" : "";
    if (messages.isArray()) {
        for (const auto &m : messages) {
            if (budget.nodes >= kMaxNodes) {
                result += "\n...[转发内容过多，已省略剩余部分]";
                break;
            }
            if (!m.isMember("message") && !m.isMember("content")) continue;

            std::string content = messageArr_to_string(
                m.isMember("message") ? m["message"] : m["content"]);

            // Check for nested forward
            size_t fwd_pos = content.find("[CQ:forward,id=");
            if (fwd_pos != std::string::npos) {
                size_t id_start = fwd_pos + 15;
                size_t id_end = content.find(']', id_start);
                if (id_end != std::string::npos) {
                    std::string nested_id =
                        content.substr(id_start, id_end - id_start);
                    result += "\n" + expand_forward_content(p, nested_id, depth + 1, budget);
                    continue;
                }
            }

            ++budget.nodes;
            // Flatten newlines
            std::replace(content.begin(), content.end(), '\n', ' ');
            std::replace(content.begin(), content.end(), '\r', ' ');

            // Simplify CQ codes（线性扫描，不用 std::regex：
            // 内联转发里的超长连续段会让回溯型执行器爆栈 SIGSEGV）
            std::string cleaned_content;
            for (const auto &seg : cqscan::split(content)) {
                if (!seg.is_cq) {
                    cleaned_content += seg.text;
                    continue;
                }
                const std::string &type = seg.type;
                if (type == "image") cleaned_content += "[图片]";
                else if (type == "record") cleaned_content += "[语音]";
                else if (type == "face") cleaned_content += "[表情]";
                else if (type == "video") cleaned_content += "[视频]";
                else if (type == "at") cleaned_content += "[@某人]";
                else if (type == "reply") cleaned_content += "[回复]";
                else cleaned_content += "[" + type + "]";
            }
            content = cleaned_content;

            if (!m.isMember("sender")) continue;

            std::string nick = m["sender"]["nickname"].isString() 
                               ? m["sender"]["nickname"].asString() 
                               : "Unknown";
            uint64_t uid = m["sender"]["user_id"].asUInt64();

            if (uid != 0) {
                result += "[" + nick + "(" + std::to_string(uid) + ")]：" + content;
            } else {
                result += "[" + nick + "]：" + content;
            }

            budget.bytes += result.size();
            if (budget.bytes >= kMaxBytes) {
                result += "\n...[转发内容过长，已省略剩余部分]";
                break;
            }
        }
    }
    return trim(result);
}

void gpt3_5::process(std::string message, const msg_meta &conf)
{
    if (conf.user_id == conf.p->get_botqq()) return;

    // 诊断：确认消息真的进来了，以及它的原始形态（转发检测依赖这个）
    // 用 conf.p->setlog 而不是 set_global_log：后者只打 stdout，
    // 而 bot 的日志是写文件的（log/<qq>/<date>/info.log）
    conf.p->setlog(LOG::INFO,
                   "openai: recv type=" + conf.message_type +
                       " user=" + std::to_string(conf.user_id) +
                       " len=" + std::to_string(message.size()) +
                       " head=" + message.substr(0, 120));

    int64_t reply_id = -1;
    strip_leading_reply_segment(message, reply_id);

    if (cmd_match_exact(message, {"你说的话我不喜欢"}) && reply_id != -1) {
        Json::Value get_msg_param;
        get_msg_param["message_id"] = reply_id;
        Json::Value msg_info =
            string_to_json(conf.p->cq_send("get_msg", get_msg_param));
        if (msg_info["data"]["sender"]["user_id"].asUInt64() ==
            conf.p->get_botqq()) {
            Json::Value del_msg_param;
            del_msg_param["message_id"] = reply_id;
            conf.p->cq_send("delete_msg", del_msg_param);
        }
        return;
    }

    int64_t id = conf.message_type == "group" ? (conf.group_id << 1)
                                              : ((conf.user_id << 1) | 1);
    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        if (pre_default.find(id) == pre_default.end() || pre_default[id].empty()) {
            pre_default[id] = default_prompt;
        }
    }

    std::string nickname = get_cached_nickname(conf);
    bool is_group = conf.message_type == "group";
    bool at_mentioned = is_group && message_mentions_bot(message, conf);
    bool keyword_mentioned = is_group && message_has_wake_keyword(message);

    std::string working_message = message;
    if (at_mentioned) {
        working_message = strip_bot_at(working_message, conf);
    }

    bool explicit_ai = starts_with(working_message, ".ai");
    bool should_reply = explicit_ai || (reply_on_at && at_mentioned) ||
                        (reply_on_keyword && keyword_mentioned) || !is_group;

    if (!should_reply) {
        return;
    }

    if (explicit_ai) {
        message = trim(working_message.substr(3));
    }
    else {
        message = trim(working_message);
    }
    message = do_black(message);
    if (!explicit_ai && at_mentioned && trim(message).empty()) {
        message = "（对方只 @ 了你，没有输入文字。请根据当前对话简短回应。）";
    }

    std::istringstream iss(message);
    std::string command;
    iss >> command;
    std::string args;
    getline(iss, args);
    args = trim(args);

    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        if (history.find(id) == history.end()) {
            pre_default[id] = default_prompt;
        }
    }

    // 只有显式 .ai 前缀才算命令，避免私聊里正常问 "help" 被拦截
    if (explicit_ai && (message == "help" || message == "帮助" ||
                        message == ".help")) {
        cmd_full_help(conf);
        return;
    }

    const std::vector<cmd_exact_rule> exact_rules = {
        {".test",
         [&]() {
             conf.p->cq_send(message, conf);
             return true;
         }},
        {".arc",
         [&]() {
             if (!is_allowed_arc(id, conf)) {
                 conf.p->cq_send("Not on op list.", conf);
                 return true;
             }
             std::istringstream arc_iss(args);
             std::string sub_cmd;
             arc_iss >> sub_cmd;
             if (sub_cmd == "list") {
                 int page = 1;
                 arc_iss >> page;
                 list_archives(id, conf, page);
             }
             else if (sub_cmd == "restore") {
                 std::string target;
                 arc_iss >> target;
                 if (target.empty()) {
                     conf.p->cq_send("用法: .ai arc restore [编号/文件名]",
                                     conf);
                 }
                 else {
                     restore_archive(id, conf, target);
                 }
             }
             else {
                 perform_archive(id, conf, false);
             }
             return true;
         }},
        {".reset",
        [&]() {
            {
                std::lock_guard<std::recursive_mutex> lock(data_lock);
                history[id].clear();
            }
            save_history(id);
            conf.p->cq_send("reset done.", conf);
            return true;
        }},
        {".compress",
         [&]() {
             if (key.size() == 0) {
                 conf.p->cq_send("No avaliable key!", conf);
                 return true;
             }
             size_t compress_keyid = get_avaliable_key();
             if (!try_acquire_session(id, compress_keyid, conf)) return true;

             std::lock_guard<std::mutex> compress_lock(gptlock[compress_keyid]);
             std::string compress_error;
             bool compressed = compress_history(id, compress_keyid, conf, &compress_error);
             release_session(compress_keyid);
             if (compressed) {
                 save_history(id);
                 conf.p->cq_send("compress done.", conf);
             }
             else {
                 conf.p->cq_send(compress_error.empty() ? "compress failed." : compress_error,
                                 conf);
             }
             return true;
         }},
        {".change",
        [&]() {
            std::string reply;
            bool need_save = false;
            {
                std::lock_guard<std::recursive_mutex> lock(data_lock);
                if (conf.p->is_op(conf.user_id) || (id & 1)) {
                    const std::string mode = args;
                    bool flg = false;
                    reply = "avaliable modes:";
                    for (const std::string &u : modes) {
                        reply += " " + u;
                        if (u == mode) {
                            flg = true;
                            history[id].clear();
                            pre_default[id] = mode;
                            reply = "change done.";
                            need_save = true;
                            break;
                        }
                    }
                }
                else {
                    reply = "Not on op list.";
                }
            }
            conf.p->cq_send(reply, conf);
            if (need_save) {
                save_history(id);
            }
            return true;
        }},
        {".sw",
        [&]() {
            bool new_state;
            {
                std::lock_guard<std::recursive_mutex> lock(data_lock);
                if (conf.p->is_op(conf.user_id)) {
                    is_open = !is_open;
                    close_message = args;
                    new_state = is_open;
                } else {
                    conf.p->cq_send("Not on op list.", conf);
                    return true;
                }
            }
            conf.p->cq_send("is_open: " + std::to_string(new_state), conf);
            return true;
        }},
        {".debug",
        [&]() {
            bool new_state;
            {
                std::lock_guard<std::recursive_mutex> lock(data_lock);
                if (conf.p->is_op(conf.user_id)) {
                    is_debug = !is_debug;
                    new_state = is_debug;
                } else {
                    conf.p->cq_send("Not on op list.", conf);
                    return true;
                }
            }
            conf.p->cq_send("is_debug: " + std::to_string(new_state), conf);
            return true;
        }},
        {".awake",
        [&]() {
            std::string reply;
            bool do_save = false;
            {
                std::lock_guard<std::recursive_mutex> lock(data_lock);
                if (!conf.p->is_op(conf.user_id)) {
                    reply = "Not on op list.";
                }
                else if (args.empty()) {
                    reply = "reply_on_at: " + std::to_string(reply_on_at) +
                            "\nreply_on_keyword: " +
                            std::to_string(reply_on_keyword);
                }
                else {
                    std::istringstream arg_iss(args);
                    std::string type;
                    int64_t num = 0;
                    if (!(arg_iss >> type >> num) || (num != 0 && num != 1)) {
                        reply = "用法: .ai.awake at|keyword 0|1";
                    }
                    else if (type == "at") {
                        reply_on_at = (num != 0);
                        reply = "set reply_on_at to " +
                                std::to_string(reply_on_at);
                        do_save = true;
                    }
                    else if (type == "keyword") {
                        reply_on_keyword = (num != 0);
                        reply = "set reply_on_keyword to " +
                                std::to_string(reply_on_keyword);
                        do_save = true;
                    }
                    else {
                        reply = "用法: .ai.awake at|keyword 0|1";
                    }
                }
            }
            conf.p->cq_send(reply, conf);
            if (do_save) save_file();
            return true;
        }},
        {".set",
        [&]() {
            std::string reply = "Not on op list.";
            bool do_save = false;
            {
                std::lock_guard<std::recursive_mutex> lock(data_lock);
                if (conf.p->is_op(conf.user_id)) {
                    std::string type;
                    int64_t num = 0;
                    std::istringstream arg_iss(args);
                    if (!(arg_iss >> type >> num)) {
                        reply = "Unknown type";
                    } else if (type == "reply") {
                        MAX_REPLY = num;
                        reply = "set MAX_REPLY to " + std::to_string(num);
                        do_save = true;
                    } else if (type == "token") {
                        MAX_TOKEN = num;
                        reply = "set MAX_TOKEN to " + std::to_string(num);
                        do_save = true;
                    } else if (type == "red") {
                        RED_LINE = num;
                        reply = "set RED_LINE to " + std::to_string(num);
                        do_save = true;
                    } else if (type == "compress") {
                        if (num < 1) {
                            reply = "compress rounds must be >= 1";
                        } else {
                            compress_recent_rounds = static_cast<int>(num);
                            reply = "set compress_recent_rounds to " + std::to_string(num);
                            do_save = true;
                        }
                    } else {
                        reply = "Unknown type";
                    }
                }
            }
            conf.p->cq_send(reply, conf);
            if (do_save) save_file();
            return true;
        }},
        {".status",
        [&]() {
            std::string reply;
            {
                std::lock_guard<std::recursive_mutex> lock(data_lock);
                int64_t compress_threshold = static_cast<int64_t>(MAX_TOKEN) -
                                            static_cast<int64_t>(RED_LINE);
                int64_t trim_threshold = static_cast<int64_t>(MAX_TOKEN) -
                                        static_cast<int64_t>(MAX_REPLY);
                reply = "current ai status:\n";
                reply += "model: " + model_name + "\n";
                reply += "mode: " + pre_default[id] + "\n";
                reply += "MAX_TOKEN: " + std::to_string(MAX_TOKEN) + "\n";
                reply += "MAX_REPLY: " + std::to_string(MAX_REPLY) + "\n";
                reply += "RED_LINE: " + std::to_string(RED_LINE) + "\n";
                reply += "compress keep rounds: " + std::to_string(compress_recent_rounds) + "\n";
                reply += "compress keep messages: " + std::to_string(compress_recent_rounds * 2) + "\n";
                reply += "compress threshold (MAX_TOKEN-RED_LINE): " +
                         std::to_string(compress_threshold) + "\n";
                reply += "trim threshold (MAX_TOKEN-MAX_REPLY): " +
                         std::to_string(trim_threshold) + "\n";
                reply += "last api prompt_tokens: " +
                         std::to_string(last_prompt_tokens[id]) + "\n";
                reply += "last api completion_tokens: " +
                         std::to_string(last_completion_tokens[id]) + "\n";
                reply += "last api total_tokens: " +
                         std::to_string(last_total_tokens[id]) + "\n";
                reply += "history message count: " +
                         std::to_string(history[id].size()) + "\n";
                reply += "reply_on_at: " + std::to_string(reply_on_at) + "\n";
                reply += "reply_on_keyword: " +
                         std::to_string(reply_on_keyword) + "\n";
                reply += "web_search: " +
                         std::string(search_enabled ? "enabled" : "disabled");
                if (search_enabled) {
                    reply += " (count=" + std::to_string(search_count) +
                             ", max_rounds=" +
                             std::to_string(search_max_rounds) + ")";
                }
                else if (trim(search_api_key).empty()) {
                    reply += " (未配置 search_api_key)";
                }
                else {
                    reply += " (search_enabled=false)";
                }
                reply += "\n";
                reply += "note: request order is prompt -> active history -> current user";
            }
            send_forward_text(reply, conf, "AI Status");
            return true;
        }},
    };

    bool handled = false;
    (void)cmd_try_dispatch(command, exact_rules, {}, handled);
    if (handled) {
        return;
    }

    if (!explicit_ai && trim(message).empty()) {
        return;
    }

    if (key.size() == 0) {
        conf.p->cq_send("No avaliable key!", conf);
        return;
    }

    size_t keyid = get_avaliable_key();
    if (!try_acquire_session(id, keyid, conf, false)) return;

    std::lock_guard<std::mutex> lock(gptlock[keyid]);
    Json::Value J, user_input_J, ign;
    user_input_J["role"] = "user";
    std::string prompt_content = format_context_message(message, conf, nickname,
                                                        reply_id, true);
    user_input_J["content"] = prompt_content;

    // 有图时把 content 换成 parts 数组（图片就地落盘、历史里只存引用）。
    // 纯文本时 build_user_content 返回 null，保持原来的字符串格式，
    // 这样旧历史和新历史在磁盘上是同一种形状。
    Json::Value parts = build_user_content(message, conf, reply_id);
    if (!parts.isNull()) {
        // 文字部分仍带上昵称/时间前缀，保持群聊语境
        std::string prefix = prompt_content.substr(
            0, prompt_content.size() > message.size()
                   ? prompt_content.size() - message.size()
                   : 0);
        if (!parts.empty() && parts[0].get("type", "").asString() == "text") {
            parts[0]["text"] = prefix + parts[0]["text"].asString();
        }
        else if (!prefix.empty()) {
            Json::Value head;
            head["type"] = "text";
            head["text"] = prefix;
            Json::Value merged(Json::arrayValue);
            merged.append(head);
            for (const auto &p : parts) merged.append(p);
            parts = merged;
        }
        user_input_J["content"] = parts;
    }

    J["model"] = model_name;
    
    {
        std::unique_lock<std::recursive_mutex> lock_data(data_lock);

        // Prefer compression before fallback trimming.
        Json::Value &h = history[id];
        while (((last_prompt_tokens[id] > 0 &&
                 last_prompt_tokens[id] > (int64_t)(MAX_TOKEN - RED_LINE)) ||
                (last_prompt_tokens[id] <= 0 &&
                 getlength(h) > (int64_t)(MAX_TOKEN - RED_LINE))) &&
               h.size() > 2) {
            lock_data.unlock();
            std::string compress_error;
            bool compressed = compress_history(id, keyid, conf, &compress_error);
            lock_data.lock();
            if (!compressed) {
                break;
            }
            break;
        }
        while (((last_prompt_tokens[id] > 0 &&
                 last_prompt_tokens[id] > (int64_t)(MAX_TOKEN - MAX_REPLY)) ||
                (last_prompt_tokens[id] <= 0 &&
                 getlength(h) > (int64_t)(MAX_TOKEN - MAX_REPLY)))) {
            if (h.size() <= 2) break;
            if (h.size() > 0) h.removeIndex(0, &ign);
            if (h.size() > 0) h.removeIndex(0, &ign);
            break;
        }

        Json::Value K = mode_prompt[pre_default[id]];
        Json::ArrayIndex sz = h.size();
        for (Json::ArrayIndex i = 0; i < sz; i++) {
            Json::Value entry = h[i];
            // 历史里存的是 image_ref 引用，发给 API 前才展开成 data URI。
            // 这样磁盘上的历史永远不含图片字节。
            if (entry["content"].isArray()) {
                entry["content"] = expand_content_for_api(entry["content"]);
            }
            K.append(entry);
        }
        if (user_input_J["content"].isArray()) {
            user_input_J["content"] =
                expand_content_for_api(user_input_J["content"]);
        }
        K.append(user_input_J);
        J["messages"] = K;
    }
    
    J["temperature"] = 0.7;
    J["max_tokens"] = MAX_REPLY;
    // 走工具循环：模型可以自己决定搜不搜、搜什么、要不要再搜一次。
    // 没有启用搜索时 build_tool_defs 返回空，行为与原来完全一致。
    J = run_chat_with_tools(J, keyid, conf);
    conf.p->setlog(LOG::INFO, "openai: user " + std::to_string(conf.user_id));
    
    {
        release_session(keyid);
    }

    if (J.isMember("error")) {
        if (J["error"]["message"].asString().find(
                "This model's maximum context length is") == 0) {
            conf.p->cq_send("Openai ERROR: history message is too long. Please "
                            "try again or try .ai.reset",
                            conf);
            {
                std::lock_guard<std::recursive_mutex> lock_data(data_lock);
                if (history[id].size() > 0) history[id].removeIndex(0, &ign);
                if (history[id].size() > 0) history[id].removeIndex(0, &ign);
            }
            save_history(id);
        }
        else {
            conf.p->cq_send("Openai ERROR: " + J["error"]["message"].asString(),
                            conf);
        }
    }
    else {
        if (!J.isMember("choices") || !J["choices"].isArray() ||
            J["choices"].empty()) {
            // 把实际收到的东西记下来，否则下次又只能看到一句笼统的
            // "缺少 choices"，无从判断是超时、鉴权还是中转抽风。
            conf.p->setlog(
                LOG::WARNING,
                "openai: 响应缺少 choices，原始内容前 400 字节=" +
                    J.toStyledString().substr(0, 400));
            conf.p->cq_send("Openai ERROR: API 响应格式异常(缺少 choices)",
                            conf);
            return;
        }

        std::string aimsg = J["choices"][0]["message"]["content"].asString();
        std::string finish_reason = J["choices"][0].isMember("finish_reason") 
                                    ? J["choices"][0]["finish_reason"].asString() 
                                    : "";

        if (trim(aimsg).empty()) {
            if (finish_reason == "content_filter") {
                conf.p->cq_send("QAQ 响应被 OpenAI 安全策略过滤了", conf);
            }
            else if (finish_reason == "length") {
                // 推理模型（如带 reasoning 的中转）会把 max_tokens 全花在
                // 思考上，content 返回空串但 completion_tokens 非零。
                // 这时说"空返回"是误导，实际是预算不够。
                conf.p->cq_send(
                    "模型把回复预算用在了思考上，没能输出正文。"
                    "可用 .ai.set reply [更大的数值] 提高上限。",
                    conf);
            }
            else {
                conf.p->cq_send("API空返回！", conf);
            }
            return;
        }

        aimsg = do_black(aimsg);

        if (finish_reason == "length") {
            aimsg += "\n(QAQ 响应因长度限制未完成)";
        } else if (finish_reason == "content_filter") {
            aimsg += "\n(QAQ 响应被 OpenAI 安全策略部分过滤)";
        }

        int64_t prompt_tokens = 0;
        int64_t completion_tokens = 0;
        int64_t total_tokens = 0;
        if (J.isMember("usage")) {
            if (J["usage"].isMember("prompt_tokens")) {
                prompt_tokens = J["usage"]["prompt_tokens"].asInt64();
            }
            if (J["usage"].isMember("completion_tokens")) {
                completion_tokens = J["usage"]["completion_tokens"].asInt64();
            }
            if (J["usage"].isMember("total_tokens")) {
                total_tokens = J["usage"]["total_tokens"].asInt64();
            }
        }

        std::string reply_msg = "[CQ:reply,id=" + std::to_string(conf.message_id) +
                                "]" + aimsg;
    {
        std::unique_lock<std::recursive_mutex> lock_data(data_lock);
            last_prompt_tokens[id] = prompt_tokens;
            last_completion_tokens[id] = completion_tokens;
            last_total_tokens[id] = total_tokens;

            if (MAX_TOKEN < prompt_tokens) {
                history[id].clear();
            }
            else {
                if (prompt_tokens > MAX_TOKEN - RED_LINE) {
                    lock_data.unlock();
                    std::string compress_error;
                    bool compressed = compress_history(id, keyid, conf, &compress_error);
                    lock_data.lock();
                    if (!compressed) {
                        for (int i = 5; i >= 1; i--) {
                            if (MAX_TOKEN - RED_LINE / i < prompt_tokens) {
                                lock_data.unlock();
                                fallback_trim_history(id, i);
                                lock_data.lock();
                                break;
                            }
                        }
                    }
                }
            }
            J.clear();
            J["role"] = "assistant";
            J["content"] = aimsg;
            // 注意：user_input_J 的 content 在上面的请求组装里已经被展开成
            // image_url（含 base64）。写回历史前必须换回未展开的 parts，
            // 否则图片字节会被 save_history 固化进磁盘。
            Json::Value stored_user = user_input_J;
            if (!parts.isNull()) {
                stored_user["content"] = parts;
            }
            history[id].append(stored_user);
            history[id].append(J);
        }
        conf.p->cq_send(reply_msg, conf);
    }
    save_history(id);
}

bool gpt3_5::check(std::string message, const msg_meta &conf)
{
    if (conf.user_id == conf.p->get_botqq()) return false;

    int64_t reply_id = -1;
    strip_leading_reply_segment(message, reply_id);

    if (cmd_match_exact(message, {"你说的话我不喜欢"})) {
        return true;
    }
    if (conf.message_type == "group" &&
        ((reply_on_at && message_mentions_bot(message, conf)) ||
         (reply_on_keyword && message_has_wake_keyword(message)))) {
        return true;
    }
    return cmd_match_prefix(message, {".ai"});
}

std::string gpt3_5::help()
{
    return "AI 对话：.ai [内容] 或 @bot。完整说明：.ai help";
}

// bot.help 会拼接所有插件的 help()，所以这里只给一行。
// 分等级时也保持一行，细节留给 .ai help 按需查看。
std::string gpt3_5::help(const msg_meta &conf, help_level_t level)
{
    (void)conf;
    switch (level) {
        case help_level_t::bot_admin:
            return "AI 对话：.ai [内容] 或 @bot；.ai help 看全部。"
                   "OP 另有 .ai.sw/.ai.set/.ai.arc";
        case help_level_t::group_admin:
            return "AI 对话：.ai [内容] 或 @bot；.ai help 看全部。"
                   "本群管理员可用 .ai.arc 归档上下文";
        case help_level_t::public_only:
        default:
            return help();
    }
}

void gpt3_5::cmd_full_help(const msg_meta &conf)
{
    const bool is_group = conf.message_type == "group";
    const bool can_manage = !is_group || conf.p->is_op(conf.user_id);

    std::string body;
    body += "闲聊\n"
            "  .ai [内容]        和模型对话\n"
            "  @bot [内容]       同上（群聊里 @ 即可）\n"
            "  直接发图          模型能看图，支持追问同一张图\n"
            "  回复某张图再问    会把被引用消息里的图一并带上\n"
            "  .ai help          显示本说明\n"
            "\n"
            "上下文\n"
            "  .ai.status        以合并转发查看模型/阈值/历史估算\n"
            "  .ai.compress      把旧上下文压成摘要，保留最近几轮\n"
            "  .ai.reset         清空当前对话（备份里仍可找回）\n"
            "  .ai.change [模式] 切换提示词模式（私聊或 OP）\n";

    if (can_manage) {
        body += "\n归档\n"
                "  .ai.arc           手动归档当前上下文\n"
                "  .ai.arc list [页] 查看归档列表（每页 5 条）\n"
                "  .ai.arc restore [编号/文件名]  从归档恢复\n";
    }

    if (conf.p->is_op(conf.user_id)) {
        body += "\nOP\n"
                "  .ai.sw [原因]            关闭/开启模型（维护用）\n"
                "  .ai.awake at|keyword 0|1 切换 @bot / 关键词触发\n"
                "  .ai.set reply|token|red|compress [数值]  调整阈值\n"
                "  .ai.debug               切换调试输出\n";
    }

    body +=
        "\n提示：群聊里 @bot 会直接进入对话，不必加 .ai。\n"
        "上下文超限时会自动压缩；也可随时 .ai.arc 手动存档。";

    if (search_enabled) {
        body += "\n联网搜索已启用：需要实时信息时模型会自己调 web_search，"
                "你不用特别说明。";
    }

    conf.p->cq_send(trim(body), conf);
}


uintmax_t gpt3_5::get_archives_total_size()
{
    std::string backup_root = bot_config_path(nullptr, "gpt3_5/backups");
    if (!fs::exists(backup_root)) return 0;
    uintmax_t total_size = 0;
    for (const auto &entry : fs::recursive_directory_iterator(backup_root)) {
        if (entry.is_regular_file()) {
            total_size += entry.file_size();
        }
    }
    return total_size;
}

void gpt3_5::perform_archive(int64_t id, const msg_meta &conf, bool is_auto,
                             bool silent)
{
    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        if (arc_is_full) {
            if (!is_auto) {
                conf.p->cq_send("当前归档文件过大，已暂停生成。请联系管理员", conf);
            }
            return;
        }
    }

    std::string backup_dir =
        bot_config_path(nullptr, "gpt3_5/backups/" + std::to_string(id));
    if (!fs::exists(backup_dir)) {
        fs::create_directories(backup_dir);
    }

    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    now += 8 * 3600;
    tm tm_res = *std::gmtime(&now);
    char time_buf[64];
    if (is_auto) {
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d_%H-%M-%S_beforeCompress", &tm_res);
    } else {
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d_%H-%M-%S", &tm_res);
    }

    std::string filename = std::string(time_buf) + ".json";
    std::string full_path = backup_dir + "/" + filename;

    if (is_auto && fs::exists(full_path)) {
        int suffix = 1;
        do {
            filename = std::string(time_buf) + "_" + std::to_string(suffix) + ".json";
            full_path = backup_dir + "/" + filename;
            ++suffix;
        } while (fs::exists(full_path));
    }

    Json::Value J;
    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        J["pre_prompt"] = pre_default[id];
        J["history"] = history[id];
        writefile(full_path, J.toStyledString());
    }

    bool need_check = false;
    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        if (++arc_check_counter >= 10) {
            arc_check_counter = 0;
            need_check = true;
        }
    }
    if (need_check) {
        bool full = (get_archives_total_size() >= 250 * 1024 * 1024);
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        arc_is_full = full;
    }

    if (!is_auto && !silent) {
        conf.p->cq_send("归档已生成: " + filename, conf);
    }
}

bool gpt3_5::is_allowed_arc(int64_t id, const msg_meta &conf)
{
    if (id & 1) return true; // Private chat
    return conf.p->is_op(conf.user_id);
}

void gpt3_5::list_archives(int64_t id, const msg_meta &conf, int page)
{
    std::string backup_dir =
        bot_config_path(nullptr, "gpt3_5/backups/" + std::to_string(id));
    if (!fs::exists(backup_dir)) {
        conf.p->cq_send("暂无归档记录。", conf);
        return;
    }

    std::vector<std::string> files;
    for (const auto &entry : fs::directory_iterator(backup_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path().filename().string());
        }
    }
    std::sort(files.rbegin(), files.rend()); // Newest first

    if (files.empty()) {
        conf.p->cq_send("暂无归档记录。", conf);
        return;
    }

    int total_pages = (files.size() + 4) / 5;
    if (page < 1) page = 1;
    if (page > total_pages) page = total_pages;

    std::string res = "归档列表 (第 " + std::to_string(page) + "/" +
                      std::to_string(total_pages) + " 页):\n";
    for (size_t i = (page - 1) * 5; i < page * 5 && i < files.size(); ++i) {
        res += std::to_string(i + 1) + ". " + files[i] + "\n";
    }
    res += "使用 .ai arc restore [编号/文件名] 恢复。";
    conf.p->cq_send(trim(res), conf);
}

void gpt3_5::restore_archive(int64_t id, const msg_meta &conf,
                             const std::string &arg)
{
    std::string backup_dir =
        bot_config_path(nullptr, "gpt3_5/backups/" + std::to_string(id));
    if (!fs::exists(backup_dir)) {
        conf.p->cq_send("暂无归档记录。", conf);
        return;
    }

    std::string target_file;
    if (std::all_of(arg.begin(), arg.end(), ::isdigit)) {
        int idx = std::stoi(arg) - 1;
        std::vector<std::string> files;
        for (const auto &entry : fs::directory_iterator(backup_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path().filename().string());
            }
        }
        std::sort(files.rbegin(), files.rend());
        if (idx >= 0 && idx < (int)files.size()) {
            target_file = files[idx];
        }
    }
    else {
        target_file = arg;
        if (target_file.find(".json") == std::string::npos)
            target_file += ".json";
    }

    if (target_file.empty()) return;

    if (target_file.find("..") != std::string::npos || 
        target_file.find('/') != std::string::npos || 
        target_file.find('\\') != std::string::npos) {
        conf.p->cq_send("非法的文件名格式。", conf);
        return;
    }

    std::string full_path = backup_dir + "/" + target_file;
    if (!fs::exists(full_path)) {
        conf.p->cq_send("找不到指定的归档文件: " + target_file, conf);
        return;
    }

    Json::Value J = string_to_json(readfile(full_path));
    if (J.isMember("history") && J.isMember("pre_prompt")) {
        {
            std::lock_guard<std::recursive_mutex> lock(data_lock);
            history[id] = J["history"];
            pre_default[id] = J["pre_prompt"].asString();
        }
        save_history(id);
        conf.p->cq_send("归档 " + target_file + " 已成功恢复。", conf);
    }
    else {
        conf.p->cq_send("归档文件格式错误。", conf);
    }
}

// ───────────────────── 多模态：图片落盘 + 历史存引用 ─────────────────────

std::string gpt3_5::media_dir() const
{
    return bot_config_path(nullptr, "gpt3_5/media");
}

/// 解析 [CQ:image,...] 里的 url 字段。QQ 的 url 带时效签名，必须当场下载。
static std::string cq_image_url(const std::string &segment)
{
    size_t pos = segment.find("url=");
    if (pos == std::string::npos) return "";
    pos += 4;
    size_t end = segment.find_first_of(",]", pos);
    if (end == std::string::npos) end = segment.size();
    return cq_decode(segment.substr(pos, end - pos));
}

/// 读文件开头若干字节，用来嗅探图片格式。失败返回空串。
static std::string read_binary_prefix(const std::string &path, size_t n)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::string buf(n, '\0');
    in.read(&buf[0], static_cast<std::streamsize>(n));
    buf.resize(static_cast<size_t>(in.gcount()));
    return buf;
}

/// 按魔术字节判断图片格式，返回 ImageMagick 认识的格式名。
///
/// 为什么需要它：store_image 把图片先下载成临时文件，扩展名不可信；
/// Magick::Image 只按扩展名选解码器，于是报
///   "no decode delegate for this image format `TMP'"
/// 这里直接看文件头，绕开扩展名。
static std::string sniff_image_magick(const std::string &head)
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

Json::Value gpt3_5::store_image(const std::string &url,
                                const msg_meta &conf)
{
    if (url.empty()) return Json::Value();

    const std::string dir = media_dir();
    // 临时名用 uuid，下载后再按内容命名，避免并发覆盖。
    //
    // 注意扩展名不能随便给：Magick::Image 靠**文件扩展名**选解码器，
    // 用 .tmp 会直接失败（实测 "no decode delegate for this image format
    // `TMP'"）。这里统一用 .img，再靠下面对文件头的嗅探判断真实格式。
    const std::string tmp_name = generate_uuid();
    const fs::path tmp_path = fs::path(dir) / (tmp_name + ".img");
    try {
        fs::create_directories(dir);
        download(url, dir, tmp_name + ".img");
    }
    catch (const std::exception &e) {
        conf.p->setlog(LOG::WARNING,
                       std::string("openai: image download failed: ") + e.what());
        return Json::Value();
    }
    catch (...) {
        conf.p->setlog(LOG::WARNING, "openai: image download failed");
        return Json::Value();
    }

    Json::Value part;
    try {
        // 空文件 / 极小的响应通常是 404 页面或鉴权失败，
        // download() 内部对 HTTP 错误码不抛异常，会把错误页写进来。
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

        // 入库时就压缩，避免历史里躺着 4000px 的原图。
        // 注意：Geometry::aspect(true) 在 resize() 里不等于"保持比例"，
        // 实测 4000x3000 会被拉成 2048x2048。这里自己算等比缩放。
        //
        // 先按文件头嗅探真实格式：临时文件没有可信扩展名，直接构造
        // Magick::Image(path) 会因"没有解码委托"而失败。这里显式把
        // 魔术字节对应的格式塞进 magick()，让 ImageMagick 按内容解码。
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
        img.write(&blob);

        std::string bytes(static_cast<const char *>(blob.data()), blob.length());
        if (bytes.size() < 64) {
            conf.p->setlog(LOG::WARNING, "openai: converted image is empty");
            part = Json::Value();
        }
        else {
            std::string sha = sha256_hex(bytes);
            std::string fname = sha + ".jpg";
            fs::path final_path = fs::path(dir) / fname;
            if (!fs::exists(final_path)) {
                write_binary(final_path, bytes);
            }

            part["type"] = "image_ref";
            part["file"] = fname;
            part["mime"] = "image/jpeg";
        }
    }
    catch (const std::exception &e) {
        conf.p->setlog(LOG::WARNING,
                       std::string("openai: image convert failed: ") + e.what());
        part = Json::Value();
    }
    catch (...) {
        part = Json::Value();
    }

    std::error_code ec;
    fs::remove(tmp_path, ec);
    return part;
}

/// 递归扫描任意 JSON 结构，找出所有看起来像图片节点的对象。
///
/// 为什么要递归而不是按 type 判断：NapCat 在「回复一条合并转发」时
/// 返回的不是 [CQ:forward,id=...]，而是把转发节点的内容**直接内联展开**，
/// 且这些内联节点没有 type 字段。实测结构：
///   data.message[0].data.content[0].message[0].data = {file, url, file_size, ...}
/// 只认 type=="image" 会漏掉全部图片。
///
/// 判定规则：一个对象同时满足
///   - 有 url 字段且非空
///   - 有 file 或 file_size 字段（避免把无关的外链当成图片）
static void scan_for_image_urls(const Json::Value &node,
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
        std::string url = data.get("url", "").asString();
        const bool looks_image =
            !url.empty() &&
            (data.isMember("file") || data.isMember("file_size") ||
             data.get("sub_type", Json::Value()).isInt());
        if (looks_image) {
            // 去重：同一张图可能在多个层级重复出现
            if (std::find(urls.begin(), urls.end(), url) == urls.end()) {
                urls.push_back(url);
            }
        }
    }

    // 继续向下找：message / content / data 都可能嵌套
    for (const char *key : {"message", "content", "data", "messages"}) {
        if (node.isMember(key)) {
            scan_for_image_urls(node[key], urls, depth + 1);
        }
    }
}

/// 从一条消息的 message/content（messageArr 或 CQ 字符串）里抽出图片 URL。
void gpt3_5::collect_images_from_message(const Json::Value &msg,
                                         std::vector<std::string> &urls)
{
    if (msg.isArray() || msg.isObject()) {
        scan_for_image_urls(msg, urls, 0);
        return;
    }

    // 字符串形态：CQ 码
    std::string s = msg.isString() ? msg.asString() : "";
    if (s.empty()) return;
    // 线性扫描代替 std::regex(R"(\[CQ:image[^\]]*\])")：后者的 [^\]]*
    // 在超长连续段上会爆栈。
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
    // 和文本展开共用同一套预算口径：QQ 虽然只允许 4 层嵌套，但每层节点
    // 数可达上百，逐层递归会让 get_forward_msg 的请求数指数增长。这里同样
    // 用 visited 去重 + 节点总数上限，避免把同一棵转发树反复拉取几十万次。
    static thread_local std::set<std::string> visited;
    static thread_local int total_nodes = 0;
    if (depth == 0) {
        // 每次顶层调用重置预算（同一条消息可能触发多次展开）
        visited.clear();
        total_nodes = 0;
    }
    if (depth > 5) return; // 和文本展开的深度上限保持一致
    if (total_nodes >= 200) return;
    if (!visited.insert(forward_id).second) return; // 已展开过，跳过

    Json::Value param;
    param["id"] = forward_id;
    Json::Value info;
    try {
        info = string_to_json(p->cq_send("get_forward_msg", param));
    }
    catch (const std::string &e) {
        p->setlog(LOG::WARNING,
               "openai: get_forward_msg threw: " + e);
        return;
    }
    catch (...) {
        p->setlog(LOG::WARNING, "openai: get_forward_msg threw (unknown)");
        return;
    }

    // 诊断：转发取不到图时，先看清楚 NapCat 到底返回了什么形状
    if (info["retcode"].asInt() != 0 || !info.isMember("data")) {
        p->setlog(LOG::WARNING,
               "openai: forward retcode=" +
                   std::to_string(info["retcode"].asInt()) +
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
        // 只打第一条消息的形状，够判断字段名了
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
        if (total_nodes >= 200) break; // 预算用完，剩下的节点不再处理
        ++total_nodes;
        // 整条 node 丢进去递归扫描：NapCat 的节点结构不固定，
        // 图片可能在 message / content / data 任意一层
        collect_images_from_message(m, urls);

        // 嵌套转发：显式的 [CQ:forward,id=...] 还是要去 fetch
        const Json::Value &body =
            m.isMember("message") ? m["message"] : m["content"];
        if (body.isArray()) {
            for (const auto &seg : body) {
                if (!seg.isObject()) continue;
                if (seg.get("type", "").asString() != "forward") continue;
                std::string nested = seg["data"].get("id", "").asString();
                if (!nested.empty()) {
                    collect_forward_images(p, nested, urls, depth + 1);
                }
            }
        }
        else if (body.isString()) {
            // 线性扫描（原来用 fwd_re 正则，超长连续段会爆栈）
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
        std::string text; // 图片时是 url
    };
    std::vector<Segment> segs;

    // 线性扫描代替 std::regex(R"(\[CQ:image[^\]]*\])")：
    // 后者的 [^\]]* 在超长连续段上会爆栈 SIGSEGV。
    for (const auto &cqseg : cqscan::split(message)) {
        if (!cqseg.is_cq || cqseg.type != "image") {
            if (!cqseg.text.empty()) segs.push_back({false, cqseg.text});
            continue;
        }
        std::string url = cq_decode(cqscan::arg_value(cqseg.args, "url"));
        segs.push_back({true, url});
    }

    // 被引用消息里的图：常见用法是「回复这张图问问题」。
    // 如果被引用的是合并转发，图藏在转发内容里，要单独展开。
    std::vector<std::string> quoted_images;
    std::vector<std::string> forward_urls;
    if (reply_id != -1) {
        try {
            Json::Value param;
            param["message_id"] = reply_id;
            Json::Value info =
                string_to_json(conf.p->cq_send("get_msg", param));
            // 诊断：被引用消息的原始结构（转发/图片都靠它判断）
            conf.p->setlog(
                LOG::INFO,
                "openai: quoted reply_id=" + std::to_string(reply_id) +
                    " retcode=" + std::to_string(info["retcode"].asInt()) +
                    " body=" +
                    Json::writeString(Json::StreamWriterBuilder(), info)
                        .substr(0, 600));
            if (info["retcode"].asInt() == 0 &&
                info["data"].isMember("message")) {
                const Json::Value &quoted_msg = info["data"]["message"];
                // 递归扫描：被引用消息可能是普通图，也可能是 NapCat
                // 内联展开的转发节点（没有 [CQ:forward，也没有 type 字段）
                collect_images_from_message(quoted_msg, quoted_images);
                conf.p->setlog(
                    LOG::INFO,
                    "openai: quoted images found=" +
                        std::to_string(quoted_images.size()));

                // 引用的消息本身是转发：展开里面的图
                std::string quoted = messageArr_to_string(quoted_msg);
                // 线性扫描（原来用 fwd_re 正则，超长连续段会爆栈）
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

    // 当前消息本身就是合并转发：展开里面的图。
    // 这些图不在 message 文本里（文本里只有 [CQ:forward,id=...]），
    // 所以不会和 segs 重复。
    {
        for (const auto &cqseg : cqscan::split(message)) {
            if (!cqseg.is_cq || cqseg.type != "forward") continue;
            const std::string id = cqscan::arg_value(cqseg.args, "id");
            if (!id.empty()) {
                collect_forward_images(conf.p, cq_decode(id), forward_urls, 0);
            }
        }
    }

    // 当前消息里有没有直接贴的图（不是转发里的）
    const bool has_inline_image =
        std::any_of(segs.begin(), segs.end(),
                    [](const Segment &s) { return s.is_image; });

    if (!has_inline_image && quoted_images.empty() && forward_urls.empty()) {
        return Json::Value(); // 纯文本：沿用旧格式
    }

    Json::Value content(Json::arrayValue);
    // segs 里的图就是"当前消息 + 被引用消息"里直接贴的图
    for (const auto &s : segs) {
        if (s.is_image) {
            Json::Value part = store_image(s.text, conf);
            if (part.isNull()) continue; // 下载/转换失败，跳过这张
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

    // 被引用消息里直接贴的图（不是转发里的）
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

    // forward_urls 才是"合并转发记录里"的图，单独追加并加说明，
    // 让模型知道这些图来自转发的聊天记录，不是当前消息。
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

Json::Value gpt3_5::expand_content_for_api(const Json::Value &content)
{
    if (content.isString()) return content;
    if (!content.isArray()) return content;

    Json::Value out(Json::arrayValue);
    for (const auto &part : content) {
        std::string type = part.get("type", "").asString();
        if (type != "image_ref") {
            out.append(part);
            continue;
        }

        const std::string file = part.get("file", "").asString();
        const std::string mime = part.get("mime", "image/jpeg").asString();
        fs::path path = fs::path(media_dir()) / file;
        if (file.empty() || !fs::exists(path)) {
            // 文件被清理掉了：降级成文字，别让整条请求失败
            Json::Value fallback;
            fallback["type"] = "text";
            fallback["text"] = "[图片]";
            out.append(fallback);
            continue;
        }

        std::string bytes = read_binary(path);
        if (bytes.empty()) {
            Json::Value fallback;
            fallback["type"] = "text";
            fallback["text"] = "[图片]";
            out.append(fallback);
            continue;
        }
        Json::Value img;
        img["type"] = "image_url";
        img["image_url"]["url"] =
            "data:" + mime + ";base64," + b64_encode(bytes);
        out.append(img);
    }
    return out;
}

std::string gpt3_5::content_to_text(const Json::Value &content)
{
    if (content.isString()) return content.asString();
    if (!content.isArray()) return "";

    std::string out;
    for (const auto &part : content) {
        std::string type = part.get("type", "").asString();
        if (type == "text") {
            out += part.get("text", "").asString();
        }
        else if (type == "image_ref" || type == "image_url") {
            out += "[图片]";
        }
    }
    return out;
}

// ───────────────────── 工具调用（web search）─────────────────────

Json::Value gpt3_5::build_tool_defs()
{
    Json::Value tools(Json::arrayValue);
    if (!search_enabled) return tools;

    Json::Value tool;
    tool["type"] = "function";
    Json::Value &f = tool["function"];
    f["name"] = "web_search";
    f["description"] =
        "搜索互联网获取实时信息。当问题涉及新闻、近期事件、具体数据、"
        "或你不确定的事实（尤其是训练数据之后发生的事）时使用。"
        "返回结果的标题、链接和摘要。可以多次调用以换用不同关键词。";

    Json::Value &params = f["parameters"];
    params["type"] = "object";
    params["properties"]["query"]["type"] = "string";
    params["properties"]["query"]["description"] =
        "搜索关键词，支持 site: / -site: / after: 等语法";
    params["required"].append("query");
    params["additionalProperties"] = false;

    tools.append(tool);
    return tools;
}

std::string gpt3_5::tool_web_search(const std::string &query,
                                    const msg_meta &conf)
{
    if (!search_enabled) return "搜索功能未启用。";

    std::string q = trim(query);
    if (q.empty()) return "搜索关键词为空。";

    Json::Value req;
    req["query"] = q;
    req["count"] = search_count;
    // 摘要（600~800 字符）比 snippet 更适合喂模型，正文按需才取
    req["need_content"] = false;

    Json::Value resp;
    try {
        resp = string_to_json(do_post(
            search_base_url, "/v1/tools/search", false, req,
            {{"Content-Type", "application/json"},
             {"Authorization", "Bearer " + search_api_key}},
            false));
    }
    catch (const std::string &e) {
        conf.p->setlog(LOG::WARNING, "openai: web_search failed: " + e);
        // 把失败明确告诉模型，让它自己决定换词重试还是如实说搜不到
        return "搜索失败（接口报错），请换一个关键词重试，或直接说明无法检索。";
    }
    catch (...) {
        conf.p->setlog(LOG::WARNING, "openai: web_search failed (unknown)");
        return "搜索失败（连接异常），请稍后重试或说明无法检索。";
    }

    if (resp.isMember("error")) {
        std::string msg = resp["error"].isMember("message")
                              ? resp["error"]["message"].asString()
                              : "unknown error";
        conf.p->setlog(LOG::WARNING, "openai: web_search api error: " + msg);
        return "搜索接口返回错误：" + msg;
    }

    const Json::Value &results = resp["results"];
    if (!results.isArray() || results.empty()) {
        return "没有找到「" + q + "」的相关结果。";
    }

    Json::Value out(Json::arrayValue);
    int idx = 0;
    for (const auto &r : results) {
        if (++idx > search_count) break;
        Json::Value item;
        item["title"] = r.get("title", "").asString();
        item["url"] = r.get("url", "").asString();
        // summary 比 snippet 信息量大，模型读这个
        std::string body = r.get("summary", "").asString();
        if (body.empty()) body = r.get("snippet", "").asString();
        item["summary"] = body;
        std::string date = r.get("date", "").asString();
        if (!date.empty()) item["date"] = date;
        out.append(item);
    }

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "  ";
    wb["emitUTF8"] = true;
    return "搜索结果（" + q + "）：\n" +
           Json::writeString(wb, out);
}

std::string gpt3_5::exec_tool_call(const Json::Value &call,
                                   const msg_meta &conf)
{
    std::string name = call["function"]["name"].asString();
    std::string raw_args = call["function"]["arguments"].asString();

    Json::Value args;
    try {
        args = string_to_json(raw_args);
    }
    catch (...) {
        // 模型偶尔给出非法 JSON，别让整轮对话挂掉
        return "工具参数解析失败，请重新组织参数后重试。";
    }

    if (name == "web_search") {
        return tool_web_search(args.get("query", "").asString(), conf);
    }
    return "未知工具：" + name;
}

/// 识别"声称调用了工具、却没给 tool_calls 内容"的畸形响应。
/// 典型来源：把流式响应拼回非流式的转接层只累积了 delta.content，
/// 却把 finish_reason 原样透传（实测 shinxbot-stream-adapter 就是这样）。
bool gpt3_5::is_malformed_tool_response(const Json::Value &resp)
{
    if (!resp.isMember("choices") || !resp["choices"].isArray() ||
        resp["choices"].empty()) {
        return false;
    }
    const Json::Value &ch = resp["choices"][0];
    if (ch.get("finish_reason", "").asString() != "tool_calls") return false;

    const Json::Value &calls = ch["message"]["tool_calls"];
    const bool has_calls = calls.isArray() && !calls.empty();
    const bool has_text = !trim(ch["message"].get("content", "").asString()).empty();
    // 有 finish_reason=tool_calls 却没有工具调用、也没有正文 → 内容丢了
    return !has_calls && !has_text;
}

Json::Value gpt3_5::run_chat_with_tools(Json::Value request, size_t keyid,
                                       const msg_meta &conf)
{
    Json::Value tools = build_tool_defs();
    if (!tools.empty()) {
        request["tools"] = tools;
        request["tool_choice"] = "auto";
    }

    // 一旦发现中转丢了 tool_calls，整轮改走兜底端点重跑。
    // 之所以是"整轮"而不是"这一次请求"：工具循环必须是同一模型的连续
    // 对话，中途换模型会让 tool_call_id 对不上。
    std::string cur_base = base_url;
    std::string cur_model = model_name;
    std::string cur_key = key[keyid];
    bool using_fallback = false;

    Json::Value resp;
    int rounds = 0;
    // 上游偶发返回空流（中转超时后透传 "choices":[] 的收尾块）。
    // 这种是瞬时的，重试往往立刻就好，不该把错误抛给用户。
    int empty_retries = 0;
    static const int kMaxEmptyRetries = 2;

    // 判断"上游给了个空壳"：choices 为空数组，且 usage 里一个 token 都没算。
    // 注意不能用 "choices 为空" 单独判定 —— 工具轮次里也可能出现空 choices，
    // 那种情况有 token 用量，不能当失败。
    auto looks_like_empty_upstream = [](const Json::Value &r) {
        if (!r.isObject()) return false;
        if (!r.isMember("choices") || !r["choices"].isArray()) return false;
        if (!r["choices"].empty()) return false;
        const Json::Value &u = r["usage"];
        if (!u.isObject()) return true; // 连 usage 都没有，八成是空壳
        const int64_t prompt = u.get("prompt_tokens", 0).asInt64();
        const int64_t completion = u.get("completion_tokens", 0).asInt64();
        return prompt == 0 && completion == 0;
    };

    while (true) {
        try {
            request["model"] = cur_model;
            const std::string raw = do_post(
                cur_base, "/v1/chat/completions", false, request,
                {{"Content-Type", "application/json"},
                 {"Authorization", "Bearer " + cur_key}},
                true);
            resp = parse_api_response(raw);
            // 中转把上游流式原文透传过来（通常是上游超时返回空流）：
            // 这既不是正常响应也不是明确报错，给出可理解的原因，
            // 而不是让它掉进"缺少 choices"那个笼统分支。
            if (resp.isNull() ||
                (resp.isObject() && resp.isMember("__sse_empty__")) ||
                looks_like_empty_upstream(resp)) {
                if (empty_retries < kMaxEmptyRetries) {
                    ++empty_retries;
                    conf.p->setlog(
                        LOG::WARNING,
                        "openai: 上游返回空响应，第 " +
                            std::to_string(empty_retries) + " 次重试");
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(800 * empty_retries));
                    continue;
                }
                Json::Value err;
                err["error"]["message"] =
                    "上游连续返回空响应（模型超时或中转中断），已重试 "
                    + std::to_string(kMaxEmptyRetries) + " 次。";
                return err;
            }
            if (!resp.isMember("choices") && !resp.isMember("error")) {
                Json::Value err;
                err["error"]["message"] =
                    "上游响应不是有效的 chat.completion。";
                return err;
            }
        }
        catch (const std::string &e) {
            Json::Value err;
            err["error"]["message"] = e;
            return err;
        }
        catch (...) {
            Json::Value err;
            err["error"]["message"] = "http connection failed.";
            return err;
        }

        // 中转丢了 tool_calls：切到兜底端点，本轮从头重来
        if (!using_fallback && is_malformed_tool_response(resp) &&
            !trim(tool_fallback_base_url).empty() &&
            !trim(tool_fallback_key).empty() &&
            !trim(tool_fallback_model).empty()) {
            using_fallback = true;
            tool_fallback_count++;
            conf.p->setlog(
                LOG::WARNING,
                "openai: 中转未返回 tool_calls，本轮改用兜底端点 "
                + tool_fallback_base_url + " model=" + tool_fallback_model);
            cur_base = tool_fallback_base_url;
            cur_model = tool_fallback_model;
            cur_key = tool_fallback_key;
            rounds = 0;
            continue;
        }

        if (resp.isMember("error")) return resp;
        if (!resp.isMember("choices") || !resp["choices"].isArray() ||
            resp["choices"].empty()) {
            return resp;
        }

        const Json::Value &msg = resp["choices"][0]["message"];
        const Json::Value &calls = msg["tool_calls"];

        // 没有工具调用 = 正常结束
        if (!calls.isArray() || calls.empty()) return resp;

        // 防止模型无限调工具烧钱
        if (rounds >= search_max_rounds) {
            conf.p->setlog(LOG::WARNING,
                           "openai: tool call round limit reached");
            return resp;
        }
        rounds++;

        // 把 assistant 的 tool_calls 原样追加进对话，再逐条回灌结果
        Json::Value history_msgs = request["messages"];
        Json::Value assistant_msg;
        assistant_msg["role"] = "assistant";
        assistant_msg["content"] = msg.get("content", Json::Value());
        assistant_msg["tool_calls"] = calls;
        history_msgs.append(assistant_msg);

        for (const auto &call : calls) {
            std::string result = exec_tool_call(call, conf);
            Json::Value tool_msg;
            tool_msg["role"] = "tool";
            tool_msg["tool_call_id"] = call.get("id", "").asString();
            tool_msg["content"] = result;
            history_msgs.append(tool_msg);
        }

        request["messages"] = history_msgs;
        // 工具轮次不再需要重复附上 tools（部分中转会报错），保持简洁
    }
}

DECLARE_FACTORY_FUNCTIONS(gpt3_5)

