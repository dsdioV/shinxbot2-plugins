#include "processable.h"

#include <jsoncpp/json/json.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <mutex>

class gpt3_5 : public processable {
private:
    std::recursive_mutex data_lock;
    std::vector<bool> is_lock;
    bool is_open, is_debug;
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
    std::vector<std::string> key;
    std::vector<std::string> wake_keywords;
    size_t key_cycle;
    std::string base_url;
    std::string model_name;
    int compress_recent_rounds = 10;
    bool reply_on_at = true;
    bool reply_on_keyword = false;
    bool active_context_show_time = true;
    bool active_context_show_user_id = true;
    std::string get_quoted_content(const bot *p, int64_t reply_id, int depth = 0);

    // 合并转发展开的预算与去重。
    //
    // QQ 端限定最多 4 层嵌套，所以「无限递归」并不成立；真正的杀手是
    // **每层节点数上百 + 逐节点同步 HTTP**：调用次数按层数指数增长
    // （实测 2 → 200 → 2万 → 200万），每次又是 get_forward_msg 的同步
    // 往返 + 几十 KB JSON 解析 + 大字符串拼接，几秒内就能把进程打死。
    // 因此只限制层数拦不住，必须同时限制：层数、重复 id、节点总数、字节数。
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

    // ── 多模态：图片落盘 + 历史存引用 ──────────────────────────────
    // 历史里绝不出现 base64（save_history 每轮全量重写，会被撑爆）。
    // 图片字节存 config/gpt3_5/media/<sha256>.jpg（内容寻址，天然去重），
    // 历史里只留内部标记
    // {"type":"image_ref","file":"<sha256>.jpg","mime":"image/jpeg"}；
    // 真正请求 API 时才展开成 data URI。
    bool media_enabled = true;
    int media_max_edge = 2048;

    // ── 工具调用（web search）──────────────────────────────────────
    // 走 OpenAI 原生 tools / tool_calls，不是"猜关键词再搜"的固定流程。
    // 这也是以后接 MCP 的同一套地基：加工具 = 往 tools 里多塞一个定义。
    //
    // 为什么需要 fallback：newapi 中转在把上游（gemini 等）响应转成
    // OpenAI 格式时会丢掉 tool_calls 数组，只留下 finish_reason="tool_calls"。
    // 实测 12 种请求变体（含 tool_choice=none）全都如此，是服务端缺陷。
    // 对策：一旦检测到这种畸形响应，就把本轮整体改用 tool_fallback_*
    // 指向的端点重跑（默认 DeepSeek 官方，它的 tool_calls 是标准的）。
    std::string search_api_key;
    std::string search_base_url = "https://api.deepseek.com";
    bool search_enabled = false;
    int search_count = 5;
    int search_max_rounds = 3; // 防止模型无限调工具
    std::string tool_fallback_base_url;
    std::string tool_fallback_model;
    std::string tool_fallback_key;
    int tool_fallback_count = 0; // 统计触发了几次，便于观察中转是否修好
    /// 一轮对话：发请求 → 模型要工具 → 执行 → 回灌 → 再请求。
    /// 返回最终的 choices[0]（或错误对象）。
    Json::Value run_chat_with_tools(Json::Value request, size_t keyid,
                                    const msg_meta &conf);
    /// 检测"声称调工具却没给 tool_calls"的畸形响应。
    static bool is_malformed_tool_response(const Json::Value &resp);
    /// 工具定义列表（OpenAI tools 格式）。
    Json::Value build_tool_defs();
    /// 执行一次工具调用，返回喂回模型的文本。
    std::string exec_tool_call(const Json::Value &call, const msg_meta &conf);
    /// DeepSeek Search API。
    std::string tool_web_search(const std::string &query, const msg_meta &conf);
    std::string media_dir() const;
    /// 下载一张图、压到 media_max_edge 内、落盘，返回内部 image_ref part。
    /// 失败返回 null（调用方跳过）。
    Json::Value store_image(const std::string &url, const msg_meta &conf);
    /// 把一段含 [CQ:image,...] 的文本拆成 parts，并补上被引用消息里的图。
    /// 纯文本时返回 null（调用方按旧格式存字符串，保证向后兼容）。
    Json::Value build_user_content(const std::string &message,
                                   const msg_meta &conf, int64_t reply_id);
    /// 收集合并转发里的图片 URL（递归展开，带深度上限）。
    /// 转到的是展开后的文本里那些 [图片] 占位符对应的真实图片。
    void collect_forward_images(bot *p, const std::string &forward_id,
                                std::vector<std::string> &urls, int depth);
    /// 从一条消息的 message 数组里抽出图片 URL。
    void collect_images_from_message(const Json::Value &msg,
                                     std::vector<std::string> &urls);
    /// 内部 part（含 image_ref）→ OpenAI content 数组（图片展开成 data URI）。
    Json::Value expand_content_for_api(const Json::Value &content);
    /// 取纯文本视图：图片降级成 "[图片]"（压缩/摘要用）。
    std::string content_to_text(const Json::Value &content);
    bool try_acquire_session(int64_t id, size_t keyid, const msg_meta &conf,
                             bool ensure_default_prompt = true);
    void release_session(size_t keyid);
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
    /// bot.help 里只保留一行简述，避免刷屏。
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