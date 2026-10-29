// Single translation unit that compiles the miniaudio implementation into the library.
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION

#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
