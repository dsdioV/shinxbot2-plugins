#include "wordle.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

/* ═══════════════════════════════════════════════════════════
   Lifecycle
   ═══════════════════════════════════════════════════════════ */

wordle::wordle() { load_banks(); }

bool wordle::reload(const msg_meta &conf)
{
    (void)conf;
    banks_.clear();
    valid_words_.clear();
    difficulty_names_.clear();
    group_games_.clear();
    private_games_.clear();
    load_banks();
    return true;
}

/* ═══════════════════════════════════════════════════════════
   processable interface
   ═══════════════════════════════════════════════════════════ */

bool wordle::check(std::string message, const msg_meta &conf)
{
    (void)conf;
    return cmd_match_prefix(trim(message), {"*wordle", "*wl"});
}

std::string wordle::help()
{
    return "Wordle 单词猜谜游戏。帮助：*wordle help";
}

void wordle::process(std::string message, const msg_meta &conf)
{
    std::string cmd;
    if (!cmd_parse_prefixed(trim(message), {"*wordle", "*wl"}, cmd)) {
        return;
    }

    std::string lower_cmd = cmd;
    for (char &c : lower_cmd)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    /* ── Exact subcommands ──────────────────────────── */
    if (lower_cmd == "start") {
        cmd_start(conf);
        return;
    }
    if (lower_cmd == "status") {
        cmd_status(conf);
        return;
    }
    if (lower_cmd == "hint") {
        cmd_hint(conf);
        return;
    }
    if (lower_cmd == "abort") {
        cmd_abort(conf);
        return;
    }
    if (lower_cmd == "help" || lower_cmd == ".help") {
        cmd_help(conf);
        return;
    }

    /* ── guess <word> ───────────────────────────────── */
    if (starts_with(lower_cmd, "guess ")) {
        cmd_guess(conf, trim(cmd.substr(6)));
        return;
    }

    /* ── set <param> [value] ────────────────────────── */
    if (starts_with(lower_cmd, "set ")) {
        std::string rest = trim(cmd.substr(4));
        size_t sp = rest.find(' ');
        std::string param = (sp == std::string::npos)
                                ? rest
                                : rest.substr(0, sp);
        std::string value = (sp == std::string::npos)
                                ? ""
                                : trim(rest.substr(sp + 1));
        cmd_set(conf, param, value);
        return;
    }

    /* ── Fallthrough ────────────────────────────────── */
    conf.p->cq_send("未知指令，发送 *wordle help 查看帮助。", conf);
}

/* ═══════════════════════════════════════════════════════════
   Bank loading
   ═══════════════════════════════════════════════════════════ */

