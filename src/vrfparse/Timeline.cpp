#include "vrfparse/Timeline.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <map>
#include <unordered_set>
#include <utility>

#include "vrfparse/BitReader.h"
#include "vrfparse/ComponentDataStream.h"
#include "vrfparse/PackageMap.h"
#include "vrfparse/RepProperties.h"

namespace vrfparse {

namespace {

constexpr int inRoundPhase = 4;
constexpr int roundEndingPhase = 5;

//! A kill and the damage that killed its victim arrive in the same bunch, in either order.
constexpr float killingDamageWindow = 1.0f;

constexpr std::string_view setPhaseFunction = "/Script/ShooterGame.ShooterGameState:MulticastSetPhase";
constexpr std::string_view killedEnemyFunction = "/Script/ShooterGame.ShooterCharacter:MulticastNotifyKilledEnemy";
constexpr std::string_view damageBaseFunction = "/Script/ShooterGame.DamageableComponent:MulticastNotifyDamage_Base";
constexpr std::string_view damagePointFunction = "/Script/ShooterGame.DamageableComponent:MulticastNotifyDamage_Point";
constexpr std::string_view healFunction = "/Script/ShooterGame.DamageableComponent:MulticastNotifyHeal";
constexpr std::string_view overhealDecayFunction = "/Script/ShooterGame.DamageableComponent:MulticastNotifyOverhealDecay";
constexpr std::string_view sectionLifeChangeFunction = "/Script/ShooterGame.DamageableComponent:MulticastSectionLifeChange";
constexpr std::string_view bombPlantedFunction = "/Game/GameModes/Components/Comp_BombEvents.Comp_BombEvents_C:BombPlantedRPC";
constexpr std::string_view bombDefusedFunction = "/Game/GameModes/Components/Comp_BombEvents.Comp_BombEvents_C:BombDefusedRPC";
constexpr std::string_view setLifeFunction = "/Script/ShooterGame.DamageSectionComponent:MulticastNotifySetLife";
//! Bought armor is an item actor with this section, whose life is the shield of the character that owns the item.
constexpr std::string_view armorSection = "AttachedDamageSection";
constexpr std::string_view remoteCharacterUpdatesFunction =
    "/Script/ShooterGame.ReplayPlayerController:ReplaysClientReceiveRemoteCharacterUpdatesSingleArrayNoAutonomous";

//! The 13.08 constructors make these subobjects, so their content blocks name no class.
constexpr std::pair<std::string_view, std::string_view> defaultSubobjectClasses[] = {
    {"DamageHandlerComponent", "/Script/ShooterGame.DamageableComponent"},
    {"Comp_BombEvents", "/Game/GameModes/Components/Comp_BombEvents.Comp_BombEvents_C"},
    {"AttachedDamageSection", "/Game/Gear/BasicArmorAttachedDamageSection.BasicArmorAttachedDamageSection_C"},
};

struct RoundResult {
    std::optional<Side> winner;
    std::optional<TeamRole> winningRole;
    std::optional<RoundOutcome> outcome;
};

struct Phase {
    float time = 0;
    int phase = 0;
};

struct KillingDamage {
    float time = 0;
    std::uint32_t victim = 0;
    std::uint32_t equippable = 0;
    bool headshot = false;
    bool wallbang = false;
    std::vector<std::uint32_t> assisters;
};

struct Life {
    float time = 0;
    std::uint32_t character = 0;
    bool health = false;
    float value = 0;
    std::string_view armorClass;
    bool fresh = false;
};

struct Move {
    float time = 0;
    std::uint32_t character = 0;
    CharacterMove move;
};

struct LifeChange {
    std::uint32_t component = 0;
    std::optional<float> result;
};

std::optional<Side> toSide(const std::string_view team) {
    if (team == "Blue")
        return Side::Blue;
    if (team == "Red")
        return Side::Red;
    return std::nullopt;
}

class FactReader {
public:
    explicit FactReader(const Match& match);

