#pragma once

// A deliberately small test harness. The suite has no third-party dependency so
// it builds and runs offline on a fresh checkout; swapping in Catch2 or
// GoogleTest is a CMakeLists change and a find-replace of the three macros.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace microtest {

struct Case {
  std::string name;
  std::function<void()> run;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failures() {
  static int count = 0;
  return count;
}

inline std::string& current() {
  static std::string name;
  return name;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> run) {
    registry().push_back(Case{name, std::move(run)});
  }
};

inline void fail(const char* file, int line, const char* expr) {
  std::printf("  FAIL %s\n    %s:%d\n    %s\n", current().c_str(), file, line, expr);
  failures() += 1;
}

inline int run_all() {
  int failed_cases = 0;
  for (Case& test : registry()) {
    current() = test.name;
    const int before = failures();
    test.run();
    if (failures() != before) {
      failed_cases += 1;
    } else {
      std::printf("  ok   %s\n", test.name.c_str());
    }
  }
  std::printf("\n%zu cases, %d failed\n", registry().size(), failed_cases);
  return failed_cases == 0 ? 0 : 1;
}

}  // namespace microtest

#define TEST(name)                                                        \
  static void name();                                                     \
  static microtest::Registrar registrar_##name(#name, name);              \
  static void name()

#define CHECK(expr)                                    \
  do {                                                 \
    if (!(expr)) microtest::fail(__FILE__, __LINE__, #expr); \
  } while (false)

#define CHECK_EQ(a, b)                                                 \
  do {                                                                 \
    if (!((a) == (b))) microtest::fail(__FILE__, __LINE__, #a " == " #b); \
  } while (false)
