#pragma once

#include "processable.h"
#include <chrono>
#include <map>
#include <mutex>
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

/* One entry on the timeline: either a guess, or a hint (a hint occupies a row
   too, since it consumes one of the player's attempts). */
struct history_entry {
    bool is_hint = false;

    std::string word;    // !is_hint: the guessed word (lowercase)
    std::string colors;  // !is_hint: per-letter result, e.g. "GYBBG"

    int position = -1;   // is_hint: revealed position
    char letter = 0;     // is_hint: revealed letter (lowercase)
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

    // history: guesses and hints in the order they happened
    std::vector<history_entry> history;

    // cooldown — prevent double-guess from two users at once
    std::chrono::steady_clock::time_point last_guess_time;

    // positions already revealed by hint (skip on next hint)
    std::set<int> hinted_positions;

    // must guess at least once before next hint
    bool hint_blocked = false;

    // per-game mutex — serialises all mutations on this game
    mutable std::mutex mtx;
};

/* ── RAII helper: holds game mutex for the caller's scope ─ */

struct locked_game {
    wordle_game &game;
    std::unique_lock<std::mutex> lock;

    explicit locked_game(wordle_game &g) : game(g), lock(g.mtx) {}
    locked_game(locked_game &&) = default;
    locked_game &operator=(locked_game &&) = default;
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
    /* Word banks — protected by bank_mtx_ */
    std::unordered_map<std::string, std::vector<word_entry>> banks_;
    std::unordered_set<std::string> valid_words_;
    std::vector<std::string> difficulty_names_;

    /* Font used when drawing the board image — resolved by load_font(),
       also protected by bank_mtx_ (same lifecycle as the banks). */
    std::string font_path_;

    /* Per-group / per-private games — map access protected by map_mtx_,
       per-entry mutations protected by wordle_game::mtx              */
    std::map<groupid_t, wordle_game> group_games_;
    std::map<userid_t, wordle_game> private_games_;

    mutable std::mutex map_mtx_;   // serialises group_games_ / private_games_ access
    mutable std::mutex bank_mtx_;  // serialises banks_ / valid_words_ / difficulty_names_

    /* Internal helpers */
    void load_banks();   // caller MUST hold bank_mtx_ or be single-threaded (ctor)
    void load_font();    // caller MUST hold bank_mtx_ or be single-threaded (ctor)
    std::vector<word_entry> parse_csv(const std::string &content);

    wordle_game &get_game(const msg_meta &conf);   // caller MUST hold map_mtx_
    locked_game acquire_game(const msg_meta &conf); // acquires both map_mtx_ (brief) + game.mtx

    static std::string clean_word(const std::string &raw);

    std::string check_guess(const std::string &guess,
                            const std::string &answer) const;
    std::string render_history(const std::vector<history_entry> &history,
                               int cols) const;
    static std::string render_color_block(const std::string &color);

    /* Command handlers — each acquires its own game lock */
    void cmd_start(const msg_meta &conf);
    void cmd_guess(const msg_meta &conf, std::string guess);
    void cmd_status(const msg_meta &conf);
    void cmd_hint(const msg_meta &conf);
    void cmd_abort(const msg_meta &conf);
    void cmd_set(const msg_meta &conf, std::string param, std::string value);
    void cmd_help(const msg_meta &conf);
};

DECLARE_FACTORY_FUNCTIONS_HEADER
