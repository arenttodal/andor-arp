// A device's saved state as integers (the Live Set's blob parameter).

#include "PerformanceModel.h"
#include "StateBlob.h"

#include <catch2/catch_test_macros.hpp>

using namespace andor::arp;

TEST_CASE ("State blob: round trips any bytes, exact even through 32-bit floats", "[unit][blob]")
{
    for (const std::string& s : { std::string(), std::string ("a"), std::string ("ab"), std::string ("abc"), std::string ("\0\xff\x80 é🎹", 9),
                                  performanceToText (defaultPerformance()) })
    {
        const auto words = blob::encode (s);
        std::vector<std::int32_t> viaFloat;
        for (auto w : words)
            viaFloat.push_back (static_cast<std::int32_t> (static_cast<float> (w)));
        std::string error;
        const auto back = blob::decode (viaFloat, &error);
        INFO (error);
        REQUIRE (back.has_value());
        CHECK (*back == s);
    }
}

TEST_CASE ("State blob: damaged, truncated, foreign or newer data is refused", "[unit][blob]")
{
    auto w = blob::encode ("{\"lane\": 1}");
    std::string error;
    auto damaged = w;
    damaged.back() ^= 1;
    CHECK_FALSE (blob::decode (damaged, &error).has_value());
    auto truncated = w;
    truncated.pop_back();
    CHECK_FALSE (blob::decode (truncated, &error).has_value());
    auto newer = w;
    newer[1] = blob::version + 1;
    CHECK_FALSE (blob::decode (newer, &error).has_value());
    CHECK (error.find ("newer") != std::string::npos);
    CHECK_FALSE (blob::decode ({ 1, 2, 3 }, &error).has_value());
    CHECK_FALSE (blob::decode ({}, &error).has_value());
}
