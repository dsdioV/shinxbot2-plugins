#include "gpt3_5.h"
#include "cqscan.h"
#include "openai_internal.h"

#include <algorithm>
#include <set>
#include <string>

std::string gpt3_5::get_quoted_content(const bot *p, int64_t reply_id, int depth)
{
    if (depth > 5) return "...(too deep)";

    Json::Value get_msg_param;
    get_msg_param["message_id"] = reply_id;
    Json::Value msg_info =
        string_to_json(p->cq_send("get_msg", get_msg_param));

    if (openai_internal::json_to_int64(msg_info["retcode"]) != 0 ||
        !msg_info.isMember("data"))
        return "[Failed to fetch message]";

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
        size_t cont_start = fwd_cont_pos + 20;
        size_t cont_end = content.find_last_of(']');
        if (cont_end != std::string::npos && cont_end > cont_start) {
            const size_t raw_len = cont_end - cont_start;
            static const size_t kMaxInlineForwardBytes = 256 * 1024;
            if (raw_len > kMaxInlineForwardBytes) {
                return "[转发记录过大，已省略]";
            }
            std::string raw_json = content.substr(cont_start, raw_len);
            raw_json = cqscan::replace_all(raw_json, "&#91;", "[");
            raw_json = cqscan::replace_all(raw_json, "&#93;", "]");
            raw_json = cqscan::replace_all(raw_json, "&#44;", ",");

            try {
                Json::Value fwd_data = string_to_json(raw_json);
                if (fwd_data.isArray()) {
                    std::string result = (depth == 0) ? "合并转发记录:" : "";
                    for (const auto &m : fwd_data) {
                        std::string msg_text = messageArr_to_string(
                            m.isMember("message") ? m["message"] : m["content"]);
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

                        std::string nick = openai_internal::json_to_string(
                            m["sender"]["nickname"]);
                        uint64_t uid = static_cast<uint64_t>(
        openai_internal::json_to_int64(m["sender"]["user_id"]));
                        if (uid != 0)
                            result += "[" + nick + "(" + std::to_string(uid) +
                                      ")]：" + cleaned;
                        else
                            result += "[" + nick + "]：" + cleaned;
                    }
                    return trim(result);
                }
            }
            catch (...) {
            }
        }
    }

    std::string nickname = "Unknown";
    if (msg_data.isMember("sender") &&
        msg_data["sender"].isMember("nickname") &&
        msg_data["sender"]["nickname"].isString()) {
        nickname = msg_data["sender"]["nickname"].asString();
    }

    uint64_t uid = 0;
    if (msg_data.isMember("sender") && msg_data["sender"].isMember("user_id")) {
        uid = static_cast<uint64_t>(
            openai_internal::json_to_int64(msg_data["sender"]["user_id"]));
    }

    content = simplify_cq_codes(content, p->get_botqq());
    if (uid != 0) {
        return "[" + nickname + "(" + std::to_string(uid) + ")]：" + content;
    }
    return "[" + nickname + "]：" + content;
}

std::string gpt3_5::expand_forward_content(const bot *p,
                                           const std::string &forward_id,
                                           int depth)
{
    forward_budget budget;
    return expand_forward_content(p, forward_id, depth, budget);
}

std::string gpt3_5::expand_forward_content(const bot *p,
                                           const std::string &forward_id,
                                           int depth, forward_budget &budget)
{
    static const int kMaxDepth = 5;
    static const int kMaxNodes = 200;
    static const size_t kMaxBytes = 8192;

    if (depth > kMaxDepth) return "[转发嵌套过深，已省略]";
    if (budget.nodes >= kMaxNodes) return "[转发内容过多，已省略]";
    if (budget.bytes >= kMaxBytes) return "[转发内容过长，已省略]";
    if (!budget.visited.insert(forward_id).second) return "";

    Json::Value get_fwd_param;
    get_fwd_param["id"] = forward_id;
    Json::Value fwd_info = string_to_json(
        p->cq_send("get_forward_msg", get_fwd_param));

    if (openai_internal::json_to_int64(fwd_info["retcode"]) != 0 ||
        !fwd_info.isMember("data"))
        return "[Failed to fetch forward message]";

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

            size_t fwd_pos = content.find("[CQ:forward,id=");
            if (fwd_pos != std::string::npos) {
                size_t id_start = fwd_pos + 15;
                size_t id_end = content.find(']', id_start);
                if (id_end != std::string::npos) {
                    std::string nested_id =
                        content.substr(id_start, id_end - id_start);
                    result += "\n" + expand_forward_content(
                                          p, nested_id, depth + 1, budget);
                    continue;
                }
            }

            ++budget.nodes;
            std::replace(content.begin(), content.end(), '\n', ' ');
            std::replace(content.begin(), content.end(), '\r', ' ');

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

            std::string nick = openai_internal::json_to_string(
                m["sender"]["nickname"]);
            if (nick.empty()) nick = "Unknown";
            uint64_t uid = static_cast<uint64_t>(
        openai_internal::json_to_int64(m["sender"]["user_id"]));

            if (uid != 0) {
                result += "[" + nick + "(" + std::to_string(uid) + ")]：" +
                          content;
            }
            else {
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
