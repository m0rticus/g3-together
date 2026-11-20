# SPDX-License-Identifier: GPL-3.0-or-later
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


TELEM_CPP_TARGET = _testbuild.register_target(
    "telemetry_schema_and_log",
    [
        _testbuild.unit("src/log.cpp", std="c++17", warnings="strict"),
        _testbuild.inline_unit(
            "telemetry_schema_runner",
            r'''
#include "telemetry.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace g3t::symbols {
bool GameDir(char (&)[260]) { return false; }
}

int main()
{
    using namespace g3t;
    using namespace g3t::telemetry::schema;

    // 1. KvWriter structured formatting, escaping, and bounded truncation safety.
    {
        char buf[512];
        KvWriter kv(buf, sizeof(buf));
        kv.Str("attacker", "Gorn")
          .Str("victim", "Orc Warrior Elite")
          .Str("empty", "")
          .Str("null_val", nullptr)
          .Str("quoted", "a\"b=c\nd")
          .TransitionInt("hp", 300, 215)
          .TransitionStr("slot", "It_Sword_01", "It_Sword_02")
          .Uint("seq", 42u)
          .Hex("epoch", 0xdeadbeefu)
          .Bool("lethal", false)
          .Pos("pos", 123.4f, -50.0f, 999.9f);

        std::string s(kv.c_str());
        assert(s.find("attacker=Gorn ") == 0);
        assert(s.find(" victim=\"Orc Warrior Elite\" ") != std::string::npos);
        assert(s.find(" empty=- ") != std::string::npos);
        assert(s.find(" null_val=- ") != std::string::npos);
        assert(s.find(" quoted=\"a'b=c d\" ") != std::string::npos);
        assert(s.find(" hp=300->215 ") != std::string::npos);
        assert(s.find(" slot=It_Sword_01->It_Sword_02 ") != std::string::npos);
        assert(s.find(" epoch=0xdeadbeef ") != std::string::npos);
        assert(s.find(" lethal=0 ") != std::string::npos);
        assert(s.find(" pos=123.4,-50.0,999.9") != std::string::npos);

        char tiny[20];
        KvWriter shortKv(tiny, sizeof(tiny));
        shortKv.Str("ok", "1").Str("", "no_key").Str("very_long_key_name", "very_long_value_ignored_safely");
        assert(std::strcmp(shortKv.c_str(), "ok=1") == 0);

        char quoteBuf[20];
        KvWriter quoteKv(quoteBuf, sizeof(quoteBuf));
        quoteKv.Str("k", "a b c d e f g h i j k");
        std::string qs(quoteKv.c_str());
        size_t quotes = 0;
        for (char c : qs) if (c == '"') ++quotes;
        assert(quotes == 0 || quotes == 2);
    }

    // 2. Monotonic sequence numbers and recent-telemetry ring buffer wraparound.
    {
        const uint64_t base = LastTelemetrySeq();
        for (int i = 0; i < 70; ++i) {
            const uint64_t seq = NextTelemetrySeq();
            char fields[64];
            KvWriter kv(fields, sizeof(fields));
            kv.Int("idx", i);
            LogTelemetryLine(seq, "test.ring", kv.c_str());
        }
        assert(LastTelemetrySeq() == base + 70);

        char recent[65536] = {};
        const size_t bytes = CopyRecentTelemetry(recent, sizeof(recent), 64);
        assert(bytes > 0);
        assert(std::strstr(recent, "idx=5\n") == nullptr);
        assert(std::strstr(recent, "idx=6") != nullptr);
        assert(std::strstr(recent, "idx=69") != nullptr);

        // Long baseline/loot lines (>2048 chars) are preserved intact without 2 KB truncation.
        std::string longPayload(3200, 'x');
        longPayload += "_ENDMARK";
        char longFields[3600];
        KvWriter longKv(longFields, sizeof(longFields));
        longKv.Str("blob", longPayload.c_str());
        LogTelemetryLine(NextTelemetrySeq(), "test.long", longKv.c_str());
        CopyRecentTelemetry(recent, sizeof(recent), 4);
        assert(std::strstr(recent, "_ENDMARK") != nullptr);
    }

    // 3. CausalStamp freshness window and structured attribution fields.
    {
        CausalStamp stamp;
        assert(!stamp.Fresh(1000, 2500));
        stamp.Record(17, 1000, "dialog.attack", "P2:Client", "Lukar", "Arena_Fangs");
        assert(stamp.Fresh(2200, 2500));
        assert(!stamp.Fresh(3600, 2500));

        char buf[256];
        KvWriter kv(buf, sizeof(buf));
        stamp.AppendTo(kv, 1250, 2500, "cause");
        std::string s(kv.c_str());
        assert(s.find("cause_seq=17") != std::string::npos);
        assert(s.find("cause_kind=dialog.attack") != std::string::npos);
        assert(s.find("cause_actor=P2:Client") != std::string::npos);
        assert(s.find("cause_detail=Arena_Fangs") != std::string::npos);
        assert(s.find("cause_age_ms=250") != std::string::npos);
    }

    // 4. GroundHitFilter suppresses 0-damage "complete" spam and duplicate episode blocks,
    //    while emitting every real HP delta and every distinct block transition.
    {
        GroundHitFilter filter;
        assert(!filter.ShouldEmit(100, 100, "GameHit", "complete", 5));
        assert(!filter.ShouldEmit(100, 100, "Filter", "", 5));
        assert(filter.ShouldEmit(100, 100, "GameHit", "blocked_held", 5));
        assert(!filter.ShouldEmit(100, 100, "GameHit", "blocked_held", 5));
        assert(filter.ShouldEmit(100, 100, "Filter", "blocked_held", 5));
        assert(filter.ShouldEmit(100, 100, "GameHit", "blocked_held", 6));
        assert(filter.ShouldEmit(100, 80, "GameHit", "complete", 6));
    }

    // 5. SlotTable tracks equipped slots through Body (17) and Helmet (18+) and diffs quality mutations.
    {
        SlotTable prev, cur;
        prev.Set(1, "It_1H_Sword_01", 0);
        prev.Set(17, "It_Armor_Merc_01", 0);
        cur.Set(1, "It_1H_Sword_01", 2);   // sharpened quality change
        cur.Set(2, "It_Shield_01", 0);     // newly equipped
        cur.Set(18, "It_Helmet_Crown_Very_Long_Template_Identifier_01", 256); // helmet slot >= 18
        // slot 17 unequipped

        int unprimedCount = 0;
        cur.DiffFrom(prev, [&](int, char const*, char const*, uint16_t, uint16_t) { ++unprimedCount; });
        assert(unprimedCount == 0);

        prev.primed = true;
        cur.primed = true;
        struct Change { int slot; std::string before; std::string after; uint16_t qb; uint16_t qa; };
        std::vector<Change> changes;
        cur.DiffFrom(prev, [&](int slot, char const* b, char const* a, uint16_t qb, uint16_t qa) {
            changes.push_back({slot, b, a, qb, qa});
        });
        assert(changes.size() == 4);
        assert(changes[0].slot == 1 && changes[0].qb == 0 && changes[0].qa == 2);
        assert(changes[1].slot == 2 && changes[1].before == "-" && changes[1].after == "It_Shield_01");
        assert(changes[2].slot == 17 && changes[2].before == "It_Armor_Merc_01" && changes[2].after == "-");
        assert(changes[3].slot == 18 && changes[3].after == "It_Helmet_Crown_Very_Long_Template_Identifier_01" && changes[3].qa == 256);
    }

    // 6. PeerSnap teleport and equipment transition detectors (including long template names > 40 chars).
    {
        PeerSnap a{}, b{};
        a.connected = b.connected = true;
        a.inWorld = b.inWorld = true;
        a.world = b.world = 0x12345678u;
        a.pos[0] = 0.0f; b.pos[0] = 500.0f;
        assert(!b.TeleportedFrom(a));
        b.pos[0] = 2500.0f;
        assert(b.TeleportedFrom(a));
        b.world = 0x87654321u;
        assert(!b.TeleportedFrom(a));

        Format(a.heldRight, "%s", "It_1H_Very_Long_Crafted_Weapon_Template_Prefix_Variant_A");
        Format(b.heldRight, "%s", "It_1H_Very_Long_Crafted_Weapon_Template_Prefix_Variant_B");
        assert(b.EquipChangedFrom(a));
        Format(b.heldRight, "%s", a.heldRight);
        assert(!b.EquipChangedFrom(a));
        Format(b.armor, "%s", "It_Armor_Paladin");
        assert(b.EquipChangedFrom(a));
    }

    // 7. QuestStatusLabel covers every Gothic 3 quest status.
    assert(std::strcmp(QuestStatusLabel(0), "Open") == 0);
    assert(std::strcmp(QuestStatusLabel(1), "Running") == 0);
    assert(std::strcmp(QuestStatusLabel(2), "Success") == 0);
    assert(std::strcmp(QuestStatusLabel(3), "Failed") == 0);
    assert(std::strcmp(QuestStatusLabel(4), "Obsolete") == 0);
    assert(std::strcmp(QuestStatusLabel(5), "Cancelled") == 0);
    assert(std::strcmp(QuestStatusLabel(6), "Lost") == 0);
    assert(std::strcmp(QuestStatusLabel(7), "Won") == 0);
    assert(std::strcmp(QuestStatusLabel(99), "Unknown") == 0);

    return 0;
}
''',
            std="c++17",
            warnings="strict",
        ),
    ],
)