std::string wordle::clean_word(const std::string &raw)
{
    std::string out;
    for (char c : raw) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            out += static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

std::vector<word_entry> wordle::parse_csv(const std::string &content)
{
    std::vector<word_entry> result;
    if (content.empty()) return result;

    std::istringstream iss(content);
    std::string line;
    std::string accumulated;
    bool in_quotes = false;

    auto process_record = [&](const std::string &record) {
        size_t comma = record.find(',');
        if (comma == std::string::npos) return;

        std::string raw_word = trim(record.substr(0, comma));
        std::string w = clean_word(raw_word);
        if (w.empty()) return;

        std::string def = trim(record.substr(comma + 1));
        // Strip surrounding quotes if present
        if (def.size() >= 2 && def.front() == '"' && def.back() == '"') {
            def = def.substr(1, def.size() - 2);
        }
        // Flatten newlines → spaces, collapse whitespace
        std::string clean_def;
        bool prev_space = false;
        for (char c : def) {
            if (c == '\n' || c == '\r') c = ' ';
            if (c == ' ') {
                if (!prev_space) clean_def += c;
                prev_space = true;
            }
            else {
                clean_def += c;
                prev_space = false;
            }
        }
        def = trim(clean_def);

        result.push_back({w, def});
    };

    while (std::getline(iss, line)) {
        // Normalise line ending
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (in_quotes) {
            accumulated += "\n" + line;
            // Close when line ends with a single "
            char last = line.back();
            if (last == '"') {
                in_quotes = false;
                process_record(accumulated);
                accumulated.clear();
            }
        }
        else {
            size_t comma = line.find(',');
            if (comma == std::string::npos) continue; // skip malformed

            // Check for multi-line quoted definition
            std::string rest = line.substr(comma + 1);
            size_t first = rest.find_first_not_of(" \t");
            bool opens_quote =
                (first != std::string::npos && rest[first] == '"');
            char last_char = line.back();

            if (opens_quote && last_char != '"') {
                in_quotes = true;
                accumulated = line;
                continue;
            }
            process_record(line);
        }
    }

    // Drain any leftover
    if (in_quotes && !accumulated.empty()) {
        process_record(accumulated);
    }

    return result;
}

void wordle::load_banks()
{
    std::string dir_path = bot_config_path(nullptr, "features/wordle");

    if (!fs::exists(dir_path)) {
        return;
    }

    // 1. Scan *.csv files
    for (const auto &entry : fs::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".csv") continue;

        std::string name = entry.path().stem().string();
        std::string content =
            readfile(entry.path().string(), "");
        if (content.empty()) continue;

        auto entries = parse_csv(content);
        if (entries.empty()) continue;

        banks_[name] = std::move(entries);
        difficulty_names_.push_back(name);

        // Feed all CSV words into validation set
        for (const auto &e : banks_[name]) {
            valid_words_.insert(e.word);
        }
    }

    std::sort(difficulty_names_.begin(), difficulty_names_.end());

    // 2. Load words.txt as extra validation vocabulary
    std::string words_path =
        bot_config_path(nullptr, "features/wordle/words.txt");
    std::string words_content = readfile(words_path, "");
    if (!words_content.empty()) {
        std::istringstream wiss(words_content);
        std::string wline;
        while (std::getline(wiss, wline)) {
            wline = trim(wline);
            if (!wline.empty()) {
                std::string cw = clean_word(wline);
                if (!cw.empty()) valid_words_.insert(cw);
            }
        }
    }

    // If difficulty_names_ is empty but we have a lone words.txt,
    // create a synthetic "words" bank so the plugin is usable OOTB.
    if (banks_.empty() && !valid_words_.empty()) {
        std::vector<word_entry> synth;
        for (const auto &w : valid_words_) {
            synth.push_back({w, ""});
        }
        banks_["words"] = std::move(synth);
        difficulty_names_.push_back("words");
    }
}

/* ═══════════════════════════════════════════════════════════
   Game helpers
   ═══════════════════════════════════════════════════════════ */

wordle_game &wordle::get_game(const msg_meta &conf)
{
    if (conf.message_type == "group") {
        return group_games_[conf.group_id];
    }
    return private_games_[conf.user_id];
}

std::string wordle::check_guess(const std::string &guess,
                                const std::string &answer) const
{
    int n = (int)guess.size();
    std::string result(n, 'B');
    std::vector<int> remaining(26, 0);

    // Pass 1: greens
    for (int i = 0; i < n; i++) {
        if (guess[i] == answer[i]) {
            result[i] = 'G';
        }
        else {
            remaining[answer[i] - 'a']++;
        }
    }

    // Pass 2: yellows
    for (int i = 0; i < n; i++) {
        if (result[i] == 'G') continue;
        int idx = guess[i] - 'a';
        if (idx >= 0 && idx < 26 && remaining[idx] > 0) {
            result[i] = 'Y';
            remaining[idx]--;
        }
    }

    return result;
}

std::string wordle::render_color_block(const std::string &color)
{
    std::ostringstream oss;
    for (char c : color) {
        switch (c) {
        case 'G': oss << "🟩"; break;
        case 'Y': oss << "🟨"; break;
        default:  oss << "⬛"; break;
        }
    }
    return oss.str();
}

std::string wordle::render_history(const wordle_game &game) const
{
    if (game.history.empty()) return "";

    std::ostringstream oss;
    for (const auto &h : game.history) {
        oss << render_color_block(h.second) << "\n";
        for (size_t i = 0; i < h.first.size(); i++) {
            if (i) oss << " ";
            oss << (char)std::toupper(static_cast<unsigned char>(h.first[i]));
        }
        oss << "\n";
    }
    return oss.str();
}

