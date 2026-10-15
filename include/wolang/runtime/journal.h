// Copyright 2026 The Wolang Authors. All rights reserved.
//
// A journal of everything a program could not have worked out itself.
//
// What this is for: "it only goes wrong on the server". Record a run and
// the clock, the input and the identifiers it saw are written down;
// replay it and the program gets the same answers in the same order,
// which makes the failure happen again on a laptop (D142).
//
// What it is not: a recording of the *program*. Threads interleave as
// the machine decides, and a program whose outcome depends on that is
// not made deterministic by this -- which is said out loud rather than
// discovered.

#ifndef WOLANG_RUNTIME_JOURNAL_H_
#define WOLANG_RUNTIME_JOURNAL_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// Starts recording into |path|. Zero on success.
int32_t JournalRecord(const char* path);

// Starts replaying |path|. Zero on success.
int32_t JournalReplay(const char* path);

// Finishes a recording: flushes and closes the file. A replay needs no
// ending, but calling this for both costs nothing.
void JournalClose();

// Whether a journal is open, and which way round.
bool JournalRecording();
bool JournalReplaying();

// The shims, registered under the names the standard library declares:
// everything a program reaches for that the machine answers.
extern "C" {
int32_t wolang_journal_clock(int32_t clock, void* out);
int64_t wolang_journal_read(int32_t fd, void* buffer, uint64_t count);
int32_t wolang_journal_getpid(void);
}

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_JOURNAL_H_
