#pragma once

#include <jsoncpp/json/json.h>
#include <mutex>
#include <string>
#include <vector>

namespace openai_mcp {

struct McpServerConfig {
    std::string name;
    bool enabled = true;
    std::string url;
    std::string host;       // 可选：直连 127.0.0.1 时指定 Host 头（如 mcp.voidsd.top）
    std::string token;      // Bearer token
    std::string ns;         // X-Namespace（支持 {session_id} 占位符）
    std::string tool_prefix; // 可选：给注册到 OpenAI 的工具名加前缀
    int timeout_sec = 30;
};

struct McpToolInfo {
    std::string server_name;
    std::string remote_name;
    std::string exposed_name;
    std::string description;
    Json::Value input_schema;
};

class McpManager {
public:
    void configure(const std::vector<McpServerConfig> &servers);
    void reset_sessions();

    /// 确保已连接并拉取过 tools/list，返回 OpenAI tools 格式定义。
    Json::Value build_openai_tool_defs();

    /// 判断某个工具名是否归属 MCP；若是，则执行 tools/call 并返回文本结果。
    bool try_call_tool(const std::string &exposed_name,
                       const Json::Value &arguments,
                       const std::string &session_id_str,
                       std::string &out_text,
                       std::string &out_log);

    /// 返回各 MCP 服务器的连接与工具数摘要（供 .ai.status / .ai.mcp 使用）。
    std::string status_summary();

private:
    struct ServerState {
        McpServerConfig cfg;
        std::string scheme;
        std::string connect_host;
        int connect_port = 80;
        std::string path;
        bool url_valid = false;

        bool initialized = false;
        std::string mcp_session_id;
        std::vector<McpToolInfo> tools;
        std::string last_error;
        long long next_rpc_id = 1;
    };

    std::mutex mu_;
    std::vector<ServerState> servers_;

    static bool parse_http_url(const std::string &url,
                               std::string &scheme,
                               std::string &host,
                               int &port,
                               std::string &path);
    static Json::Value parse_mcp_body(const std::string &raw);
    static std::string substitute_ns(const std::string &tmpl,
                                     const std::string &session_id_str);

    bool ensure_initialized_locked(ServerState &st, std::string &err);
    bool post_rpc_locked(ServerState &st,
                         const Json::Value &payload,
                         const std::string &ns_value,
                         int &http_status,
                         std::string &resp_session_id,
                         Json::Value &resp_json,
                         std::string &err);
};

} // namespace openai_mcp
