// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "core/device_memory_manager.h"

namespace VideoCommon::Nx2ProtectDiag {

/// EDEN_NX2_PROTECT_DIAG=1 (with EDEN_NX2_GPU_PROFILE): page protects and unprotects the buffer
/// cache's tracker asks for, by caller (the rasterizer entry point), by buffer cache path, and the
/// pages and 64 KiB regions with the most of them. Logged with the buffer cache counters.
inline const bool enabled = [] {
    const char* const value = std::getenv("EDEN_NX2_PROTECT_DIAG");
    return value != nullptr && value[0] == '1';
}();

/// Rasterizer entry point (thread-local, outermost scope wins).
enum class Caller : u8 {
    Gpu, ///< none: the GPU thread's own buffer cache work (draws, dispatches, frame end)
    CpuWriteTrap,
    WriteHandoff,
    CacheInvalidation,
    TryCacheInvalidation,
    InvalidateRegion,
    InnerInvalidation,
    Unmap,
    InlineToMemory,
    FlushRegion,
    Count,
};

/// Buffer cache path (thread-local, innermost scope wins).
enum class Site : u8 {
    Other,
    Sync,
    WriteMemory,
    CachedWrite,
    Download,
    Hot,
    FrameFlush,
    DeleteBuffer,
    Count,
};

inline constexpr std::array<const char*, size_t(Caller::Count)> caller_names{
    "gpu thread", "cpu write trap", "write handoff", "cache invalidation",
    "try cache invalidation", "invalidate region", "inner invalidation", "unmap",
    "inline to memory", "flush region",
};
inline constexpr std::array<const char*, size_t(Site::Count)> site_names{
    "other", "sync", "write memory", "cached write", "download", "hot pages", "frame flush",
    "delete buffer",
};

inline thread_local Caller g_caller = Caller::Gpu;
inline thread_local Site g_site = Site::Other;

struct CallerScope {
    explicit CallerScope(Caller caller) : previous{g_caller} {
        if (previous == Caller::Gpu) {
            g_caller = caller;
        }
    }
    ~CallerScope() {
        g_caller = previous;
    }
    CallerScope(const CallerScope&) = delete;
    CallerScope& operator=(const CallerScope&) = delete;

private:
    Caller previous;
};

struct SiteScope {
    explicit SiteScope(Site site) : previous{g_site} {
        g_site = site;
    }
    ~SiteScope() {
        g_site = previous;
    }
    SiteScope(const SiteScope&) = delete;
    SiteScope& operator=(const SiteScope&) = delete;

private:
    Site previous;
};

struct State {
    /// Pages per [caller][site][protect, unprotect].
    std::array<std::array<std::array<std::atomic<u64>, 2>, size_t(Site::Count)>,
               size_t(Caller::Count)>
        pages{};
    std::mutex mutex;
    std::unordered_map<u64, std::array<u32, 2>> per_page;
};

inline State& GetState() {
    static State state;
    return state;
}

inline void Note(std::span<const std::pair<u64, size_t>> ranges, int delta) {
    if (!enabled) [[likely]] {
        return;
    }
    State& state = GetState();
    const size_t kind = delta > 0 ? 0 : 1;
    u64 total = 0;
    std::scoped_lock lock{state.mutex};
    for (const auto& [addr, size] : ranges) {
        const u64 first = addr >> Core::DEVICE_PAGEBITS;
        const u64 last = (addr + size + Core::DEVICE_PAGESIZE - 1) >> Core::DEVICE_PAGEBITS;
        total += last - first;
        for (u64 page = first; page < last; ++page) {
            ++state.per_page[page][kind];
        }
    }
    state.pages[size_t(g_caller)][size_t(g_site)][kind].fetch_add(total,
                                                                  std::memory_order_relaxed);
}

/// Logs and resets the counts of the last `window_ns`.
inline void Report(u64 window_ns) {
    if (!enabled || window_ns == 0) {
        return;
    }
    static constexpr size_t TOP = 8;
    static constexpr u64 REGION_PAGE_BITS = 16 - Core::DEVICE_PAGEBITS;
    State& state = GetState();
    const double scale = 1e9 / static_cast<double>(window_ns);
    struct Row {
        std::string name;
        u64 protects;
        u64 unprotects;
    };
    const auto by_total = [](const Row& a, const Row& b) {
        return a.protects + a.unprotects > b.protects + b.unprotects;
    };
    const auto format_rows = [&](std::vector<Row>& rows) {
        const size_t count = (std::min)(rows.size(), TOP);
        std::partial_sort(rows.begin(), rows.begin() + count, rows.end(), by_total);
        std::string text;
        for (size_t i = 0; i < count; ++i) {
            text += fmt::format("{}{} +{:.0f}/-{:.0f}", i == 0 ? "" : ", ", rows[i].name,
                                static_cast<double>(rows[i].protects) * scale,
                                static_cast<double>(rows[i].unprotects) * scale);
        }
        return text;
    };
    std::vector<Row> sites;
    for (size_t c = 0; c < size_t(Caller::Count); ++c) {
        for (size_t s = 0; s < size_t(Site::Count); ++s) {
            const u64 protects = state.pages[c][s][0].exchange(0, std::memory_order_relaxed);
            const u64 unprotects = state.pages[c][s][1].exchange(0, std::memory_order_relaxed);
            if (protects + unprotects != 0) {
                sites.push_back(Row{fmt::format("{}/{}", caller_names[c], site_names[s]),
                                    protects, unprotects});
            }
        }
    }
    std::unordered_map<u64, std::array<u32, 2>> per_page;
    {
        std::scoped_lock lock{state.mutex};
        per_page.swap(state.per_page);
    }
    std::vector<Row> pages;
    std::unordered_map<u64, std::array<u64, 2>> per_region;
    pages.reserve(per_page.size());
    for (const auto& [page, counts] : per_page) {
        pages.push_back(Row{fmt::format("{:#x}", page << Core::DEVICE_PAGEBITS), counts[0],
                            counts[1]});
        auto& region = per_region[page >> REGION_PAGE_BITS];
        region[0] += counts[0];
        region[1] += counts[1];
    }
    std::vector<Row> regions;
    regions.reserve(per_region.size());
    for (const auto& [region, counts] : per_region) {
        regions.push_back(Row{fmt::format("{:#x}", region << 16), counts[0], counts[1]});
    }
    LOG_INFO(HW_GPU, "Tracker protects/unprotects (pages) per second by caller/path: {}",
             format_rows(sites));
    LOG_INFO(HW_GPU, "Tracker protects/unprotects per second, top pages ({} touched): {}",
             pages.size(), format_rows(pages));
    LOG_INFO(HW_GPU, "Tracker protects/unprotects per second, top 64 KiB regions ({} touched): {}",
             regions.size(), format_rows(regions));
}

} // namespace VideoCommon::Nx2ProtectDiag
