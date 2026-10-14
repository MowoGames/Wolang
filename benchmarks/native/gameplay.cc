// Copyright 2026 The Wolang Authors. All rights reserved.
//
// The same four kernels as `benchmarks/gameplay.wo`, in C++, so that "within
// 3x of C++" is a measurement rather than a claim. Built with -O2, which is
// what a gameplay build uses.
//
// Each kernel's result is passed through a barrier the optimizer cannot see
// into, because the interesting loops here all have closed forms and an
// unobserved result would let the compiler write them down instead of
// running them.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <string>

namespace {

// Keeps a value observable without writing it anywhere.
template <typename T>
inline void Observe(const T& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

// The work has to be opaque to the optimizer or there is nothing to time:
// `Fib(27)` is a constant expression, and so is a counting loop with a
// known bound. Reading the inputs through a volatile is how a benchmark
// keeps a compiler honest -- and gameplay code's trip counts are runtime
// values anyway.
volatile int32_t kFibSeed = 27;
volatile int32_t kSumIterations = 3000000;
volatile int32_t kVectorIterations = 1000000;
volatile int32_t kArrayIterations = 2000000;

struct Vec3 {
  float x;
  float y;
  float z;
};

int32_t Fib(int32_t n) {
  if (n < 2) {
    return n;
  }
  return Fib(n - 1) + Fib(n - 2);
}

int32_t BenchFibonacci() {
  return Fib(kFibSeed);
}

int32_t BenchSumLoop() {
  const int32_t n = kSumIterations;
  int32_t total = 0;
  int32_t i = 0;
  while (i < n) {
    total = total + i % 7;
    i = i + 1;
  }
  return total;
}

float Dot(Vec3 a, Vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Scale(Vec3 v, float k) {
  return Vec3{v.x * k, v.y * k, v.z * k};
}

int32_t BenchVectorDot() {
  const int32_t n = kVectorIterations;
  float total = 0.0f;
  int32_t i = 0;
  while (i < n) {
    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b = Scale(a, 0.5f);
    total = total + Dot(a, b);
    i = i + 1;
  }
  return static_cast<int32_t>(total);
}

int32_t BenchArraySum() {
  const int32_t data[8] = {3, 1, 4, 1, 5, 9, 2, 6};
  const int32_t n = kArrayIterations;
  int32_t total = 0;
  int32_t i = 0;
  while (i < n) {
    total = total + data[i % 8];
    i = i + 1;
  }
  return total;
}

void Time(const char* name, int32_t (*kernel)()) {
  // The best of several runs, the same way `wolang bench` measures.
  constexpr int kRuns = 5;
  Observe(kernel());
  double ms = 0;
  int32_t result = 0;
  for (int run = 0; run < kRuns; ++run) {
    const auto start = std::chrono::steady_clock::now();
    result = kernel();
    const auto finish = std::chrono::steady_clock::now();
    Observe(result);
    const double took =
        std::chrono::duration<double, std::milli>(finish - start).count();
    ms = run == 0 ? took : (took < ms ? took : ms);
  }
  std::printf("@%-24s %10.3f ms   result %d\n", name, ms, result);
}

}  // namespace

int main() {
  Time("BenchFibonacci", &BenchFibonacci);
  Time("BenchSumLoop", &BenchSumLoop);
  Time("BenchVectorDot", &BenchVectorDot);
  Time("BenchArraySum", &BenchArraySum);
  return 0;
}
