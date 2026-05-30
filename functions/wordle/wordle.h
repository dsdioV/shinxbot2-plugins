#pragma once

#include "processable.h"
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/* ── Data structures ─────────────────────────────────────── */

struct word_entry {
    std::string word;       // lowercase, alpha-only
    std::string definition; // cleaned (newlines → spaces, collapsed whitespace)
};

struct wordle_game {
    bool active = false;
    std::string answer;       // lowercase
    std::string definition;
    int word_length = 5;
    int max_attempts = 6;
    int attempts_used = 0;
    userid_t starter = 0;
    std::string difficulty;   // e.g. "CET4"

    // history: (guess, color-string like "GGYYB")
    std::vector<std::pair<std::string, std::string>> history;

    // cooldown — prevent double-guess from two users at once
    std::chrono::steady_clock::time_point last_guess_time;

    // positions already revealed by hint (skip on next hint)
    std::set<int> hinted_positions;

    // must guess at least once before next hint
    bool hint_blocked = false;
};

/* ── Plugin class ────────────────────────────────────────── */

class wordle : public processable {
public:
    wordle();

    // processable interface
    void process(std::string message, const msg_meta &conf) override;
    bool check(std::string message, const msg_meta &conf) override;
    std::string help() override;
    bool reload(const msg_meta &conf) override;

private:
    /* Word banks */
    // difficulty → word list (parsed from CSV files)
    std::unordered_map<std::string, std::vector<word_entry>> banks_;
    // all valid guess words (from all CSVs + words.txt)
    std::unordered_set<std::string> valid_words_;
    // available difficulty names (sorted)
    std::vector<std::string> difficulty_names_;

    /* Per-group / per-private games */
    std::map<groupid_t, wordle_game> group_games_;
    std::map<userid_t, wordle_game> private_games_;

    /* Internal helpers */
    void load_banks();
    std::vector<word_entry> parse_csv(const std::string &content);
    wordle_game &get_game(const msg_meta &conf);
    static std::string clean_word(const std::string &raw);

    std::string check_guess(const std::string &guess,
                            const std::string &answer) const;
    std::string render_history(const wordle_game &game) const;
    static std::string render_color_block(const std::string &color);

    /* Command handlers */
    void cmd_start(const msg_meta &conf);
    void cmd_guess(const msg_meta &conf, std::string guess);
    void cmd_status(const msg_meta &conf);
    void cmd_hint(const msg_meta &conf);
    void cmd_abort(const msg_meta &conf);
    void cmd_set(const msg_meta &conf, std::string param, std::string value);
    void cmd_help(const msg_meta &conf);
};

DECLARE_FACTORY_FUNCTIONS_HEADER
