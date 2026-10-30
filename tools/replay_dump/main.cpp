// Prints a replay's events or match facts in the C# parser's dump format, for parity runs: replay_dump count|events|match <file.vrf>.
#include <cstdio>
#include <bit>
#include <fmt/format.h>

#include "replay/error.h"
#include "replay/match.h"
#include "replay/timeline.h"
#include <fstream>
#include <sstream>

namespace {

std::string bits(const float value) { return fmt::format("{:08x}", std::bit_cast<std::uint32_t>(value)); }
std::string vector(const replay::Vector& v) { return fmt::format("{},{},{}", bits(v.x), bits(v.y), bits(v.z)); }
std::string hex(const std::span<const std::uint8_t> payload) {
    std::string text;
    for (const std::uint8_t byte : payload) text += fmt::format("{:02x}", byte);
    return text;
}

}

int main(const int argc, char** argv) {
    if (argc < 3) return 2;
    try {
        const replay::Match match(argv[2], std::stop_token());
        std::string out;
        if (std::string_view(argv[1]) == "count") out = fmt::format("{} events\n", match.getEvents().size());
        if (std::string_view(argv[1]) == "events") {
            for (const replay::Event& e : match.getEvents()) {
                const std::string time = bits(e.time);
                switch (e.kind) {
                case replay::EventKind::ActorOpen: {
                    const bool dynamic = e.spawn != nullptr;
                    out += fmt::format("open\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", time, e.channel, e.actor, e.classGuid, e.outerGuid,
                                       dynamic ? 105 : 0, e.path, e.name, dynamic ? vector(e.spawn->location) : "-",
                                       dynamic ? fmt::format("{},{},{}", bits(e.spawn->rotation.pitch), bits(e.spawn->rotation.yaw), bits(e.spawn->rotation.roll)) : "-",
                                       dynamic ? vector(e.spawn->scale) : "-", dynamic ? vector(e.spawn->velocity) : "-");
                    break;
                }
                case replay::EventKind::ActorClose:
                    out += fmt::format("close\t{}\t{}\t{}\t{}\n", time, e.channel, e.actor, static_cast<int>(e.closeReason));
                    break;
                case replay::EventKind::NetGuid:
                    out += fmt::format("guid\t{}\t{}\t{}\t{}\n", time, e.object, e.outerGuid, e.path);
                    break;
                case replay::EventKind::ExportGroup:
                    out += fmt::format("group\t{}\t{}\t{}\t{}\t{}\n", time, e.object, e.path, e.fieldNames.size(), fmt::join(e.fieldNames, ","));
                    break;
                case replay::EventKind::ContentBlock: {
                    const unsigned flags = (e.isActorBlock ? 1u : 0u) | (e.hasRepLayout ? 2u : 0u) | (e.deleted ? 4u : 0u) | (unsigned{e.deleteFlags} << 8);
                    out += fmt::format("block\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", time, e.channel, e.actor, e.object, e.classGuid, e.outerGuid, flags, e.path);
                    break;
                }
                case replay::EventKind::Field:
                    out += fmt::format("field\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", time, e.channel, e.actor, e.object, e.handle, e.name, e.payloadBits, hex(e.payload));
                    break;
                case replay::EventKind::ClassNetCache:
                    out += fmt::format("cache\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", time, e.channel, e.actor, e.object, e.path, e.hasRepLayout ? 2 : 0, e.payloadBits, hex(e.payload));
                    break;
                case replay::EventKind::BunchEnd:
                    out += fmt::format("end\t{}\t{}\t{}\n", time, e.channel, e.actor);
                    break;
                }
                if (out.size() > (1 << 20)) {
                    std::fwrite(out.data(), 1, out.size(), stdout);
                    out.clear();
                }
            }
            out += fmt::format("header\t{}\t{}\t{}\t{}\n", match.getBranch(), match.getChangelist(), static_cast<int>(match.getLength() * 1000.0f + 0.5f), fmt::join(match.getLevels(), ","));
            for (const replay::Marker& m : match.getMarkers())
                out += fmt::format("marker\t{}\t{}\t{}\t{}\t{}\n", m.id, m.group, m.metadata, bits(m.time), bits(m.endTime));
            out += fmt::format("problems\t{}\n", match.getProblems().size());
            for (const std::string& p : match.getProblems()) out += fmt::format("problem\t{}\n", p);
        }
        if (std::string_view(argv[1]) == "match") {
            const replay::Timeline timeline(match);
            out += fmt::format("gamestate\t{}\nplayers\t{}\nwins\t{}\t{}\n", timeline.getGameStateClass(), timeline.getPlayerCount(),
                               timeline.getWins(replay::Side::Blue), timeline.getWins(replay::Side::Red));
            const auto optional = [](const std::optional<float>& value) { return value ? bits(*value) : std::string("-"); };
            for (const replay::TimelineRound& r : timeline.getRounds())
                out += fmt::format("round\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", r.number, bits(r.start), optional(r.combatStart), optional(r.end), bits(r.close),
                                   r.winner ? (*r.winner == replay::Side::Blue ? "Blue" : "Red") : "-", static_cast<int>(r.winnerRole),
                                   r.outcome ? static_cast<int>(*r.outcome) : -1);
            for (const replay::Kill& k : timeline.getKills())
                out += fmt::format("kill\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", bits(k.time), k.killer, k.victim, k.weaponClass, k.headshot ? 1 : 0, k.wallbang ? 1 : 0, fmt::join(k.assisters, ","));
            for (const replay::Spike& p : timeline.getSpikes())
                out += fmt::format("spike\t{}\t{}\t{}\t{}\t{},{}\n", bits(p.time), static_cast<int>(p.action), p.character, p.site, bits(p.x), bits(p.y));
            out += fmt::format("problems\t{}\n", timeline.getProblems().size());
            for (const std::string& p : timeline.getProblems()) out += fmt::format("problem\t{}\n", p);
            // Queries: "pose|health|shield <character> <time bits>".
            std::ifstream queries(argv[3]);
            std::string kind;
            std::uint32_t character;
            std::string timeBits;
            while (queries >> kind >> character >> timeBits) {
                const float time = std::bit_cast<float>(static_cast<std::uint32_t>(std::stoul(timeBits, nullptr, 16)));
                if (kind == "pose") {
                    const auto pose = timeline.findPose(character, time);
                    out += fmt::format("pose\t{}\t{}\t{}\n", character, timeBits, pose ? fmt::format("{},{},{}", bits(pose->x), bits(pose->y), bits(pose->yaw)) : "-");
                } else {
                    out += fmt::format("{}\t{}\t{}\t{}\n", kind, character, timeBits, bits(kind == "health" ? timeline.getHealth(character, time) : timeline.getShield(character, time)));
                }
            }
        }
        std::fwrite(out.data(), 1, out.size(), stdout);
    } catch (const replay::Error& error) {
        fmt::println(stderr, "{} ({})", error.what(), error.getCode());
        return 1;
    }
    return 0;
}
