#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <new>
#include <type_traits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "vrfparse/BitReader.h"

namespace vrfparse {

struct Vector {
    float x = 0;
    float y = 0;
    float z = 0;
};

struct Rotator {
    float pitch = 0;
    float yaw = 0;
    float roll = 0;
};

enum class ChannelCloseReason : std::uint8_t { Destroyed, Dormancy, LevelUnloaded, Relevancy, TearOff };

struct ActorSpawn {
    Vector location;
    Rotator rotation;
    Vector scale{1, 1, 1};
    Vector velocity;
};

enum class EventKind : std::uint8_t {
    ActorOpen,
    ActorClose,
    NetGuid,
    ExportGroup,
    ContentBlock,
    Field,
    ClassNetCache,
    BunchEnd,
};

//! Times are replay seconds; actors and objects are NetGUIDs.
struct Event {
    EventKind kind = EventKind::BunchEnd;
    bool isActorBlock = false;
    bool hasRepLayout = false;
    bool deleted = false;
    std::uint8_t deleteFlags = 0;
    std::uint8_t payloadOffset = 0;
    ChannelCloseReason closeReason = ChannelCloseReason::Destroyed;
    float time = 0;
    std::uint32_t channel = 0;
    std::uint32_t actor = 0;
    std::uint32_t object = 0;
    std::uint32_t classGuid = 0;
    std::uint32_t outerGuid = 0;
    std::int32_t handle = -1;
    std::string_view path;
    std::string_view name;
    std::span<const std::string_view> fieldNames;
    std::span<const std::uint8_t> payload;
    std::int64_t payloadBits = 0;
    const ActorSpawn* spawn = nullptr;

    BitReader readPayload() const {
        BitReader bytes(payload, payloadOffset + payloadBits);
        bytes.readBits(payloadOffset);
        return bytes.readSubReader(payloadBits);
    }
};

//! Nothing that it gives out moves, except the events.
class EventLog {
public:
    EventLog() = default;
    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;

    void setTime(const float frameTime) { time = frameTime; }
    void add(Event event) {
        event.time = time;
        pending.back().push_back(event);
    }
    std::span<const Event> getEvents() const { return {events.get(), eventCount}; }
    void addPendingChunk(std::size_t expected);
    std::span<const std::vector<Event>> getPending() const { return pending; }
    void clearPending() { pending = {}; }
    std::span<Event> allocateEvents(std::size_t count);

    std::string_view intern(std::string_view text);
    std::span<const std::string_view> addNames(std::vector<std::string_view> names);
    const ActorSpawn* addSpawn(const ActorSpawn& spawn) { return &spawns.emplace_back(spawn); }
    std::span<std::uint8_t> allocatePayload(std::int64_t bits);

private:
    float time = 0;
    struct FreeEvents {
        void operator()(Event* events) const { ::operator delete(events); }
    };

    std::unique_ptr<Event, FreeEvents> events;
    std::size_t eventCount = 0;
    std::vector<std::vector<Event>> pending;
    std::unordered_set<std::string> strings;
    std::deque<std::vector<std::string_view>> nameLists;
    std::deque<ActorSpawn> spawns;
    std::vector<std::unique_ptr<std::uint8_t[]>> blocks;
    std::size_t blockUsed = 0;
    std::size_t blockSize = 0;
};

}
