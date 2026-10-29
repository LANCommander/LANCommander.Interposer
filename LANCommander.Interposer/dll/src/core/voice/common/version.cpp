#include "LANCommander.Interposer.Comms.h"
#include "instance.h"
#include "version_info.h"

extern "C" {

INTERPOSER_API uint32_t INTERPOSER_CALL interposer_comms_get_api_version(void)
{
    FORWARD_TO_PRIMARY(interposer_comms_get_api_version);
    return INTERPOSER_COMMS_API_VERSION;
}

INTERPOSER_API const char* INTERPOSER_CALL interposer_comms_get_version_string(void)
{
    FORWARD_TO_PRIMARY(interposer_comms_get_version_string);
    // Voice ships in the Interposer, so it carries the Interposer's version.
    return VER_PRODUCTVERSION_STR;
}

INTERPOSER_API const char* INTERPOSER_CALL interposer_comms_result_to_string(interposer_comms_result result)
{
    FORWARD_TO_PRIMARY(interposer_comms_result_to_string, result);
    switch (result) {
    case INTERPOSER_COMMS_OK:                    return "OK";
    case INTERPOSER_COMMS_ERR_UNKNOWN:           return "Unknown error";
    case INTERPOSER_COMMS_ERR_INVALID_ARGUMENT:  return "Invalid argument";
    case INTERPOSER_COMMS_ERR_OUT_OF_MEMORY:     return "Out of memory";
    case INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED:   return "Not implemented";
    case INTERPOSER_COMMS_ERR_NOT_FOUND:         return "Not found";
    case INTERPOSER_COMMS_ERR_ALREADY_EXISTS:    return "Already exists";
    case INTERPOSER_COMMS_ERR_LIMIT_REACHED:     return "Limit reached";
    case INTERPOSER_COMMS_ERR_PERMISSION_DENIED: return "Permission denied";
    case INTERPOSER_COMMS_ERR_INVALID_STATE:     return "Invalid state";
    case INTERPOSER_COMMS_ERR_NETWORK:           return "Network error";
    case INTERPOSER_COMMS_ERR_AUDIO_DEVICE:      return "Audio device error";
    case INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL:  return "Buffer too small";
    case INTERPOSER_COMMS_ERR_VERSION_MISMATCH:  return "Version mismatch";
    case INTERPOSER_COMMS_ERR_AUTH_FAILED:       return "Authentication failed";
    case INTERPOSER_COMMS_ERR_TIMEOUT:           return "Timed out";
    }
    return "Unrecognized result";
}

} // extern "C"
