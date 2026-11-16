#pragma once

#include "mcp_client.h"
#include "processable.h"

#include <jsoncpp/json/json.h>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

class gpt3_5 : public processable {
private:
    struct RunningTask {
        int64_t id = 0;
        bool is_group = false;
        userid_t user_id = 0;
        std::string nickname;
        std::string stage;
        std::chrono::steady_clock::time_point started_at;
    };

    struct ModeOverride {
        bool has_base_url = false;
        std::string base_url;
        bool has_model = false;
        std::string model;
        bool has_temperature = false;
        double temperature = 0.7;
        bool has_search_enabled = false;
        bool search_enabled = false;
        bool has_web_fetch_enabled = false;
        bool web_fetch_enabled = true;
        bool has_mcp_enabled = false;
        bool mcp_enabled = true;
        bool has_bash_enabled = false;
        bool bash_enabled = false;
    };

    struct ResolvedModeConfig {
        std::string base_url;
        std::string model;
        double temperature = 0.7;
        bool search_enabled = false;
        bool web_fetch_enabled = true;
        bool mcp_enabled = true;
        bool bash_enabled = false;
    };

    std::recursive_mutex data_lock;
    std::map<int64_t, RunningTask> running_tasks;
    std::map<std::pair<int64_t, userid_t>, std::chrono::steady_clock::time_point>
        active_followup_until;
    bool is_open = true;
    bool is_debug = false;
    std::string close_message; // The reason for is_open=false
    std::string default_prompt;
    std::string compress_summary_prompt;
    std::map<int64_t, std::string> pre_default;
    std::map<int64_t, Json::Value> pre_prompt;
    std::map<int64_t, Json::Value> history;
    std::map<userid_t, std::string> nickname_cache;
    std::map<int64_t, int64_t> last_prompt_tokens;
    std::map<int64_t, int64_t> last_completion_tokens;
    std::map<int64_t, int64_t> last_total_tokens;
    std::vector<std::string> modes;
    std::set<std::string> black_list;
    std::map<std::string, Json::Value> mode_prompt;
    std::map<std::string, ModeOverride> mode_overrides;
    std::vector<std::string> key;
    std::vector<std::string> wake_keywords;
    size_t key_cycle = 0;
    std::string base_url;
    std::string model_name;
    double default_temperature = 0.7;
    int MAX_TOKEN = 4000;
    int MAX_REPLY = 1000;
    int RED_LINE = 1000;
    int compress_recent_rounds = 10;
    bool reply_on_at = true;
    bool reply_on_keyword = false;
    int group_followup_sec = 0;
    bool active_context_show_time = true;
    bool active_context_show_user_id = true;

    std::string get_quoted_content(const bot *p, int64_t reply_id, int depth = 0);

    // 合并转发展开的预算与去重。
    struct forward_budget {
        std::set<std::string> visited; // 已展开的转发 id，兼作循环检测
        int nodes = 0;                 // 已计入的节点数
        size_t bytes = 0;              // 已展开文本的累计字节数
    };
    std::string expand_forward_content(const bot *p, const std::string &forward_id,
                                       int depth);
    std::string expand_forward_content(const bot *p, const std::string &forward_id,
                                       int depth, forward_budget &budget);
    std::string get_cached_nickname(const msg_meta &conf);
    /// 诊断：把请求体按来源拆开算体积，用于定位 413 的成因
    std::string describe_request_size(const Json::Value &request,
                                      int tool_round, int retry_no);
    std::string get_short_time();
    std::string simplify_cq_codes(const std::string &message, userid_t botqq = 0);
    std::string strip_bot_at(const std::string &message, const msg_meta &conf);
    std::string format_context_message(const std::string &message,
                                       const msg_meta &conf,
                                       const std::string &nickname,
                                       int64_t reply_id = -1,
                                       bool include_quote = false);
    bool message_mentions_bot(const std::string &message, const msg_meta &conf);
    bool message_has_wake_keyword(const std::string &message);
    bool is_followup_active(int64_t group_id, userid_t user_id);
    void refresh_followup(int64_t group_id, userid_t user_id);

    // ── 多模态：图片落盘 + 历史存引用 ──────────────────────────────
    bool media_enabled = true;
    int media_max_edge = 1568;
    int media_quality = 82;
    double media_budget_mb = 8.0;

    // ── 工具调用（web search / web fetch / MCP）───────────────────
    std::string search_api_key;
    std::string search_base_url = "https://api.deepseek.com";
    bool search_enabled = false;
    int search_count = 5;
    int search_max_rounds = 3; // 防止模型无限调工具
    bool web_fetch_enabled = true;
    int web_fetch_max_chars = 16000;
    bool bash_enabled = false;
    bool bash_op_only = true;
    int bash_timeout_sec = 30;
    int bash_max_output_chars = 12000;
    std::string bash_workdir;
    std::string tool_fallback_base_url;
    std::string tool_fallback_model;
    std::string tool_fallback_key;
    int tool_fallback_count = 0;

