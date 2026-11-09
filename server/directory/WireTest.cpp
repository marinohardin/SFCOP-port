#include <doctest/doctest.h>

#include "TestSupport.h"
#include "Wire.h"

using namespace sfcop::directory;
using sfcop::directory::test::hex;
using sfcop::directory::test::utf16;

namespace {

// The client's campaign query (DIRECTORY_API.md, "GetDir request"), 81 bytes.
const char* const kCampaignQuery =
    "51 00 00 00 05 02 00 67 00 77 0e 02 06 00 00 1f 00 2f 00 53 00 74 00 61 00 72 00 46 00 6c 00"
    "65 00 65 00 74 00 43 00 6f 00 6d 00 6d 00 61 00 6e 00 64 00 32 00 2f 00 47 00 61 00 6d 00 65"
    "00 2f 00 52 00 65 00 6c 00 65 00 61 00 73 00 65 00 00 00";

// The 169-byte tCampaignInfo with ID 1, TemplateName "TestCampaign" and the rest at defaults.
const char* const kCampaignBlob =
    "03 74 42 e5 f5 00 b0 da 64 2b 01 00 00 00 00 00 00 00 0c 00 00 00 54 65 73 74 43 61 6d 70 61"
    // Description, the three map names, two empty lists, three numbers, three strings, a
    // number, a string (56 zero bytes), then tCurrentTime: version 0, its id, nine zero words
    "69 67 6e 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
    "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
    "00 00 45 00 62 b6 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
    "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 07 00 00"
    "00 32 2e 35 2e 35 2e 32 07 00 00 00 32 2e 35 2e 35 2e 32";

}  // namespace

TEST_CASE("the object-stream CRC gives the ids the client checks") {
    CHECK(streamCrc(".?AVtCampaignInfo@@") == 0xF5E54274u);
    CHECK(streamCrc(".?AVtID@nDatabase@@") == 0x2B64DAB0u);
    CHECK(streamCrc(".?AVtCurrentTime@@") == 0xB6620045u);
}

TEST_CASE("the client's campaign query decodes to its flags and path") {
    GetDirRequest q;
    REQUIRE(decodeGetDirRequest(hex(kCampaignQuery), q));
    CHECK(q.flags == 0x06020E77u);
    CHECK(q.unknown == 0);
    CHECK(q.path == "/StarFleetCommand2/Game/Release");
    CHECK(q.dataTypes.empty());
}

TEST_CASE("the client's firewall query decodes, and encoding gives the same bytes back") {
    Bytes frame = hex("3f 00 00 00 05 02 00 67 00 37 08 02 06 00 00 16 00" +
                      utf16("/TitanServers/Firewall") + "00 00");
    REQUIRE(frame.size() == 63);
    GetDirRequest q;
    REQUIRE(decodeGetDirRequest(frame, q));
    CHECK(q.flags == 0x06020837u);
    CHECK(q.path == "/TitanServers/Firewall");
    CHECK(encodeGetDirRequest(q) == frame);
    CHECK(encodeGetDirRequest(GetDirRequest{0x06020E77u, 0, "/StarFleetCommand2/Game/Release", {}}) ==
          hex(kCampaignQuery));
}

TEST_CASE("a query with a wrong length, header, short field or trailing byte is refused") {
    GetDirRequest q;
    Bytes good = hex(kCampaignQuery);
    Bytes b = good;
    b[0] = 0x50;  // the length no longer matches
    CHECK_FALSE(decodeGetDirRequest(b, q));
    b = good;
    b[4] = 6;  // header type
    CHECK_FALSE(decodeGetDirRequest(b, q));
    b = good;
    b[7] = 0x68;  // message type
    CHECK_FALSE(decodeGetDirRequest(b, q));
    b = good;
    b.pop_back();
    b[0] = 0x50;  // consistent length, but the filter count is cut
    CHECK_FALSE(decodeGetDirRequest(b, q));
    b = good;
    b.push_back(0);
    b[0] = 0x52;  // one byte after the filter count
    CHECK_FALSE(decodeGetDirRequest(b, q));
    b = good;
    b[15] = 0xff;  // a path longer than the frame
    CHECK_FALSE(decodeGetDirRequest(b, q));
    CHECK_FALSE(decodeGetDirRequest(Bytes{}, q));
}

