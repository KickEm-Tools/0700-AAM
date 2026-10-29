#pragma once
#include <mutex>
#include <thread> // std::this_thread::sleep_for (Daw/DawEngine.cpp, Daw/DawRouting.cpp)
#include <chrono>

namespace AudIO
{
    using recursive_mutex = std::recursive_mutex;
    //using autolock = std::lock_guard<recursive_mutex>;
    using autolock = std::scoped_lock<recursive_mutex>;
}
