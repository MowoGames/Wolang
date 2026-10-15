// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_FILES_H_
#define WOLANG_RUNTIME_FILES_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// Opening a file, with the platform's own numbers.
//
// `open` itself is a C call the language can make, but its flags are
// not the same on two systems: `O_CREAT` is 64 on Linux and 512 on
// Darwin. A library written in Wolang cannot say both, and guessing is
// how a file ends up opened read-only on one of them -- so the flags stay
// here, where the header that defines them is (D92).
extern "C" {

// Opens an existing file for reading. Negative on failure.
int32_t wolang_open_read(const char* path, uint64_t length);

// Opens a file for writing, making it or emptying it first.
int32_t wolang_open_write(const char* path, uint64_t length);

// The rest of a filesystem (D149). Paths arrive as a view with a
// length, the way every string in this language does.
int32_t wolang_fs_kind(const uint8_t* path, uint64_t length);
int64_t wolang_fs_size(const uint8_t* path, uint64_t length);
int64_t wolang_fs_changed(const uint8_t* path, uint64_t length);
int32_t wolang_fs_remove(const uint8_t* path, uint64_t length);
int32_t wolang_fs_rename(const uint8_t* from,
                         uint64_t from_length,
                         const uint8_t* to,
                         uint64_t to_length);
int32_t wolang_fs_make_directory(const uint8_t* path,
                                 uint64_t length,
                                 int32_t parents);
// The names in a directory, separated by line breaks. How many bytes
// were written, or -1 when the buffer is too small.
int64_t wolang_fs_list(const uint8_t* path,
                       uint64_t length,
                       uint8_t* into,
                       uint64_t room);


// Where the program is, where the person's things are, and where
// something temporary belongs. Each answers how many bytes it wrote, or
// -1 -- including when the room was too small, which a caller fixes by
// asking again with more (D179).
int64_t wolang_fs_current(uint8_t* into, uint64_t room);
int32_t wolang_fs_set_current(const uint8_t* path, uint64_t length);
int64_t wolang_fs_home(uint8_t* into, uint64_t room);
int64_t wolang_fs_temp_directory(uint8_t* into, uint64_t room);
int64_t wolang_fs_temp_name(const uint8_t* prefix, uint64_t length,
                            uint8_t* into, uint64_t room);

// What a path really is, with `.` and `..` resolved, whether or not it
// is there yet.
int64_t wolang_fs_absolute(const uint8_t* path, uint64_t length,
                           uint8_t* into, uint64_t room);

int32_t wolang_fs_copy(const uint8_t* from, uint64_t from_length,
                       const uint8_t* to, uint64_t to_length,
                       int32_t over);
int32_t wolang_fs_remove_tree(const uint8_t* path, uint64_t length);
int64_t wolang_fs_walk(const uint8_t* path, uint64_t length,
                       uint8_t* into, uint64_t room);
int32_t wolang_fs_append(const uint8_t* path, uint64_t length,
                         const uint8_t* bytes, uint64_t count);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_FILES_H_
