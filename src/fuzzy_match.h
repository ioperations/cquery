#pragma once

#include <limits.h>
#include <string_view.h>

#include <string>

class FuzzyMatcher {
   public:
    constexpr static int k_max_pat = 100;
    constexpr static int k_max_text = 200;
    // Negative but far from INT_MIN so that intermediate results are hard to
    // overflow.
    constexpr static int k_min_score = INT_MIN / 4;

    FuzzyMatcher(std::string_view pattern);
    int Match(std::string_view text);

   private:
    std::string pat;
    std::string_view text;
    int pat_set, text_set;
    char low_pat[k_max_pat], low_text[k_max_text];
    int pat_role[k_max_pat], text_role[k_max_text];
    int dp[2][k_max_text + 1][2];

    int MatchScore(int i, int j, bool last);
    int MissScore(int j, bool last);
};
