#include "mcp_client.h"
#include "httplib.h"
#include "openai_internal.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace openai_mcp {

namespace {

std::string trim_copy(const std::string &s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string truncate_utf8(const std::string &s, size_t max_bytes)
{
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return s.substr(0, cut) + "\n...[MCP 返回内容过长，已截断，原文共 " +
           std::to_string(s.size()) + " 字节]";
}

} // namespace

bool McpManager::parse_http_url(const std::string &url,
                                std::string &scheme,
                                std::string &host,
                                int &port,
                                std::string &path)
{
    std::string u = trim_copy(url);
    size_t pos = u.find("://");
    if (pos == std::string::npos) return false;

    scheme = u.substr(0, pos);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (scheme != "http") {
        // 插件未链接 OpenSSL，当前仅支持直连 HTTP（如 http://127.0.0.1:20022/...）
        return false;
    }

    std::string rest = u.substr(pos + 3);
    size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);

    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        host = authority.substr(0, colon);
        try {
            port = std::stoi(authority.substr(colon + 1));
        }
        catch (...) {
            return false;
        }
    }
    else {
        host = authority;
        port = 80;
    }

    return !host.empty() && port > 0 && port <= 65535;
}

Json::Value McpManager::parse_mcp_body(const std::string &raw)
{
    if (raw.empty()) return Json::Value();

    {
        Json::Value root;
        Json::Reader re;
        if (re.parse(raw, root)) return root;
    }

    // Streamable HTTP 常返回 "event: message\ndata: {...}\n\n"
    Json::Value last_valid;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();
        std::string line = raw.substr(pos, eol - pos);
        pos = eol + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        const std::string prefix = "data:";
        if (line.size() < prefix.size() ||
            line.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        std::string payload = trim_copy(line.substr(prefix.size()));
        if (payload.empty() || payload == "[DONE]") continue;

        Json::Value chunk;
        Json::Reader re;
        if (re.parse(payload, chunk) && chunk.isObject()) {
            last_valid = chunk;
        }
    }
    return last_valid;
}

std::string McpManager::substitute_ns(const std::string &tmpl,
                                      const std::string &session_id_str)
{
    if (tmpl.empty()) return "";
    std::string out = tmpl;
    const std::string needle = "{session_id}";
    size_t pos = 0;
    while ((pos = out.find(needle, pos)) != std::string::npos) {
        out.replace(pos, needle.size(), session_id_str);
        pos += session_id_str.size();
    }
    return out;
}

void McpManager::configure(const std::vector<McpServerConfig> &servers)
{
    std::lock_guard<std::mutex> lock(mu_);
    servers_.clear();
    for (const auto &cfg : servers) {
        ServerState st;
        st.cfg = cfg;
        st.url_valid = parse_http_url(cfg.url, st.scheme, st.connect_host,
                                      st.connect_port, st.path);
        if (!st.url_valid) {
            st.last_error = "invalid or unsupported url (need http://...)";
        }
        servers_.push_back(std::move(st));
    }
}

void McpManager::reset_sessions()
{
    std::lock_guard<std::mutex> lock(mu_);
    for (auto &st : servers_) {
        st.initialized = false;
        st.mcp_session_id.clear();
        st.tools.clear();
        st.last_error.clear();
    }
}

bool McpManager::post_rpc_locked(ServerState &st,
                                 const Json::Value &payload,
                                 const std::string &ns_value,
                                 int &http_status,
                                 std::string &resp_session_id,
                                 Json::Value &resp_json,
                                 std::string &err)
{
    http_status = 0;
    resp_session_id.clear();
    resp_json = Json::Value();

    if (!st.url_valid) {
        err = st.last_error.empty() ? "invalid mcp url" : st.last_error;
        return false;
    }

    httplib::Client cli(st.connect_host, st.connect_port);
    cli.set_connection_timeout(5, 0);
    int rto = st.cfg.timeout_sec > 0 ? st.cfg.timeout_sec : 30;
    cli.set_read_timeout(rto, 0);
    cli.set_write_timeout(10, 0);

    httplib::Headers headers;
    headers.emplace("Accept", "application/json, text/event-stream");
    if (!st.cfg.host.empty()) {
        headers.emplace("Host", st.cfg.host);
    }
    if (!st.cfg.token.empty()) {
        headers.emplace("Authorization", "Bearer " + st.cfg.token);
    }
    if (!st.mcp_session_id.empty()) {
        headers.emplace("Mcp-Session-Id", st.mcp_session_id);
    }
    if (!ns_value.empty()) {
        headers.emplace("X-Namespace", ns_value);
    }

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    wb["emitUTF8"] = true;
    const std::string body = Json::writeString(wb, payload);

    auto res = cli.Post(st.path, headers, body, "application/json");
    if (!res) {
        err = "http error: " + httplib::to_string(res.error());
        return false;
    }

    http_status = res->status;
    resp_session_id = res->get_header_value("Mcp-Session-Id");
    if (http_status < 200 || http_status >= 300) {
        err = "HTTP " + std::to_string(http_status) + ": " +
              res->body.substr(0, 200);
        return false;
    }

    if (res->body.empty()) {
        return true;
    }

    resp_json = parse_mcp_body(res->body);
    if (resp_json.isNull()) {
        err = "failed to parse MCP response body";
        return false;
    }
    return true;
}

