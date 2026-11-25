#include "gpt3_5.h"
#include "cqscan.h"
#include "openai_internal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <regex>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

std::string strip_html_for_llm(const std::string &raw)
{
    std::string lower_head = raw.substr(0, std::min<size_t>(raw.size(), 512));
    std::transform(lower_head.begin(), lower_head.end(), lower_head.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool looks_html =
        lower_head.find("<html") != std::string::npos ||
        lower_head.find("<!doctype html") != std::string::npos ||
        lower_head.find("<head") != std::string::npos ||
        lower_head.find("<body") != std::string::npos;
    if (!looks_html) {
        return raw;
    }

    std::string s = raw;
    try {
        static const std::regex re_script(
            R"(<script\b[^>]*>[\s\S]*?</script>)", std::regex::icase);
        static const std::regex re_style(
            R"(<style\b[^>]*>[\s\S]*?</style>)", std::regex::icase);
        static const std::regex re_comment(R"(<!--[\s\S]*?-->)");
        static const std::regex re_block(
            R"(</?(?:p|div|br|li|tr|h[1-6]|section|article|header|footer|pre)\b[^>]*>)",
            std::regex::icase);
        static const std::regex re_tag(R"(<[^>]+>)");
        s = std::regex_replace(s, re_script, " ");
        s = std::regex_replace(s, re_style, " ");
        s = std::regex_replace(s, re_comment, " ");
        s = std::regex_replace(s, re_block, "\n");
        s = std::regex_replace(s, re_tag, " ");
    }
    catch (...) {
    }

    s = cqscan::replace_all(s, "&nbsp;", " ");
    s = cqscan::replace_all(s, "&amp;", "&");
    s = cqscan::replace_all(s, "&lt;", "<");
    s = cqscan::replace_all(s, "&gt;", ">");
    s = cqscan::replace_all(s, "&quot;", "\"");
    s = cqscan::replace_all(s, "&#39;", "'");

    std::string out;
    out.reserve(s.size());
    int nl_count = 0;
    bool prev_space = false;
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n') {
            prev_space = false;
            if (nl_count < 2) {
                out.push_back('\n');
                ++nl_count;
            }
        }
        else if (c == ' ' || c == '\t') {
            if (!prev_space && nl_count == 0) {
                out.push_back(' ');
                prev_space = true;
            }
        }
        else {
            nl_count = 0;
            prev_space = false;
            out.push_back(c);
        }
    }
    return trim(out);
}

std::string truncate_utf8_chars(const std::string &s, size_t max_bytes)
{
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return s.substr(0, cut) + "\n...[内容过长，已截断，原文共 " +
           std::to_string(s.size()) + " 字节]";
}

} // namespace

bool gpt3_5::can_use_bash(const msg_meta &conf,
                          const ResolvedModeConfig &mcfg) const
{
    if (!mcfg.bash_enabled) return false;
    if (bash_op_only && (conf.p == nullptr || !conf.p->is_op(conf.user_id))) {
        return false;
    }
    return true;
}

