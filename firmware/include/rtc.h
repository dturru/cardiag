#pragma once

// PCF8563 RTC, runtime half (I2C). Decode/validity is rtc8563.h.

#include <stdint.h>

// Probe the RTC and, if its voltage-low flag is clear and the time is
// plausible, set the session anchor with source "rtc" (lowest trust). Never
// sets an anchor otherwise. Call once from setup(), after sessionBegin().
void rtcBegin();

// Write the session's current anchored time to the RTC (which clears VL).
// Only when the anchor came from the hub as gps or ntp: writing an rtc-sourced
// anchor back would just copy the clock to itself. Call after a hub time push.
void rtcSyncFromAnchor();

// Whether an RTC answered at boot.
bool rtcPresent();
