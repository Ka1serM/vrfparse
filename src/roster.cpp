#include "vrfparse/roster.h"

#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "vrfparse/bit_reader.h"

namespace vrfparse {

namespace {

// A field of an actor's own content block, by its export name.
struct ActorField {
    float time = 0;
    std::uint32_t actor = 0;
    const Event* event = nullptr;
};

}

Roster::Roster(const Match& match) {
    // The roster reads facts of the replicated state without the net driver: a pawn's PlayerState, a player state's
    // Subject and AssignedTeamState, and a team state's TeamComponent. A pawn is a character when its class replicates
    // ACharacter::ReplicatedMovementMode.
    std::unordered_map<std::uint32_t, bool> actorBlock;
    std::unordered_set<std::uint32_t> characters;
    std::unordered_map<std::uint32_t, std::string_view> netGuidPaths;
    std::vector<ActorField> playerStates, subjects, assignedTeams, teamComponents;
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
        if (name == "AssignedTeamState")
            assignedTeams.push_back({event.time, event.actor, &event});
        if (name == "TeamComponent")
            teamComponents.push_back({event.time, event.actor, &event});
    }

    // A pawn's PlayerState clears when it is unpossessed, so the last reference that is not 0 names the player.
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
    // The last assignment that is not 0 names the team.
    std::unordered_map<std::uint32_t, std::uint32_t> teamOf;
    for (const ActorField& field : assignedTeams) {
        BitReader payload = field.event->readPayload();
        const std::uint32_t teamState = payload.serializeIntPacked();
        if (teamState != 0 && !payload.isError())
            teamOf[field.actor] = teamState;
    }
    std::map<std::uint32_t, std::string_view> teamNames;
    for (const ActorField& field : teamComponents) {
        BitReader payload = field.event->readPayload();
        const std::uint32_t component = payload.serializeIntPacked();
        if (payload.isError())
            continue;
        const auto path = netGuidPaths.find(component);
        teamNames[field.actor] = path == netGuidPaths.end() ? std::string_view() : path->second;
    }
    for (const auto& [playerState, teamState] : teamOf)
        teamNames.try_emplace(teamState);
    for (const auto& [teamState, name] : teamNames)
        teams.push_back({.teamState = teamState, .name = name});

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
        players.push_back({.playerState = playerState, .subject = subject == subjectOf.end() ? std::string() : subject->second,
                           .characters = std::move(owned), .teamState = teamOf.contains(playerState) ? teamOf[playerState] : 0});
    }
}

}