Json::Value gpt3_5::build_tool_defs(const msg_meta &conf,
                                    const ResolvedModeConfig &mcfg)
{
    Json::Value tools(Json::arrayValue);

    if (mcfg.search_enabled && search_enabled) {
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
    }

    if (mcfg.web_fetch_enabled && web_fetch_enabled) {
        Json::Value tool;
        tool["type"] = "function";
        Json::Value &f = tool["function"];
        f["name"] = "web_fetch";
        f["description"] =
            "抓取指定 HTTP/HTTPS 网页或纯文本接口（如 GitHub README、raw 文件、文档页、API JSON）的正文内容。"
            "当用户给出具体链接，或 web_search 返回的摘要不够详细需要看全文时使用。";

        Json::Value &params = f["parameters"];
        params["type"] = "object";
        params["properties"]["url"]["type"] = "string";
        params["properties"]["url"]["description"] = "要抓取的完整 HTTP 或 HTTPS URL";
        params["properties"]["max_chars"]["type"] = "integer";
        params["properties"]["max_chars"]["description"] =
            "可选，最大返回字符/字节数，默认按服务端配置截断";
        params["required"].append("url");
        params["additionalProperties"] = false;

        tools.append(tool);
    }

    if (mcfg.mcp_enabled) {
        Json::Value mcp_tools = mcp_manager.build_openai_tool_defs();
        if (mcp_tools.isArray()) {
            for (const auto &mt : mcp_tools) {
                tools.append(mt);
            }
        }
    }

    if (can_use_bash(conf, mcfg)) {
        Json::Value tool;
        tool["type"] = "function";
        Json::Value &f = tool["function"];
        f["name"] = "bash";
        f["description"] =
            "在当前运行环境执行一条 bash 命令并返回 stdout/stderr 与退出码。"
            "可用于查看文件、用 curl/python/git 调研 GitHub 项目、处理数据等。"
            "注意受超时与输出长度限制，禁止交互式命令。";

        Json::Value &params = f["parameters"];
        params["type"] = "object";
        params["properties"]["command"]["type"] = "string";
        params["properties"]["command"]["description"] = "要执行的 bash 命令";
        params["properties"]["workdir"]["type"] = "string";
        params["properties"]["workdir"]["description"] =
            "可选，命令的工作目录";
        params["properties"]["timeout_sec"]["type"] = "integer";
        params["properties"]["timeout_sec"]["description"] =
            "可选，超时秒数（不超过配置上限）";
        params["required"].append("command");
        params["additionalProperties"] = false;

        tools.append(tool);
    }

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
    req["need_content"] = false;

    Json::Value resp;
    try {
        resp = string_to_json(do_post(
            search_base_url, "/v1/tools/search", false, req,
            {{"Authorization", "Bearer " + search_api_key}},
            false));
    }
    catch (const std::string &e) {
        conf.p->setlog(LOG::WARNING, "openai: web_search failed: " + e);
        return "搜索失败（接口报错），请换一个关键词重试，或直接说明无法检索。";
    }
    catch (...) {
        conf.p->setlog(LOG::WARNING, "openai: web_search failed (unknown)");
        return "搜索失败（连接异常），请稍后重试或说明无法检索。";
    }

    if (resp.isMember("error")) {
        std::string msg = resp["error"].isMember("message")
                              ? openai_internal::json_to_string(
                                    resp["error"]["message"])
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

std::string gpt3_5::tool_web_fetch(const std::string &url, int max_chars,
                                   const msg_meta &conf)
{
    if (!web_fetch_enabled) return "网页抓取功能未启用。";

    std::string u = trim(url);
    if (u.empty()) return "URL 为空。";
    if (!starts_with(u, "http://") && !starts_with(u, "https://")) {
        return "仅支持 http:// 或 https:// 协议的 URL。";
    }

    int limit = web_fetch_max_chars;
    if (max_chars > 0 && max_chars < limit) {
        limit = max_chars;
    }
    if (limit < 500) limit = 500;

    std::string raw;
    try {
        raw = do_get(u, {{"User-Agent", "shinxbot2-openai/1.0"}}, false);
    }
    catch (const std::string &e) {
        conf.p->setlog(LOG::WARNING, "openai: web_fetch failed: " + e);
        return "抓取失败（" + e + "）。";
    }
    catch (...) {
        conf.p->setlog(LOG::WARNING, "openai: web_fetch failed (unknown)");
        return "抓取失败（网络或解析异常）。";
    }

    if (raw.empty()) {
        return "目标网页返回空内容。";
    }

    std::string cleaned = strip_html_for_llm(raw);
    if (cleaned.empty()) {
        return "网页正文提取为空（可能是纯脚本渲染页面）。";
    }

    return "网页内容（" + u + "）：\n" +
           truncate_utf8_chars(cleaned, static_cast<size_t>(limit));
}

std::string gpt3_5::tool_bash(const std::string &command,
                              const std::string &workdir,
                              int timeout_sec,
                              const msg_meta &conf)
{
    std::string cmd = trim(command);
    if (cmd.empty()) return "命令为空。";

    std::string cwd = trim(workdir);
    if (cwd.empty()) cwd = bash_workdir;
    if (!cwd.empty()) {
        std::error_code ec;
        if (!std::filesystem::exists(cwd, ec) ||
            !std::filesystem::is_directory(cwd, ec)) {
            return "工作目录不存在或不可访问：" + cwd;
        }
    }

    int limit_sec = bash_timeout_sec > 0 ? bash_timeout_sec : 30;
    if (timeout_sec > 0 && timeout_sec < limit_sec) {
        limit_sec = timeout_sec;
    }
    if (limit_sec < 1) limit_sec = 1;
    if (limit_sec > 120) limit_sec = 120;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return "创建管道失败。";
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "创建子进程失败。";
    }

    if (pid == 0) {
        setpgid(0, 0);
        close(pipefd[0]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) != 0) {
                _exit(126);
            }
        }
        execl("/bin/bash", "bash", "-c", cmd.c_str(), nullptr);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }

    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    }

    std::string output;
    const size_t max_capture =
        static_cast<size_t>(std::max(bash_max_output_chars, 500)) * 2 + 4096;
    bool timed_out = false;
    bool truncated_capture = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(limit_sec);

    char buf[4096];
    bool eof = false;
    while (!eof) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timed_out = true;
            kill(-pid, SIGKILL);
            break;
        }
        auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - now)
                          .count();
        int wait_ms = static_cast<int>(std::min<long long>(rem_ms, 500));
        if (wait_ms < 1) wait_ms = 1;

        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN | POLLHUP;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        while (true) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n > 0) {
                if (output.size() < max_capture) {
                    size_t take = std::min<size_t>(
                        static_cast<size_t>(n), max_capture - output.size());
                    output.append(buf, take);
                    if (take < static_cast<size_t>(n)) {
                        truncated_capture = true;
                    }
                }
                else {
                    truncated_capture = true;
                }
            }
            else if (n == 0) {
                eof = true;
                break;
            }
            else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                eof = true;
                break;
            }
        }
    }

    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    int exit_code = -1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }

    if (conf.p != nullptr) {
        conf.p->setlog(LOG::INFO,
                       "openai: bash user=" + std::to_string(conf.user_id) +
                           " exit=" + std::to_string(exit_code) +
                           " timeout=" + std::to_string(timed_out) +
                           " cmd=" + cmd.substr(0, 120));
    }

    std::string header = "exit_code: " + std::to_string(exit_code);
    if (timed_out) {
        header += " (TIMED OUT after " + std::to_string(limit_sec) + "s)";
    }
    if (output.empty()) {
        return header + "\n(no output)";
    }
    std::string body =
        truncate_utf8_chars(output, static_cast<size_t>(bash_max_output_chars));
    if (truncated_capture && output.size() <= static_cast<size_t>(bash_max_output_chars)) {
        body += "\n...[输出过多，已截断]";
    }
    return header + "\n" + body;
}

