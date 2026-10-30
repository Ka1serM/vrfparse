#include "replay/package_map.h"

#include <fmt/format.h>

#include <algorithm>

#include "net/bit_reader.h"
#include "net/unreal_names.h"
#include "replay/byte_reader.h"

namespace replay {

namespace {

// UPackageMapClient::InternalLoadObject stops at this depth of outers.
constexpr int maxOuterDepth = 16;
constexpr std::string_view defaultPrefix = "Default__";

// EGuidPackageMapExportFlags.
constexpr std::uint8_t hasPathFlag = 1;
constexpr std::uint8_t hasNetworkChecksumFlag = 4;

bool isDynamic(const std::uint32_t guid) { return guid > 0 && (guid & 1) == 0; }

std::string hardcodedName(const std::uint32_t index) {
    const std::optional<std::string_view> name = net::findHardcodedName(index);
    if (!name) throw Error("replay.parse", fmt::format("The replay names the hardcoded name {}, which Unreal does not have.", index));
    return std::string(*name);
}

// FName's text: the number is the suffix + 1, and 0 has none.
std::string nameText(std::string text, const std::int32_t number) {
    if (number > 0) text += fmt::format("_{}", number - 1);
    return text;
}

// UPackageMap::StaticSerializeName of a byte archive: bHardcoded takes a byte.
std::string readExportName(ByteReader& reader) {
    if (reader.readUInt8() != 0) return hardcodedName(reader.readIntPacked());
    std::string text = reader.readString();
    return nameText(std::move(text), reader.readInt32());
}

}

std::string PackageMap::readName(net::BitReader& reader) {
    if (reader.readBit()) return hardcodedName(reader.serializeIntPacked());
    std::string text = reader.readString();
    return nameText(std::move(text), reader.readInt32());
}

std::uint32_t PackageMap::loadObject(net::BitReader& reader, const bool exporting) { return loadObject(reader, exporting, 0); }

std::uint32_t PackageMap::loadObject(net::BitReader& reader, const bool exporting, const int depth) {
    if (depth >= maxOuterDepth) throw Error("replay.parse", fmt::format("A NetGUID export has more than {} outers.", maxOuterDepth));
    const std::uint32_t guid = reader.serializeIntPacked();
    if (guid == 0 || reader.isError()) return guid;
    // FNetworkGUID::IsDefault.
    const std::uint8_t flags = guid == 1 || exporting ? reader.readByte() : 0;
    if ((flags & hasPathFlag) == 0) return guid;
    const std::uint32_t outer = loadObject(reader, exporting, depth + 1);
    const std::string name = reader.readString();
    if (flags & hasNetworkChecksumFlag) reader.readUInt32();
    if (reader.isError()) return guid;
    Export& entry = exports[guid];
    if (!entry.name.empty() && (entry.name != name || entry.outer != outer)) classPaths.clear();
    entry = {.name = log.intern(name), .outer = outer};
    log.add({.kind = EventKind::NetGuid, .object = guid, .outerGuid = outer, .path = entry.name});
    return guid;
}

void PackageMap::receiveNetGuidBunch(net::BitReader& reader) {
    // bHasRepLayoutExport: the export groups of old replays, which came in bunches.
    if (reader.readBit()) throw Error("replay.parse", "The replay sends export groups in bunches, as replays before Unreal 4.14 did.");
    const std::int32_t count = reader.readInt32();
    for (std::int32_t i = 0; i < count && !reader.isError(); ++i) loadObject(reader, true);
}

void PackageMap::receiveExportData(ByteReader& reader) {
    receiveNetFieldExports(reader);
    // UPackageMapClient::ReceiveNetExportGUIDs: each export in a byte array of its own.
    const std::uint32_t count = reader.readIntPacked();
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::span<const std::uint8_t> bytes = reader.readByteArray();
        net::BitReader exportReader(bytes, static_cast<std::int64_t>(bytes.size()) * 8);
        loadObject(exportReader, true);
        if (exportReader.isError()) reader.fail("a NetGUID export does not fit its bytes");
    }
}

