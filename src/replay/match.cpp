#include "replay/match.h"

#include "replay/demo_reader.h"
#include "replay/error.h"

namespace replay {

Match::Match(const std::filesystem::path& file, const std::stop_token stop) {
    const LocalFileReplay replay(file);
    const NetworkDemoHeader header(replay.getHeader());
    branch = header.branch;
    changelist = header.changelist;
    length = static_cast<float>(replay.getLengthMs()) / 1000.0f;
    levels = header.levels;
    markers.assign(replay.getMarkers().begin(), replay.getMarkers().end());
    DemoReader reader(header, log, problems);
    for (std::size_t chunk = 0; chunk < replay.getDataChunkCount(); ++chunk) {
        if (stop.stop_requested()) throw Error("cancelled", "The replay read was cancelled.");
        reader.readFrames(replay.readDataChunk(chunk));
    }
    reader.finish();
    for (const Event& event : getEvents())
        if (event.kind == EventKind::ContentBlock && event.isActorBlock && !event.path.empty()) classPaths.try_emplace(event.actor, event.path);
}

std::string_view Match::getClassPath(const std::uint32_t actor) const {
    const auto found = classPaths.find(actor);
    return found == classPaths.end() ? std::string_view() : found->second;
}

}