bool McpManager::ensure_initialized_locked(ServerState &st, std::string &err)
{
    if (st.initialized) return true;
    if (!st.cfg.enabled) {
        err = "server disabled";
        return false;
    }
    if (!st.url_valid) {
        err = st.last_error;
        return false;
    }

    st.mcp_session_id.clear();
    st.tools.clear();

    // 1) initialize
    Json::Value init_req;
    init_req["jsonrpc"] = "2.0";
    init_req["id"] = static_cast<Json::Int64>(st.next_rpc_id++);
    init_req["method"] = "initialize";
    init_req["params"]["protocolVersion"] = "2025-03-26";
    init_req["params"]["capabilities"] = Json::Value(Json::objectValue);
    init_req["params"]["clientInfo"]["name"] = "shinxbot2-openai";
    init_req["params"]["clientInfo"]["version"] = "1.0";

    int status = 0;
    std::string sid;
    Json::Value resp;
    if (!post_rpc_locked(st, init_req, "", status, sid, resp, err)) {
        st.last_error = err;
        return false;
    }
    if (resp.isMember("error")) {
        err = openai_internal::json_to_string(
            resp["error"].isMember("message") ? resp["error"]["message"]
                                             : Json::Value());
        st.last_error = err;
        return false;
    }
    if (!sid.empty()) {
        st.mcp_session_id = sid;
    }

    // 2) notifications/initialized
    {
        Json::Value notif;
        notif["jsonrpc"] = "2.0";
        notif["method"] = "notifications/initialized";
        int n_status = 0;
        std::string n_sid;
        Json::Value n_resp;
        std::string n_err;
        if (!post_rpc_locked(st, notif, "", n_status, n_sid, n_resp, n_err)) {
            err = "notifications/initialized failed: " + n_err;
            st.last_error = err;
            return false;
        }
    }

    // 3) tools/list
    Json::Value list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = static_cast<Json::Int64>(st.next_rpc_id++);
    list_req["method"] = "tools/list";
    list_req["params"] = Json::Value(Json::objectValue);

    status = 0;
    sid.clear();
    resp = Json::Value();
    if (!post_rpc_locked(st, list_req, "", status, sid, resp, err)) {
        st.last_error = err;
        return false;
    }
    if (resp.isMember("error")) {
        err = openai_internal::json_to_string(
            resp["error"].isMember("message") ? resp["error"]["message"]
                                             : Json::Value());
        st.last_error = err;
        return false;
    }

    const Json::Value &arr = resp["result"]["tools"];
    if (arr.isArray()) {
        for (const auto &t : arr) {
            if (!t.isObject()) continue;
            std::string rname = t.get("name", "").asString();
            if (rname.empty()) continue;
            McpToolInfo info;
            info.server_name = st.cfg.name;
            info.remote_name = rname;
            info.exposed_name = st.cfg.tool_prefix + rname;
            info.description = t.get("description", "").asString();
            info.input_schema = t.isMember("inputSchema") && t["inputSchema"].isObject()
                                    ? t["inputSchema"]
                                    : Json::Value(Json::objectValue);
            if (!info.input_schema.isMember("type")) {
                info.input_schema["type"] = "object";
            }
            if (!info.input_schema.isMember("properties")) {
                info.input_schema["properties"] = Json::Value(Json::objectValue);
            }
            st.tools.push_back(std::move(info));
        }
    }

    st.initialized = true;
    st.last_error.clear();
    return true;
}