void PackageMap::receiveNetFieldExports(ByteReader& reader) {
    std::vector<std::size_t> changed;
    const std::uint32_t count = reader.readIntPacked();
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t pathIndex = reader.readIntPacked();
        const bool needsExport = reader.readIntPacked() == 1;
        std::size_t group;
        if (needsExport) {
            const std::string path = reader.readString();
            const std::uint32_t exportCount = reader.readIntPacked();
            const std::string_view interned = log.intern(path);
            const auto [found, added] = groupsByPath.try_emplace(interned, groups.size());
            if (added) groups.push_back({.path = interned, .pathIndex = pathIndex, .names = std::vector<std::string_view>(exportCount)});
            group = found->second;
            groupsByIndex[pathIndex] = group;
        } else {
            const auto found = groupsByIndex.find(pathIndex);
            if (found == groupsByIndex.end()) reader.fail(fmt::format("the export group {} was not exported", pathIndex));
            group = found->second;
        }
        // FNetFieldExport's operator<<.
        if (reader.readUInt8() != 1) continue;
        const std::uint32_t handle = reader.readIntPacked();
        // CompatibleChecksum.
        reader.skip(4);
        const std::string name = readExportName(reader);
        std::vector<std::string_view>& names = groups[group].names;
        if (handle >= names.size()) continue;
        names[handle] = log.intern(name);
        if (std::ranges::find(changed, group) == std::ranges::end(changed)) changed.push_back(group);
    }
    for (const std::size_t group : changed) {
        const ExportGroup& entry = groups[group];
        log.add({.kind = EventKind::ExportGroup, .object = entry.pathIndex, .path = entry.path, .fieldNames = log.addNames(entry.names)});
    }
}

std::string_view PackageMap::getName(const std::uint32_t guid) const {
    const auto found = exports.find(guid);
    return found == exports.end() ? std::string_view() : found->second.name;
}

std::string PackageMap::getPathName(const std::uint32_t guid) const {
    const auto found = exports.find(guid);
    if (found == exports.end()) return {};
    const Export& entry = found->second;
    if (entry.outer == 0 || isDynamic(entry.outer)) return std::string(entry.name);
    std::string outerPath = getPathName(entry.outer);
    if (outerPath.empty()) return {};
    // SUBOBJECT_DELIMITER after an object whose outer is a package.
    const auto outer = exports.find(entry.outer);
    const auto outerOuter = exports.find(outer->second.outer);
    const bool outerInPackage = outer->second.outer != 0 && !isDynamic(outer->second.outer) && outerOuter != exports.end() &&
                                outerOuter->second.outer == 0;
    return fmt::format("{}{}{}", outerPath, outerInPackage ? ':' : '.', entry.name);
}

std::string_view PackageMap::getClassPath(const std::uint32_t classGuid) {
    const auto cached = classPaths.find(classGuid);
    if (cached != classPaths.end()) return cached->second;
    const std::string_view path = log.intern(getPathName(classGuid));
    if (!path.empty()) classPaths.emplace(classGuid, path);
    return path;
}

std::string_view PackageMap::getArchetypeClassPath(const std::uint32_t archetypeGuid) {
    const std::string_view name = getName(archetypeGuid);
    if (!name.starts_with(defaultPrefix)) return {};
    // "/Game/A/B.B_C" of "/Game/A/B.Default__B_C": a class default object is in the package of its class.
    const auto outer = exports.find(exports.at(archetypeGuid).outer);
    if (outer == exports.end() || outer->second.outer != 0) return {};
    return log.intern(fmt::format("{}.{}", outer->second.name, name.substr(defaultPrefix.size())));
}

const ExportGroup* PackageMap::findGroup(const std::string_view path) const {
    const auto found = groupsByPath.find(path);
    return found == groupsByPath.end() ? nullptr : &groups[found->second];
}

}
