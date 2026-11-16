#include "gpt3_5.h"
#include "openai_internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

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
                conf.p->cq_send("当前归档文件过大，已暂停生成。请联系管理员",
                                conf);
            }
            return;
        }
    }

    std::string backup_dir =
        bot_config_path(nullptr, "gpt3_5/backups/" + std::to_string(id));
    if (!fs::exists(backup_dir)) {
        fs::create_directories(backup_dir);
    }

    std::time_t now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    now += 8 * 3600;
    tm tm_res = *std::gmtime(&now);
    char time_buf[64];
    if (is_auto) {
        std::strftime(time_buf, sizeof(time_buf),
                      "%Y-%m-%d_%H-%M-%S_beforeCompress", &tm_res);
    }
    else {
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d_%H-%M-%S", &tm_res);
    }

    std::string filename = std::string(time_buf) + ".json";
    std::string full_path = backup_dir + "/" + filename;

    if (is_auto && fs::exists(full_path)) {
        int suffix = 1;
        do {
            filename = std::string(time_buf) + "_" + std::to_string(suffix) +
                       ".json";
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
            if (entry.is_regular_file() &&
                entry.path().extension() == ".json") {
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
