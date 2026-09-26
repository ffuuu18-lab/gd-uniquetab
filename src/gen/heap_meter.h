// heap_meter.h - the heap the generator itself holds, counted where it is allocated.
//
// The process working set cannot say what the generation costs inside the game: the start-up
// worker runs it while the engine is still building its main menu, and everything the engine
// loads in those seconds lands in the same number. The meter counts the bytes that pass
// through operator new / operator delete ON THE METERED THREAD while it runs, which is exactly
// the generator's containers and buffers - the archives' indexes, the records, the pages, the
// plates - and nothing the engine does beside it.
//
// This file's .cpp replaces the global operator new / delete of the module it is linked into
// (the DLL and the offline harness). Off the metered thread they are malloc and free and
// nothing else. One meter at a time; a second one started while another runs stays inactive
// and reports 0.
#pragma once

#include <cstddef>

namespace gen {

class HeapMeter {
public:
    HeapMeter();
    ~HeapMeter();
    HeapMeter(const HeapMeter&) = delete;
    HeapMeter& operator=(const HeapMeter&) = delete;

    bool active() const { return m_active; }
    // The most this thread held at once since the meter started, in bytes and in MB.
    std::size_t peakBytes() const;
    double peakMb() const { return double(peakBytes()) / (1024.0 * 1024.0); }

private:
    bool m_active;
};

} // namespace gen
