/*
 * A host shared object that embeds the Interposer, for the opt-in feature
 * matrix (INTERPOSER_FEATURE_MATRIX in Linux.cmake): linked with
 * --no-undefined against each trimmed LANCommander.Interposer.Static, so a
 * missing stub or guard fails the build. Attaches and detaches as a game
 * server plugin does; nothing runs it.
 */
#include "LANCommander.Interposer.Embedded.h"

__attribute__((visibility("default"))) int EmbeddedProbeAttach(void)
{
    return InterposerEmbeddedAttach();
}

__attribute__((visibility("default"))) void EmbeddedProbeDetach(void)
{
    InterposerEmbeddedDetach(0);
}
