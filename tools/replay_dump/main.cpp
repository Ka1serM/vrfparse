// Prints a replay's events or match facts in the C# parser's dump format, for parity runs: replay_dump count|events|match <file.vrf>.
// replay_dump summary <file.vrf> prints the match facts of the overview, read from the last checkpoint on.
#include <bit>
#include <cstdio>
#include <format>
#include <fstream>
#include <print>
#include <vector>

#include "vrfparse/match.h"
#include "vrfparse/timeline.h"

namespace {

std::string bits(const float value) { return std::format("{:08x}", std::bit_cast<std::uint32_t>(value)); }
std::string vector(const vrfparse::Vector& v) { return std::format("{},{},{}", bits(v.x), bits(v.y), bits(v.z)); }
// The payload's bytes, with zeros after its bits.
std::string hex(const vrfparse::Event& event) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>((event.payloadBits + 7) / 8));
    event.readPayload().readBits(bytes, event.payloadBits);
    std::string text;
    for (const std::uint8_t byte : bytes) std::format_to(std::back_inserter(text), "{:02x}", byte);
    return text;
}

template <class Range>
std::string join(const Range& values) {
    std::string text;
    std::string_view separator;
    for (const auto& value : values) {
        std::format_to(std::back_inserter(text), "{}{}", separator, value);
        separator = ",";
    }
    return text;
}

}