/* ═══════════════════════════════════════════════════════════
   Command handlers
   ═══════════════════════════════════════════════════════════ */

void wordle::cmd_start(const msg_meta &conf)
{
    auto &game = get_game(conf);

    if (game.active) {
        conf.p->cq_send(
            "当前已有进行中的对局！\n"
            "发送 *wordle status 查看状态，或 *wordle abort 终止后重开。",
            conf);
        return;
    }

    // Resolve difficulty — use persistent setting or first available
    if (game.difficulty.empty() ||
        banks_.find(game.difficulty) == banks_.end()) {
        game.difficulty =
            difficulty_names_.empty() ? "" : difficulty_names_.front();
    }
    auto bank_it = banks_.find(game.difficulty);
    if (bank_it == banks_.end() || bank_it->second.empty()) {
        conf.p->cq_send(
            "没有可用词库。请将 CSV 词表放入 config/features/wordle/ 目录。",
            conf);
        return;
    }

    // Filter by word length
    std::vector<const word_entry *> pool;
    for (const auto &e : bank_it->second) {
        if ((int)e.word.size() == game.word_length) {
            pool.push_back(&e);
        }
    }

    if (pool.empty()) {
        conf.p->cq_send(
            fmt::format("难度 '{}' 下没有长度为 {} 的单词。"
                        "请用 *wordle set wordlength <3~8> 调整。",
                        game.difficulty, game.word_length),
            conf);
        return;
    }

    // Pick
    const word_entry *chosen = pool[get_random((int)pool.size())];

    // Init game
    game.active = true;
    game.answer = chosen->word;
    game.definition = chosen->definition;
    game.attempts_used = 0;
    game.starter = conf.user_id;
    game.history.clear();
    game.hinted_positions.clear();
    game.hint_blocked = false;
    game.last_guess_time = std::chrono::steady_clock::time_point{};

    // Hides real length behind a placeholder so players don't deduce the word
    // from help text, but honestly in a chat game they can just count

    conf.p->cq_send(
        fmt::format("Wordle 开始！\n"
                    "单词长度: {} | 可猜次数: {} | 难度: {}\n"
                    "任何群友都可以发送 *wordle guess <{}个字母> 参与猜词！",
                    game.word_length, game.max_attempts, game.difficulty,
                    game.word_length),
        conf);
}

void wordle::cmd_guess(const msg_meta &conf, std::string guess)
{
    auto &game = get_game(conf);

    if (!game.active) {
        conf.p->cq_send("当前无进行中的对局。发送 *wordle start 开始。",
                        conf);
        return;
    }

    // Sanitise
    std::string lower;
    for (char c : guess) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            lower += static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        }
    }

    if ((int)lower.size() != game.word_length) {
        conf.p->cq_send(
            fmt::format("单词长度必须为 {}（你输入了 {} 个字母）",
                        game.word_length, lower.size()),
            conf);
        return;
    }

    // Existence check
    if (valid_words_.find(lower) == valid_words_.end()) {
        conf.p->cq_send(
            fmt::format("'{}' 不在词库里，换一个试试～", lower), conf);
        return;
    }

    // Cooldown (4s) — prevent two users guessing simultaneously
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - game.last_guess_time);
        if (game.last_guess_time.time_since_epoch().count() > 0 &&
            elapsed.count() < 4) {
            conf.p->cq_send(
                fmt::format("猜得太快啦！请 {} 秒后再猜。",
                           4 - (int)elapsed.count()),
                conf);
            return;
        }
        game.last_guess_time = now;
    }

    // Attempts guard
    if (game.attempts_used >= game.max_attempts) {
        std::ostringstream oss;
        oss << "次数用尽！\n"
            << "答案: " << game.answer;
        if (!game.definition.empty())
            oss << "\n释义: " << game.definition;
        conf.p->cq_send(oss.str(), conf);
        game.active = false;
        return;
    }

    // Unblock hint after any guess
    game.hint_blocked = false;

    // Duplicate guard
    for (const auto &h : game.history) {
        if (h.first == lower) {
            conf.p->cq_send("很不幸, '" + lower + "' 已经猜过了，换一个试试。", conf);
            return;
        }
    }

    // Evaluate
    std::string color = check_guess(lower, game.answer);
    game.history.push_back({lower, color});
    game.attempts_used++;

    bool win = true;
    for (char c : color) {
        if (c != 'G') {
            win = false;
            break;
        }
    }

    std::ostringstream oss;
    oss << render_history(game);
    oss << "—— " << game.attempts_used << "/" << game.max_attempts << " ——";

    if (win) {
        oss << "\n恭喜猜中！";
        oss << "\n答案: " << game.answer;
        if (!game.definition.empty())
            oss << "\n释义: " << game.definition;
        game.active = false;
    }
    else if (game.attempts_used >= game.max_attempts) {
        oss << "\n次数用尽！";
        oss << "\n答案: " << game.answer;
        if (!game.definition.empty())
            oss << "\n释义: " << game.definition;
        game.active = false;
    }

    conf.p->cq_send(oss.str(), conf);
}

