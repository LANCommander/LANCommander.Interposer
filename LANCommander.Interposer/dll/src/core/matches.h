#pragma once

// Match statistics: the InterposerMatch* API (declared in the public header).
//
// A match is recorded in memory under the handle InterposerMatchStart returns,
// and every step of it is passed to the match callback and the launcher's
// event pipe. Where it is delivered is chosen once per match, at its start:
//
//   HTTP      MatchStatistics.ServerUrl + Token + GameId are set. A worker
//             thread sends it to /api/Matches itself, and keeps what is not
//             yet accepted in .interposer/Matches/<id>/ so a crash or a
//             restart resumes where it left off.
//   LAUNCHER  the launcher's event pipe is connected: the launcher sends it
//             with the signed-in player's credentials.
//   NONE      otherwise: logged and passed to the callback only.
//
// See Documentation/MatchStatistics.md.

// Load the matches a previous run left unsent and start delivering them.
// Call after LoadConfig and ConnectEventPipe.
void InitMatches();

// Stop the worker. Nothing is sent from here: what has not been accepted is
// already on disk and goes with the next run. Skipped at process exit, where
// the OS has already ended the thread.
void ShutdownMatches(bool processTerminating);