std::string gpt3_5::exec_tool_call(const Json::Value &call,
                                   const msg_meta &conf,
                                   const ResolvedModeConfig &mcfg)
{
    std::string name = openai_internal::json_to_string(
        call.isMember("function") ? call["function"]["name"]
                                   : Json::Value());
    std::string raw_args = openai_internal::json_to_string(
        call.isMember("function") ? call["function"]["arguments"]
                                   : Json::Value());

    Json::Value args;
    try {
        args = string_to_json(raw_args);
    }
    catch (...) {
        return "工具参数解析失败，请重新组织参数后重试。";
    }

    if (name == "web_search" && mcfg.search_enabled && search_enabled) {
        return tool_web_search(args.get("query", "").asString(), conf);
    }
    if (name == "web_fetch" && mcfg.web_fetch_enabled && web_fetch_enabled) {
        int mc = args.get("max_chars", 0).asInt();
        return tool_web_fetch(args.get("url", "").asString(), mc, conf);
    }
    if (name == "bash" && can_use_bash(conf, mcfg)) {
        return tool_bash(args.get("command", "").asString(),
                         args.get("workdir", "").asString(),
                         args.get("timeout_sec", 0).asInt(), conf);
    }

    if (mcfg.mcp_enabled) {
        std::string out_text;
        std::string out_log;
        const std::string sid_str = std::to_string(session_id_from_conf(conf));
        if (mcp_manager.try_call_tool(name, args, sid_str, out_text, out_log)) {
            if (!out_log.empty()) {
                conf.p->setlog(LOG::WARNING, "openai: " + out_log);
            }
            return out_text;
        }
    }

    return "未知工具：" + name;
}

