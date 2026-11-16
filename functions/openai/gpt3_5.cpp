#include "gpt3_5.h"
#include "cqscan.h"
#include "openai_internal.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace {

class SessionGuard {
public:
    SessionGuard(gpt3_5 &owner, uint64_t task_id,
                 std::function<void(uint64_t)> rel)
        : owner_(owner),
          task_id_(task_id),
          rel_(std::move(rel)),
          active_(task_id != 0) {}
    ~SessionGuard() { release(); }
    void release()
    {
        if (active_) {
            active_ = false;
            rel_(task_id_);
        }
    }
    SessionGuard(const SessionGuard &) = delete;
    SessionGuard &operator=(const SessionGuard &) = delete;

private:
    gpt3_5 &owner_;
    uint64_t task_id_;
    std::function<void(uint64_t)> rel_;
    bool active_;
};

} // namespace

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
        of << R"({"base_url": "https://api.openai.com","keys": [""],"model": "gpt-3.5-turbo-1106","mode": ["default"],"default": [{"role": "system", "content": "You are a helpful assistant."}],"black_list": ["股票"],"compress_summary_prompt": "","compress_recent_rounds": 10,"reply_on_at": true,"reply_on_keyword": false,"wake_keywords": ["bot", "猫猫"],"active_context_show_time": true,"active_context_show_user_id": true,"MAX_TOKEN": 4000,"MAX_REPLY": 700,"RED_LINE": 1000})";
        of.close();
    }

    std::string ans = readfile(openai_conf_path);
    Json::Value res = string_to_json(ans);

    static const Json::ArrayIndex kMaxKeys = 255;
    Json::ArrayIndex sz = res["keys"].size();
    if (sz > kMaxKeys) sz = kMaxKeys;
    for (Json::ArrayIndex i = 0; i < sz; ++i) {
        key.push_back(res["keys"][i].asString());
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
    default_temperature = res.get("temperature", 0.7).asDouble();
    compress_summary_prompt = res["compress_summary_prompt"].asString();
    compress_recent_rounds = res.get("compress_recent_rounds", 10).asInt();
    if (compress_recent_rounds < 1) compress_recent_rounds = 1;
    reply_on_at = res.get("reply_on_at", true).asBool();
    reply_on_keyword = res.get("reply_on_keyword", false).asBool();
    group_followup_sec = res.get("group_followup_sec", 0).asInt();
    if (group_followup_sec < 0) group_followup_sec = 0;
    active_context_show_time =
        res.get("active_context_show_time", true).asBool();
    active_context_show_user_id =
        res.get("active_context_show_user_id", true).asBool();
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
    if (search_max_rounds > 12) search_max_rounds = 12;

    web_fetch_enabled = res.get("web_fetch_enabled", true).asBool();
    web_fetch_max_chars = res.get("web_fetch_max_chars", 16000).asInt();
    if (web_fetch_max_chars < 500) web_fetch_max_chars = 500;

    bash_enabled = res.get("bash_enabled", false).asBool();
    bash_op_only = res.get("bash_op_only", true).asBool();
    bash_timeout_sec = res.get("bash_timeout_sec", 30).asInt();
    if (bash_timeout_sec < 1) bash_timeout_sec = 1;
    if (bash_timeout_sec > 120) bash_timeout_sec = 120;
    bash_max_output_chars = res.get("bash_max_output_chars", 12000).asInt();
    if (bash_max_output_chars < 500) bash_max_output_chars = 500;
    bash_workdir = trim(res.get("bash_workdir", "").asString());

    tool_fallback_base_url = res.get("tool_fallback_base_url", "").asString();
    tool_fallback_model = res.get("tool_fallback_model", "").asString();
    tool_fallback_key = res.get("tool_fallback_key", "").asString();
    if (tool_fallback_base_url.empty()) tool_fallback_base_url = search_base_url;
    if (tool_fallback_model.empty()) tool_fallback_model = "deepseek-chat";
    if (tool_fallback_key.empty()) tool_fallback_key = search_api_key;

    if (res.isMember("mode_overrides") && res["mode_overrides"].isObject()) {
        const Json::Value &mo = res["mode_overrides"];
        for (const std::string &mname : mo.getMemberNames()) {
            const Json::Value &item = mo[mname];
            if (!item.isObject()) continue;
            ModeOverride ov;
            if (item.isMember("base_url") && item["base_url"].isString()) {
                ov.has_base_url = true;
                ov.base_url = item["base_url"].asString();
            }
            if (item.isMember("model") && item["model"].isString()) {
                ov.has_model = true;
                ov.model = item["model"].asString();
            }
            if (item.isMember("temperature") && item["temperature"].isNumeric()) {
                ov.has_temperature = true;
                ov.temperature = item["temperature"].asDouble();
            }
            if (item.isMember("search_enabled") && item["search_enabled"].isBool()) {
                ov.has_search_enabled = true;
                ov.search_enabled = item["search_enabled"].asBool();
            }
            if (item.isMember("web_fetch_enabled") && item["web_fetch_enabled"].isBool()) {
                ov.has_web_fetch_enabled = true;
                ov.web_fetch_enabled = item["web_fetch_enabled"].asBool();
            }
            if (item.isMember("mcp_enabled") && item["mcp_enabled"].isBool()) {
                ov.has_mcp_enabled = true;
                ov.mcp_enabled = item["mcp_enabled"].asBool();
            }
            if (item.isMember("bash_enabled") && item["bash_enabled"].isBool()) {
                ov.has_bash_enabled = true;
                ov.bash_enabled = item["bash_enabled"].asBool();
            }
            mode_overrides[mname] = std::move(ov);
        }
    }

    if (res.isMember("mcp_servers") && res["mcp_servers"].isArray()) {
        for (const auto &item : res["mcp_servers"]) {
            if (!item.isObject()) continue;
            openai_mcp::McpServerConfig cfg;
            cfg.name = item.get("name", "mcp").asString();
            cfg.enabled = item.get("enabled", true).asBool();
            cfg.url = item.get("url", "").asString();
            cfg.host = item.get("host", "").asString();
            cfg.token = item.get("token", "").asString();
            cfg.ns = item.get("namespace", "").asString();
            cfg.tool_prefix = item.get("tool_prefix", "").asString();
            cfg.timeout_sec = item.get("timeout_sec", 30).asInt();
            if (!trim(cfg.url).empty()) {
                mcp_servers.push_back(std::move(cfg));
            }
        }
        mcp_manager.configure(mcp_servers);
    }

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
    J["temperature"] = default_temperature;
    for (const std::string &u : key) J["keys"].append(u);
    for (const std::string &u : modes) J["mode"].append(u);
    for (const std::string &u : wake_keywords) J["wake_keywords"].append(u);
    J["black_list"] = parse_set_to_json(black_list);
    J["compress_summary_prompt"] = compress_summary_prompt;
    J["compress_recent_rounds"] = compress_recent_rounds;
    J["reply_on_at"] = reply_on_at;
    J["reply_on_keyword"] = reply_on_keyword;
    J["group_followup_sec"] = group_followup_sec;
    J["active_context_show_time"] = active_context_show_time;
    J["active_context_show_user_id"] = active_context_show_user_id;
    J["MAX_TOKEN"] = MAX_TOKEN;
    J["MAX_REPLY"] = MAX_REPLY;
    J["RED_LINE"] = RED_LINE;
    J["search_api_key"] = search_api_key;
    J["search_base_url"] = search_base_url;
    J["search_enabled"] = search_enabled;
    J["search_count"] = search_count;
    J["search_max_rounds"] = search_max_rounds;
    J["web_fetch_enabled"] = web_fetch_enabled;
    J["web_fetch_max_chars"] = web_fetch_max_chars;
    J["bash_enabled"] = bash_enabled;
    J["bash_op_only"] = bash_op_only;
    J["bash_timeout_sec"] = bash_timeout_sec;
    J["bash_max_output_chars"] = bash_max_output_chars;
    if (!bash_workdir.empty()) J["bash_workdir"] = bash_workdir;
    J["tool_fallback_base_url"] = tool_fallback_base_url;
    J["tool_fallback_model"] = tool_fallback_model;
    J["tool_fallback_key"] = tool_fallback_key;
    for (const std::string &u : modes) {
        J[u] = mode_prompt[u];
    }
    if (!mode_overrides.empty()) {
        Json::Value mo(Json::objectValue);
        for (const auto &kv : mode_overrides) {
            Json::Value item(Json::objectValue);
            const ModeOverride &ov = kv.second;
            if (ov.has_base_url) item["base_url"] = ov.base_url;
            if (ov.has_model) item["model"] = ov.model;
            if (ov.has_temperature) item["temperature"] = ov.temperature;
            if (ov.has_search_enabled) item["search_enabled"] = ov.search_enabled;
            if (ov.has_web_fetch_enabled) item["web_fetch_enabled"] = ov.web_fetch_enabled;
            if (ov.has_mcp_enabled) item["mcp_enabled"] = ov.mcp_enabled;
            if (ov.has_bash_enabled) item["bash_enabled"] = ov.bash_enabled;
            mo[kv.first] = item;
        }
        J["mode_overrides"] = mo;
    }
    if (!mcp_servers.empty()) {
        Json::Value arr(Json::arrayValue);
        for (const auto &cfg : mcp_servers) {
            Json::Value item(Json::objectValue);
            item["name"] = cfg.name;
            item["enabled"] = cfg.enabled;
            item["url"] = cfg.url;
            if (!cfg.host.empty()) item["host"] = cfg.host;
            if (!cfg.token.empty()) item["token"] = cfg.token;
            if (!cfg.ns.empty()) item["namespace"] = cfg.ns;
            if (!cfg.tool_prefix.empty()) item["tool_prefix"] = cfg.tool_prefix;
            item["timeout_sec"] = cfg.timeout_sec;
            arr.append(item);
        }
        J["mcp_servers"] = arr;
    }
    writefile(bot_config_path(nullptr, "features/openai/openai.json"),
              J.toStyledString());
}

