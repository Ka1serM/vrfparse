#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vrfparse/event.h"

namespace vrfparse {

class ByteReader;

// FNetworkGUID::IsDynamic: the server assigns even NetGUIDs to dynamic objects.
inline bool isDynamicGuid(const std::uint32_t guid) { return guid > 0 && (guid & 1) == 0; }

// FNetFieldExportGroup: the export names of a RepLayout, a class net cache, or a function, by handle. A handle that the
// replay has not exported yet has an empty name.
struct ExportGroup {
    std::string_view path;
    std::uint32_t pathIndex = 0;
    std::vector<std::string_view> names;
};

// UPackageMapClient's NetGUID cache and export groups, as far as reading needs them: paths and names, not objects.
// What it receives goes to the event log.
class PackageMap {
public:
    explicit PackageMap(EventLog& log) : log(log) {}

    // UPackageMapClient::InternalLoadObject: the NetGUID, and its export when the NetGUID has a path. An export bunch
    // sends the export flags of every NetGUID; elsewhere only the default NetGUID (1) has them. The reader's error is
    // set when it runs out.
    std::uint32_t loadObject(BitReader& reader, bool exporting);
    // UPackageMapClient::ReceiveNetGUIDBunch.
    void receiveNetGuidBunch(BitReader& reader);
    // UPackageMapClient::ReceiveExportData of a demo frame: ReceiveNetFieldExports, then ReceiveNetExportGUIDs.
    void receiveExportData(ByteReader& reader);
    // The NetGUID cache of a checkpoint (UDemoNetDriver::LoadCheckpoint), then its export groups
    // (UPackageMapClient::SerializeNetFieldExportGroupMap).
    void receiveCheckpoint(ByteReader& reader);

    // The name of the NetGUID in its outer, as it was exported. Empty when it was not.
    std::string_view getName(std::uint32_t guid) const;
    // UObject::GetPathName of the NetGUID: its outers' names, then its own. Empty when an outer was not exported.
    std::string getPathName(std::uint32_t guid) const;
    // The path name of a class, such as the class of a subobject. Empty when an outer was not exported.
    std::string_view getClassPath(std::uint32_t classGuid);
    // The class of a dynamic actor: the class of its archetype, a class default object. Empty for an archetype that is
    // not a class default object: without the archetype's package, it names no class.
    std::string_view getArchetypeClassPath(std::uint32_t archetypeGuid);

    // UPackageMap::StaticSerializeName: a hardcoded name's text, or the text that the reader reads into the buffer. A
    // hardcoded name that Unreal does not have sets the reader's error.
    static std::string_view readName(BitReader& reader, std::string& text);

private:
    struct Export {
        std::string_view name;
        std::uint32_t outer = 0;
    };

    std::uint32_t loadObject(BitReader& reader, bool exporting, int depth);
    void receiveNetFieldExports(ByteReader& reader);
    void addExport(std::uint32_t guid, std::uint32_t outer, std::string_view name);
    // The group of the path, added with the export count when it is new.
    std::size_t addGroup(std::string_view path, std::uint32_t pathIndex, std::uint32_t exportCount);
    // FNetFieldExport's operator<<, into the group. True when it names a field.
    bool readFieldExport(ByteReader& reader, ExportGroup& group, std::string& text);
    void addGroupEvent(const ExportGroup& group);

    EventLog& log;
    std::unordered_map<std::uint32_t, Export> exports;
    std::unordered_map<std::uint32_t, std::string_view> classPaths;
    std::vector<ExportGroup> groups;
    std::unordered_map<std::uint32_t, std::size_t> groupsByIndex;
    std::unordered_map<std::string_view, std::size_t> groupsByPath;
};

}