TEST_CASE("the firewall reply carries one detector entity with its address") {
    ServiceEntity e;
    e.name = "TitanFirewallDetector";
    e.displayName = "TitanFirewallDetector";
    e.address = 0xC0000214;  // 192.0.2.20
    e.port = 2021;
    Bytes expected = hex(
        "74 00 00 00 05 02 00 03 00 00 00 80 37 08 02 06 01 00 53 15 00" +
        utf16("TitanFirewallDetector") + "06 07 e5 c0 00 02 14 15 00" +
        utf16("TitanFirewallDetector") + "00 00");
    CHECK(encodeGetDirReply(0, 0x06020837u, {e}) == expected);
}

TEST_CASE("the campaign reply carries the entity, its date and the tCampaignInfo object") {
    CampaignInfo info;
    info.id = 1;
    info.templateName = "TestCampaign";
    CHECK(encodeCampaignInfo(info) == hex(kCampaignBlob));

    ServiceEntity e;
    e.name = "TestCampaign";
    e.displayName = "TestCampaign";
    e.address = 0xC000020A;  // 192.0.2.10
    e.port = 27000;
    e.date = 0x3F000000;
    e.objects.push_back(DataObject{kCampaignInfoType, encodeCampaignInfo(info)});
    Bytes expected = hex(
        std::string("13 01 00 00 05 02 00 03 00 00 00 80 77 0e 02 06 01 00 53 0c 00") +
        utf16("TestCampaign") + "06 69 78 c0 00 02 0a 0c 00" + utf16("TestCampaign") +
        "00 00 00 3f 01 00 13" + "63 6c 61 73 73 20 74 43 61 6d 70 61 69 67 6e 49 6e 66 6f" +
        "a9 00" + kCampaignBlob);
    CHECK(expected.size() == 0x113);
    CHECK(encodeGetDirReply(0, 0x06020E77u, {e}) == expected);
}

TEST_CASE("a negative status is sent with no entities") {
    ServiceEntity e;
    e.name = "ignored";
    CHECK(encodeGetDirReply(-1200, 0x06020E77u, {e}) ==
          hex("12 00 00 00 05 02 00 03 00 50 fb 80 77 0e 02 06 00 00"));
}

TEST_CASE("data objects are left out unless the flags carry both type and payload") {
    ServiceEntity e;
    e.objects.push_back(DataObject{"x", Bytes{1}});
    // flags 0x810: the type byte and a data-object count, which is 0
    CHECK(encodeGetDirReply(0, 0x810u, {e}) ==
          hex("15 00 00 00 05 02 00 03 00 00 00 80 10 08 00 00 01 00 53 00 00"));
}

TEST_CASE("the detection request decodes and the reply is success") {
    Bytes request = hex("0f 00 05 03 00 01 00 fc 08 01 20 4e 00 00 00");
    DetectRequest q;
    REQUIRE(decodeDetectRequest(request, q));
    CHECK(q.port == 2300);
    CHECK(q.flagA == 1);
    CHECK(q.waitMs == 20000);
    CHECK(q.flagB == 0);
    CHECK(encodeDetectRequest(q) == request);
    CHECK(encodeDetectReply(0) == hex("09 00 05 03 00 02 00 00 00"));
    CHECK(encodeDetectReply(-17) == hex("09 00 05 03 00 02 00 ef ff"));

    Bytes bad = request;
    bad[3] = 2;  // service 2 is the directory, not the detector
    CHECK_FALSE(decodeDetectRequest(bad, q));
    bad = request;
    bad.pop_back();
    CHECK_FALSE(decodeDetectRequest(bad, q));
}