gpt3_5::ResolvedModeConfig gpt3_5::resolve_mode_config(const std::string &mode)
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    ResolvedModeConfig out;
    out.base_url = base_url;
    out.model = model_name;
    out.temperature = default_temperature;
    out.search_enabled = search_enabled;
    out.web_fetch_enabled = web_fetch_enabled;
    out.mcp_enabled = !mcp_servers.empty();
    out.bash_enabled = bash_enabled;

    auto it = mode_overrides.find(mode);
    if (it != mode_overrides.end()) {
        const ModeOverride &ov = it->second;
        if (ov.has_base_url && !trim(ov.base_url).empty()) out.base_url = ov.base_url;
        if (ov.has_model && !trim(ov.model).empty()) out.model = ov.model;
        if (ov.has_temperature) out.temperature = ov.temperature;
        if (ov.has_search_enabled) out.search_enabled = ov.search_enabled;
        if (ov.has_web_fetch_enabled) out.web_fetch_enabled = ov.web_fetch_enabled;
        if (ov.has_mcp_enabled) out.mcp_enabled = ov.mcp_enabled;
        if (ov.has_bash_enabled) out.bash_enabled = ov.bash_enabled;
    }
    return out;
}

std::string gpt3_5::do_black(std::string message)
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    bool filtered = false;
    for (std::string u : black_list) {
        if (u.empty()) continue;
        if (message.find(u) == std::string::npos) continue;

        filtered = true;
        std::string escaped_u;
        for (char c : u) {
            if (strchr(".^$*+?()[]{}|\\", c)) escaped_u += '\\';
            escaped_u += c;
        }
        std::regex black_regex;
        try {
            if (openai_internal::isASCII(u))
                black_regex = std::regex("\\b" + escaped_u + "\\b");
            else
                black_regex = std::regex(escaped_u);
            message = std::regex_replace(message, black_regex, "__");
        }
        catch (...) {
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
    if (key.empty()) return 0;
    size_t u = key_cycle % key.size();
    key_cycle = (u + 1) % key.size();
    return u;
}

int64_t gpt3_5::session_id_from_conf(const msg_meta &conf)
{
    return conf.message_type == "group" ? (conf.group_id << 1)
                                        : ((conf.user_id << 1) | 1);
}

uint64_t gpt3_5::try_acquire_session(int64_t id, const msg_meta &conf,
                                     const std::string &nickname,
                                     const std::string &stage,
                                     bool ensure_default_prompt)
{
    std::string busy_msg;
    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        if (!is_open) {
            busy_msg = "已关闭。" + close_message;
        }
        else {
            if (ensure_default_prompt && history.find(id) == history.end()) {
                pre_default[id] = default_prompt;
            }
            uint64_t tid = next_task_id++;
            if (tid == 0) tid = next_task_id++;
            RunningTask task;
            task.task_id = tid;
            task.id = id;
            task.is_group = (conf.message_type == "group");
            task.user_id = conf.user_id;
            task.nickname = nickname;
            task.stage = stage;
            task.started_at = std::chrono::steady_clock::now();
            running_tasks[tid] = std::move(task);
            return tid;
        }
    }
    conf.p->cq_send(busy_msg, conf);
    return 0;
}

