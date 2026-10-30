#include "package_map.h"

#include <algorithm>
#include <format>

#include "vrfparse/bit_reader.h"
#include "vrfparse/unreal_names.h"
#include "byte_reader.h"

namespace vrfparse {

namespace {

// UPackageMapClient::InternalLoadObject stops at this depth of outers.
constexpr int maxOuterDepth = 16;
// No class has more properties and functions; a larger export count is a misread.
constexpr std::uint32_t maxExportCount = 1 << 16;
constexpr std::string_view defaultPrefix = "Default__";

// EGuidPackageMapExportFlags.
constexpr std::uint8_t hasPathFlag = 1;
constexpr std::uint8_t hasNetworkChecksumFlag = 4;

// FName's text: the number is the suffix + 1, and 0 has none.
std::string_view appendNumber(std::string& text, const std::int32_t number) {
    if (number > 0)
        std::format_to(std::back_inserter(text), "_{}", number - 1);
    return text;
}

// UPackageMap::StaticSerializeName of a byte archive: bHardcoded takes a byte.
std::string_view readExportName(ByteReader& reader, std::string& text) {
    if (reader.readUInt8() != 0) {
        const std::optional<std::string_view> name = findHardcodedName(reader.readIntPacked());
        if (!name)
            reader.setError("a hardcoded name is not one of Unreal's");
        return name.value_or("");
    }
    text = reader.readString();
    return appendNumber(text, reader.readInt32());
}

}

std::string_view PackageMap::readName(BitReader& reader, std::string& text) {
    if (reader.readBit()) {
        const std::optional<std::string_view> name = findHardcodedName(reader.serializeIntPacked());
        if (!name)
            reader.setError();
        return name.value_or("");
    }
    text = reader.readString();
    return appendNumber(text, reader.readInt32());
}

std::uint32_t PackageMap::loadObject(BitReader& reader, const bool exporting) { return loadObject(reader, exporting, 0); }

std::uint32_t PackageMap::loadObject(BitReader& reader, const bool exporting, const int depth) {
    if (depth >= maxOuterDepth) {
        reader.setError();
        return 0;
    }
    const std::uint32_t guid = reader.serializeIntPacked();
    if (guid == 0 || reader.isError())
        return guid;
    // FNetworkGUID::IsDefault.
    const std::uint8_t flags = guid == 1 || exporting ? reader.readByte() : 0;
    if ((flags & hasPathFlag) == 0)
        return guid;
    const std::uint32_t outer = loadObject(reader, exporting, depth + 1);
    const std::string name = reader.readString();
    if (flags & hasNetworkChecksumFlag)
        reader.readUInt32();
    if (reader.isError())
        return guid;
    addExport(guid, outer, name);
    return guid;
}

void PackageMap::addExport(const std::uint32_t guid, const std::uint32_t outer, const std::string_view name) {
    Export& entry = exports[guid];
    if (!entry.name.empty() && (entry.name != name || entry.outer != outer))
        classPaths.clear();
    entry = {.name = log.intern(name), .outer = outer};
    log.add({.kind = EventKind::NetGuid, .object = guid, .outerGuid = outer, .path = entry.name});
}

void PackageMap::receiveNetGuidBunch(BitReader& reader) {
    // bHasRepLayoutExport: the export groups of old replays, which came in bunches.
    // Unreal 5.3 does not send them.
    if (reader.readBit())
        return reader.setError();
    const std::int32_t count = reader.readInt32();
    for (std::int32_t i = 0; i < count && !reader.isError(); ++i)
        loadObject(reader, true);
}

void PackageMap::receiveExportData(ByteReader& reader) {
    receiveNetFieldExports(reader);
    // UPackageMapClient::ReceiveNetExportGUIDs: each export in a byte array of its own.
    const std::uint32_t count = reader.readIntPacked();
    for (std::uint32_t i = 0; i < count && !reader.isError(); ++i) {
        const std::span<const std::uint8_t> bytes = reader.readByteArray();
        BitReader exportReader(bytes, static_cast<std::int64_t>(bytes.size()) * 8);
        loadObject(exportReader, true);
        if (exportReader.isError())
            reader.setError("a NetGUID export does not fit its bytes");
    }
}

void PackageMap::receiveNetFieldExports(ByteReader& reader) {
    std::vector<std::size_t> changed;
    std::string text;
    const std::uint32_t count = reader.readIntPacked();
    for (std::uint32_t i = 0; i < count && !reader.isError(); ++i) {
        const std::uint32_t pathIndex = reader.readIntPacked();
        const bool needsExport = reader.readIntPacked() == 1;
        std::size_t group;
        if (needsExport) {
            const std::string path = reader.readString();
            const std::uint32_t exportCount = reader.readIntPacked();
            if (exportCount > maxExportCount)
                reader.setError("an export count is too large");
            if (reader.isError())
                return;
            group = addGroup(path, pathIndex, exportCount);
        } else {
            const auto found = groupsByIndex.find(pathIndex);
            if (found == groupsByIndex.end())
                return reader.setError("an export group was not exported");
            group = found->second;
        }
        const bool named = readFieldExport(reader, groups[group], text);
        if (reader.isError())
            return;
        if (named && std::ranges::find(changed, group) == std::ranges::end(changed))
            changed.push_back(group);
    }
    for (const std::size_t group : changed)
        addGroupEvent(groups[group]);
}

void PackageMap::receiveCheckpoint(ByteReader& reader) {
    // FReplayCustomVersion::GuidNameTable: a path that the cache sent before comes as its index in the sent paths.
    std::vector<std::string_view> paths;
    const std::int32_t guidCount = reader.readInt32();
    if (guidCount < 0)
        return reader.setError("the NetGUID count is negative");
    for (std::int32_t i = 0; i < guidCount && !reader.isError(); ++i) {
        const std::uint32_t guid = reader.readIntPacked();
        const std::uint32_t outer = reader.readIntPacked();
        std::string_view path;
        if (reader.readUInt8() == 1) {
            path = paths.emplace_back(log.intern(reader.readString()));
        } else {
            const std::uint32_t index = reader.readIntPacked();
            if (index >= paths.size())
                return reader.setError("a NetGUID names a path that the checkpoint did not send");
            path = paths[index];
        }
        // bNoLoad and bIgnoreWhenMissing.
        reader.readUInt8();
        if (!reader.isError())
            addExport(guid, outer, path);
    }
    std::string text;
    const std::uint32_t groupCount = reader.readUInt32();
    for (std::uint32_t i = 0; i < groupCount && !reader.isError(); ++i) {
        const std::string path = reader.readString();
        const std::uint32_t pathIndex = reader.readIntPacked();
        const std::uint32_t exportCount = reader.readIntPacked();
        if (exportCount > maxExportCount)
            reader.setError("an export count is too large");
        if (reader.isError())
            return;
        ExportGroup& group = groups[addGroup(path, pathIndex, exportCount)];
        for (std::uint32_t field = 0; field < exportCount && !reader.isError(); ++field)
            readFieldExport(reader, group, text);
        if (!reader.isError())
            addGroupEvent(group);
    }
}

std::size_t PackageMap::addGroup(const std::string_view path, const std::uint32_t pathIndex, const std::uint32_t exportCount) {
    const std::string_view interned = log.intern(path);
    const auto [found, added] = groupsByPath.try_emplace(interned, groups.size());
    if (added)
        groups.push_back({.path = interned, .pathIndex = pathIndex, .names = std::vector<std::string_view>(exportCount)});
    groupsByIndex[pathIndex] = found->second;
    return found->second;
}

bool PackageMap::readFieldExport(ByteReader& reader, ExportGroup& group, std::string& text) {
    if (reader.readUInt8() != 1)
        return false;
    const std::uint32_t handle = reader.readIntPacked();
    // CompatibleChecksum.
    reader.skip(4);
    const std::string_view name = readExportName(reader, text);
    if (reader.isError() || handle >= group.names.size())
        return false;
    group.names[handle] = log.intern(name);
    return true;
}

void PackageMap::addGroupEvent(const ExportGroup& group) {
    log.add({.kind = EventKind::ExportGroup, .object = group.pathIndex, .path = group.path, .fieldNames = log.addNames(group.names)});
}

std::string_view PackageMap::getName(const std::uint32_t guid) const {
    const auto found = exports.find(guid);
    return found == exports.end() ? std::string_view() : found->second.name;
}

std::string PackageMap::getPathName(const std::uint32_t guid) const {
    const auto found = exports.find(guid);
    if (found == exports.end())
        return {};
    const Export& entry = found->second;
    if (entry.outer == 0 || isDynamicGuid(entry.outer))
        return std::string(entry.name);
    std::string outerPath = getPathName(entry.outer);
    if (outerPath.empty())
        return {};
    // SUBOBJECT_DELIMITER after an object whose outer is a package.
    const auto outer = exports.find(entry.outer);
    const auto outerOuter = exports.find(outer->second.outer);
    const bool outerInPackage = outer->second.outer != 0 && !isDynamicGuid(outer->second.outer) && outerOuter != exports.end() &&
                                outerOuter->second.outer == 0;
    return std::format("{}{}{}", outerPath, outerInPackage ? ':' : '.', entry.name);
}

std::string_view PackageMap::getClassPath(const std::uint32_t classGuid) {
    const auto cached = classPaths.find(classGuid);
    if (cached != classPaths.end())
        return cached->second;
    const std::string_view path = log.intern(getPathName(classGuid));
    if (!path.empty())
        classPaths.emplace(classGuid, path);
    return path;
}

std::string_view PackageMap::getArchetypeClassPath(const std::uint32_t archetypeGuid) {
    const std::string_view name = getName(archetypeGuid);
    if (!name.starts_with(defaultPrefix))
        return {};
    // "/Game/A/B.B_C" of "/Game/A/B.Default__B_C": a class default object is in the package of its class.
    const auto outer = exports.find(exports.find(archetypeGuid)->second.outer);
    if (outer == exports.end() || outer->second.outer != 0)
        return {};
    return log.intern(std::format("{}.{}", outer->second.name, name.substr(defaultPrefix.size())));
}

}