bool gpt3_5::is_malformed_tool_response(const Json::Value &resp)
{
    if (!resp.isMember("choices") || !resp["choices"].isArray() ||
        resp["choices"].empty()) {
        return false;
    }
    const Json::Value &ch = resp["choices"][0];
    if (openai_internal::json_to_string(ch.get("finish_reason", Json::Value())) !=
        "tool_calls") {
        return false;
    }

    const Json::Value &calls = ch["message"]["tool_calls"];
    const bool has_calls = calls.isArray() && !calls.empty();
    const bool has_text = !trim(openai_internal::message_content_text(
                              ch["message"].get("content", Json::Value())))
                              .empty();
    return !has_calls && !has_text;
}

std::string gpt3_5::describe_request_size(const Json::Value &request,
                                          int tool_round, int retry_no)
{
    auto jlen = [](const Json::Value &v) {
        if (v.isString()) return v.asString().size();
        if (v.isNull()) return size_t(0);
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        wb["emitUTF8"] = true;
        return Json::writeString(wb, v).size();
    };

    size_t total = jlen(request);
    size_t n_system = 0, n_user = 0, n_assistant = 0, n_tool = 0;
    size_t n_image = 0, n_tools_def = 0;
    int c_image = 0, c_msgs = 0;

    const Json::Value &msgs = request["messages"];
    if (msgs.isArray()) {
        c_msgs = static_cast<int>(msgs.size());
        for (const auto &m : msgs) {
            const std::string role = m.get("role", "").asString();
            const Json::Value &content = m["content"];
            size_t sz = jlen(content);

            if (content.isArray()) {
                for (const auto &part : content) {
                    if (part.get("type", "").asString() == "image_url") {
                        n_image += jlen(part);
                        ++c_image;
                    }
                }
            }
            if (role == "system") n_system += sz;
            else if (role == "user") n_user += sz;
            else if (role == "assistant") n_assistant += sz;
            else if (role == "tool") n_tool += sz;
        }
    }
    n_tools_def = jlen(request["tools"]);

    auto mb = [](size_t n) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2fMB", n / 1024.0 / 1024.0);
        return std::string(buf);
    };

    return "openai: 请求体积 total=" + mb(total) +
           " system=" + mb(n_system) +
           " user=" + mb(n_user) +
           " assistant=" + mb(n_assistant) +
           " tool=" + mb(n_tool) +
           " image=" + mb(n_image) + "(" + std::to_string(c_image) + "张)" +
           " tools_def=" + mb(n_tools_def) +
           " msgs=" + std::to_string(c_msgs) +
           " rounds=" + std::to_string(tool_round) +
           " retry=" + std::to_string(retry_no);
}

