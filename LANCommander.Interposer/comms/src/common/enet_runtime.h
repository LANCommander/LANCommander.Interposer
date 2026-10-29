#pragma once

#include <enet/enet.h>

#include <mutex>

namespace comms {

// enet_initialize/deinitialize are process-global; reference-count them across server and client instances.
class EnetRuntime {
public:
    static bool acquire()
    {
        std::lock_guard lock(mutex());
        if (count() == 0 && enet_initialize() != 0)
            return false;
        ++count();
        return true;
    }

    static void release()
    {
        std::lock_guard lock(mutex());
        if (count() > 0 && --count() == 0)
            enet_deinitialize();
    }

private:
    static std::mutex& mutex() { static std::mutex m; return m; }
    static int& count() { static int c = 0; return c; }
};

} // namespace comms
