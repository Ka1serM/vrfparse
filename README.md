# vrfparse

vrfparse is a C++23 library for reading Valorant replay files (`.vrf`). Under the hood, a `.vrf` is an Unreal Engine
local file replay with Valorant's own magic number and header on top.

```cpp
#include <limits>
#include <print>

#include "vrfparse/match.h"
#include "vrfparse/roster.h"
#include "vrfparse/timeline.h"

int main() {
    vrfparse::Match match;
    const vrfparse::Interval wholeReplay{0, std::numeric_limits<float>::infinity()};
    const std::expected<void, vrfparse::Error> read = match.read("replay.vrf", wholeReplay, {});
    if (!read) {
        std::println(stderr, "{}", read.error().message);
        return 1;
    }

    const vrfparse::Roster roster(match);
    for (const vrfparse::Player& player : roster.getPlayers())
        std::println("player {}", player.subject);

    const vrfparse::Timeline timeline(match);
    for (const vrfparse::TimelineRound& round : timeline.getRounds())
        std::println("round {} starts at {:.0f} s", round.number, round.start);
    std::println("Blue {} : {} Red", timeline.getWins(vrfparse::Side::Blue), timeline.getWins(vrfparse::Side::Red));
}
```

## What you get

- **`Match`** holds the replay header and every network event, in the same order the game plays them back: actor
  channels opening and closing, NetGUID exports, content blocks, and their replicated properties. Payloads stay
  serialized until you call `Event::readPayload`.
- **`Roster`** lists the players and teams.
- **`Timeline`** gives you the match itself: rounds, kills, spike plants and defuses, and where each character was
  and how much health and shield they had at any point in time.
- **`BitReader`** and `vrfparse/rep_properties.h` are there if you want to decode payloads yourself.

## Supported replays

vrfparse reads replays from `++Ares-Core+release-` 11.06 to 11.11, 12.00 to 12.11, 13.00 to 13.02, 13.04 to 13.06,
13.08, and `++Ares-Core+release-china-13.05`. Every release scrambles its payloads in its own way (see
`src/payload_transform.cpp`), so replays from other releases fail with `ErrorCode::Version`.

## Building

You need Clang. The build uses libc++, LLVM's runtime (compiler-rt and libunwind) and LLVM's linker. No part of GCC is
used. On Fedora:

```sh
sudo dnf install clang lld libcxx-devel libcxx-static libcxxabi-static compiler-rt llvm-libunwind-devel \
    llvm-libunwind-static cmake ninja-build
```

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C build
build/tools/replay_dump match path/to/replay.vrf
```

To use it from your own CMake project:

```cmake
add_subdirectory(external/vrfparse)
target_link_libraries(app PRIVATE vrfparse::vrfparse)
```