    std::vector<openai_mcp::McpServerConfig> mcp_servers;
    openai_mcp::McpManager mcp_manager;

    ResolvedModeConfig resolve_mode_config(const std::string &mode);

    /// 一轮对话：发请求 → 模型要工具 → 执行 → 回灌 → 再请求。
    Json::Value run_chat_with_tools(Json::Value request, size_t keyid,
                                    const msg_meta &conf,
                                    const ResolvedModeConfig &mcfg);
    /// 检测"声称调工具却没给 tool_calls"的畸形响应。
    static bool is_malformed_tool_response(const Json::Value &resp);
    /// 判断当前请求者是否有权使用 bash 工具。
    bool can_use_bash(const msg_meta &conf, const ResolvedModeConfig &mcfg) const;
    /// 工具定义列表（OpenAI tools 格式）。
    Json::Value build_tool_defs(const msg_meta &conf,
                                const ResolvedModeConfig &mcfg);
    /// 执行一次工具调用，返回喂回模型的文本。
    std::string exec_tool_call(const Json::Value &call, const msg_meta &conf,
                               const ResolvedModeConfig &mcfg);
    /// DeepSeek Search API。
    std::string tool_web_search(const std::string &query, const msg_meta &conf);
    /// 抓取指定 HTTP(S) URL 正文（支持 HTML 清洗与长度截断）。
    std::string tool_web_fetch(const std::string &url, int max_chars,
                               const msg_meta &conf);
    /// 执行受权限与超时保护的 bash 命令。
    std::string tool_bash(const std::string &command, const std::string &workdir,
                          int timeout_sec, const msg_meta &conf);

    std::string media_dir() const;
    /// 下载一张图、压到 media_max_edge 内、落盘，返回内部 image_ref part。
    Json::Value store_image(const std::string &url, const msg_meta &conf);
    /// 把一段含 [CQ:image,...] 的文本拆成 parts，并补上被引用消息里的图。
    Json::Value build_user_content(const std::string &message,
                                   const msg_meta &conf, int64_t reply_id);
    /// 收集合并转发里的图片 URL（递归展开，带深度上限）。
    void collect_forward_images(bot *p, const std::string &forward_id,
                                std::vector<std::string> &urls, int depth);
    /// 从一条消息的 message 数组里抽出图片 URL。
    void collect_images_from_message(const Json::Value &msg,
                                     std::vector<std::string> &urls);
    /// 内部 part（含 image_ref）→ OpenAI content 数组（图片展开成 data URI）。
    Json::Value expand_content_for_api(const Json::Value &content,
                                       size_t *budget = nullptr);
    /// 把一条消息里的 image_ref 全部降级成 [图片] 文本（用于体积裁剪）。
    Json::Value degrade_images(const Json::Value &content);
    /// 取纯文本视图：图片降级成 "[图片]"（压缩/摘要用）。
    std::string content_to_text(const Json::Value &content);

    static int64_t session_id_from_conf(const msg_meta &conf);
    bool try_acquire_session(int64_t id, const msg_meta &conf,
                             const std::string &nickname,
                             const std::string &stage = "准备上下文",
                             bool ensure_default_prompt = true);
    void release_session(int64_t id);
    void set_session_stage(int64_t id, const std::string &stage);
    std::string format_running_tasks();

    bool compress_history(int64_t id, size_t keyid, const msg_meta &conf,
                          std::string *error_message = nullptr);
    void fallback_trim_history(int64_t id, int rounds = 1);

    // Archive and Restore features
    void perform_archive(int64_t id, const msg_meta &conf, bool is_auto = false,
                         bool silent = false);
    uintmax_t get_archives_total_size();
    void list_archives(int64_t id, const msg_meta &conf, int page);
    void restore_archive(int64_t id, const msg_meta &conf, const std::string &arg);
    bool is_allowed_arc(int64_t id, const msg_meta &conf);

    int arc_check_counter = 0;
    bool arc_is_full = false;

    /// 完整用法说明（.ai help / .ai.help 触发）。
    void cmd_full_help(const msg_meta &conf);

public:
    gpt3_5();
    size_t get_avaliable_key();
    void save_file();
    void save_history(int64_t id);
    std::string do_black(std::string u);
    void process(std::string message, const msg_meta &conf) override;
    bool check(std::string message, const msg_meta &conf) override;
    std::string help() override;
    std::string help(const msg_meta &conf, help_level_t level) override;
};

DECLARE_FACTORY_FUNCTIONS_HEADER
