#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vrfparse/Event.h"

namespace vrfparse {

class ByteReader;

//! The server assigns even NetGUIDs to dynamic objects.
inline bool isDynamicGuid(const std::uint32_t guid) { return guid > 0 && (guid & 1) == 0; }

struct ExportGroup {
    std::string_view path;
    std::uint32_t pathIndex = 0;
    std::vector<std::string_view> names;
};

class PackageMap {
public:
    explicit PackageMap(EventLog& log) : log(log) {}

    std::uint32_t loadObject(BitReader& reader, bool exporting);
    void receiveNetGuidBunch(BitReader& reader);
    void receiveExportData(ByteReader& reader);
    void receiveCheckpoint(ByteReader& reader);

    std::string_view getName(std::uint32_t guid) const;
    std::string getPathName(std::uint32_t guid) const;
    std::string_view getClassPath(std::uint32_t classGuid);
    std::string_view getArchetypeClassPath(std::uint32_t archetypeGuid);

    static std::string_view readName(BitReader& reader, std::string& text);

private:
    struct Export {
        std::string_view name;
        std::uint32_t outer = 0;
    };

    std::uint32_t loadObject(BitReader& reader, bool exporting, int depth);
    void receiveNetFieldExports(ByteReader& reader);
    void addExport(std::uint32_t guid, std::uint32_t outer, std::string_view name);
    std::size_t addGroup(std::string_view path, std::uint32_t pathIndex, std::uint32_t exportCount);
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