Json::Value McpManager::build_openai_tool_defs()
{
    std::lock_guard<std::mutex> lock(mu_);
    Json::Value out(Json::arrayValue);

    for (auto &st : servers_) {
        if (!st.cfg.enabled) continue;
        std::string err;
        if (!ensure_initialized_locked(st, err)) {
            continue;
        }
        for (const auto &t : st.tools) {
            Json::Value tool;
            tool["type"] = "function";
            Json::Value &f = tool["function"];
            f["name"] = t.exposed_name;
            f["description"] =
                t.description.empty()
                    ? ("MCP tool " + t.remote_name + " from " + t.server_name)
                    : t.description;
            f["parameters"] = t.input_schema;
            out.append(tool);
        }
    }
    return out;
}

bool McpManager::try_call_tool(const std::string &exposed_name,
                               const Json::Value &arguments,
                               const std::string &session_id_str,
                               std::string &out_text,
                               std::string &out_log)
{
    std::lock_guard<std::mutex> lock(mu_);
    ServerState *target_st = nullptr;
    McpToolInfo target_tool;

    for (auto &st : servers_) {
        if (!st.cfg.enabled) continue;
        std::string err;
        if (!ensure_initialized_locked(st, err)) continue;
        for (const auto &t : st.tools) {
            if (t.exposed_name == exposed_name) {
                target_st = &st;
                target_tool = t;
                break;
            }
        }
        if (target_st) break;
    }

    if (!target_st) return false;

    const std::string ns_val =
        substitute_ns(target_st->cfg.ns, session_id_str);

    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string err;
        if (!ensure_initialized_locked(*target_st, err)) {
            out_log = "mcp init failed (" + target_st->cfg.name + "): " + err;
            out_text = "MCP 服务未就绪：" + err;
            return true;
        }

        Json::Value call_req;
        call_req["jsonrpc"] = "2.0";
        call_req["id"] = static_cast<Json::Int64>(target_st->next_rpc_id++);
        call_req["method"] = "tools/call";
        call_req["params"]["name"] = target_tool.remote_name;
        call_req["params"]["arguments"] =
            arguments.isObject() ? arguments : Json::Value(Json::objectValue);

        int status = 0;
        std::string sid;
        Json::Value resp;
        if (!post_rpc_locked(*target_st, call_req, ns_val, status, sid, resp,
                             err)) {
            if (attempt == 0 && (status == 400 || status == 404)) {
                target_st->initialized = false;
                target_st->mcp_session_id.clear();
                continue;
            }
            out_log = "mcp call failed (" + target_st->cfg.name + "/" +
                      target_tool.remote_name + "): " + err;
            out_text = "MCP 工具调用失败：" + err;
            return true;
        }

        if (resp.isMember("error")) {
            std::string emsg = openai_internal::json_to_string(
                resp["error"].isMember("message")
                    ? resp["error"]["message"]
                    : Json::Value());
            if (emsg.empty()) emsg = "unknown mcp error";
            out_log = "mcp rpc error (" + target_tool.remote_name + "): " + emsg;
            out_text = "MCP 工具返回错误：" + emsg;
            return true;
        }

        const Json::Value &result = resp["result"];
        std::string text;
        if (result.isMember("content") && result["content"].isArray()) {
            for (const auto &item : result["content"]) {
                if (!item.isObject()) continue;
                if (openai_internal::json_to_string(item["type"]) == "text") {
                    if (!text.empty()) text += "\n";
                    text += openai_internal::json_to_string(item["text"]);
                }
            }
        }
        if (text.empty() && result.isMember("structuredContent")) {
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "  ";
            wb["emitUTF8"] = true;
            text = Json::writeString(wb, result["structuredContent"]);
        }
        if (text.empty()) {
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "  ";
            wb["emitUTF8"] = true;
            text = Json::writeString(wb, result);
        }

        if (result.get("isError", false).asBool()) {
            out_text = "MCP 工具执行报错：\n" + truncate_utf8(text, 24000);
        }
        else {
            out_text = truncate_utf8(text, 24000);
        }
        return true;
    }

    return true;
}

std::string McpManager::status_summary()
{
    std::lock_guard<std::mutex> lock(mu_);
    if (servers_.empty()) return "未配置 MCP 服务器";
    std::string out;
    for (auto &st : servers_) {
        if (!out.empty()) out += "\n";
        out += "- " + st.cfg.name + ": ";
        if (!st.cfg.enabled) {
            out += "disabled";
            continue;
        }
        std::string err;
        if (ensure_initialized_locked(st, err)) {
            out += "connected (" + std::to_string(st.tools.size()) + " tools)";
        }
        else {
            out += "error (" + err + ")";
        }
    }
    return out;
}

} // namespace openai_mcp
