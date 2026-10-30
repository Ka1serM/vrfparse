#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace replay {

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

// EChannelCloseReason.
enum class ChannelCloseReason : std::uint8_t { Destroyed, Dormancy, LevelUnloaded, Relevancy, TearOff };

// The spawn data of a dynamic actor (UPackageMapClient::SerializeNewActor). What the replay does not send keeps the
// default.
struct ActorSpawn {
    Vector location;
    Rotator rotation;
    Vector scale{1, 1, 1};
    Vector velocity;
};

// What the replay's packets carry, in the order that UDemoNetDriver plays them.
enum class EventKind : std::uint8_t {
    // An actor channel opens (UPackageMapClient::SerializeNewActor): channel, actor, classGuid (the archetype), outerGuid
    // (the level), path (the archetype's name), name (the actor's name, for a static actor), and spawn for a dynamic
    // actor.
    ActorOpen,
    // An actor channel closes: channel, actor, closeReason.
    ActorClose,
    // A NetGUID export (UPackageMapClient::InternalLoadObject): object, outerGuid, path (the object's name in its outer,
    // or a package path).
    NetGuid,
    // An export group (UPackageMapClient::ReceiveNetFieldExports): object (its path index), path, fieldNames by handle.
    // An empty name is not exported yet.
    ExportGroup,
    // A content block header (UActorChannel::ReadContentBlockHeader): channel, actor, object, classGuid (the class of a
    // new subobject), outerGuid, the block flags, and path: the object's class, or its export group when the block has
    // a RepLayout. Empty when the replay does not name the class.
    ContentBlock,
    // A RepLayout property of the last content block: handle, name, payload.
    Field,
    // The class net cache fields of the last content block (custom delta properties and RPCs), unframed: payload.
    ClassNetCache,
    // The end of a bunch of the channel (UActorChannel::ProcessBunch).
    BunchEnd,
};

// Times are replay seconds; actors and objects are NetGUIDs.
struct Event {
    EventKind kind = EventKind::BunchEnd;
    // ContentBlock: the object is the channel's actor.
    bool isActorBlock = false;
    // ContentBlock: the payload starts with the RepLayout properties.
    bool hasRepLayout = false;
    // ContentBlock: the subobject is destroyed, with deleteFlags (ESubObjectDeleteFlag).
    bool deleted = false;
    std::uint8_t deleteFlags = 0;
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
    // The first bit is the lowest bit of the first byte; the bits after payloadBits are zero.
    std::span<const std::uint8_t> payload;
    std::int64_t payloadBits = 0;
    const ActorSpawn* spawn = nullptr;
};

// The events of a replay and the storage that their views point into. Nothing that it gave out moves.
class EventLog {
public:
    EventLog() = default;
    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;

    // The time of the events that follow: the time of the demo frame.
    void setTime(const float frameTime) { time = frameTime; }
    void add(Event event) {
        event.time = time;
        events.push_back(event);
    }
    std::span<const Event> getEvents() const { return events; }

    std::string_view intern(std::string_view text);
    std::span<const std::string_view> addNames(std::vector<std::string_view> names);
    const ActorSpawn* addSpawn(const ActorSpawn& spawn) { return &spawns.emplace_back(spawn); }
    // A buffer for a payload of the bits, zeroed.
    std::span<std::uint8_t> allocatePayload(std::int64_t bits);

private:
    float time = 0;
    std::vector<Event> events;
    std::unordered_set<std::string> strings;
    std::deque<std::vector<std::string_view>> nameLists;
    std::deque<ActorSpawn> spawns;
    std::vector<std::unique_ptr<std::uint8_t[]>> blocks;
    std::size_t blockUsed = 0;
    std::size_t blockSize = 0;
};

}
