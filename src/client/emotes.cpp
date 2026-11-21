// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/emotes.h"

#include <array>

namespace xi::client
{

namespace
{

struct EmoteInfo
{
    uint8_t     id;
    const char* command;
    const char* verb;   // without a target
    const char* toward; // with a target: "<who> <toward> <target>."
};

constexpr std::array kEmotes = {
    EmoteInfo{ 0, "point", "points", "points at" },        EmoteInfo{ 1, "bow", "bows", "bows to" },
    EmoteInfo{ 2, "salute", "salutes", "salutes" },        EmoteInfo{ 3, "kneel", "kneels", "kneels before" },
    EmoteInfo{ 4, "laugh", "laughs", "laughs at" },        EmoteInfo{ 5, "cry", "cries", "cries at" },
    EmoteInfo{ 6, "no", "shakes their head", "shakes their head at" }, EmoteInfo{ 7, "yes", "nods", "nods at" },
    EmoteInfo{ 8, "wave", "waves", "waves to" },           EmoteInfo{ 9, "goodbye", "bids farewell", "bids farewell to" },
    EmoteInfo{ 10, "welcome", "welcomes everyone", "welcomes" }, EmoteInfo{ 11, "joy", "jumps for joy", "jumps for joy at" },
    EmoteInfo{ 12, "cheer", "cheers", "cheers for" },      EmoteInfo{ 13, "clap", "claps", "applauds" },
    EmoteInfo{ 14, "praise", "gives praise", "praises" },  EmoteInfo{ 15, "smile", "smiles", "smiles at" },
    EmoteInfo{ 16, "poke", "pokes the air", "pokes" },     EmoteInfo{ 17, "slap", "slaps the air", "slaps" },
    EmoteInfo{ 18, "stagger", "staggers", "staggers toward" }, EmoteInfo{ 19, "sigh", "sighs", "sighs at" },
    EmoteInfo{ 20, "comfort", "looks comforting", "comforts" }, EmoteInfo{ 21, "surprised", "is surprised", "is surprised by" },
    EmoteInfo{ 22, "amazed", "is amazed", "is amazed by" }, EmoteInfo{ 23, "stare", "stares", "stares at" },
    EmoteInfo{ 24, "blush", "blushes", "blushes at" },     EmoteInfo{ 25, "angry", "is angry", "is angry with" },
    EmoteInfo{ 26, "disgusted", "is disgusted", "is disgusted with" }, EmoteInfo{ 27, "muted", "is speechless", "is speechless at" },
    EmoteInfo{ 28, "doze", "dozes off", "dozes off near" }, EmoteInfo{ 29, "panic", "panics", "panics at" },
    EmoteInfo{ 30, "grin", "grins", "grins at" },          EmoteInfo{ 31, "dance", "dances", "dances with" },
    EmoteInfo{ 32, "think", "thinks", "thinks about" },    EmoteInfo{ 33, "fume", "fumes", "fumes at" },
    EmoteInfo{ 34, "doubt", "looks doubtful", "doubts" },  EmoteInfo{ 35, "sulk", "sulks", "sulks at" },
    EmoteInfo{ 36, "psych", "psychs up", "psychs up with" }, EmoteInfo{ 37, "huh", "is puzzled", "is puzzled by" },
    EmoteInfo{ 38, "shocked", "is shocked", "is shocked by" }, EmoteInfo{ 43, "hurray", "cheers \"Hurray!\"", "cheers \"Hurray!\" with" },
    EmoteInfo{ 44, "toss", "tosses something", "tosses something at" },
    // Aliases and the later emotes (LSB enums/emote.h). The log text comes from the game's table (file 7025).
    EmoteInfo{ 7, "nod", "nods", "nods at" },              EmoteInfo{ 9, "farewell", "bids farewell", "bids farewell to" },
    EmoteInfo{ 26, "upset", "is upset", "is upset with" }, // the Emote List's name for /disgusted (55675 #95)
    EmoteInfo{ 65, "dance1", "dances", "dances for" },     EmoteInfo{ 66, "dance2", "dances", "dances for" },
    EmoteInfo{ 67, "dance3", "dances", "dances for" },     EmoteInfo{ 68, "dance4", "dances", "dances for" },
    EmoteInfo{ 73, "bell", "rings a bell", "rings a bell for" }, EmoteInfo{ 74, "jobemote", "strikes a pose", "strikes a pose at" },
    EmoteInfo{ 96, "aim", "readies a weapon", "aims at" },
};

} // namespace

std::optional<uint8_t> emoteId(std::string_view command)
{
    if (command.starts_with("/"))
    {
        command.remove_prefix(1);
    }
    for (const auto& e : kEmotes)
    {
        if (command == e.command)
        {
            return e.id;
        }
    }
    return std::nullopt;
}

uint16_t emoteParam(uint8_t id, std::string_view argument)
{
    if (id == 73) // /bell: C3, C3+, D3 ... C5 (file 96 records 51..75) -> 6 .. 30
    {
        constexpr std::array<std::string_view, 12> kNotes = { "C", "C+", "D", "D+", "E", "F", "F+", "G", "G+", "A", "A+", "B" };
        if (argument.size() < 2)
        {
            return 0;
        }
        const bool  sharp  = argument.back() == '+';
        const char  octave = argument[sharp ? argument.size() - 2 : argument.size() - 1];
        std::string note(argument.substr(0, 1));
        if (sharp)
        {
            note += '+';
        }
        for (std::size_t i = 0; i < kNotes.size(); ++i)
        {
            if (kNotes[i] == note && octave >= '3' && octave <= '5')
            {
                const int n = static_cast<int>(i) + 12 * (octave - '3');
                return n <= 24 ? static_cast<uint16_t>(6 + n) : 0;
            }
        }
        return 0;
    }
    if (id == 74) // /jobemote: war .. run (LSB JOBTYPE 1 .. 22) -> 0x1E + job
    {
        constexpr std::array<std::string_view, 22> kJobs = { "war", "mnk", "whm", "blm", "rdm", "thf", "pld", "drk", "bst", "brd", "rng",
                                                             "sam", "nin", "drg", "smn", "blu", "cor", "pup", "dnc", "sch", "geo", "run" };
        for (std::size_t i = 0; i < kJobs.size(); ++i)
        {
            if (kJobs[i] == argument)
            {
                return static_cast<uint16_t>(0x1E + i + 1);
            }
        }
    }
    return 0;
}

std::string emoteText(uint8_t id, const std::string& who, const std::string& target)
{
    for (const auto& e : kEmotes)
    {
        if (e.id == id)
        {
            return target.empty() || target == who ? who + " " + e.verb + "." : who + " " + e.toward + " " + target + ".";
        }
    }
    return who + " makes a gesture.";
}

} // namespace xi::client
