#include "voice/common/log.h"
#include "instance.h"
#include "config.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {

std::mutex    g_log_mutex;
interposer_comms_log_fn    g_log_fn = nullptr;
interposer_comms_log_level g_log_min = INTERPOSER_COMMS_LOG_OFF;
void*         g_log_user = nullptr;

// The session log's threshold, set by the patch through
// interposer_comms_set_session_logging rather than by Config.yml: voice only
// runs when a patch drives it, so the patch is what knows it wants the lines.
std::atomic<int> g_session_min{INTERPOSER_COMMS_LOG_OFF};

bool session_log_wants(interposer_comms_log_level level)
{
    return level < INTERPOSER_COMMS_LOG_OFF && level >= g_session_min.load(std::memory_order_relaxed);
}

const wchar_t* session_log_verb(interposer_comms_log_level level)
{
    switch (level) {
    case INTERPOSER_COMMS_LOG_WARN:  return L"VOICE WARNING";
    case INTERPOSER_COMMS_LOG_ERROR: return L"VOICE ERROR";
    default:                         return L"VOICE";
    }
}

} // namespace

namespace comms {

// Two sinks: the consumer's handler (interposer_comms_set_log_handler), and
// the Interposer's session log. Either may be off; the line is formatted only
// if one of them wants it.
void log(interposer_comms_log_level level, const char* fmt, ...)
{
    const bool session = session_log_wants(level);

    std::unique_lock lock(g_log_mutex);
    const bool handler = g_log_fn && level >= g_log_min;
    if (!handler && !session)
        return;

    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (handler)
        g_log_fn(level, buffer, g_log_user);
    lock.unlock();

    if (session)
        LogVoice(session_log_verb(level), platform::Utf8ToWide(buffer).c_str());
}

} // namespace comms

extern "C" INTERPOSER_API void INTERPOSER_CALL interposer_comms_set_log_handler(interposer_comms_log_fn fn, interposer_comms_log_level min_level, void* user_data)
{
    FORWARD_TO_PRIMARY(interposer_comms_set_log_handler, fn, min_level, user_data);
    std::lock_guard lock(g_log_mutex);
    g_log_fn = fn;
    g_log_min = fn ? min_level : INTERPOSER_COMMS_LOG_OFF;
    g_log_user = user_data;
}

extern "C" INTERPOSER_API void INTERPOSER_CALL interposer_comms_set_session_logging(interposer_comms_log_level min_level)
{
    FORWARD_TO_PRIMARY(interposer_comms_set_session_logging, min_level);
    g_session_min.store(min_level, std::memory_order_relaxed);
}