class TelemetryTests(unittest.TestCase):
    def test_native_schema_and_log_ring(self):
        executed = TELEM_CPP_TARGET.run(timeout=20)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_all_src_files_remain_under_1000_lines(self):
        oversized = []
        for path in sorted((ROOT / "src").glob("*")):
            if path.suffix not in {".h", ".cpp"}:
                continue
            line_count = len(path.read_text(encoding="utf-8", errors="replace").splitlines())
            if line_count >= 1000:
                oversized.append((path.name, line_count))
        self.assertEqual(oversized, [], f"src/ files must stay strictly < 1,000 lines: {oversized}")

    def test_zero_gameplay_rate_limits_and_full_hook_wiring(self):
        telem_h = _testbuild.src_text("telemetry.h")
        telem_internal_h = _testbuild.src_text("telemetry_internal.h")
        telem_cpp = _testbuild.src_text("telemetry.cpp")
        telem_events_cpp = _testbuild.src_text("telemetry_events.cpp")
        trace_cpp = _testbuild.src_text("trace.cpp")

        for forbidden in ("RateLimit", "Throttle", "TokenBucket", "CreditCap"):
            self.assertNotIn(forbidden, telem_h)
            self.assertNotIn(forbidden, telem_internal_h)
            self.assertNotIn(forbidden, telem_cpp)
            self.assertNotIn(forbidden, telem_events_cpp)

        self.assertIn("telem=#%llu", trace_cpp)
        self.assertIn("CallerChain", trace_cpp)

        # Every declared telemetry entrypoint in telemetry.h must be wired at >= 1 call site outside telemetry*.cpp.
        other_sources = "\n".join(
            p.read_text(encoding="utf-8", errors="replace")
            for p in (ROOT / "src").glob("*.cpp")
            if not p.name.startswith("telemetry")
        )
        entrypoints = re.findall(r"^void\s+((?:On|Note|Tick)\w*)\s*\(", telem_h, flags=re.MULTILINE)
        self.assertGreaterEqual(len(entrypoints), 25)
        for fn in entrypoints:
            self.assertIsNotNone(
                re.search(rf"\btelemetry::{re.escape(fn)}\b", other_sources),
                f"telemetry::{fn} declared in telemetry.h has no call site in src/*.cpp",
            )
        self.assertIn("telemetry::TransferScope", other_sources)


if __name__ == "__main__":
    unittest.main()