    std::string_view gameStateClass;
    std::unordered_set<std::string> subjects;
    std::map<std::uint32_t, RoundResult> roundResults;
    std::vector<Phase> phases;
    std::vector<Kill> kills;
    std::vector<KillingDamage> killingDamages;
    std::vector<Life> lives;
    std::vector<Move> moves;
    std::vector<Spike> spikes;
    std::vector<std::string> problems;

private:
    std::span<const std::string_view> findNames(std::string_view path) const;
    void readField(const Event& field);
    void readClassNetCache(const Event& cache);
    void readRpc(const Event& cache, std::string_view classPath, std::string_view function, BitReader& payload);
    void readDamage(const Event& cache, BitReader& payload, std::string_view function);
    void readLifeChanges(const Event& cache, BitReader& payload, std::span<const std::string_view> names);
    void readRemoteCharacterUpdates(const Event& cache, BitReader& payload);
    void addLife(float time, std::uint32_t character, const LifeChange& change);
    void report(const Event& event, std::string_view what);

    std::unordered_map<std::string_view, std::span<const std::string_view>> groups;
    const Match& match;
    std::unordered_map<std::uint32_t, std::string_view> names;
    std::unordered_map<std::uint32_t, std::uint32_t> outers;
    std::unordered_map<std::uint32_t, std::uint32_t> owners;
    //! By armor item actor.
    std::unordered_set<std::uint32_t> armors;
    std::unordered_map<std::uint32_t, float> armorLives;
    const Event* block = nullptr;
    std::string cachePath;
};

FactReader::FactReader(const Match& match) : match(match) {
    for (const Event& event : match.getEvents()) {
        switch (event.kind) {
        case EventKind::NetGuid:
            names[event.object] = event.path;
            outers[event.object] = event.outerGuid;
            if (event.path == armorSection)
                armors.insert(event.outerGuid);
            break;
        case EventKind::ExportGroup:
            groups[event.path] = event.fieldNames;
            break;
        case EventKind::ContentBlock:
            block = &event;
            break;
        case EventKind::Field:
            if (block && block->isActorBlock)
                readField(event);
            break;
        case EventKind::ClassNetCache:
            readClassNetCache(event);
            break;
        case EventKind::ActorOpen:
        case EventKind::ActorClose:
        case EventKind::BunchEnd:
            break;
        }
    }
}

std::span<const std::string_view> FactReader::findNames(const std::string_view path) const {
    const auto found = groups.find(path);
    return found == groups.end() ? std::span<const std::string_view>() : found->second;
}

void FactReader::readField(const Event& field) {
    BitReader payload = field.readPayload();
    if (field.name == "Owner") {
        const std::uint32_t owner = readObject(payload);
        if (payload.isError())
            return;
        const std::uint32_t before = std::exchange(owners[field.actor], owner);
        if (!armors.contains(field.actor) || owner == before)
            return;
        //! Sold or dropped armor no longer shields its last owner.
        if (before != 0)
            lives.push_back({.time = field.time, .character = before, .health = false, .value = 0});
        if (owner == 0)
            return;
        const auto life = armorLives.find(field.actor);
        lives.push_back({.time = field.time,
                         .character = owner,
                         .health = false,
                         .value = life == armorLives.end() ? 0 : life->second,
                         .armorClass = match.getClassPath(field.actor),
                         .fresh = life == armorLives.end()});
        return;
    }
    if (field.name == "Subject") {
        std::string subject = payload.readString();
        if (!subject.empty() && !payload.isError())
            subjects.insert(std::move(subject));
        return;
    }
    if (field.name != "RoundResults")
        return;
    const std::span<const std::string_view> fields = findNames(field.path);
    std::string text;
    const bool read = readArray(payload, [&](const std::uint32_t round, BitReader& stream) {
        RoundResult& result = roundResults[round];
        return readProperties(stream, fields, [&](const std::string_view name, BitReader& value) {
            if (name == "WinningTeam")
                result.winner = toSide(PackageMap::readName(value, text));
            if (name == "WinningTeamRole") {
                const std::uint32_t role = readEnum(value);
                if (role <= static_cast<std::uint32_t>(TeamRole::Any))
                    result.winningRole = static_cast<TeamRole>(role);
            }
            if (name == "RoundResult") {
                const std::uint32_t outcome = readEnum(value);
                if (outcome <= static_cast<std::uint32_t>(RoundOutcome::Surrendered))
                    result.outcome = static_cast<RoundOutcome>(outcome);
            }
        });
    });
    if (!read || payload.isError())
        report(field, "RoundResults");
}

void FactReader::readClassNetCache(const Event& cache) {
    std::string_view classPath = cache.path;
    if (classPath.empty() && cache.object != cache.actor) {
        const auto name = names.find(cache.object);
        if (name == names.end())
            return;
        const auto found = std::ranges::find(defaultSubobjectClasses, name->second, &std::pair<std::string_view, std::string_view>::first);
        if (found == std::end(defaultSubobjectClasses))
            return;
        classPath = found->second;
    }
    if (classPath.empty())
        return;
    cachePath.assign(classPath).append("_ClassNetCache");
    const std::span<const std::string_view> functions = findNames(cachePath);
    if (functions.empty())
        return;
    BitReader stream = cache.readPayload();
    while (stream.getBitsLeft() > 0) {
        const std::uint32_t handle = stream.serializeInt(static_cast<std::uint32_t>(std::max<std::size_t>(functions.size(), 2)));
        const std::uint32_t bits = stream.serializeIntPacked();
        if (stream.isError() || handle >= functions.size() || bits > stream.getBitsLeft())
            return report(cache, cachePath);
        BitReader payload = stream.readSubReader(bits);
        readRpc(cache, classPath, functions[handle], payload);
    }
}

void FactReader::readRpc(const Event& cache, const std::string_view classPath, const std::string_view function, BitReader& payload) {
    const float time = cache.time;
    bool read = true;
    if (function == "MulticastSetPhase") {
        int phase = -1;
        read = readParameters(payload, findNames(setPhaseFunction), [&](const std::string_view name, BitReader& value) {
            if (name == "NewPhase")
                phase = static_cast<int>(readEnum(value));
        });
        if (gameStateClass.empty())
            gameStateClass = classPath;
        if (read && phase >= 0)
            phases.push_back({.time = time, .phase = phase});
    } else if (function == "MulticastNotifyKilledEnemy") {
        Kill kill{.time = time};
        read = readParameters(payload, findNames(killedEnemyFunction), [&](const std::string_view name, BitReader& value) {
            if (name == "KillerCharacter")
                kill.killer = readObject(value);
            if (name == "KilledCharacter")
                kill.victim = readObject(value);
        });
        if (read)
            kills.push_back(std::move(kill));
    } else if (function == "MulticastNotifyDamage_Base" || function == "MulticastNotifyDamage_Point") {
        readDamage(cache, payload, function == "MulticastNotifyDamage_Base" ? damageBaseFunction : damagePointFunction);
    } else if (function == "MulticastNotifyHeal" || function == "MulticastNotifyOverhealDecay") {
        const std::span<const std::string_view> names = findNames(function == "MulticastNotifyHeal" ? healFunction : overhealDecayFunction);
        read = readParameters(payload, names, [&](const std::string_view name, BitReader& value) {
            if (name == "LifeChangeBySection")
                readLifeChanges(cache, value, names);
        });
    } else if (function == "MulticastSectionLifeChange") {
        const std::span<const std::string_view> names = findNames(sectionLifeChangeFunction);
        read = readParameters(payload, names, [&](const std::string_view name, BitReader& value) {
            if (name == "LifeChangeEvents")
                readLifeChanges(cache, value, names);
        });
    } else if (function == "MulticastNotifySetLife") {
        const auto section = names.find(cache.object);
        const auto owner = owners.find(cache.actor);
        if (section == names.end() || section->second != armorSection || owner == owners.end())
            return;
        //! A parameter at its default value is not sent: no NewLife is 0.
        float life = 0;
        read = readParameters(payload, findNames(setLifeFunction), [&](const std::string_view name, BitReader& value) {
            if (name == "NewLife")
                life = value.readFloat();
        });
        if (read) {
            armorLives[cache.actor] = life;
            lives.push_back({.time = time, .character = owner->second, .health = false, .value = life, .armorClass = match.getClassPath(cache.actor)});
        }
    } else if (function == "BombPlantedRPC") {
        //! A parameter at its default value is not sent: no PlantSite is site A.
        Spike spike{.time = time, .action = SpikeAction::Plant, .site = 0};
        read = readParameters(payload, findNames(bombPlantedFunction), [&](const std::string_view name, BitReader& value) {
            if (name == "BombPlanter")
                spike.character = readObject(value);
            if (name == "PlantSite")
                spike.site = static_cast<int>(readEnum(value));
            if (name != "PlantLocation")
                return;
            spike.x = static_cast<float>(value.readDouble());
            spike.y = static_cast<float>(value.readDouble());
        });
        if (read)
            spikes.push_back(spike);
    } else if (function == "BombDefusedRPC") {
        Spike spike{.time = time, .action = SpikeAction::Defuse};
        read = readParameters(payload, findNames(bombDefusedFunction), [&](const std::string_view name, BitReader& value) {
            if (name == "DefusingCharacter")
                spike.character = readObject(value);
        });
        if (read)
            spikes.push_back(spike);
    } else if (function == "ReplaysClientReceiveRemoteCharacterUpdatesSingleArrayNoAutonomous") {
        readRemoteCharacterUpdates(cache, payload);
    }
    if (!read || payload.isError())
        report(cache, function);
}

//! MulticastNotifyDamage_Base and _Point run on the victim's DamageableComponent.
void FactReader::readDamage(const Event& cache, BitReader& payload, const std::string_view function) {
    const std::span<const std::string_view> names = findNames(function);
    KillingDamage damage{.time = cache.time, .victim = cache.actor};
    bool killed = false;
    bool hasLifeChanges = false;
    LifeChange change;
    std::uint32_t assister = 0;
    std::vector<std::uint32_t> assists;
    const bool read = readParameters(payload, names, [&](const std::string_view name, BitReader& value) {
        if (name == "bDamageKilledTarget")
            killed = value.readBit();
        if (name == "EquippableUsed")
            damage.equippable = readObject(value);
        if (name == "RegionalDamage")
            damage.headshot = readEnum(value) == 1;
        if (name == "bIsWallPenetration")
            damage.wallbang = value.readBit();
        if (name == "Assister")
            assister = readObject(value);
        if (name == "ChangedComponent")
            change.component = readObject(value);
        if (name == "LifeResult")
            change.result = value.readFloat();
        if (name == "LifeChangeEvents") {
            hasLifeChanges = true;
            readLifeChanges(cache, value, names);
        }
        if (name == "AssistsList")
            readArray(value, [&](std::uint32_t, BitReader& stream) {
                return readProperties(stream, names, [&](const std::string_view member, BitReader& memberValue) {
                    if (member == "Assister")
                        assists.push_back(readObject(memberValue));
                });
            });
    });
    if (!read)
        return;
    if (!hasLifeChanges)
        addLife(cache.time, cache.actor, change);
    if (!killed)
        return;
    if (assister != 0)
        damage.assisters.push_back(assister);
    for (const std::uint32_t assist : assists)
        if (assist != 0 && std::ranges::find(damage.assisters, assist) == std::ranges::end(damage.assisters))
            damage.assisters.push_back(assist);
    killingDamages.push_back(std::move(damage));
}

void FactReader::readLifeChanges(const Event& cache, BitReader& payload, const std::span<const std::string_view> names) {
    readArray(payload, [&](std::uint32_t, BitReader& stream) {
        LifeChange change;
        const bool read = readProperties(stream, names, [&](const std::string_view name, BitReader& value) {
            if (name == "ChangedComponent")
                change.component = readObject(value);
            if (name == "LifeResult")
                change.result = value.readFloat();
        });
        addLife(cache.time, cache.actor, change);
        return read;
    });
}

void FactReader::readRemoteCharacterUpdates(const Event& cache, BitReader& payload) {
    const std::span<const std::string_view> names = findNames(remoteCharacterUpdatesFunction);
    readParameters(payload, names, [&](const std::string_view name, BitReader& value) {
        if (name != "RemoteCharacterUpdates")
            return;
        readArray(value, [&](std::uint32_t, BitReader& stream) {
            std::optional<std::uint32_t> character;
            std::optional<CharacterMove> move;
            const bool read = readProperties(stream, names, [&](const std::string_view member, BitReader& memberValue) {
                if (member == "ShooterCharacterNetGuidValue")
                    character = memberValue.readUInt32();
                if (member == "ComponentDataStream")
                    move = readLatestMove(memberValue);
            });
            if (character && move)
                moves.push_back({.time = cache.time, .character = *character, .move = *move});
            return read;
        });
    });
}

void FactReader::addLife(const float time, const std::uint32_t character, const LifeChange& change) {
    if (!change.result)
        return;
    const auto name = names.find(change.component);
    if (name == names.end())
        return;
    if (name->second != "HealthDamageSection" && name->second != "ShieldDamageSection" && name->second != armorSection)
        return;
    std::string_view armorClass;
    if (name->second == armorSection)
        if (const auto armor = outers.find(change.component); armor != outers.end()) {
            armorLives[armor->second] = *change.result;
            armorClass = match.getClassPath(armor->second);
        }
    lives.push_back({.time = time, .character = character, .health = name->second == "HealthDamageSection", .value = *change.result, .armorClass = armorClass});
}

void FactReader::report(const Event& event, const std::string_view what) {
    problems.push_back(std::format("replay.rpc: {} of actor {} at {:.3f} s does not decode.", what, event.actor, event.time));
}

float valueAt(const std::vector<float>& times, const std::vector<float>& values, const float time) {
    const auto after = std::ranges::upper_bound(times, time);
    if (after == times.begin())
        return 0;
    return values[static_cast<std::size_t>(std::distance(times.begin(), after)) - 1];
}

}

Timeline::Timeline(const Match& match) {
    FactReader facts(match);
    gameStateClass = facts.gameStateClass;
    playerCount = facts.subjects.size();
    problems = std::move(facts.problems);

    //! The game sets the RoundStarting phase when it adds the round's marker.
    for (const Round& round : match.getRounds())
        rounds.push_back({.number = round.number, .start = round.time.start, .close = round.time.end});
    for (const Phase& phase : facts.phases) {
        const auto after = std::ranges::upper_bound(rounds, phase.time, {}, &TimelineRound::start);
        if (after == rounds.begin())
            continue;
        TimelineRound& round = *std::prev(after);
        if (phase.phase == inRoundPhase)
            round.combatStart = phase.time;
        if (phase.phase == roundEndingPhase)
            round.end = phase.time;
    }

    for (const auto& [number, result] : facts.roundResults) {
        const std::optional<Side> winner = result.winner;
        if (winner == Side::Blue)
            ++blueWins;
        if (winner == Side::Red)
            ++redWins;
        if (number >= rounds.size())
            continue;
        TimelineRound& round = rounds[number];
        round.winner = winner;
        round.winnerRole = result.winningRole.value_or(TeamRole::None);
        round.outcome = result.outcome;
    }

    kills = std::move(facts.kills);
    for (Kill& kill : kills) {
        const auto distance = [&](const KillingDamage& damage) { return std::abs(damage.time - kill.time); };
        const KillingDamage* closest = nullptr;
        for (const KillingDamage& damage : facts.killingDamages)
            if (damage.victim == kill.victim && distance(damage) <= killingDamageWindow && (!closest || distance(damage) < distance(*closest)))
                closest = &damage;
        if (!closest)
            continue;
        if (closest->equippable != 0)
            kill.weaponClass = match.getClassPath(closest->equippable);
        kill.headshot = closest->headshot;
        kill.wallbang = closest->wallbang;
        kill.assisters = closest->assisters;
    }
    spikes = std::move(facts.spikes);

    //! An RPC's time is its frame's, and the frames are in time order.
    for (const Move& move : facts.moves) {
        Track& track = tracks[move.character];
        track.moveTimes.push_back(move.time);
        track.poses.push_back({.time = move.time, .x = move.move.location.x, .y = move.move.location.y, .yaw = move.move.yaw});
    }
    for (const Life& life : facts.lives) {
        Track& track = tracks[life.character];
        if (life.health) {
            track.healthTimes.push_back(life.time);
            track.health.push_back(life.value);
            continue;
        }
        track.shieldTimes.push_back(life.time);
        track.shield.push_back({.life = life.value, .armorClass = life.armorClass, .fresh = life.fresh});
        if (life.fresh && !std::ranges::contains(armorClasses, life.armorClass))
            armorClasses.push_back(life.armorClass);
    }
}

const Timeline::Track* Timeline::findTrack(const std::uint32_t character) const {
    const auto found = tracks.find(character);
    return found == tracks.end() ? nullptr : &found->second;
}

std::optional<Pose> Timeline::findPose(const std::uint32_t character, const float time) const {
    const Track* track = findTrack(character);
    if (!track)
        return std::nullopt;
    const auto after = std::ranges::upper_bound(track->moveTimes, time);
    if (after == track->moveTimes.begin())
        return std::nullopt;
    return track->poses[static_cast<std::size_t>(std::distance(track->moveTimes.begin(), after)) - 1];
}

float Timeline::getHealth(const std::uint32_t character, const float time) const {
    const Track* track = findTrack(character);
    return track ? valueAt(track->healthTimes, track->health, time) : 0;
}

Shield Timeline::getShield(const std::uint32_t character, const float time) const {
    const Track* track = findTrack(character);
    if (!track)
        return {};
    const auto after = std::ranges::upper_bound(track->shieldTimes, time);
    if (after == track->shieldTimes.begin())
        return {};
    return track->shield[static_cast<std::size_t>(std::distance(track->shieldTimes.begin(), after)) - 1];
}

}
