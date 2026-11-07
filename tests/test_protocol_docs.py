"""The network protocol documents are well formed (src/net/PROTOCOL.md, RENDEZVOUS.md, LOBBY_API.md).

Every binary message table parses, its offsets follow on, its field sizes add up to the size the
document states, and its minimum follows the rule in PROTOCOL.md, "Reading the tables". Hand-written
datagrams, the same bytes src/hle/dplay/WireTest.cpp checks the encoders against, walk the tables
field by field and end exactly at their last byte. Every message or object a document names is defined exactly once across the
three. Every version table counts up. The session messages match the datagrams in
src/hle/dplay/Wire.h. Expected values are written out by hand.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NET = ROOT / "src" / "net"
DOCS = {name: NET / name for name in ("PROTOCOL.md", "RENDEZVOUS.md", "LOBBY_API.md")}
BINARY_DOCS = ("PROTOCOL.md", "RENDEZVOUS.md")
WIRE_H = ROOT / "src" / "hle" / "dplay" / "Wire.h"

PRIMITIVES = {"u8": 1, "u16": 2, "u32": 4, "u64": 8, "guid": 16}
VARIABLE = {"str", "bytes", "rest"}
JSON_TYPES = {"string", "integer", "boolean", "object"}
BINARY_HEADER = ["Offset", "Size", "Field", "Type", "Meaning"]
OBJECT_HEADER = ["Field", "JSON type", "Required", "Meaning"]
VERSION_HEADER = ["Version", "Date", "Package", "Change"]

HEADING = re.compile(r"^### `(\w+)`\s*$")
SIZE_LINE = re.compile(r"^Size: (?:(\d+) bytes\.|at least (\d+) bytes?\. Minimum: (\d+) bytes\.)$")
REST_MINIMUM = re.compile(r"\bat least (\d+) bytes?\b", re.I)
ARRAY = re.compile(r"^(\w+)\[(n|\d+)\]$")


def cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def tables(lines):
    """Yields (index of the header line, header cells, rows of cells) for every table."""
    i = 0
    while i < len(lines):
        if lines[i].startswith("|") and i + 1 < len(lines) and re.match(r"^\|[\s:|-]+\|$", lines[i + 1].strip()):
            header = cells(lines[i])
            rows = []
            j = i + 2
            while j < len(lines) and lines[j].startswith("|"):
                rows.append(cells(lines[j]))
                j += 1
            yield i, header, rows
            i = j
        else:
            i += 1


def definitions(text):
    """{name: (kind, header, rows, size line or None)} for every `### \\`Name\\`` section.

    kind is "structure" when the section's first paragraph starts with "Structure.", else
    "message". The table is the first one after the heading, before the next heading.
    """
    lines = text.splitlines()
    out = {}
    heads = [(i, HEADING.match(l).group(1)) for i, l in enumerate(lines) if HEADING.match(l)]
    for n, (start, name) in enumerate(heads):
        end = len(lines)
        for k in range(start + 1, len(lines)):
            if lines[k].startswith("## ") or lines[k].startswith("### "):
                end = k
                break
        body = lines[start + 1:end]
        if name in out:
            raise AssertionError(f"{name} defined twice in one document")
        prose = next((l for l in body if l.strip() and not l.startswith("|")), "")
        kind = "structure" if prose.startswith("Structure.") else "message"
        found = list(tables(body))
        if not found:
            raise AssertionError(f"{name} has no table")
        _, header, rows = found[0]
        size = next((l.strip() for l in body if l.startswith("Size:")), None)
        out[name] = (kind, header, rows, size)
    return out


def check_binary(name, rows, size_line, sizes):
    """Checks one binary table; returns (fixed size, exact, minimum). `sizes` maps structure
    names to (fixed size, exact, minimum) for the structures already checked."""
    offset = 0
    variable = False
    total = 0
    minimum = 0
    fields = set()
    for row in rows:
        if len(row) != 5:
            raise AssertionError(f"{name}: row {row} has {len(row)} cells")
        off, size, field, typ = row[0], row[1], row[2], row[3].strip("`")
        if field in fields:
            raise AssertionError(f"{name}: field {field} twice")
        fields.add(field)
        if variable:
            if off != "—":
                raise AssertionError(f"{name}.{field}: offset {off} after a variable field, expected —")
        elif off != str(offset):
            raise AssertionError(f"{name}.{field}: offset {off}, expected {offset}")
        m = ARRAY.match(typ)
        if typ in PRIMITIVES:
            want = PRIMITIVES[typ]
        elif typ in VARIABLE:
            want = None
            if typ == "str":
                minimum += 2
            elif typ == "bytes":
                minimum += 4
            else:
                least = REST_MINIMUM.search(row[4])
                minimum += int(least.group(1)) if least else 0
        elif m and m.group(2) == "n":
            want = None
            if m.group(1) not in PRIMITIVES and m.group(1) not in sizes:
                raise AssertionError(f"{name}.{field}: unknown element type {m.group(1)}")
        elif m:
            if m.group(1) not in PRIMITIVES:
                raise AssertionError(f"{name}.{field}: fixed array of {m.group(1)}")
            want = PRIMITIVES[m.group(1)] * int(m.group(2))
        elif typ in sizes:
            fixed, exact, least = sizes[typ]
            want = fixed if exact else None
            if want is None:
                minimum += least
        else:
            raise AssertionError(f"{name}.{field}: unknown type {typ}")
        if want is None:
            if size != "var":
                raise AssertionError(f"{name}.{field}: {typ} is variable, size {size}")
            variable = True
        else:
            if size != str(want):
                raise AssertionError(f"{name}.{field}: {typ} is {want} bytes, size {size}")
            total += want
            minimum += want
            offset += want
    if size_line is None:
        raise AssertionError(f"{name}: no Size line")
    m = SIZE_LINE.match(size_line)
    if not m:
        raise AssertionError(f"{name}: cannot read {size_line!r}")
    at_least = m.group(1) is None
    stated = int(m.group(1) or m.group(2))
    if stated != total:
        raise AssertionError(f"{name}: fields add up to {total}, the document says {stated}")
    if at_least != variable:
        raise AssertionError(f"{name}: 'at least' must be used exactly when a field is variable")
    if at_least and int(m.group(3)) != minimum:
        raise AssertionError(f"{name}: the minimum by the rule is {minimum}, the document says {m.group(3)}")
    return total, not variable, minimum if variable else total


def check_binary_doc(defs, sizes):
    """Checks every binary definition, structures first in dependency order."""
    pending = dict(defs)
    while pending:
        progress = False
        for name, (kind, header, rows, size) in list(pending.items()):
            if header != BINARY_HEADER:
                raise AssertionError(f"{name}: header {header}")
            deps = set()
            for row in rows:
                typ = row[3].strip("`")
                m = ARRAY.match(typ)
                base = m.group(1) if m else typ
                if base not in PRIMITIVES and base not in VARIABLE:
                    deps.add(base)
            if deps - set(sizes) - {name}:
                continue
            sizes[name] = check_binary(name, rows, size, sizes)
            del pending[name]
            progress = True
        if not progress:
            raise AssertionError(f"unresolved types in {sorted(pending)}")


def index_names(text):
    """The backticked names in the first column of every table whose first header is Name."""
    out = []
    for _, header, rows in tables(text.splitlines()):
        if header and header[0] == "Name":
            codes = [r[1] for r in rows]
            if len(codes) != len(set(codes)):
                raise AssertionError(f"duplicate type codes in index {header}")
            out += [r[0].strip("`") for r in rows]
    return out


def walk(defs, name, data, pos=0, top=True):
    """Reads `data` from `pos` by the table of `name`; returns ({field: value}, end position).

    Integers are ints, `guid` and `bytes` are bytes, a `str` is its text or None, `rest` is the
    remaining bytes, a structure is its own dict and an array a list. At the top level the table
    must end exactly at the last byte, as every decoder demands.
    """
    _kind, _header, rows, _size = defs[name]
    out = {}
    last = None
    base = pos

    def take(count):
        nonlocal pos
        if pos + count > len(data):
            raise AssertionError(f"{name}: runs past the end at {pos}")
        chunk = data[pos:pos + count]
        pos += count
        return chunk

    def one(typ):
        nonlocal pos
        if typ == "guid":
            return take(16)
        if typ in PRIMITIVES:
            return int.from_bytes(take(PRIMITIVES[typ]), "little")
        if typ == "str":
            length = int.from_bytes(take(2), "little")
            return None if length == 0xFFFF else take(length).decode()
        if typ == "bytes":
            return take(int.from_bytes(take(4), "little"))
        if typ == "rest":
            return take(len(data) - pos)
        value, pos = walk(defs, typ, data, pos, top=False)
        return value

    for row in rows:
        field, typ = row[2], row[3].strip("`")
        m = ARRAY.match(typ)
        if row[0] != "—" and int(row[0]) != pos - base:
            raise AssertionError(f"{name}.{field}: at offset {pos - base}, the table says {row[0]}")
        if m:
            count = last if m.group(2) == "n" else int(m.group(2))
            out[field] = [one(m.group(1)) for _ in range(count)]
        else:
            out[field] = one(typ)
        last = out[field]
    if top and pos != len(data):
        raise AssertionError(f"{name}: {len(data) - pos} bytes left over")
    return out, pos


def check_versions(text):
    count = 0
    for _, header, rows in tables(text.splitlines()):
        if header != VERSION_HEADER:
            continue
        count += 1
        versions = [int(r[0]) for r in rows]
        dates = [r[1] for r in rows]
        if versions != list(range(1, len(versions) + 1)):
            raise AssertionError(f"versions {versions} do not count up from 1")
        for d in dates:
            if not re.match(r"^\d{4}-\d{2}-\d{2}$", d):
                raise AssertionError(f"date {d}")
        if dates != sorted(dates):
            raise AssertionError(f"dates {dates} go back")
    return count


def check_object(name, header, rows, objects):
    if header != OBJECT_HEADER:
        raise AssertionError(f"{name}: header {header}")
    fields = set()
    for row in rows:
        field, typ, req = row[0], row[1], row[2]
        if field in fields:
            raise AssertionError(f"{name}.{field} twice")
        fields.add(field)
        base = typ[:-2] if typ.endswith("[]") else typ
        if base not in JSON_TYPES and base not in objects:
            raise AssertionError(f"{name}.{field}: unknown JSON type {typ}")
        if req not in ("yes", "no"):
            raise AssertionError(f"{name}.{field}: required is {req}")


def wire_enums():
    """{enum name: {enumerator without k: value}} from Wire.h."""
    text = WIRE_H.read_text()
    out = {}
    for enum, body in re.findall(r"enum class (\w+) : std::uint8_t \{(.*?)\};", text, re.S):
        out[enum] = {n: int(v) for n, v in re.findall(r"k(\w+) = (\d+)", body)}
    return out


class ParserTest(unittest.TestCase):
    """The checks themselves refuse bad tables (hand-written fixtures)."""

    def table(self, rows, size):
        text = "### `Thing`\n\nStructure.\n\n| Offset | Size | Field | Type | Meaning |\n| ---: | ---: | --- | --- | --- |\n"
        text += "".join(f"| {a} | {b} | {c} | {d} | |\n" for a, b, c, d in rows)
        return definitions(text + "\n" + size + "\n")

    def test_a_good_table_passes(self):
        defs = self.table([("0", "4", "a", "u32"), ("4", "2", "b", "u16"), ("6", "var", "c", "str")], "Size: at least 6 bytes. Minimum: 8 bytes.")
        sizes = {}
        check_binary_doc(defs, sizes)
        self.assertEqual(sizes["Thing"], (6, False, 8))

    def test_a_minimum_that_breaks_the_rule_fails(self):
        defs = self.table([("0", "4", "a", "u32"), ("4", "var", "b", "bytes")], "Size: at least 4 bytes. Minimum: 4 bytes.")
        with self.assertRaises(AssertionError):
            check_binary_doc(defs, {})

    def test_sizes_that_do_not_add_up_fail(self):
        defs = self.table([("0", "4", "a", "u32"), ("4", "2", "b", "u16")], "Size: 8 bytes.")
        with self.assertRaises(AssertionError):
            check_binary_doc(defs, {})

    def test_a_size_that_disagrees_with_the_type_fails(self):
        defs = self.table([("0", "2", "a", "u32")], "Size: 2 bytes.")
        with self.assertRaises(AssertionError):
            check_binary_doc(defs, {})

    def test_an_offset_gap_fails(self):
        defs = self.table([("0", "4", "a", "u32"), ("5", "4", "b", "u32")], "Size: 8 bytes.")
        with self.assertRaises(AssertionError):
            check_binary_doc(defs, {})

    def test_an_offset_after_a_variable_field_fails(self):
        defs = self.table([("0", "var", "a", "str"), ("2", "4", "b", "u32")], "Size: at least 4 bytes. Minimum: 6 bytes.")
        with self.assertRaises(AssertionError):
            check_binary_doc(defs, {})

    def test_exact_size_with_a_variable_field_fails(self):
        defs = self.table([("0", "4", "a", "u32"), ("4", "var", "b", "rest")], "Size: 4 bytes.")
        with self.assertRaises(AssertionError):
            check_binary_doc(defs, {})

    def test_an_unknown_type_fails(self):
        defs = self.table([("0", "4", "a", "Nowhere")], "Size: 4 bytes.")
        with self.assertRaises(AssertionError):
            check_binary_doc(defs, {})

    def test_versions_that_go_back_fail(self):
        text = "| Version | Date | Package | Change |\n| --- | --- | --- | --- |\n| 1 | 2026-11-06 | X | a |\n| 1 | 2026-11-07 | Y | b |\n"
        with self.assertRaises(AssertionError):
            check_versions(text)


class DocumentsTest(unittest.TestCase):
    def setUp(self):
        self.text = {name: path.read_text() for name, path in DOCS.items()}
        self.defs = {name: definitions(t) for name, t in self.text.items()}

    def test_every_name_is_defined_once_across_the_documents(self):
        seen = {}
        for doc, defs in self.defs.items():
            for name in defs:
                self.assertNotIn(name, seen, f"{name} in {doc} and {seen.get(name)}")
                seen[name] = doc

    def test_every_binary_table_adds_up(self):
        sizes = {}
        for doc in BINARY_DOCS:
            check_binary_doc(self.defs[doc], sizes)
        # Hand-written: the sizes the documents promise.
        expected = {
            "EnvelopeHeader": (32, True, 32), "EnvelopeData": (32, False, 33),
            "EnvelopeAck": (40, True, 40), "EnvelopePing": (40, True, 40),
            "EnvelopePong": (40, True, 40), "EnvelopeClose": (36, True, 36),
            "Header": (8, True, 8), "PeerHeader": (16, True, 16),
            "SessionDescription": (60, False, 64), "Name": (0, False, 4),
            "PeerInfo": (10, True, 10), "Entity": (15, False, 23),
            "EnumRequest": (28, True, 28), "EnumReply": (12, False, 76),
            "JoinRequest": (24, False, 26), "JoinReject": (28, True, 28),
            "Reliable": (20, False, 21), "Ack": (20, True, 20), "Unreliable": (24, False, 24),
            "Keepalive": (16, True, 16), "Leave": (16, True, 16),
            "JoinAccept": (13, False, 77), "PeerAdd": (11, True, 11),
            "PeerRemove": (5, True, 5), "EntityAdd": (1, False, 24),
            "EntityRemove": (5, True, 5), "SetData": (5, False, 9), "SetName": (5, False, 9),
            "MemberAdd": (9, True, 9), "MemberRemove": (9, True, 9),
            "SessionDesc": (1, False, 65), "Data": (9, False, 9),
            "RvHeader": (12, True, 12), "Ticket": (36, True, 36), "Candidate": (8, True, 8),
            "Bind": (50, False, 50), "BindReply": (28, True, 28), "Connect": (28, True, 28),
            "PeerCandidates": (32, False, 32), "Probe": (32, True, 32),
            "ProbeReply": (32, True, 32), "Unbind": (24, True, 24), "RvError": (16, True, 16),
        }
        for name, want in expected.items():
            self.assertEqual(sizes[name], want, name)

    def test_every_indexed_message_is_defined_in_its_document(self):
        for doc in BINARY_DOCS:
            names = index_names(self.text[doc])
            self.assertEqual(len(names), len(set(names)), doc)
            defs = self.defs[doc]
            for name in names:
                self.assertIn(name, defs, f"{doc} indexes {name} but does not define it")
                self.assertEqual(defs[name][0], "message", name)
            for name, (kind, *_rest) in defs.items():
                if kind == "message":
                    self.assertIn(name, names, f"{doc} defines {name} but its index does not name it")

    def test_the_envelope_kinds_are_as_written(self):
        rows = next(r for _, h, r in tables(self.text["PROTOCOL.md"].splitlines()) if h[:2] == ["Name", "Kind"])
        self.assertEqual({r[0].strip("`"): int(r[1]) for r in rows}, {
            "EnvelopeData": 1, "EnvelopeAck": 2, "EnvelopePing": 3, "EnvelopePong": 4, "EnvelopeClose": 5})

    def test_the_session_messages_are_the_datagrams_of_wire_h(self):
        enums = wire_enums()
        found = {}
        for _, header, rows in tables(self.text["PROTOCOL.md"].splitlines()):
            if header[:2] == ["Name", "Type"]:
                found.update({r[0].strip("`"): int(r[1]) for r in rows})
        # Hand-written from Wire.h at the time of writing; the second check keeps it honest.
        packets = {"EnumRequest": 1, "EnumReply": 2, "JoinRequest": 3, "JoinReject": 4, "Reliable": 5,
                   "Ack": 6, "Unreliable": 7, "Keepalive": 8, "Leave": 9}
        inner = {"JoinAccept": 1, "PeerAdd": 2, "PeerRemove": 3, "EntityAdd": 4, "EntityRemove": 5,
                 "SetData": 6, "SetName": 7, "MemberAdd": 8, "MemberRemove": 9, "SessionDesc": 10, "Data": 11}
        self.assertEqual(enums["PacketType"], packets)
        self.assertEqual(enums["InnerType"], inner)
        for name, code in list(packets.items()) + list(inner.items()):
            self.assertEqual(found.get(name), code, name)

    def test_the_lobby_objects_and_endpoints_parse(self):
        defs = self.defs["LOBBY_API.md"]
        objects = set(defs)
        for name, (_kind, header, rows, _size) in defs.items():
            check_object(name, header, rows, objects)
        endpoints = next(r for _, h, r in tables(self.text["LOBBY_API.md"].splitlines()) if h[:2] == ["Method", "Path"])
        self.assertEqual([(r[0], r[1].strip("`")) for r in endpoints], [
            ("GET", "/v1/health"),
            ("GET", "/v1/sessions?protocol=<n>&application=<guid>"),
            ("POST", "/v1/sessions"),
            ("PUT", "/v1/sessions/<session>"),
            ("DELETE", "/v1/sessions/<session>"),
            ("POST", "/v1/sessions/<session>/tickets"),
        ])
        for row in endpoints:
            for ref in (row[2], row[3]):
                if ref != "—":
                    self.assertIn(ref.strip("`"), objects, row)

    def test_error_and_result_codes_are_unique(self):
        errors = next(r for _, h, r in tables(self.text["LOBBY_API.md"].splitlines()) if h[:2] == ["Code", "HTTP"])
        codes = [r[0] for r in errors]
        self.assertEqual(len(codes), len(set(codes)))
        self.assertIn("`rate_limited`", codes)
        results = next(r for _, h, r in tables(self.text["RENDEZVOUS.md"].splitlines()) if h[:2] == ["Code", "Name"])
        self.assertEqual([int(r[0]) for r in results], list(range(0, 9)))

    def test_every_version_table_counts_up(self):
        counts = {doc: check_versions(t) for doc, t in self.text.items()}
        self.assertEqual(counts, {"PROTOCOL.md": 2, "RENDEZVOUS.md": 1, "LOBBY_API.md": 1})

    def test_the_magics_differ_in_their_first_bytes(self):
        # One socket carries all three; the first four bytes tell them apart.
        magics = {"SFNE": 0x454E4653, "SFDP": 0x50444653, "SFRV": 0x56524653}
        for text, value in magics.items():
            self.assertEqual(value.to_bytes(4, "little"), text.encode())
        joined = self.text["PROTOCOL.md"] + self.text["RENDEZVOUS.md"]
        for value in magics.values():
            self.assertIn(f"`0x{value:08X}`", joined)


def hexbytes(text):
    return bytes.fromhex(text.replace(" ", ""))


# Two GUIDs, plainly invented, in their 16-byte memory form.
INSTANCE = hexbytes("10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F")
APPLICATION = hexbytes("44 33 22 11 66 55 88 77 99 AA BB CC DD EE FF 00")


class WireLayoutTest(unittest.TestCase):
    """PROTOCOL.md's session tables read the datagrams Wire.cpp writes.

    The byte sequences are the ones src/hle/dplay/WireTest.cpp checks the encoders against, written
    out again by hand; the expected field values are written by hand too.
    """

    def setUp(self):
        self.defs = definitions((NET / "PROTOCOL.md").read_text())

    def read(self, name, data):
        return walk(self.defs, name, data)[0]

    def test_a_join_request_with_and_without_a_password(self):
        header = {"magic": 0x50444653, "version": 1, "type": 3, "reserved": 0}
        given = self.read("JoinRequest", hexbytes("53 46 44 50 01 03 00 00") + INSTANCE + hexbytes("02 00 70 77"))
        self.assertEqual(given, {"header": header, "instance": INSTANCE, "password": "pw"})
        absent = hexbytes("53 46 44 50 01 03 00 00") + INSTANCE + hexbytes("FF FF")
        self.assertEqual(self.read("JoinRequest", absent)["password"], None)
        self.assertEqual(len(absent), 26)  # its Minimum

    def test_a_reliable_datagram_and_its_inner_message(self):
        data = hexbytes("53 46 44 50 01 05 00 00 0D F0 AD 0B 01 00 00 00 03 00 00 00 05 01 00 01 00")
        reliable = self.read("Reliable", data)
        self.assertEqual(reliable["header"]["header"]["type"], 5)
        self.assertEqual(reliable["header"]["sessionKey"], 0x0BADF00D)
        self.assertEqual(reliable["header"]["sender"], 1)
        self.assertEqual(reliable["seq"], 3)
        self.assertEqual(self.read("EntityRemove", reliable["inner"]), {"type": 5, "id": 0x00010001})

    def test_an_entity_with_a_name_and_a_member(self):
        data = hexbytes("04 02 00 02 00 01 00 00 00 00 02 00 00 00"
                        "05 00 46 6C 65 65 74 FF FF 00 00 00 00 01 00 01 00 02 00")
        self.assertEqual(self.read("EntityAdd", data), {"type": 4, "entity": {
            "id": 0x00020002, "group": 1, "flags": 0, "owner": 2,
            "name": {"shortName": "Fleet", "longName": None}, "data": b"",
            "memberCount": 1, "members": [0x00020001]}})

    def test_the_other_inner_messages(self):
        self.assertEqual(self.read("SetName", hexbytes("07 01 00 02 00 02 00 58 4F FF FF")),
                         {"type": 7, "id": 0x00020001, "name": {"shortName": "XO", "longName": None}})
        self.assertEqual(self.read("SetData", hexbytes("06 01 00 02 00 01 00 00 00 AA")),
                         {"type": 6, "id": 0x00020001, "data": b"\xaa"})
        self.assertEqual(self.read("PeerAdd", hexbytes("02 03 00 00 00 03 02 00 C0 50 C3")),
                         {"type": 2, "peer": {"id": 3, "address": 0xC0000203, "port": 50000}})
        self.assertEqual(self.read("Data", hexbytes("0B 01 00 02 00 00 00 00 00 01 00 00 00 00 00 00 00")),
                         {"type": 11, "from": 0x00020001, "to": 0, "payload": hexbytes("01 00 00 00 00 00 00 00")})

    def test_a_join_accept_with_one_player(self):
        desc = (hexbytes("44 20 00 00") + INSTANCE + APPLICATION + hexbytes("08 00 00 00 01 00 00 00")
                + hexbytes("01 00 00 00 02 00 00 00 03 00 00 00 04 00 00 00")
                + hexbytes("08 00 54 65 73 74 47 61 6D 65 FF FF"))
        inner = (hexbytes("01 02 00 00 00 03 00 00 00") + desc + hexbytes("00 00 01 00")
                 + hexbytes("01 00 01 00 00 00 00 00 00 01 00 00 00")
                 + hexbytes("0B 00 54 65 73 74 43 61 70 74 61 69 6E FF FF")
                 + hexbytes("04 00 00 00 DE AD BE EF 00 00"))
        accept = self.read("JoinAccept", inner)
        self.assertEqual((accept["yourPeer"], accept["nextPeer"]), (2, 3))
        self.assertEqual(accept["desc"]["flags"], 0x2044)
        self.assertEqual(accept["desc"]["instance"], INSTANCE)
        self.assertEqual(accept["desc"]["application"], APPLICATION)
        self.assertEqual(accept["desc"]["maxPlayers"], 8)
        self.assertEqual([accept["desc"][f"user{i}"] for i in range(1, 5)], [1, 2, 3, 4])
        self.assertEqual((accept["desc"]["name"], accept["desc"]["password"]), ("TestGame", None))
        self.assertEqual((accept["peerCount"], accept["peers"], accept["entityCount"]), (0, [], 1))
        self.assertEqual(accept["entities"], [{
            "id": 0x00010001, "group": 0, "flags": 0, "owner": 1,
            "name": {"shortName": "TestCaptain", "longName": None},
            "data": hexbytes("DE AD BE EF"), "memberCount": 0, "members": []}])

    def test_the_walk_refuses_a_short_or_long_datagram(self):
        good = hexbytes("53 46 44 50 01 06 00 00 0D F0 AD 0B 02 00 00 00 05 00 00 00")
        self.assertEqual(self.read("Ack", good)["cumulative"], 5)
        with self.assertRaises(AssertionError):
            self.read("Ack", good[:-1])
        with self.assertRaises(AssertionError):
            self.read("Ack", good + b"\x00")


if __name__ == "__main__":
    unittest.main()