void gpt3_5::release_session(uint64_t task_id)
{
    if (task_id == 0) return;
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    running_tasks.erase(task_id);
}

void gpt3_5::set_session_stage(uint64_t task_id, const std::string &stage)
{
    if (task_id == 0) return;
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    auto it = running_tasks.find(task_id);
    if (it != running_tasks.end()) {
        it->second.stage = stage;
    }
}

std::string gpt3_5::format_running_tasks(int64_t id)
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    std::vector<RunningTask> matched;
    for (const auto &kv : running_tasks) {
        if (kv.second.id == id) {
            matched.push_back(kv.second);
        }
    }
    if (matched.empty()) {
        return "当前会话没有正在运行的 AI 请求。";
    }
    const auto now = std::chrono::steady_clock::now();
    std::string out = "当前会话运行中的 AI 请求（共 " +
                      std::to_string(matched.size()) + " 个）：\n";
    for (const auto &t : matched) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           now - t.started_at)
                           .count();
        out += "- " + t.nickname + "(" + std::to_string(t.user_id) + ")：" +
               t.stage + "（已耗时 " + std::to_string(elapsed) + "s）\n";
    }
    return trim(out);
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
                              std::string *error_message, uint64_t task_id)
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
    Json::ArrayIndex recent_keep =
        static_cast<Json::ArrayIndex>(compress_recent_rounds * 2);
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

    const ResolvedModeConfig mcfg =
        resolve_mode_config(current_mode.empty() ? default_prompt : current_mode);

    set_session_stage(task_id, "压缩历史上下文");
    perform_archive(id, conf, true, true);

    Json::Value req;
    req["model"] = mcfg.model;
    req["temperature"] = 0.3;
    req["max_tokens"] = MAX_REPLY;

    Json::Value messages(Json::arrayValue);
    Json::Value prompt_messages =
        mode_prompt[current_mode.empty() ? default_prompt : current_mode];
    for (Json::ArrayIndex i = 0; i < prompt_messages.size(); ++i) {
        messages.append(prompt_messages[i]);
    }
    for (Json::ArrayIndex i = 0; i < older_history.size(); ++i) {
        Json::Value entry = older_history[i];
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
        resp = openai_internal::parse_api_response(
            do_post(mcfg.base_url, "/v1/chat/completions", false, req,
                    {{"Authorization", "Bearer " + key[keyid]}}, true));
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
        if (error_message)
            *error_message = resp["error"]["message"].asString();
        return false;
    }
    if (!resp.isMember("choices") || !resp["choices"].isArray() ||
        resp["choices"].empty()) {
        if (error_message) *error_message = "summary response format invalid";
        return false;
    }

    std::string summary =
        trim(resp["choices"][0]["message"]["content"].asString());
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
        if (!openai_internal::json_array_starts_with(history[id],
                                                     old_history)) {
            if (error_message)
                *error_message = "history changed while compressing.";
            return false;
        }
        for (Json::ArrayIndex i = old_history.size(); i < history[id].size();
             ++i) {
            tail.append(history[id][i]);
        }
        for (Json::ArrayIndex i = 0; i < tail.size(); ++i) {
            new_history.append(tail[i]);
        }
        history[id] = new_history;
        last_prompt_tokens[id] = 0;
        if (pre_default.find(id) == pre_default.end()) {
            pre_default[id] =
                current_mode.empty() ? default_prompt : current_mode;
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
    std::time_t now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    now += 8 * 3600;
    tm tm_utc8_res = *std::gmtime(&now);
    char time_buf[32] = "??/?? ??:??";
    std::strftime(time_buf, sizeof(time_buf), "%m/%d %H:%M", &tm_utc8_res);
    return time_buf;
}

