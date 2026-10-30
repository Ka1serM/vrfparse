# vrfparse

A C++23 library that reads Valorant replays (`.vrf`): Unreal's local file replay format with Valorant's own file
magic and header block. It is the C++ port of
[ValorantReplayParser](https://github.com/Ka1serM/ValorantReplayParser), split out of Verylia.

- `replay::LocalFileReplay`: the file's chunks (FLocalFileNetworkReplayStreamer), with Oodle decompression.
- `replay::DemoReader`: packets, bunches and actor channels, as UDemoNetDriver plays them, into an event log. The
  payloads stay serialized.
- `replay::Match`: a whole replay's header and events. `replay::Timeline` and `replay::Roster`: rounds, players,
  damage and kills.
- `net::BitReader`: FBitReader.

Playing the events into a UObject world (rep layouts, actor channels) is not part of this library; Verylia does that.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
build/replay_dump match path/to/replay.vrf
```

Dependencies are submodules: fmt and ooz (Oodle). A parent project's `fmt::fmt` target is used when it exists.

```cmake
add_subdirectory(external/vrfparse)
target_link_libraries(app PRIVATE vrfparse::vrfparse)
```

`replay::Match` takes a `std::stop_token`; with Emscripten's libc++ 17 that needs `-fexperimental-library`.

## License

MIT (see `LICENSE`). The Oodle decoder, ooz, is GPL-3.0: a binary that links it is under the GPL-3.0.
