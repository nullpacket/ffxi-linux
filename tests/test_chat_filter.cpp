// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/chat_filter.h"

#include <catch2/catch_test_macros.hpp>

using xi::client::ChatFilter;

TEST_CASE("chat filter: term mode drops shouts with a prohibited term unless allowed")
{
    ChatFilter f;
    f.set("gil seller\r\n(n)Spammer\nb[uo]y.ng\n", "gil seller guild\n");
    f.setMode(1);
    CHECK(f.check(1, "Someone", "Best GIL SELLER here") == ChatFilter::Verdict::Drop);
    CHECK(f.check(1, "Someone", "the gil seller guild meets") == ChatFilter::Verdict::Show); // allowed.txt wins
    CHECK(f.check(26, "Someone", "now buying cheap") == ChatFilter::Verdict::Drop);         // class and wildcard
    CHECK(f.check(26, "Someone", "now baying cheap") == ChatFilter::Verdict::Show);
    CHECK(f.check(0, "Someone", "gil seller") == ChatFilter::Verdict::Show);  // say is not filtered
    CHECK(f.check(1, "Spammer", "hello") == ChatFilter::Verdict::Drop);       // (n) entries test the sender in both modes
}

TEST_CASE("chat filter: mode 2 replaces matching lines (sender or text)")
{
    ChatFilter f;
    f.set("(n)Spammer\ngil seller\n", "");
    f.setMode(2);
    CHECK(f.check(1, "spammer", "hello") == ChatFilter::Verdict::Deleted);
    CHECK(f.check(1, "Spammers", "hello") == ChatFilter::Verdict::Show); // the whole name
    CHECK(f.check(1, "Someone", "cheap gil seller") == ChatFilter::Verdict::Deleted);
    f.setMode(0);
    CHECK(f.check(1, "Spammer", "hello") == ChatFilter::Verdict::Show);
}

TEST_CASE("chat filter: profanity masks matches with stars")
{
    ChatFilter f;
    f.setProfanity("darn\n\\.dot\n");
    CHECK(f.censor("Well DARN it") == "Well **** it");
    CHECK(f.censor("a .dot and xdot") == "a **** and xdot");
    f.setProfanityOn(false);
    CHECK(f.censor("darn") == "darn");
}
