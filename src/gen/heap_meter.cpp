// heap_meter.cpp - see heap_meter.h.
#include "gen/heap_meter.h"

#include <cstdlib>
#include <malloc.h>
#include <new>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

volatile LONG g_owner = 0;    // the metered thread's id; 0 = no meter running
long long g_live = 0;         // written by the owner thread only
long long g_peak = 0;

inline bool metered() {
    return g_owner != 0 && DWORD(g_owner) == GetCurrentThreadId();
}

void* allocate(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    if (metered()) {
        g_live += (long long)_msize(p);
        if (g_live > g_peak) g_peak = g_live;
    }
    return p;
}

void release(void* p) noexcept {
    if (!p) return;
    if (metered()) g_live -= (long long)_msize(p);
    std::free(p);
}

} // namespace

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }

namespace gen {

HeapMeter::HeapMeter() : m_active(false) {
    // A structured exception out of a generation skips the destructor; the same thread may
    // then start again, and takes its own stale meter over.
    const LONG me = LONG(GetCurrentThreadId());
    const LONG was = InterlockedCompareExchange(&g_owner, me, 0);
    if (was == 0 || was == me) {
        g_live = 0;
        g_peak = 0;
        m_active = true;
    }
}

HeapMeter::~HeapMeter() {
    if (m_active) InterlockedExchange(&g_owner, 0);
}

std::size_t HeapMeter::peakBytes() const {
    return m_active && g_peak > 0 ? std::size_t(g_peak) : 0;
}

} // namespace gen
