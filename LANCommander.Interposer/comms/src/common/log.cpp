#include "common/log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {

std::mutex    g_log_mutex;
interposer_comms_log_fn    g_log_fn = nullptr;
interposer_comms_log_level g_log_min = INTERPOSER_COMMS_LOG_OFF;
void*         g_log_user = nullptr;

} // namespace

namespace comms {

void log(interposer_comms_log_level level, const char* fmt, ...)
{
    std::lock_guard lock(g_log_mutex);
    if (!g_log_fn || level < g_log_min)
        return;

    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    g_log_fn(level, buffer, g_log_user);
}

} // namespace comms

extern "C" INTERPOSER_COMMS_API void INTERPOSER_COMMS_CALL interposer_comms_set_log_handler(interposer_comms_log_fn fn, interposer_comms_log_level min_level, void* user_data)
{
    std::lock_guard lock(g_log_mutex);
    g_log_fn = fn;
    g_log_min = fn ? min_level : INTERPOSER_COMMS_LOG_OFF;
    g_log_user = user_data;
}