Json::Value gpt3_5::run_chat_with_tools(Json::Value request, size_t keyid,
                                        uint64_t task_id,
                                        const msg_meta &conf,
                                        const ResolvedModeConfig &mcfg)
{
    Json::Value tools = build_tool_defs(conf, mcfg);
    if (!tools.empty()) {
        request["tools"] = tools;
        request["tool_choice"] = "auto";
    }

    std::string cur_base = mcfg.base_url;
    std::string cur_model = mcfg.model;
    std::string cur_key = key[keyid];
    bool using_fallback = false;

    Json::Value resp;
    int rounds = 0;
    int empty_retries = 0;
    static const int kMaxEmptyRetries = 2;
    int empty_body_retries = 0;
    static const int kMaxEmptyBodyRetries = 3;

    auto looks_like_empty_upstream = [](const Json::Value &r) {
        if (!r.isObject()) return false;
        if (!r.isMember("choices") || !r["choices"].isArray()) return false;
        if (!r["choices"].empty()) return false;
        const Json::Value &u = r["usage"];
        if (!u.isObject()) return true;
        const int64_t prompt = openai_internal::json_to_int64(
            u.get("prompt_tokens", Json::Value()));
        const int64_t completion = openai_internal::json_to_int64(
            u.get("completion_tokens", Json::Value()));
        return prompt == 0 && completion == 0;
    };

    while (true) {
        try {
            request["model"] = cur_model;
            set_session_stage(task_id, "请求模型（第 " + std::to_string(rounds + 1) +
                                       " 轮）");
            if (rounds == 0 && empty_retries == 0 &&
                empty_body_retries == 0) {
                conf.p->setlog(LOG::INFO,
                               describe_request_size(request, rounds,
                                                     empty_retries));
            }
            const std::string raw = do_post(
                cur_base, "/v1/chat/completions", false, request,
                {{"Authorization", "Bearer " + cur_key}},
                true);
            resp = openai_internal::parse_api_response(raw);
            if (resp.isNull() ||
                (resp.isObject() && resp.isMember("__sse_empty__")) ||
                looks_like_empty_upstream(resp)) {
                if (empty_retries < kMaxEmptyRetries) {
                    ++empty_retries;
                    conf.p->setlog(
                        LOG::WARNING,
                        "openai: 上游返回空响应，第 " +
                            std::to_string(empty_retries) + " 次重试");
                    set_session_stage(task_id, "上游空响应，重试第 " +
                                               std::to_string(empty_retries) +
                                               " 次");
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(800 * empty_retries));
                    continue;
                }
                bool has_image = false;
                const Json::Value &msgs_probe = request["messages"];
                if (msgs_probe.isArray()) {
                    for (const auto &pm : msgs_probe) {
                        const Json::Value &pc = pm["content"];
                        if (!pc.isArray()) continue;
                        for (const auto &part : pc) {
                            if (part.get("type", "").asString() ==
                                "image_url") {
                                has_image = true;
                                break;
                            }
                        }
                        if (has_image) break;
                    }
                }
                if (!using_fallback && !has_image &&
                    !trim(tool_fallback_base_url).empty() &&
                    !trim(tool_fallback_key).empty() &&
                    !trim(tool_fallback_model).empty()) {
                    using_fallback = true;
                    conf.p->setlog(
                        LOG::WARNING,
                        "openai: 上游连续返回空响应，切到兜底端点 " +
                            tool_fallback_base_url + " model=" +
                            tool_fallback_model);
                    cur_base = tool_fallback_base_url;
                    cur_model = tool_fallback_model;
                    cur_key = tool_fallback_key;
                    empty_retries = 0;
                    continue;
                }
                Json::Value err;
                err["error"]["message"] =
                    "上游连续返回空响应（模型超时或中转中断），已重试 " +
                    std::to_string(kMaxEmptyRetries) + " 次。";
                // 保险丝标记：让上层识别这类失败并计数/收缩历史。
                err["__empty_upstream__"] = true;
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

        if (!using_fallback && is_malformed_tool_response(resp) &&
            !trim(tool_fallback_base_url).empty() &&
            !trim(tool_fallback_key).empty() &&
            !trim(tool_fallback_model).empty()) {
            using_fallback = true;
            tool_fallback_count++;
            conf.p->setlog(
                LOG::WARNING,
                "openai: 中转未返回 tool_calls，本轮改用兜底端点 " +
                    tool_fallback_base_url + " model=" + tool_fallback_model);
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

        if (!calls.isArray() || calls.empty()) {
            const std::string body = openai_internal::message_content_text(
                msg.get("content", Json::Value()));
            const std::string fr = openai_internal::json_to_string(
                resp["choices"][0].get("finish_reason", Json::Value()));
            if (trim(body).empty() && fr != "content_filter" &&
                fr != "length" && empty_body_retries < kMaxEmptyBodyRetries) {
                ++empty_body_retries;
                conf.p->setlog(
                    LOG::WARNING,
                    "openai: 正文为空（finish_reason=" + fr +
                        "，completion_tokens=" +
                        std::to_string(openai_internal::json_to_int64(
                            resp["usage"].isObject()
                                ? resp["usage"].get("completion_tokens",
                                                          Json::Value())
                                : Json::Value())) +
                        "），重发第 " + std::to_string(empty_body_retries) +
                        " 次");
                set_session_stage(task_id, "正文为空，重发第 " +
                                           std::to_string(empty_body_retries) +
                                           " 次");
                // 原样重发大概率还是同样的结果：模型把内容全写进了思考块。
                // 这里追加一条提示，明确要求把答案写在正文里（而不是只思考）。
                {
                    Json::Value hint;
                    hint["role"] = "user";
                    hint["content"] =
                        "（你上一条回复只有思考、没有正文。"
                        "请现在直接把要发给我的内容用正文输出，不要只输出思考。）";
                    request["messages"].append(hint);
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(600 * empty_body_retries));
                continue;
            }
            return resp;
        }

        if (rounds >= search_max_rounds) {
            conf.p->setlog(LOG::WARNING,
                           "openai: tool call round limit reached (" +
                               std::to_string(rounds) + ")，强制收尾");
            Json::Value tail_msgs = request["messages"];
            Json::Value tail_assistant;
            tail_assistant["role"] = "assistant";
            tail_assistant["content"] = msg.get("content", Json::Value());
            tail_assistant["tool_calls"] = calls;
            tail_msgs.append(tail_assistant);
            for (const auto &call : calls) {
                set_session_stage(task_id, "调用工具：" +
                    openai_internal::json_to_string(
                        call["function"]["name"]));
                Json::Value tool_msg;
                tool_msg["role"] = "tool";
                tool_msg["tool_call_id"] = openai_internal::json_to_string(
                    call.get("id", Json::Value()));
                tool_msg["content"] = exec_tool_call(call, conf, mcfg);
                tail_msgs.append(tool_msg);
            }

            Json::Value final_req = request;
            final_req["messages"] = tail_msgs;
            final_req["model"] = cur_model;
            final_req.removeMember("tools");
            final_req.removeMember("tool_choice");

            try {
                set_session_stage(task_id, "整理工具结果并生成最终回复");
                Json::Value final_resp = openai_internal::parse_api_response(
                    do_post(cur_base, "/v1/chat/completions", false, final_req,
                            {{"Authorization", "Bearer " + cur_key}},
                            true));
                const Json::Value &fcalls = final_resp["choices"];
                if (fcalls.isArray() && !fcalls.empty() &&
                    !trim(openai_internal::message_content_text(
                              fcalls[0]["message"].get("content",
                                                          Json::Value())))
                         .empty()) {
                    return final_resp;
                }
                conf.p->setlog(
                    LOG::WARNING,
                    "openai: 强制收尾仍未给出正文，回退到原响应");
            }
            catch (...) {
                conf.p->setlog(LOG::WARNING, "openai: 强制收尾请求失败");
            }
            return resp;
        }
        rounds++;

        Json::Value history_msgs = request["messages"];
        Json::Value assistant_msg;
        assistant_msg["role"] = "assistant";
        assistant_msg["content"] = msg.get("content", Json::Value());
        assistant_msg["tool_calls"] = calls;
        history_msgs.append(assistant_msg);

        for (const auto &call : calls) {
            set_session_stage(task_id, "调用工具：" +
                openai_internal::json_to_string(call["function"]["name"]));
            std::string result = exec_tool_call(call, conf, mcfg);
            Json::Value tool_msg;
            tool_msg["role"] = "tool";
            tool_msg["tool_call_id"] = openai_internal::json_to_string(
                call.get("id", Json::Value()));
            tool_msg["content"] = result;
            history_msgs.append(tool_msg);
        }

        request["messages"] = history_msgs;
    }
}