void wordle::cmd_status(const msg_meta &conf)
{
    auto &game = get_game(conf);

    if (!game.active) {
        conf.p->cq_send("当前无进行中的对局。", conf);
        return;
    }

    std::ostringstream oss;
    oss << "Wordle 对局状态\n"
        << "难度: " << game.difficulty
        << " | 单词长度: " << game.word_length
        << " | 已用: " << game.attempts_used << "/" << game.max_attempts
        << "\n";
    if (game.history.empty()) {
        oss << "还没有人猜过。";
    }
    else {
        oss << render_history(game);
        oss << "—— " << game.attempts_used << "/" << game.max_attempts
            << " ——";
    }

    conf.p->cq_send(oss.str(), conf);
}

void wordle::cmd_hint(const msg_meta &conf)
{
    auto &game = get_game(conf);

    if (!game.active) {
        conf.p->cq_send("当前无进行中的对局。", conf);
        return;
    }

    if (game.attempts_used >= game.max_attempts) {
        conf.p->cq_send("次数已用尽！", conf);
        return;
    }

    if (game.hint_blocked) {
        conf.p->cq_send("请先猜一次词再使用提示。", conf);
        return;
    }

    // Find first letter position not yet correctly guessed
    std::set<int> green_positions;
    for (const auto &h : game.history) {
        for (size_t i = 0; i < h.second.size(); i++) {
            if (h.second[i] == 'G') green_positions.insert((int)i);
        }
    }

    int hint_pos = -1;
    for (int i = 0; i < game.word_length; i++) {
        if (green_positions.find(i) == green_positions.end() &&
            game.hinted_positions.find(i) == game.hinted_positions.end()) {
            hint_pos = i;
            break;
        }
    }

    if (hint_pos == -1) {
        conf.p->cq_send("所有字母位置都已经揭示过了！", conf);
        return;
    }

    game.hinted_positions.insert(hint_pos);
    game.hint_blocked = true;
    char revealed = game.answer[hint_pos];
    game.attempts_used++;

    std::ostringstream oss;
    oss << "提示：第 " << (hint_pos + 1) << " 个字母是 '"
        << (char)std::toupper(static_cast<unsigned char>(revealed))
        << "'\n"
        << "（消耗一次猜测机会）\n";
    oss << "—— " << game.attempts_used << "/" << game.max_attempts << " ——";

    if (game.attempts_used >= game.max_attempts) {
        oss << "\n次数用尽！\n答案: " << game.answer;
        if (!game.definition.empty())
            oss << "\n释义: " << game.definition;
        game.active = false;
    }

    conf.p->cq_send(oss.str(), conf);
}

void wordle::cmd_abort(const msg_meta &conf)
{
    auto &game = get_game(conf);

    if (!game.active) {
        conf.p->cq_send("当前无进行中的对局。", conf);
        return;
    }

    // Permissions: starter, bot admin, or group admin
    bool can_abort = conf.p->is_op(conf.user_id) ||
                     conf.user_id == game.starter;
    if (!can_abort && conf.message_type == "group") {
        can_abort =
            is_group_op(conf.p, conf.group_id, conf.user_id);
    }

    if (!can_abort) {
        conf.p->cq_send("只有对局发起人或管理员可以终止对局。", conf);
        return;
    }

    std::ostringstream oss;
    oss << "对局已终止。\n答案: " << game.answer;
    if (!game.definition.empty())
        oss << "\n释义: " << game.definition;
    conf.p->cq_send(oss.str(), conf);

    game.active = false;
}

