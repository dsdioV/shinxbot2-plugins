#include "wordle.h"
#include "utils.h"

#include <Magick++.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

/* ═══════════════════════════════════════════════════════════
   Board image rendering

   The board is drawn from scratch instead of being typeset as text: emoji
   tiles and letters have different advance widths depending on the client's
   font, so a text board cannot be aligned reliably.

   Glyphs are rendered individually, trimmed to their ink box and composited
   onto the tile centre.  Centring via annotate() + CenterGravity() would be
   less code, but it centres the full line box (ascent + descent); with
   all-caps text the empty descent pushes the visible ink up by ~10% of the
   tile height, which is plainly visible.
   ═══════════════════════════════════════════════════════════ */

namespace {

constexpr int TILE = 40;             // tile edge, px
constexpr int GAP = 10;              // gap between tiles, px
constexpr int MARGIN = 20;           // outer margin, px
constexpr double CAP_RATIO = 0.55;   // cap height / tile edge

const char *kBoardBg = "#FFFFFF";
const char *kEmptyEdge = "#C8C8C8";
const char *kLetter = "#FFFFFF";
const char *kGreen = "#538D4E";
const char *kYellow = "#B59F3B";
const char *kGray = "#787C7E";

const char *tile_color(char result)
{
    switch (result) {
    case 'G': return kGreen;
    case 'Y': return kYellow;
    default:  return kGray;
    }
}

/* Render one glyph as a transparent image trimmed to its ink box.
   Returns false when no usable font is available at all. */
bool make_glyph(char ch, double pointsize, const std::string &font,
                Magick::Image &out)
{
    try {
        Magick::Image img;
        img.backgroundColor(Magick::Color("none"));
        img.fillColor(Magick::Color(kLetter));
        if (!font.empty()) img.font(font);
        img.fontPointsize(pointsize);
        img.read(std::string("label:") + ch);
        img.trim();
        if (img.columns() == 0 || img.rows() == 0) return false;
        out = img;
        return true;
    }
    catch (Magick::Exception &) {
        return false;
    }
}

/* config/features/wordle/font.ttf if present, else whatever fc-match
   resolves for "sans".  An empty result means "let ImageMagick decide". */
std::string lookup_font()
{
    const std::string configured =
        bot_config_path(nullptr, "features/wordle/font.ttf");
    if (fs::exists(configured)) return configured;

    std::array<char, 256> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(
        popen("fc-match --format=%{file} sans", "r"), pclose);
    if (pipe) {
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
    }
    while (!result.empty() &&
           (result.back() == '\n' || result.back() == '\r' ||
            result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

/* Everything the renderer needs, copied out from under the game lock so the
   drawing (and the network send) can happen after it is released. */
struct board_state {
    std::vector<history_entry> history;
    int rows = 0;
    int cols = 0;
};

bool draw_board(const board_state &state, const std::string &font,
                const std::string &out_path)
{
    if (state.rows <= 0 || state.cols <= 0) return false;

    try {
        Magick::Image probe;
        if (!make_glyph('H', 100.0, font, probe)) return false;
        const double pointsize =
            100.0 * CAP_RATIO * TILE / static_cast<double>(probe.rows());

        std::map<char, Magick::Image> glyphs;
        const auto glyph_of = [&](char ch) -> const Magick::Image * {
            // Letters are stored lowercase but the board shows them uppercase.
            const char key = static_cast<char>(
                std::toupper(static_cast<unsigned char>(ch)));
            auto it = glyphs.find(key);
            if (it == glyphs.end()) {
                Magick::Image img;
                if (!make_glyph(key, pointsize, font, img)) return nullptr;
                it = glyphs.emplace(key, img).first;
            }
            return &it->second;
        };

        const int width = MARGIN * 2 + state.cols * TILE + (state.cols - 1) * GAP;
        const int height = MARGIN * 2 + state.rows * TILE + (state.rows - 1) * GAP;
        Magick::Image board(Magick::Geometry(width, height),
                            Magick::Color(kBoardBg));

        for (int r = 0; r < state.rows; r++) {
            const int y = MARGIN + r * (TILE + GAP);
            const history_entry *entry =
                (r < static_cast<int>(state.history.size()))
                    ? &state.history[r]
                    : nullptr;

            for (int c = 0; c < state.cols; c++) {
                const int x = MARGIN + c * (TILE + GAP);

                const bool is_guess =
                    entry && !entry->is_hint &&
                    c < static_cast<int>(entry->word.size());
                const bool is_hint_cell =
                    entry && entry->is_hint && entry->position == c;

                if (!is_guess && !is_hint_cell) {
                    // unplayed cell, or an unrevealed cell of a hint row
                    board.strokeColor(Magick::Color(kEmptyEdge));
                    board.strokeWidth(2);
                    board.fillColor(Magick::Color("none"));
                    board.draw(Magick::DrawableRectangle(
                        x + 1, y + 1, x + TILE - 2, y + TILE - 2));
                    continue;
                }

                char letter = is_guess ? entry->word[c] : entry->letter;
                const Magick::Image *glyph = glyph_of(letter);
                if (!glyph) return false;

                board.strokeColor(Magick::Color("none"));
                board.fillColor(Magick::Color(
                    is_guess ? tile_color(entry->colors[c]) : kGreen));
                board.draw(Magick::DrawableRectangle(
                    x, y, x + TILE - 1, y + TILE - 1));
                board.composite(*glyph, x + (TILE - glyph->columns()) / 2,
                                y + (TILE - glyph->rows()) / 2,
                                Magick::OverCompositeOp);
            }
        }

        board.write(out_path);
        return true;
    }
    catch (Magick::Exception &) {
        return false;
    }
}

}   // namespace

/* Draw the board to a temporary png.  Returns true and fills `path` on
   success; the caller is responsible for deleting the file after sending. */
static bool render_board_file(const board_state &state, const std::string &font,
                              std::string &path)
{
    const std::string target =
        bot_resource_path(nullptr, "wordle/" + generate_uuid() + ".png");
    std::error_code ec;
    fs::create_directories(fs::path(target).parent_path(), ec);
    if (!draw_board(state, font, target)) {
        fs::remove(target, ec);
        return false;
    }
    path = target;
    return true;
}

/* ═══════════════════════════════════════════════════════════
   Lifecycle
   ═══════════════════════════════════════════════════════════ */

wordle::wordle()
{
    load_banks();
    load_font();
}

bool wordle::reload(const msg_meta &conf)
{
    (void)conf;
    std::lock_guard<std::mutex> bank_lock(bank_mtx_);
    banks_.clear();
    valid_words_.clear();
    difficulty_names_.clear();
    load_banks();   // bank_mtx_ still held — safe
    load_font();
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
   Bank loading  (caller MUST hold bank_mtx_ or be ctor)
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
        if (def.size() >= 2 && def.front() == '"' && def.back() == '"') {
            def = def.substr(1, def.size() - 2);
        }
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
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (in_quotes) {
            accumulated += "\n" + line;
            char last = line.back();
            if (last == '"') {
                in_quotes = false;
                process_record(accumulated);
                accumulated.clear();
            }
        }
        else {
            size_t comma = line.find(',');
            if (comma == std::string::npos) continue;

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

    if (in_quotes && !accumulated.empty()) {
        process_record(accumulated);
    }

    return result;
}

void wordle::load_banks()
{
    std::string dir_path = bot_config_path(nullptr, "features/wordle");
    if (!fs::exists(dir_path)) return;

    for (const auto &entry : fs::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".csv") continue;

        std::string name = entry.path().stem().string();
        std::string content = readfile(entry.path().string(), "");
        if (content.empty()) continue;

        auto entries = parse_csv(content);
        if (entries.empty()) continue;

        banks_[name] = std::move(entries);
        difficulty_names_.push_back(name);

        for (const auto &e : banks_[name])
            valid_words_.insert(e.word);
    }

    std::sort(difficulty_names_.begin(), difficulty_names_.end());

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

    if (banks_.empty() && !valid_words_.empty()) {
        std::vector<word_entry> synth;
        for (const auto &w : valid_words_)
            synth.push_back({w, ""});
        banks_["words"] = std::move(synth);
        difficulty_names_.push_back("words");
    }
}

void wordle::load_font()
{
    const std::string configured =
        bot_config_path(nullptr, "features/wordle/font.ttf");
    std::string chosen = lookup_font();

    if (!chosen.empty()) {
        // ImageMagick only emits a warning when it cannot read a font and
        // quietly substitutes its own default, so probe it here: a font we
        // were explicitly told to use must not fail silently.
        Magick::Image probe;
        if (!make_glyph('H', 40.0, chosen, probe)) {
            if (fs::exists(configured)) {
                set_global_log(LOG::WARNING, "wordle: 无法加载字体 " +
                                                 configured +
                                                 "，图片输出将退回文字棋盘");
            }
            chosen.clear();
        }
    }

    font_path_ = chosen;
}

/* ═══════════════════════════════════════════════════════════
   Game helpers
   ═══════════════════════════════════════════════════════════ */

wordle_game &wordle::get_game(const msg_meta &conf)
{
    // Caller MUST hold map_mtx_
    if (conf.message_type == "group") {
        return group_games_[conf.group_id];
    }
    return private_games_[conf.user_id];
}

locked_game wordle::acquire_game(const msg_meta &conf)
{
    // Lock ordering: map_mtx_ → game.mtx  (never the reverse)
    std::unique_lock<std::mutex> map_lock(map_mtx_);
    wordle_game &g = get_game(conf);
    locked_game lg(g);          // locks g.mtx
    map_lock.unlock();          // release map lock, keep game lock
    return lg;
}

std::string wordle::check_guess(const std::string &guess,
                                const std::string &answer) const
{
    int n = (int)guess.size();
    std::string result(n, 'B');
    std::vector<int> remaining(26, 0);

    for (int i = 0; i < n; i++) {
        if (guess[i] == answer[i]) {
            result[i] = 'G';
        }
        else {
            remaining[answer[i] - 'a']++;
        }
    }

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

std::string wordle::render_history(const std::vector<history_entry> &history,
                                   int cols) const
{
    if (history.empty()) return "";

    std::ostringstream oss;
    for (const auto &h : history) {
        if (h.is_hint) {
            // A hint consumes a row of its own: green where the letter was
            // revealed, empty everywhere else.
            std::string colors(cols, 'B');
            if (h.position >= 0 && h.position < cols) colors[h.position] = 'G';
            oss << render_color_block(colors) << "\n";
            for (int i = 0; i < cols; i++) {
                if (i) oss << " ";
                if (i == h.position) {
                    oss << (char)std::toupper(
                        static_cast<unsigned char>(h.letter));
                }
                else {
                    oss << "-";
                }
            }
            oss << "\n";
        }
        else {
            oss << render_color_block(h.colors) << "\n";
            for (size_t i = 0; i < h.word.size(); i++) {
                if (i) oss << " ";
                oss << (char)std::toupper(
                    static_cast<unsigned char>(h.word[i]));
            }
            oss << "\n";
        }
    }
    return oss.str();
}

/* ═══════════════════════════════════════════════════════════
   Command handlers
   ═══════════════════════════════════════════════════════════ */

void wordle::cmd_start(const msg_meta &conf)
{
    auto lg = acquire_game(conf);
    auto &game = lg.game;

    if (game.active) {
        conf.p->cq_send(
            "当前已有进行中的对局！\n"
            "发送 *wordle status 查看状态，或 *wordle abort 终止后重开。",
            conf);
        return;
    }

    // ── Bank access (under bank_mtx_) ─────────────────────
    const word_entry *chosen = nullptr;
    {
        std::lock_guard<std::mutex> bank_lock(bank_mtx_);

        if (game.difficulty.empty() ||
            banks_.find(game.difficulty) == banks_.end()) {
            game.difficulty =
                difficulty_names_.empty() ? "" : difficulty_names_.front();
        }
        auto bank_it = banks_.find(game.difficulty);
        if (bank_it == banks_.end() || bank_it->second.empty()) {
            conf.p->cq_send(
                "没有可用词库。请将 CSV 词表放入 "
                "config/features/wordle/ 目录。",
                conf);
            return;
        }

        std::vector<const word_entry *> pool;
        for (const auto &e : bank_it->second) {
            if ((int)e.word.size() == game.word_length)
                pool.push_back(&e);
        }

        if (pool.empty()) {
            conf.p->cq_send(
                fmt::format("难度 '{}' 下没有长度为 {} 的单词。"
                            "请用 *wordle set wordlength <3~8> 调整。",
                            game.difficulty, game.word_length),
                conf);
            return;
        }

        chosen = pool[get_random((int)pool.size())];
    }   // release bank_mtx_

    // ── Init game ─────────────────────────────────────────
    game.active = true;
    game.answer = chosen->word;
    game.definition = chosen->definition;
    game.attempts_used = 0;
    game.starter = conf.user_id;
    game.history.clear();
    game.hinted_positions.clear();
    game.hint_blocked = false;
    game.last_guess_time = std::chrono::steady_clock::time_point{};

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
    auto lg = acquire_game(conf);
    auto &game = lg.game;

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

    // Existence check (under bank_mtx_)
    std::string font;
    {
        std::lock_guard<std::mutex> bank_lock(bank_mtx_);
        if (valid_words_.find(lower) == valid_words_.end()) {
            conf.p->cq_send(
                fmt::format("'{}' 不在词库里，换一个试试～", lower), conf);
            return;
        }
        font = font_path_;
    }

    // Cooldown (4s)
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
        if (!h.is_hint && h.word == lower) {
            conf.p->cq_send("'" + lower + "' 已经猜过了，换一个试试。", conf);
            return;
        }
    }

    // Evaluate
    history_entry guess_entry;
    guess_entry.word = lower;
    guess_entry.colors = check_guess(lower, game.answer);
    game.history.push_back(guess_entry);
    game.attempts_used++;

    bool win = true;
    for (char c : guess_entry.colors) {
        if (c != 'G') {
            win = false;
            break;
        }
    }

    const bool finished = win || game.attempts_used >= game.max_attempts;
    if (finished) game.active = false;

    // Snapshot everything the renderer needs and drop the game lock before
    // drawing: rendering and sending are slow and must not block other
    // players in the same chat.
    board_state state;
    state.history = game.history;
    state.rows = game.max_attempts;
    state.cols = game.word_length;
    const std::string answer = game.answer;
    const std::string definition = game.definition;
    const int used = game.attempts_used;

    lg.lock.unlock();

    std::string image_path;
    std::string board;
    if (render_board_file(state, font, image_path)) {
        board = "[CQ:image,file=file://" +
                fs::absolute(fs::path(image_path)).string() + ",id=40000]";
    }
    else {
        board = render_history(state.history, state.cols);
    }

    std::ostringstream oss;
    oss << board << "\n—— " << used << "/" << state.rows << " ——";

    if (win) {
        oss << "\n恭喜猜中！";
        oss << "\n答案: " << answer;
        if (!definition.empty()) oss << "\n释义: " << definition;
    }
    else if (used >= state.rows) {
        oss << "\n次数用尽！";
        oss << "\n答案: " << answer;
        if (!definition.empty()) oss << "\n释义: " << definition;
    }

    conf.p->cq_send(oss.str(), conf);

    if (!image_path.empty()) {
        std::error_code ec;
        fs::remove(image_path, ec);
    }
}

void wordle::cmd_status(const msg_meta &conf)
{
    board_state state;
    std::string header;
    int used = 0;
    std::string font;
    {
        auto lg = acquire_game(conf);
        auto &game = lg.game;

        if (!game.active) {
            conf.p->cq_send("当前无进行中的对局。", conf);
            return;
        }

        std::ostringstream hdr;
        hdr << "Wordle 对局状态\n"
            << "难度: " << game.difficulty
            << " | 单词长度: " << game.word_length
            << " | 已用: " << game.attempts_used << "/" << game.max_attempts;
        header = hdr.str();

        state.history = game.history;
        state.rows = game.max_attempts;
        state.cols = game.word_length;
        used = game.attempts_used;

        std::lock_guard<std::mutex> bank_lock(bank_mtx_);
        font = font_path_;
    }   // game lock released before drawing

    const std::string counter =
        "—— " + std::to_string(used) + "/" + std::to_string(state.rows) + " ——";

    std::string image_path;
    std::string board;
    if (state.history.empty()) {
        board = "还没有人猜过。";
    }
    else if (render_board_file(state, font, image_path)) {
        board = "[CQ:image,file=file://" +
                fs::absolute(fs::path(image_path)).string() + ",id=40000]\n" +
                counter;
    }
    else {
        board = render_history(state.history, state.cols) + counter;
    }

    conf.p->cq_send(header + "\n" + board, conf);

    if (!image_path.empty()) {
        std::error_code ec;
        fs::remove(image_path, ec);
    }
}

void wordle::cmd_hint(const msg_meta &conf)
{
    auto lg = acquire_game(conf);
    auto &game = lg.game;

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
        if (h.is_hint) continue;
        for (size_t i = 0; i < h.colors.size(); i++) {
            if (h.colors[i] == 'G') green_positions.insert((int)i);
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

    // A hint burns one attempt, so it takes a row of its own on the board.
    history_entry hint_entry;
    hint_entry.is_hint = true;
    hint_entry.position = hint_pos;
    hint_entry.letter = revealed;
    game.history.push_back(hint_entry);

    const bool exhausted = game.attempts_used >= game.max_attempts;
    if (exhausted) game.active = false;

    // Snapshot and draw outside the game lock, same as a guess: the reply
    // shows the board so the extra row spent on the hint is visible instead
    // of the attempt counter simply jumping.
    board_state state;
    state.history = game.history;
    state.rows = game.max_attempts;
    state.cols = game.word_length;
    const std::string answer = game.answer;
    const std::string definition = game.definition;
    const int used = game.attempts_used;
    std::string font;
    {
        std::lock_guard<std::mutex> bank_lock(bank_mtx_);
        font = font_path_;
    }

    lg.lock.unlock();

    std::string image_path;
    std::string board;
    if (render_board_file(state, font, image_path)) {
        board = "[CQ:image,file=file://" +
                fs::absolute(fs::path(image_path)).string() + ",id=40000]";
    }
    else {
        board = render_history(state.history, state.cols);
    }

    std::ostringstream oss;
    oss << "提示：第 " << (hint_pos + 1) << " 个字母是 '"
        << (char)std::toupper(static_cast<unsigned char>(revealed))
        << "'\n"
        << "（消耗一次猜测机会）\n"
        << board << "\n—— " << used << "/" << state.rows << " ——";

    if (exhausted) {
        oss << "\n次数用尽！\n答案: " << answer;
        if (!definition.empty()) oss << "\n释义: " << definition;
    }

    conf.p->cq_send(oss.str(), conf);

    if (!image_path.empty()) {
        std::error_code ec;
        fs::remove(image_path, ec);
    }
}

void wordle::cmd_abort(const msg_meta &conf)
{
    auto lg = acquire_game(conf);
    auto &game = lg.game;

    if (!game.active) {
        conf.p->cq_send("当前无进行中的对局。", conf);
        return;
    }

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
    auto lg = acquire_game(conf);
    auto &game = lg.game;

    if (game.active) {
        conf.p->cq_send("当前对局进行中，无法修改设置。请等待对局结束后再试。",
                        conf);
        return;
    }

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
        std::lock_guard<std::mutex> bank_lock(bank_mtx_);
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
        << "棋盘以图片发送；若未配置字体（config/features/wordle/font.ttf）\n"
        << "则退回文字棋盘。\n\n"
        << "当前可选难度: ";

    auto lg = acquire_game(conf);
    auto &game = lg.game;

    {
        std::lock_guard<std::mutex> bank_lock(bank_mtx_);
        if (difficulty_names_.empty()) {
            oss << "（无可用词库，请在 config/features/wordle/ 下放置 CSV 词表）";
        }
        else {
            for (size_t i = 0; i < difficulty_names_.size(); i++) {
                if (i) oss << ", ";
                oss << difficulty_names_[i];
            }
        }
    }
    oss << "\n\n当前设置: 难度=" << game.difficulty
        << " | 长度=" << game.word_length
        << " | 次数=" << game.max_attempts;

    conf.p->cq_send(oss.str(), conf);
}

/* ═══════════════════════════════════════════════════════════
   Factory
   ═══════════════════════════════════════════════════════════ */

DECLARE_FACTORY_FUNCTIONS(wordle)
