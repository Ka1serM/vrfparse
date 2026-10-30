# vrfparse

A C++23 library that reads Valorant replays (`.vrf`): Unreal's local file replay format with Valorant's own file
magic and header block. It is the C++ port of
[ValorantReplayParser](https://github.com/Ka1serM/ValorantReplayParser), split out of Verylia.

```cpp
#include "vrfparse/match.h"
#include "vrfparse/timeline.h"

vrfparse::Match match;
if (const vrfparse::Result<> read = match.read("replay.vrf", std::stop_token()); !read) {
    std::println(stderr, "{} ({})", read.error().message, vrfparse::getErrorName(read.error().code));
    return 1;
}
for (const vrfparse::Event& event : match.getEvents())
    if (event.kind == vrfparse::EventKind::Field && event.name == "Subject") std::println("{}", event.readPayload().readString());
const vrfparse::Timeline timeline(match);
```

- `vrfparse::Match`: a whole replay's header and its events, in the order that `UDemoNetDriver` plays them: actor
  channels, NetGUID exports, export groups, content blocks, their RepLayout fields and class net cache fields. The
  payloads stay serialized; `Event::readPayload` reads them.
- `vrfparse::Timeline` and `vrfparse::Roster`: rounds, players, teams, kills, spike events, and each character's
  poses, health, and shield by time.
- `vrfparse::BitReader`: `FBitReader`, as a view of the bits. `vrfparse/rep_properties.h` reads property streams.

Playing the events into a UObject world (rep layouts, actor channels) is not part of this library; Verylia does that.

## Supported replays

Every release that ValorantReplayParser reads: `++Ares-Core+release-` 11.06 to 11.11, 12.00 to 12.11, 13.00 to 13.02,
13.04 to 13.06, 13.08, and `++Ares-Core+release-china-13.05`. Each release obfuscates the content block payloads with
its own transform (`src/payload_transform.cpp`). A replay of another release fails with `ErrorCode::Version`.

## Errors

The library throws no exceptions. As Unreal's archives, a reader sets an error that stays and the caller checks it at
the boundary; `Match::read` returns a `vrfparse::Result<>` (`std::expected<void, vrfparse::Error>`). What the reader
skips in a replay that it can read is in `Match::getProblems`.

## Speed

The file is read by chunk; the next replay data chunk decompresses while the frames of the chunk are read. The bunches
are framed in order, as the game does; the content blocks of a chunk are decoded in parallel, and the events are
placed into their final storage in parallel, with each payload copied once. The fields are views into their blocks.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C build
build/tools/replay_dump match path/to/replay.vrf
```

Dependencies are submodules: ooz (Oodle) and oneTBB. A parent project's `TBB::tbb` and CppUE5Parse's `ue5parse_ooz`
are used when they exist.

```cmake
add_subdirectory(external/vrfparse)
target_link_libraries(app PRIVATE vrfparse::vrfparse)
```

## License

MIT (see `LICENSE`). The Oodle decoder, ooz, is GPL-3.0: a binary that links it is under the GPL-3.0.
