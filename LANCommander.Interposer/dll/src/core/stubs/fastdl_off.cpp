// FastDL's export, for an embedded build with INTERPOSER_FEATURE_FASTDL off:
// FALSE, as when FastDL is switched off. See core/interposer_features.h.

#include "instance.h"

extern "C" {

INTERPOSER_API BOOL InterposerSetFastDLBaseUrl(const wchar_t* baseUrl)
{
    FORWARD_TO_PRIMARY(InterposerSetFastDLBaseUrl, baseUrl);
    return FALSE;
}

} // extern "C"
