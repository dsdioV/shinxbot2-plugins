#pragma once

// CQ 码线性扫描工具。
//
// 为什么需要它：插件里原本用 std::regex 解析 CQ 码，典型模式是
//     R"(\[CQ:([^,\]]+)[^\]]*\])"
// 这个模式在「一个很长的、不含 ',' 也不含 ']' 的连续段」上会让
// libstdc++ 的回溯型执行器无限深递归（_M_dfs → _M_handle_match →
// _M_rep_once_more 循环），把线程栈耗尽 → SIGSEGV。
//
// 实测（8MB 默认栈）：连续段达 28KB 即崩溃；正常 CQ 码里 ',' 和 ']'
// 密集，每段都很短，所以平时从不出事。合并转发的内联展开会把整棵
// 转发树塞进一个 CQ 码参数里，正好制造出几十 KB 的超长连续段。
//
// 本文件用纯线性扫描替代正则：不回溯、不递归、不额外分配，
// 复杂度严格 O(n)，对任意长度输入都是常数栈深度。

#include <cstddef>
#include <string>
#include <vector>

namespace cqscan {

struct Segment {
    // text 段：literal 为原文；CQ 段：type 为 CQ 类型，args 为原始参数串
    bool is_cq = false;
    std::string text;     // is_cq == false 时有效
    std::string type;     // is_cq == true 时有效，如 "image" / "forward"
    std::string args;     // is_cq == true 时有效，"k=v,k=v" 形式（不含类型）
    size_t begin = 0;     // 在源串中的起始偏移（'{' 位置）
    size_t end = 0;       // 在源串中的结束偏移（'}' 后一位）
};

/// 把消息串切成 text / CQ 段。等价于原来那套 sregex_iterator 的遍历。
///
/// 语义与 `\[CQ:([^,\]]+)[^\]]*\]` 对齐：以 "[CQ:" 起头，
/// 类型读到第一个 ',' 或 ']'，整段读到第一个 ']'。
/// 没有闭合 ']' 的 "[CQ:" 视为普通文本，不吞掉后续内容。
inline std::vector<Segment> split(const std::string &s)
{
    std::vector<Segment> out;
    const size_t n = s.size();
    size_t pos = 0;

    auto push_text = [&](size_t b, size_t e) {
        if (e <= b) return;
        Segment seg;
        seg.is_cq = false;
        seg.text.assign(s, b, e - b);
        seg.begin = b;
        seg.end = e;
        out.push_back(std::move(seg));
    };

    while (pos < n) {
        const size_t p = s.find("[CQ:", pos);
        if (p == std::string::npos) {
            push_text(pos, n);
            break;
        }
        push_text(pos, p);

        // 类型：从 "[CQ:" 之后读到第一个 ',' 或 ']'
        size_t type_end = p + 4;
        while (type_end < n && s[type_end] != ',' && s[type_end] != ']') {
            ++type_end;
        }
        if (type_end >= n) {
            // 到串尾都没有 ',' 或 ']'，不是合法 CQ 码，当文本处理
            push_text(p, n);
            break;
        }

        const size_t close = s.find(']', type_end);
        if (close == std::string::npos) {
            // 没有闭合，剩余部分全是文本（不吞掉，避免丢内容）
            push_text(p, n);
            break;
        }

        Segment seg;
        seg.is_cq = true;
        seg.type.assign(s, p + 4, type_end - (p + 4));
        seg.args.assign(s, type_end, close - type_end);
        seg.begin = p;
        seg.end = close + 1;
        out.push_back(std::move(seg));

        pos = close + 1;
    }
    return out;
}

/// 在 CQ 参数串里按 ',' 切出 "k=v" 项（不解码，也不复制到别的容器）。
inline std::vector<std::pair<std::string, std::string>>
split_args(const std::string &args)
{
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos < args.size()) {
        size_t comma = args.find(',', pos);
        if (comma == std::string::npos) comma = args.size();
        if (comma > pos) {
            std::string item = args.substr(pos, comma - pos);
            const size_t eq = item.find('=');
            if (eq != std::string::npos) {
                out.emplace_back(item.substr(0, eq), item.substr(eq + 1));
            }
            else {
                out.emplace_back(std::string(), item);
            }
        }
        pos = comma + 1;
    }
    return out;
}

/// 取某个参数的值；不存在返回空串。
inline std::string arg_value(const std::string &args, const std::string &key)
{
    size_t pos = 0;
    while (pos < args.size()) {
        size_t comma = args.find(',', pos);
        if (comma == std::string::npos) comma = args.size();
        const std::string item = args.substr(pos, comma - pos);
        if (item.size() > key.size() && item.compare(0, key.size(), key) == 0 &&
            item[key.size()] == '=') {
            return item.substr(key.size() + 1);
        }
        pos = comma + 1;
    }
    return std::string();
}

/// 线性替换（替代 std::regex_replace 做的实体解码）。
inline std::string replace_all(std::string s, const std::string &from,
                               const std::string &to)
{
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

} // namespace cqscan