std::string gpt3_5::simplify_cq_codes(const std::string &message,
                                       userid_t botqq)
{
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

std::string gpt3_5::strip_bot_at(const std::string &message,
                                 const msg_meta &conf)
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

bool gpt3_5::message_mentions_bot(const std::string &message,
                                  const msg_meta &conf)
{
    std::string bot_at = "[CQ:at,qq=" + std::to_string(conf.p->get_botqq());
    return message.find(bot_at) != std::string::npos;
}

bool gpt3_5::message_has_wake_keyword(const std::string &message)
{
    std::string lower_message = message;
    std::transform(
        lower_message.begin(), lower_message.end(), lower_message.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    for (std::string keyword : wake_keywords) {
        keyword = trim(keyword);
        if (keyword.empty()) continue;
        std::transform(keyword.begin(), keyword.end(), keyword.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (lower_message.find(keyword) != std::string::npos) return true;
    }
    return false;
}

bool gpt3_5::is_followup_active(int64_t group_id, userid_t user_id)
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    if (group_followup_sec <= 0) return false;
    auto key_pair = std::make_pair(group_id, user_id);
    auto it = active_followup_until.find(key_pair);
    if (it == active_followup_until.end()) return false;
    if (std::chrono::steady_clock::now() > it->second) {
        active_followup_until.erase(it);
        return false;
    }
    return true;
}

void gpt3_5::refresh_followup(int64_t group_id, userid_t user_id)
{
    std::lock_guard<std::recursive_mutex> lock(data_lock);
    if (group_followup_sec <= 0) return;
    active_followup_until[std::make_pair(group_id, user_id)] =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(group_followup_sec);
}

void gpt3_5::process(std::string message, const msg_meta &conf)
{
    if (conf.user_id == conf.p->get_botqq()) return;

    conf.p->setlog(LOG::INFO,
                   "openai: recv type=" + conf.message_type +
                       " user=" + std::to_string(conf.user_id) +
                       " len=" + std::to_string(message.size()) +
                       " head=" + message.substr(0, 120));

    int64_t reply_id = -1;
    openai_internal::strip_leading_reply_segment(message, reply_id);

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

    int64_t id = session_id_from_conf(conf);
    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        if (pre_default.find(id) == pre_default.end() ||
            pre_default[id].empty()) {
            pre_default[id] = default_prompt;
        }
    }

    std::string nickname = get_cached_nickname(conf);
    bool is_group = conf.message_type == "group";
    bool at_mentioned = is_group && message_mentions_bot(message, conf);
    bool keyword_mentioned = is_group && message_has_wake_keyword(message);
    bool followup_hit =
        is_group && !starts_with(trim(message), "bot.") &&
        (!starts_with(trim(message), ".") || starts_with(trim(message), ".ai")) &&
        is_followup_active(conf.group_id, conf.user_id);

    std::string working_message = message;
    if (at_mentioned) {
        working_message = strip_bot_at(working_message, conf);
    }

    bool explicit_ai = starts_with(working_message, ".ai");
    bool should_reply = explicit_ai || (reply_on_at && at_mentioned) ||
                        (reply_on_keyword && keyword_mentioned) ||
                        followup_hit || !is_group;

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

    if (explicit_ai &&
        (message == "help" || message == "帮助" || message == ".help")) {
        cmd_full_help(conf);
        return;
    }
    if (explicit_ai && (command == "bg" || command == ".bg")) {
        conf.p->cq_send(format_running_tasks(id), conf);
        return;
    }
    if (explicit_ai && (command == "mcp" || command == ".mcp")) {
        if (args == "reload" && conf.p->is_op(conf.user_id)) {
            mcp_manager.reset_sessions();
        }
        conf.p->cq_send(mcp_manager.status_summary(), conf);
        return;
    }

    const std::vector<cmd_exact_rule> exact_rules = {
        {".test",
         [&]() {
             conf.p->cq_send(message, conf);
             return true;
         }},
        {".bg",
         [&]() {
             conf.p->cq_send(format_running_tasks(id), conf);
             return true;
         }},
        {".mcp",
         [&]() {
             if (args == "reload" && conf.p->is_op(conf.user_id)) {
                 mcp_manager.reset_sessions();
             }
             conf.p->cq_send(mcp_manager.status_summary(), conf);
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
             if (key.empty()) {
                 conf.p->cq_send("No avaliable key!", conf);
                 return true;
             }
             uint64_t task_id = try_acquire_session(id, conf, nickname, "手动压缩上下文");
             if (task_id == 0) {
                 return true;
             }
             SessionGuard guard(
                 *this, task_id, [this](uint64_t tid) { release_session(tid); });

             size_t compress_keyid = get_avaliable_key();
             std::string compress_error;
             bool compressed = compress_history(
                 id, compress_keyid, conf, &compress_error, task_id);
             guard.release();
             if (compressed) {
                 save_history(id);
                 conf.p->cq_send("compress done.", conf);
             }
             else {
                 conf.p->cq_send(compress_error.empty() ? "compress failed."
                                                        : compress_error,
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
                     reply = "avaliable modes:";
                     for (const std::string &u : modes) {
                         reply += " " + u;
                         if (u == mode) {
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
                 }
                 else {
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
                 }
                 else {
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
                     }
                     else if (type == "reply") {
                         MAX_REPLY = num;
                         reply = "set MAX_REPLY to " + std::to_string(num);
                         do_save = true;
                     }
                     else if (type == "token") {
                         MAX_TOKEN = num;
                         reply = "set MAX_TOKEN to " + std::to_string(num);
                         do_save = true;
                     }
                     else if (type == "red") {
                         RED_LINE = num;
                         reply = "set RED_LINE to " + std::to_string(num);
                         do_save = true;
                     }
                     else if (type == "compress") {
                         if (num < 1) {
                             reply = "compress rounds must be >= 1";
                         }
                         else {
                             compress_recent_rounds = static_cast<int>(num);
                             reply = "set compress_recent_rounds to " +
                                     std::to_string(num);
                             do_save = true;
                         }
                     }
                     else if (type == "followup") {
                         if (num < 0) {
                             reply = "followup seconds must be >= 0";
                         }
                         else {
                             group_followup_sec = static_cast<int>(num);
                             reply = "set group_followup_sec to " +
                                     std::to_string(num);
                             do_save = true;
                         }
                     }
                     else {
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
             std::string cur_mode;
             {
                 std::lock_guard<std::recursive_mutex> lock(data_lock);
                 cur_mode = pre_default[id];
             }
             const ResolvedModeConfig mcfg = resolve_mode_config(cur_mode);
             {
                 std::lock_guard<std::recursive_mutex> lock(data_lock);
                 int64_t compress_threshold =
                     static_cast<int64_t>(MAX_TOKEN) -
                     static_cast<int64_t>(RED_LINE);
                 int64_t trim_threshold = static_cast<int64_t>(MAX_TOKEN) -
                                          static_cast<int64_t>(MAX_REPLY);
                 reply = "current ai status:\n";
                 reply += "model: " + mcfg.model + " (default: " + model_name + ")\n";
                 reply += "mode: " + cur_mode + "\n";
                 reply += "temperature: " + std::to_string(mcfg.temperature) + "\n";
                 reply += "MAX_TOKEN: " + std::to_string(MAX_TOKEN) + "\n";
                 reply += "MAX_REPLY: " + std::to_string(MAX_REPLY) + "\n";
                 reply += "RED_LINE: " + std::to_string(RED_LINE) + "\n";
                 reply += "compress keep rounds: " +
                          std::to_string(compress_recent_rounds) + "\n";
                 reply += "compress keep messages: " +
                          std::to_string(compress_recent_rounds * 2) + "\n";
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
                 reply += "active running tasks: " +
                          std::to_string(running_tasks.size()) + "\n";
                 reply += "reply_on_at: " + std::to_string(reply_on_at) + "\n";
                 reply += "reply_on_keyword: " +
                          std::to_string(reply_on_keyword) + "\n";
                 reply += "group_followup_sec: " +
                          std::to_string(group_followup_sec) + "\n";
                 reply += "web_search: " +
                          std::string((mcfg.search_enabled && search_enabled) ? "enabled" : "disabled");
                 if (mcfg.search_enabled && search_enabled) {
                     reply += " (count=" + std::to_string(search_count) +
                              ", max_rounds=" +
                              std::to_string(search_max_rounds) + ")";
                 }
                 else if (trim(search_api_key).empty()) {
                     reply += " (未配置 search_api_key)";
                 }
                 else {
                     reply += " (disabled)";
                 }
                 reply += "\n";
                 reply += "web_fetch: " +
                          std::string((mcfg.web_fetch_enabled && web_fetch_enabled) ? "enabled" : "disabled") +
                          "\n";
                 reply += "mcp: " + std::string(mcfg.mcp_enabled ? "enabled" : "disabled") +
                          " (" + std::to_string(mcp_servers.size()) + " servers)\n";
                 reply += "bash: " + std::string(mcfg.bash_enabled ? "enabled" : "disabled") +
                          " (op_only=" + std::to_string(bash_op_only) +
                          ", timeout=" + std::to_string(bash_timeout_sec) + "s)\n";
                 reply += "note: request order is prompt -> active history -> "
                          "current user";
             }
             openai_internal::send_forward_text(reply, conf, "AI Status");
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

    if (key.empty()) {
        conf.p->cq_send("No avaliable key!", conf);
        return;
    }

    uint64_t task_id =
        try_acquire_session(id, conf, nickname, "准备上下文与图片", false);
    if (task_id == 0) {
        return;
    }
    SessionGuard session_guard(
        *this, task_id, [this](uint64_t tid) { release_session(tid); });

    std::string cur_mode;
    {
        std::lock_guard<std::recursive_mutex> lock(data_lock);
        cur_mode = pre_default[id];
    }
    const ResolvedModeConfig mcfg = resolve_mode_config(cur_mode);

    size_t keyid = get_avaliable_key();
    Json::Value J, user_input_J, ign;
    user_input_J["role"] = "user";
    std::string prompt_content =
        format_context_message(message, conf, nickname, reply_id, true);
    user_input_J["content"] = prompt_content;

    Json::Value parts = build_user_content(message, conf, reply_id);
    if (!parts.isNull()) {
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

    J["model"] = mcfg.model;

    {
        std::unique_lock<std::recursive_mutex> lock_data(data_lock);
        Json::Value &h = history[id];
        while (((last_prompt_tokens[id] > 0 &&
                 last_prompt_tokens[id] > (int64_t)(MAX_TOKEN - RED_LINE)) ||
                (last_prompt_tokens[id] <= 0 &&
                 openai_internal::getlength(h) >
                     (int64_t)(MAX_TOKEN - RED_LINE))) &&
               h.size() > 2) {
            lock_data.unlock();
            std::string compress_error;
            bool compressed =
                compress_history(id, keyid, conf, &compress_error, task_id);
            lock_data.lock();
            if (!compressed) {
                break;
            }
            break;
        }
        while (((last_prompt_tokens[id] > 0 &&
                 last_prompt_tokens[id] > (int64_t)(MAX_TOKEN - MAX_REPLY)) ||
                (last_prompt_tokens[id] <= 0 &&
                 openai_internal::getlength(h) >
                     (int64_t)(MAX_TOKEN - MAX_REPLY)))) {
            if (h.size() <= 2) break;
            if (h.size() > 0) h.removeIndex(0, &ign);
            if (h.size() > 0) h.removeIndex(0, &ign);
            break;
        }

        Json::Value K = mode_prompt[cur_mode];
        Json::ArrayIndex sz = h.size();

        size_t media_budget =
            static_cast<size_t>(media_budget_mb * 1024 * 1024);

        std::vector<bool> keep_image(sz, false);
        for (Json::ArrayIndex idx = sz; idx > 0; --idx) {
            const Json::Value &entry = h[idx - 1];
            const Json::Value &content = entry["content"];
            if (!content.isArray()) continue;
            size_t need = 0;
            for (const auto &part : content) {
                if (part.get("type", "").asString() != "image_ref") continue;
                const std::string file = part.get("file", "").asString();
                fs::path p = fs::path(media_dir()) / file;
                std::error_code ec;
                auto fsz = fs::file_size(p, ec);
                if (ec) continue;
                need += fsz / 3 * 4 + 8;
            }
            if (need == 0) continue;
            if (need <= media_budget) {
                media_budget -= need;
                keep_image[idx - 1] = true;
            }
        }

        for (Json::ArrayIndex i = 0; i < sz; i++) {
            Json::Value entry = h[i];
            if (entry["content"].isArray()) {
                if (keep_image[i]) {
                    entry["content"] =
                        expand_content_for_api(entry["content"]);
                }
                else {
                    entry["content"] = degrade_images(entry["content"]);
                }
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

    J["temperature"] = mcfg.temperature;
    J["max_tokens"] = MAX_REPLY;
    J = run_chat_with_tools(J, keyid, task_id, conf, mcfg);
    conf.p->setlog(LOG::INFO, "openai: user " + std::to_string(conf.user_id));

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
            const std::string emsg = J["error"]["message"].asString();
            if (emsg.find("empty_content") != std::string::npos ||
                emsg.find("aggregated content empty") != std::string::npos) {
                conf.p->cq_send(
                    "模型这次只输出了思考、没有正文（跟回复上限无关）。"
                    "再发一次通常就好。",
                    conf);
            }
            else {
                conf.p->cq_send("Openai ERROR: " + emsg, conf);
            }
        }
    }
    else {
        if (!J.isMember("choices") || !J["choices"].isArray() ||
            J["choices"].empty()) {
            conf.p->setlog(
                LOG::WARNING,
                "openai: 响应缺少 choices，原始内容前 400 字节=" +
                    J.toStyledString().substr(0, 400));
            conf.p->cq_send("Openai ERROR: API 响应格式异常(缺少 choices)",
                            conf);
            return;
        }

        std::string aimsg = J["choices"][0]["message"]["content"].asString();
        std::string finish_reason =
            J["choices"][0].isMember("finish_reason")
                ? J["choices"][0]["finish_reason"].asString()
                : "";

        if (trim(aimsg).empty()) {
            const Json::Value &u = J["usage"];
            int64_t comp_tok =
                u.isObject() ? u.get("completion_tokens", 0).asInt64() : 0;
            int64_t prom_tok =
                u.isObject() ? u.get("prompt_tokens", 0).asInt64() : 0;
            conf.p->setlog(
                LOG::WARNING,
                "openai: 空正文 finish_reason=" + finish_reason +
                    " completion_tokens=" + std::to_string(comp_tok) +
                    " prompt_tokens=" + std::to_string(prom_tok) +
                    " tool_calls=" +
                    std::to_string(
                        J["choices"][0]["message"]["tool_calls"].size()) +
                    " reasoning_len=" +
                    std::to_string(J["choices"][0]["message"]
                                       .get("reasoning_content", "")
                                       .asString()
                                       .size()) +
                    " model=" +
                    J["choices"][0]
                        .get("message", Json::Value())
                        .get("model", "")
                        .asString() +
                    " raw=" +
                    J["choices"][0]["message"].toStyledString().substr(0, 300));

            if (finish_reason == "content_filter") {
                conf.p->cq_send("QAQ 响应被上游安全过滤拦掉了", conf);
            }
            else if (finish_reason == "length") {
                conf.p->cq_send(
                    "模型把回复预算用在了思考上，没能输出正文。"
                    "可用 .ai.set reply [更大的数值] 提高上限。",
                    conf);
            }
            else if (finish_reason == "tool_calls") {
                conf.p->cq_send(
                    "搜索轮次用尽了，模型没能给出结论。请再问一次，"
                    "或者把问题缩小一点。",
                    conf);
            }
            else if (comp_tok == 0) {
                conf.p->cq_send(
                    "上游没有返回内容（通道超时或中断）。请重试。", conf);
            }
            else {
                conf.p->cq_send(
                    "模型这次只输出了思考、没有正文（跟回复上限无关）。"
                    "已经自动重发过，还是这样；换个说法再问一次试试。",
                    conf);
            }
            return;
        }

        aimsg = do_black(aimsg);

        std::string send_msg = aimsg;
        if (finish_reason == "length") {
            send_msg += "\n(QAQ 响应因长度限制未完成)";
        }
        else if (finish_reason == "content_filter") {
            send_msg += "\n(QAQ 响应被上游安全过滤截断)";
            conf.p->setlog(
                LOG::WARNING,
                "openai: 回复被上游安全过滤截断 len=" +
                    std::to_string(aimsg.size()) + " tail=" +
                    aimsg.substr(aimsg.size() > 80 ? aimsg.size() - 80 : 0));
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

        std::string reply_msg =
            "[CQ:reply,id=" + std::to_string(conf.message_id) + "]" + send_msg;
        {
            std::unique_lock<std::recursive_mutex> lock_data(data_lock);
            last_prompt_tokens[id] = prompt_tokens;
            last_completion_tokens[id] = completion_tokens;
            last_total_tokens[id] = total_tokens;

            if (MAX_TOKEN < prompt_tokens || prompt_tokens > MAX_TOKEN - RED_LINE) {
                lock_data.unlock();
                std::string compress_error;
                bool compressed =
                    compress_history(id, keyid, conf, &compress_error);
                lock_data.lock();
                if (!compressed) {
                    if (MAX_TOKEN < prompt_tokens) {
                        lock_data.unlock();
                        fallback_trim_history(id, 3);
                        lock_data.lock();
                    }
                    else {
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
            Json::Value stored_user = user_input_J;
            if (!parts.isNull()) {
                stored_user["content"] = parts;
            }
            history[id].append(stored_user);
            history[id].append(J);
        }
        save_history(id);
        if (is_group) {
            refresh_followup(conf.group_id, conf.user_id);
        }
        session_guard.release();
        conf.p->cq_send(reply_msg, conf);
    }
}

bool gpt3_5::check(std::string message, const msg_meta &conf)
{
    if (conf.user_id == conf.p->get_botqq()) return false;

    int64_t reply_id = -1;
    openai_internal::strip_leading_reply_segment(message, reply_id);

    if (cmd_match_exact(message, {"你说的话我不喜欢"})) {
        return true;
    }
    if (conf.message_type == "group") {
        if ((reply_on_at && message_mentions_bot(message, conf)) ||
            (reply_on_keyword && message_has_wake_keyword(message))) {
            return true;
        }
        const std::string t = trim(message);
        if (!starts_with(t, "bot.") &&
            (!starts_with(t, ".") || starts_with(t, ".ai")) &&
            is_followup_active(conf.group_id, conf.user_id)) {
            return true;
        }
    }
    return cmd_match_prefix(message, {".ai"});
}

std::string gpt3_5::help()
{
    return "AI 对话：.ai [内容] 或 @bot。完整说明：.ai help";
}

std::string gpt3_5::help(const msg_meta &conf, help_level_t level)
{
    (void)conf;
    switch (level) {
        case help_level_t::bot_admin:
            return "AI 对话：.ai [内容] 或 @bot；.ai help 看全部。"
                   "OP 另有 .ai.sw/.ai.set/.ai.arc/.ai.mcp";
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
            "  .ai.bg            查看当前运行中的请求与进度\n"
            "  .ai.mcp           查看 MCP 服务与已注册工具状态\n"
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
                "  .ai.set reply|token|red|compress|followup [数值]  调整阈值\n"
                "  .ai.mcp reload           重置并重新加载 MCP 工具\n"
                "  .ai.debug               切换调试输出\n";
    }

    body +=
        "\n提示：群聊里 @bot 会直接进入对话，不必加 .ai。\n"
        "上下文超限时会自动压缩；也可随时 .ai.arc 手动存档。";

    if (search_enabled) {
        body += "\n联网搜索已启用：需要实时信息时模型会自己调 web_search，"
                "你不用特别说明。";
    }
    if (web_fetch_enabled) {
        body += "\n网页抓取已启用：贴出链接或需要读全文时模型会自动调 web_fetch。";
    }
    if (bash_enabled && (!bash_op_only || conf.p->is_op(conf.user_id))) {
        body += "\nBash 工具已启用：可由模型执行受超时保护的命令。";
    }

    conf.p->cq_send(trim(body), conf);
}

DECLARE_FACTORY_FUNCTIONS(gpt3_5)