void wordle::cmd_set(const msg_meta &conf, std::string param,
                     std::string value)
{
    auto &game = get_game(conf);

    if (game.active) {
        conf.p->cq_send("当前对局进行中，无法修改设置。请等待对局结束后再试。",
                        conf);
        return;
    }

    // Normalise
    for (char &c : param)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char &c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (value.empty()) {
        conf.p->cq_send(
            "用法: *wordle set <参数> <值>\n"
            "可用参数: difficulty | wordlength | attempts",
            conf);
        return;
    }

    /* ── difficulty ──────────────────────────────────── */
    if (param == "difficulty") {
        // Case-insensitive match
        for (const auto &name : difficulty_names_) {
            std::string lower_name = name;
            for (char &c : lower_name)
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            if (lower_name == value) {
                game.difficulty = name;
                conf.p->cq_send("已切换难度至: " + name, conf);
                return;
            }
        }
        // Not found — list available
        std::ostringstream oss;
        oss << "未知难度: " << value << "\n当前可选: ";
        for (size_t i = 0; i < difficulty_names_.size(); i++) {
            if (i) oss << ", ";
            oss << difficulty_names_[i];
        }
        conf.p->cq_send(oss.str(), conf);
        return;
    }

    /* ── wordlength ──────────────────────────────────── */
    if (param == "wordlength") {
        int len = (int)my_string2int64(string_to_wstring(value));
        if (len < 3 || len > 8) {
            conf.p->cq_send("单词长度范围为 3 ~ 8。", conf);
            return;
        }
        game.word_length = len;
        conf.p->cq_send(
            fmt::format("单词长度已设为: {}", len), conf);
        return;
    }

    /* ── attempts ────────────────────────────────────── */
    if (param == "attempts") {
        int n = (int)my_string2int64(string_to_wstring(value));
        if (n < 1 || n > 20) {
            conf.p->cq_send("可猜次数范围为 1 ~ 20。", conf);
            return;
        }
        game.max_attempts = n;
        conf.p->cq_send(
            fmt::format("可猜次数已设为: {}", n), conf);
        return;
    }

    conf.p->cq_send(
        "未知参数: " + param +
            "\n可用参数: difficulty | wordlength | attempts",
        conf);
}

void wordle::cmd_help(const msg_meta &conf)
{
    std::ostringstream oss;
    oss << "Wordle 帮助\n\n"
        << "指令:\n"
        << "  *wordle start          开始新对局\n"
        << "  *wordle guess <单词>   猜词\n"
        << "  *wordle status         查看当前棋盘\n"
        << "  *wordle hint           揭示一个字母（消耗1次）\n"
        << "  *wordle abort          终止对局（发起人/管理员）\n"
        << "  *wordle set <参数> <值> 修改设置\n"
        << "  *wordle help           查看本帮助\n\n"
        << "设置:\n"
        << "  difficulty  <名称>     切换难度\n"
        << "  wordlength  <3~8>      设置单词长度\n"
        << "  attempts    <1~20>     设置可猜次数\n\n"
        << "当前可选难度: ";

    if (difficulty_names_.empty()) {
        oss << "（无可用词库，请在 config/features/wordle/ 下放置 CSV 词表）";
    }
    else {
        for (size_t i = 0; i < difficulty_names_.size(); i++) {
            if (i) oss << ", ";
            oss << difficulty_names_[i];
        }
    }

    auto &game_cfg = get_game(conf);
    oss << "\n\n当前设置: 难度=" << game_cfg.difficulty
        << " | 长度=" << game_cfg.word_length
        << " | 次数=" << game_cfg.max_attempts;

    conf.p->cq_send(oss.str(), conf);
}

/* ═══════════════════════════════════════════════════════════
   Factory
   ═══════════════════════════════════════════════════════════ */

DECLARE_FACTORY_FUNCTIONS(wordle)
