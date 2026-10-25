# Rules for AI agents working on lisbench

1. Test first: write the failing test, then the code. Every protocol rule
   has a test.
2. Modern C++ is mandatory (C++23 where GCC 13, Clang 19 and MSVC 2022 all
   support it). Use the standard feature instead of hand-written code:
   `std::span`, `std::expected`, `std::variant`, ranges, `<format>`,
   designated initializers, `constexpr`, concepts. Stand-ins for missing
   library parts live only in `include/lisbench/compat.hpp`.
3. clang-tidy and clang-format (versions in CI) must pass; every NOLINT names
   the check and the reason.
4. The core (`include/`, `src/`) does no I/O and never sleeps; drivers own
   sockets, ports and clocks.
5. Clean room: no code, schemas, defect lists, captures or names from any
   employer's or customer's products. Test data is synthetic or from public
   manuals with source. CLSI documents are referenced, never copied.
6. Use the CLSI terms (LIS01-A2, LIS02-A2, session, message, record, frame,
   field, repeat, component); ASTM numbers only as reference.
7. A PR description names which modern feature replaced which old pattern.
