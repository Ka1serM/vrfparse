#include "vrfparse/Roster.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "vrfparse/BitReader.h"

namespace vrfparse {

namespace {

struct ActorField {
    float time = 0;
    std::uint32_t actor = 0;
    const Event* event = nullptr;
};

std::optional<Side> sideOf(const std::string_view teamComponent) {
    if (teamComponent == "BlueTeam")
        return Side::Blue;
    if (teamComponent == "RedTeam")
        return Side::Red;
    return std::nullopt;
}

template <class Change>
const Change* findChange(const std::vector<Change>& changes, const float time) {
    const auto after = std::ranges::upper_bound(changes, time, {}, &Change::time);
    return after == changes.begin() ? nullptr : &*std::prev(after);
}

}

std::uint32_t Player::findSpawnedCharacter(const float time) const {
    const CharacterChange* change = findChange(spawnedCharacters, time);
    return change == nullptr ? 0 : change->character;
}

std::optional<Side> Player::findSide(const float time) const {
    const TeamChange* change = findChange(teams, time);
    return change == nullptr ? std::nullopt : change->side;
}

Roster::Roster(const Match& match) {
    std::unordered_map<std::uint32_t, bool> actorBlock;
    std::unordered_set<std::uint32_t> characters;
    std::unordered_map<std::uint32_t, std::string_view> netGuidPaths;
    std::vector<ActorField> playerStates, subjects, spawnedCharacters, assignedTeams, teamComponents;
    for (const Event& event : match.getEvents()) {
        if (event.kind == EventKind::NetGuid)
            netGuidPaths.try_emplace(event.object, event.path);
        if (event.kind == EventKind::ContentBlock)
            actorBlock[event.channel] = event.isActorBlock;
        if (event.kind != EventKind::Field || !actorBlock[event.channel])
            continue;
        const std::string_view name = event.name;
        if (name == "ReplicatedMovementMode")
            characters.insert(event.actor);
        if (name == "PlayerState")
            playerStates.push_back({event.time, event.actor, &event});
        if (name == "Subject")
            subjects.push_back({event.time, event.actor, &event});
        if (name == "SpawnedCharacter")
            spawnedCharacters.push_back({event.time, event.actor, &event});
        if (name == "AssignedTeamState")
            assignedTeams.push_back({event.time, event.actor, &event});
        if (name == "TeamComponent")
            teamComponents.push_back({event.time, event.actor, &event});
    }

    //! A pawn's PlayerState clears when it is unpossessed.
    std::map<std::uint32_t, std::uint32_t> playerOfCharacter;
    for (const ActorField& field : playerStates) {
        if (!characters.contains(field.actor))
            continue;
        BitReader payload = field.event->readPayload();
        const std::uint32_t playerState = payload.serializeIntPacked();
        if (playerState != 0 && !payload.isError())
            playerOfCharacter[field.actor] = playerState;
    }
    std::unordered_map<std::uint32_t, std::string> subjectOf;
    for (const ActorField& field : subjects) {
        BitReader payload = field.event->readPayload();
        std::string subject = payload.readString();
        if (!payload.isError())
            subjectOf[field.actor] = std::move(subject);
    }
    std::unordered_map<std::uint32_t, std::vector<CharacterChange>> spawnedOf;
    for (const ActorField& field : spawnedCharacters) {
        BitReader payload = field.event->readPayload();
        const std::uint32_t character = payload.serializeIntPacked();
        if (!payload.isError())
            spawnedOf[field.actor].push_back({.time = field.time, .character = character});
    }
    std::unordered_map<std::uint32_t, std::optional<Side>> sideOfTeam;
    for (const ActorField& field : teamComponents) {
        BitReader payload = field.event->readPayload();
        const std::uint32_t component = payload.serializeIntPacked();
        if (payload.isError())
            continue;
        const auto path = netGuidPaths.find(component);
        sideOfTeam[field.actor] = path == netGuidPaths.end() ? std::nullopt : sideOf(path->second);
    }
    std::unordered_map<std::uint32_t, std::vector<TeamChange>> teamsOf;
    for (const ActorField& field : assignedTeams) {
        BitReader payload = field.event->readPayload();
        const std::uint32_t teamState = payload.serializeIntPacked();
        if (payload.isError())
            continue;
        const auto side = sideOfTeam.find(teamState);
        teamsOf[field.actor].push_back({.time = field.time, .side = side == sideOfTeam.end() ? std::nullopt : side->second});
    }

    std::unordered_map<std::uint32_t, float> firstSeen;
    for (const Event& event : match.getEvents())
        if (event.kind == EventKind::ActorOpen)
            firstSeen.try_emplace(event.actor, event.time);
    std::map<std::uint32_t, std::vector<std::uint32_t>> charactersByPlayer;
    for (const auto& [character, playerState] : playerOfCharacter)
        charactersByPlayer[playerState].push_back(character);
    for (auto& [playerState, owned] : charactersByPlayer) {
        std::ranges::stable_sort(owned, {}, [&](const std::uint32_t character) {
            const auto found = firstSeen.find(character);
            return found == firstSeen.end() ? std::numeric_limits<float>::infinity() : found->second;
        });
        const auto subject = subjectOf.find(playerState);
        players.push_back({.playerState = playerState,
                           .subject = subject == subjectOf.end() ? std::string() : subject->second,
                           .characters = std::move(owned),
                           .spawnedCharacters = std::move(spawnedOf[playerState]),
                           .teams = std::move(teamsOf[playerState])});
    }
}

}
