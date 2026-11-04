#include "hle/dplay/Layout.h"

#include <optional>
#include <string>

namespace sfcop::hle::dplay {

namespace {

// A fixed-size structure followed by the strings and data it points to.
class BlobBuilder {
public:
    explicit BlobBuilder(std::uint32_t fixedBytes) { blob_.bytes.assign(fixedBytes, 0); }

    void U32(std::uint32_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            blob_.bytes[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }
    void Id(std::uint32_t at, const Guid &g) {
        for (std::size_t i = 0; i < 16; ++i) {
            blob_.bytes[at + i] = g.bytes[i];
        }
    }
    // Appends `s` with its NUL and points the field at `at` to it; an absent string stays null.
    void String(std::uint32_t at, const std::optional<std::string> &s) {
        if (!s) {
            return;
        }
        std::uint32_t offset = static_cast<std::uint32_t>(blob_.bytes.size());
        blob_.bytes.insert(blob_.bytes.end(), s->begin(), s->end());
        blob_.bytes.push_back(0);
        pointTo(at, offset);
    }
    // Appends `data` and points the field at `at` to it; empty data stays null.
    void Data(std::uint32_t at, const std::vector<std::uint8_t> &data) {
        if (data.empty()) {
            return;
        }
        std::uint32_t offset = static_cast<std::uint32_t>(blob_.bytes.size());
        blob_.bytes.insert(blob_.bytes.end(), data.begin(), data.end());
        pointTo(at, offset);
    }
    // A DPNAME at `at`.
    void NameAt(std::uint32_t at, const Name &name) {
        U32(at, kNameBytes);
        String(at + 8, name.shortName);
        String(at + 12, name.longName);
    }
    // A DPSESSIONDESC2 at `at`.
    void SessionDescAt(std::uint32_t at, const SessionDesc &d) {
        U32(at + 0, kSessionDescBytes);
        U32(at + 4, d.flags);
        Id(at + 8, d.instance);
        Id(at + 24, d.application);
        U32(at + 40, d.maxPlayers);
        U32(at + 44, d.currentPlayers);
        String(at + 48, d.name);
        String(at + 52, d.password);
        for (std::uint32_t i = 0; i < 4; ++i) {
            U32(at + 64 + 4 * i, d.user[i]);
        }
    }
    Blob Take() { return std::move(blob_); }

private:
    void pointTo(std::uint32_t at, std::uint32_t offset) {
        U32(at, offset);
        blob_.pointers.push_back(at);
    }

    Blob blob_;
};

} // namespace

Blob layoutName(const Name &name) {
    BlobBuilder b(kNameBytes);
    b.NameAt(0, name);
    return b.Take();
}

Blob layoutSessionDesc(const SessionDesc &desc) {
    BlobBuilder b(kSessionDescBytes);
    b.SessionDescAt(0, desc);
    return b.Take();
}

Blob layoutCreatePlayerOrGroup(std::uint32_t playerType, Dpid id, std::uint32_t currentPlayers,
                               const std::vector<std::uint8_t> &data, const Name &name,
                               std::uint32_t flags) {
    BlobBuilder b(48);
    b.U32(0, kSysCreatePlayerOrGroup);
    b.U32(4, playerType);
    b.U32(8, id);
    b.U32(12, currentPlayers);
    b.U32(20, static_cast<std::uint32_t>(data.size()));
    b.U32(44, flags);
    b.Data(16, data);
    b.NameAt(24, name);
    return b.Take();
}

Blob layoutDestroyPlayerOrGroup(std::uint32_t playerType, Dpid id,
                                const std::vector<std::uint8_t> &localData,
                                const std::vector<std::uint8_t> &remoteData, const Name &name,
                                std::uint32_t flags) {
    BlobBuilder b(52);
    b.U32(0, kSysDestroyPlayerOrGroup);
    b.U32(4, playerType);
    b.U32(8, id);
    b.U32(16, static_cast<std::uint32_t>(localData.size()));
    b.U32(24, static_cast<std::uint32_t>(remoteData.size()));
    b.U32(48, flags);
    b.Data(12, localData);
    b.Data(20, remoteData);
    b.NameAt(28, name);
    return b.Take();
}

Blob layoutGroupMembership(std::uint32_t type, Dpid group, Dpid player) {
    BlobBuilder b(12);
    b.U32(0, type);
    b.U32(4, group);
    b.U32(8, player);
    return b.Take();
}

Blob layoutSetPlayerOrGroupData(std::uint32_t playerType, Dpid id,
                                const std::vector<std::uint8_t> &data) {
    BlobBuilder b(20);
    b.U32(0, kSysSetPlayerOrGroupData);
    b.U32(4, playerType);
    b.U32(8, id);
    b.U32(16, static_cast<std::uint32_t>(data.size()));
    b.Data(12, data);
    return b.Take();
}

Blob layoutSetPlayerOrGroupName(std::uint32_t playerType, Dpid id, const Name &name) {
    BlobBuilder b(28);
    b.U32(0, kSysSetPlayerOrGroupName);
    b.U32(4, playerType);
    b.U32(8, id);
    b.NameAt(12, name);
    return b.Take();
}

Blob layoutSetSessionDesc(const SessionDesc &desc) {
    BlobBuilder b(4 + kSessionDescBytes);
    b.U32(0, kSysSetSessionDesc);
    b.SessionDescAt(4, desc);
    return b.Take();
}

Blob layoutTypeOnly(std::uint32_t type) {
    BlobBuilder b(4);
    b.U32(0, type);
    return b.Take();
}

} // namespace sfcop::hle::dplay
