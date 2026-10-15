// Copyright 2026 The Wolang Authors. All rights reserved.
//
// What `wolang bind` makes of C declarations.

#include "wolang/bind/bind.h"

#include <string>

#include "harness/test.h"

namespace {

std::string Of(const std::string& header, const std::string& prefix = "") {
  wolang::bind::Options options;
  options.header = "test.h";
  options.prefix = prefix;
  return wolang::bind::Generate(header, options).source;
}

bool Holds(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

WOLANG_TEST(BindTest, MapsTheTypesACAbiPasses) {
  const std::string out = Of(R"(
int plain(int a, unsigned b, long c, unsigned long long d);
double maths(float x, double y);
void nothing(void);
_Bool flag(char c, unsigned char u, short s);
)");
  WOLANG_EXPECT(Holds(out, "fun plain(a: s32, b: u32, c: s64, d: u64) -> s32"));
  WOLANG_EXPECT(Holds(out, "fun maths(x: f32, y: f64) -> f64"));
  // Nothing in and nothing out: no parameters and no result.
  WOLANG_EXPECT(Holds(out, "fun nothing()\n"));
  WOLANG_EXPECT(Holds(out, "fun flag(c: s8, u: u8, s: s16) -> bool"));
}

WOLANG_TEST(BindTest, TreatsAPointerAsAnAddress) {
  const std::string out = Of(R"(
typedef struct handle handle;
handle* open(const char* name);
int read(handle* it, unsigned char* into, size_t count);
int sized(int32_t a, uint64_t b, int8_t* c);
)");
  // A `const char*` may not be written through; a `char*` may.
  WOLANG_EXPECT(Holds(out, "fun open(name: *u8) -> *mut u8"));
  WOLANG_EXPECT(
      Holds(out, "fun read(it: *mut u8, into: *mut u8, count: u64) -> s32"));
  WOLANG_EXPECT(Holds(out, "fun sized(a: s32, b: u64, c: *mut s8) -> s32"));
}

WOLANG_TEST(BindTest, MakesConstantsOfEnumMembers) {
  const std::string out = Of(R"(
enum flags {
  NONE = 0,
  FIRST = 1,
  SECOND,
  BIG = 0x10,
};
)");
  WOLANG_EXPECT(Holds(out, "const NONE: int = 0"));
  WOLANG_EXPECT(Holds(out, "const FIRST: int = 1"));
  // A member with no value is the one before it plus one, which is what C
  // says.
  WOLANG_EXPECT(Holds(out, "const SECOND: int = 2"));
  WOLANG_EXPECT(Holds(out, "const BIG: int = 0x10"));
}

WOLANG_TEST(BindTest, FollowsSimpleTypedefs) {
  const std::string out = Of(R"(
typedef unsigned int id;
typedef double seconds;
id make(seconds when);
)");
  WOLANG_EXPECT(Holds(out, "type id = u32"));
  WOLANG_EXPECT(Holds(out, "type seconds = f64"));
  WOLANG_EXPECT(Holds(out, "fun make(when: f64) -> u32"));
}

WOLANG_TEST(BindTest, SaysWhatItSkipped) {
  wolang::bind::Options options;
  const wolang::bind::Result result = wolang::bind::Generate(R"(
void logline(const char* format, ...);
struct big { int a; int b; };
struct big by_value(struct big value);
int fine(int a);
)",
                                                             options);
  // A variadic has no one calling convention, and a struct by value is
  // not something this generates -- both said out loud rather than
  // dropped (D130).
  WOLANG_EXPECT_EQ(result.functions, 1);
  WOLANG_EXPECT(result.skipped.size() >= 2);
  WOLANG_EXPECT(Holds(result.source, "fun fine(a: s32) -> s32"));
}

WOLANG_TEST(BindTest, KeepsOnlyWhatThePrefixNames) {
  const std::string out = Of(R"(
int sdl_init(int flags);
int other_thing(int x);
enum e { SDL_FIRST = 1, OTHER = 2 };
)",
                             "sdl_,SDL_");
  WOLANG_EXPECT(Holds(out, "sdl_init"));
  WOLANG_EXPECT(!Holds(out, "other_thing"));
  WOLANG_EXPECT(Holds(out, "SDL_FIRST"));
  WOLANG_EXPECT(!Holds(out, "OTHER"));
}

WOLANG_TEST(BindTest, IgnoresWhatIsNotADeclaration) {
  // Comments, strings and a function body: a header has all three, and
  // none of them is a declaration.
  const std::string out = Of(R"(
// int commented(void);
/* int blocked(void); */
static inline int inlined(int a) { return a + 1; }
extern int real(int a);
)");
  WOLANG_EXPECT(!Holds(out, "commented"));
  WOLANG_EXPECT(!Holds(out, "blocked"));
  WOLANG_EXPECT(Holds(out, "fun real(a: s32) -> s32"));
}
