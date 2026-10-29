#pragma once

#include <comms/comms.h>

namespace comms {

void log(interposer_comms_log_level level, const char* fmt, ...);

} // namespace comms

#define INTERPOSER_COMMS_LOG_TRACE_F(...) ::comms::log(INTERPOSER_COMMS_LOG_TRACE, __VA_ARGS__)
#define INTERPOSER_COMMS_LOG_DEBUG_F(...) ::comms::log(INTERPOSER_COMMS_LOG_DEBUG, __VA_ARGS__)
#define INTERPOSER_COMMS_LOG_INFO_F(...)  ::comms::log(INTERPOSER_COMMS_LOG_INFO, __VA_ARGS__)
#define INTERPOSER_COMMS_LOG_WARN_F(...)  ::comms::log(INTERPOSER_COMMS_LOG_WARN, __VA_ARGS__)
#define INTERPOSER_COMMS_LOG_ERROR_F(...) ::comms::log(INTERPOSER_COMMS_LOG_ERROR, __VA_ARGS__)