int main(const int argc, char** argv) {
    if (argc < 3) return 2;
    vrfparse::Match match;
    const bool summary = std::string_view(argv[1]) == "summary";
    if (const vrfparse::Result<> read = summary ? match.readFromLastCheckpoint(argv[2], std::stop_token()) : match.read(argv[2], std::stop_token()); !read) {
        std::println(stderr, "{} ({})", read.error().message, vrfparse::getErrorName(read.error().code));
        return 1;
    }
    {
        std::string out;
        if (std::string_view(argv[1]) == "count") out = std::format("{} events\n", match.getEvents().size());
        if (std::string_view(argv[1]) == "events") {
            for (const vrfparse::Event& e : match.getEvents()) {
                const std::string time = bits(e.time);
                switch (e.kind) {
                case vrfparse::EventKind::ActorOpen: {
                    const bool dynamic = e.spawn != nullptr;
                    out += std::format("open\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", time, e.channel, e.actor, e.classGuid, e.outerGuid,
                                       dynamic ? 105 : 0, e.path, e.name, dynamic ? vector(e.spawn->location) : "-",
                                       dynamic ? std::format("{},{},{}", bits(e.spawn->rotation.pitch), bits(e.spawn->rotation.yaw), bits(e.spawn->rotation.roll)) : "-",
                                       dynamic ? vector(e.spawn->scale) : "-", dynamic ? vector(e.spawn->velocity) : "-");
                    break;
                }
                case vrfparse::EventKind::ActorClose:
                    out += std::format("close\t{}\t{}\t{}\t{}\n", time, e.channel, e.actor, static_cast<int>(e.closeReason));
                    break;
                case vrfparse::EventKind::NetGuid:
                    out += std::format("guid\t{}\t{}\t{}\t{}\n", time, e.object, e.outerGuid, e.path);
                    break;
                case vrfparse::EventKind::ExportGroup:
                    out += std::format("group\t{}\t{}\t{}\t{}\t{}\n", time, e.object, e.path, e.fieldNames.size(), join(e.fieldNames));
                    break;
                case vrfparse::EventKind::ContentBlock: {
                    const unsigned flags = (e.isActorBlock ? 1u : 0u) | (e.hasRepLayout ? 2u : 0u) | (e.deleted ? 4u : 0u) | (unsigned{e.deleteFlags} << 8);
                    out += std::format("block\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", time, e.channel, e.actor, e.object, e.classGuid, e.outerGuid, flags, e.path);
                    break;
                }
                case vrfparse::EventKind::Field:
                    out += std::format("field\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", time, e.channel, e.actor, e.object, e.handle, e.name, e.payloadBits, hex(e));
                    break;
                case vrfparse::EventKind::ClassNetCache:
                    out += std::format("cache\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", time, e.channel, e.actor, e.object, e.path, e.hasRepLayout ? 2 : 0, e.payloadBits, hex(e));
                    break;
                case vrfparse::EventKind::BunchEnd:
                    out += std::format("end\t{}\t{}\t{}\n", time, e.channel, e.actor);
                    break;
                }
                if (out.size() > (1 << 20)) {
                    std::fwrite(out.data(), 1, out.size(), stdout);
                    out.clear();
                }
            }
            out += std::format("header\t{}\t{}\t{}\t{}\n", match.getBranch(), match.getChangelist(), static_cast<int>(match.getLength() * 1000.0f + 0.5f), join(match.getLevels()));
            for (const vrfparse::Marker& m : match.getMarkers())
                out += std::format("marker\t{}\t{}\t{}\t{}\t{}\n", m.id, m.group, m.metadata, bits(m.time), bits(m.endTime));
            out += std::format("problems\t{}\n", match.getProblems().size());
            for (const std::string& p : match.getProblems()) out += std::format("problem\t{}\n", p);
        }
        if (summary) {
            const vrfparse::Timeline timeline(match);
            out += std::format("gamestate\t{}\nplayers\t{}\nwins\t{}\t{}\n", timeline.getGameStateClass(), timeline.getPlayerCount(),
                               timeline.getWins(vrfparse::Side::Blue), timeline.getWins(vrfparse::Side::Red));
            out += std::format("problems\t{}\n", match.getProblems().size() + timeline.getProblems().size());
            for (const std::string& p : match.getProblems()) out += std::format("problem\t{}\n", p);
            for (const std::string& p : timeline.getProblems()) out += std::format("problem\t{}\n", p);
        }
        if (std::string_view(argv[1]) == "match") {
            const vrfparse::Timeline timeline(match);
            out += std::format("gamestate\t{}\nplayers\t{}\nwins\t{}\t{}\n", timeline.getGameStateClass(), timeline.getPlayerCount(),
                               timeline.getWins(vrfparse::Side::Blue), timeline.getWins(vrfparse::Side::Red));
            const auto optional = [](const std::optional<float>& value) { return value ? bits(*value) : std::string("-"); };
            for (const vrfparse::TimelineRound& r : timeline.getRounds())
                out += std::format("round\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", r.number, bits(r.start), optional(r.combatStart), optional(r.end), bits(r.close),
                                   r.winner ? (*r.winner == vrfparse::Side::Blue ? "Blue" : "Red") : "-", static_cast<int>(r.winnerRole),
                                   r.outcome ? static_cast<int>(*r.outcome) : -1);
            for (const vrfparse::Kill& k : timeline.getKills())
                out += std::format("kill\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", bits(k.time), k.killer, k.victim, k.weaponClass, k.headshot ? 1 : 0, k.wallbang ? 1 : 0, join(k.assisters));
            for (const vrfparse::Spike& p : timeline.getSpikes())
                out += std::format("spike\t{}\t{}\t{}\t{}\t{},{}\n", bits(p.time), static_cast<int>(p.action), p.character, p.site, bits(p.x), bits(p.y));
            out += std::format("problems\t{}\n", timeline.getProblems().size());
            for (const std::string& p : timeline.getProblems()) out += std::format("problem\t{}\n", p);
            // Queries: "pose|health|shield <character> <time bits>".
            std::ifstream queries(argc > 3 ? argv[3] : "");
            std::string kind;
            std::uint32_t character;
            std::string timeBits;
            while (queries >> kind >> character >> timeBits) {
                const float time = std::bit_cast<float>(static_cast<std::uint32_t>(std::stoul(timeBits, nullptr, 16)));
                if (kind == "pose") {
                    const auto pose = timeline.findPose(character, time);
                    out += std::format("pose\t{}\t{}\t{}\n", character, timeBits, pose ? std::format("{},{},{}", bits(pose->x), bits(pose->y), bits(pose->yaw)) : "-");
                } else {
                    out += std::format("{}\t{}\t{}\t{}\n", kind, character, timeBits, bits(kind == "health" ? timeline.getHealth(character, time) : timeline.getShield(character, time)));
                }
            }
        }
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    return 0;
}
