// SPDX-License-Identifier: GPL-3.0-or-later
// Test bot for disposable game copies (ported from the Myrtana evaluation helper).
// Menus are driven through the game's GUI API plus Enter posted to the game's own window, so the
// game handles each key in its normal message pump. No OS input, no address patterns.
#include "qa.h"
#include "log.h"
#include "qa_dispatch.h"

#include "ai.h"
#include "body.h"
#include "combat.h"
#include "hitprobe.h"
#include "story.h"
#include "address.h"
#include "p7probe.h"
#include "fightmem.h"
#include "partner.h"
#include "talk.h"
#include "items.h"
#include "containers.h"
#include "crashlog.h"
#include "trade.h"
#include "mapmarks.h"
#include "ransack.h"
#include "witness.h"
#include "guards.h"
#include "pickpocket.h"
#include "respawn.h"
#include "occupancy.h"
#include "dialogaudio.h"
#include "explore.h"
#include "npcs.h"
#include "npcplayback.h"
#include "offer.h"
#include "downed.h"
#include "transform.h"
#include "ranged.h"
#include "shot.h"
#include "spells.h"
#include "summon.h"
#include "hud.h"
#include "range.h"
#include "journal.h"
#include "persist.h"
#include "hero.h"
#include "clock.h"
#include "guide.h"
#include "arena.h"
#include "steam.h"
#include "ending.h"
#include "infocmd.h"
#include "xpshare.h"
#include "remote.h"
#include "trace.h"
#include "world.h"
#include "social.h"
#include "enclaves.h"
#include "doors.h"
#include "net.h"

#include <Game.h>
#include <Windows.h>
#include <winternl.h>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

namespace g3t::qa {
namespace {
bool enabled = false;
bool detached = false;
char root[MAX_PATH] = {};
DWORD lastPoll = 0;
unsigned lastSequence = 0;
WNDPROC originalWindowProc = nullptr;
HWND gameWindow = nullptr;
constexpr UINT commandMessage = WM_APP + 0x36A;
char pendingAction[32] = {};
char pendingArg[64] = {};
char argument[64] = {};
unsigned commandDepth = 0;
bool newGamePending = false;

bool Path(char (&path)[MAX_PATH], const char* file)
{
    return *root && Format(path, "%s\\%s", root, file) == static_cast<int>(std::strlen(root) + 1 + std::strlen(file));  // no cut path
}

void Log(const char* text)
{
    char path[MAX_PATH] = {};
    if (!Path(path, "qa-engine.log")) return;
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}

void Detach()
{
    detached = true;
    if (gameWindow && originalWindowProc)
        SetWindowLongPtrA(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalWindowProc));
}

// Gothic3.exe exits with code 0 while the named mutex GenomeMutex exists, so a second test copy can
// only start after this process closes its own handle to it.
using NtQueryObjectFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

bool MutexName(HANDLE handle, wchar_t (&name)[260])
{
    static const auto query = reinterpret_cast<NtQueryObjectFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryObject"));
    alignas(8) unsigned char buffer[1024] = {};
    ULONG size = 0;
    if (!query || query(handle, 2, buffer, sizeof(buffer), &size) < 0) return false;  // ObjectTypeInformation
    const auto* type = reinterpret_cast<const UNICODE_STRING*>(buffer);
    if (type->Length != 12 || std::wcsncmp(type->Buffer, L"Mutant", 6)) return false;
    if (query(handle, 1, buffer, sizeof(buffer), &size) < 0) return false;  // ObjectNameInformation
    const auto* text = reinterpret_cast<const UNICODE_STRING*>(buffer);
    if (!text->Length) return false;
    swprintf_s(name, L"%.*s", static_cast<int>(text->Length / 2), text->Buffer);
    return true;
}

void InstanceLocks(const wchar_t* release)
{
    char line[600] = {};
    for (ULONG_PTR value = 4; value < 0x10000; value += 4) {
        HANDLE handle = reinterpret_cast<HANDLE>(value);
        wchar_t name[260] = {};
        if (!MutexName(handle, name)) continue;
        const bool close = release && std::wcsstr(name, release);
        Format(line, "LOCK mutex handle=%#Ix name=%ls%s", value, name, close ? " -> released" : "");
        Log(line);
        if (close) CloseHandle(handle);
    }
    Log("LOCK scan complete");
}

void State(gCSession& session)
{
    char line[1024] = {};
    gCWorld* world = gCWorld::GetCurrentWorld();
    eCDynamicEntity* hero = session.GetHero();
    const bCString name = world ? world->GetFileBaseName() : bCString("none");
    const bCString folder = gCWorld::GetSaveGameFolder();
    Format(line, "STATE running=%d paused=%d save_allowed=%d world=%s hero=%s save_folder=%s", session.IsGameRunning(),
              session.IsPaused(), session.IsSaveAllowed(), name.GetText(), hero ? hero->GetName().GetText() : "none",
              folder.GetText());
    Log(line);
    if (hero) {
        const bCVector& p = hero->GetWorldPosition();
        Format(line, "POS x=%.1f y=%.1f z=%.1f", p.GetX(), p.GetY(), p.GetZ());
        Log(line);
    }
    CFFGFCWnd* focus = CFFGFCWnd::FromHandle(CFFGUIModule::GetInstance().GetFocusWnd());
    if (focus) {
        Format(line, "FOCUS id=%u text=%.512s", focus->GetDlgCtrlID(), focus->GetWindowTextA().GetAnsiText().GetText());
        Log(line);
    }
}

void InspectMenu()
{
    CFFGUIModule& gui = CFFGUIModule::GetInstance();
    std::vector<IFFGUIWindow*> seen;
    char info[768] = {};
    for (int y = 8; y < 768; y += 16) {
        for (int x = 8; x < 1024; x += 16) {
            CFFGFCWnd* window = CFFGFCWnd::FromHandle(gui.WindowFromPoint(bCPoint(x, y), 0));
            for (int depth = 0; window && depth < 8; ++depth, window = window->GetParentWindow()) {
                IFFGUIWindow* handle = window->GetHandle();
                bool known = false;
                for (auto* previous : seen) if (previous == handle) { known = true; break; }
                if (known) break;
                seen.push_back(handle);
                char text[512] = {};
                const bCUnicodeString caption = window->GetWindowTextA();
                WideCharToMultiByte(CP_UTF8, 0, caption.GetText(), -1, text, sizeof(text), nullptr, nullptr);
                Format(info, "MENU id=%u visible=%d enabled=%d sample=%d,%d depth=%d text=%s", window->GetDlgCtrlID(),
                          window->IsWindowVisible(), window->IsWindowEnabled(), x, y, depth, text);
                Log(info);
                for (unsigned id = 1; id < 22000; ++id) {
                    if (id == 256) id = 20000;
                    CFFGFCWnd* child = window->GetDlgItem(id);
                    if (!child || !child->IsWindowVisible()) continue;
                    Format(info, "MENU child id=%u enabled=%d text=%.512s", id, child->IsWindowEnabled(),
                              child->GetWindowTextA().GetAnsiText().GetText());
                    Log(info);
                }
            }
        }
    }
    Log("MENU scan complete (game GUI API; no OS input)");
}

// Focus a visible, enabled control with this id and caption. For the shared Yes button (20711) the
// dialog question (20709) must contain prompt.
bool Focus(unsigned id, const char* caption, const char* prompt = nullptr,
           const char* alternateCaption = nullptr, const char* alternatePrompt = nullptr)
{
    CFFGUIModule& gui = CFFGUIModule::GetInstance();
    char line[512] = {};
    for (int y = 8; y < 768; y += 16) {
        for (int x = 8; x < 1024; x += 16) {
            CFFGFCWnd* button = CFFGFCWnd::FromHandle(gui.WindowFromPoint(bCPoint(x, y), 0));
            if (!button) continue;
            if (button->GetDlgCtrlID() != id) button = button->GetDlgItem(id);
            if (!button || !button->IsWindowVisible() || !button->IsWindowEnabled()) continue;
            const bCString text = button->GetWindowTextA().GetAnsiText();
            if (std::strcmp(text.GetText(), caption) &&
                (!alternateCaption || std::strcmp(text.GetText(), alternateCaption))) continue;
            CFFGFCWnd* parent = button->GetParentWindow();
            if (!parent) return false;
            if (prompt) {
                CFFGFCWnd* question = parent->GetDlgItem(20709);
                if (!question) continue;
                const bCString questionText = question->GetWindowTextA().GetAnsiText();
                if (!std::strstr(questionText.GetText(), prompt) &&
                    (!alternatePrompt || !std::strstr(questionText.GetText(), alternatePrompt))) continue;
            }
            button->SetFocus();
            Format(line, "MENU_FOCUS id=%u focused=%d text=%s", id, button->HasFocus(), caption);
            Log(line);
            return button->HasFocus();
        }
    }
    Log("REFUSED menu command: exact visible/enabled control not found");
    return false;
}

void PostKey(UINT key, UINT scan)
{
    PostMessageA(gameWindow, WM_KEYDOWN, key, 1 | scan << 16);
    PostMessageA(gameWindow, WM_KEYUP, key, 1 | scan << 16 | 0xC0000000);
}

void Quest(gCSession& session, const char* action)
{
    char line[256] = {};
    gCQuestManager_PS* quests = gCQuestManager_PS::GetInstance();
    if (!quests || !*argument || !session.IsGameRunning()) { Log("REFUSED quest: no running game or quest id"); return; }
    const bCString id(argument);
    gCQuest_PS* quest = quests->GetQuest(id);
    if (!quest) { Format(line, "QUEST id=%s missing=1", argument); Log(line); return; }
    int changed = -1;
    if (!std::strcmp(action, "runquest")) changed = quests->RunQuest(id);
    else if (!std::strcmp(action, "succeedquest")) changed = quests->SucceedQuest(id);
    const gEQuestStatus status = static_cast<gEQuestStatus const&>(quest->AccessStatus());
    Format(line, "QUEST id=%s status=%d changed=%d", argument, static_cast<int>(status), changed);
    Log(line);
}

// QA (stageArtifactFound): the hero receives an item as a pickup or a dialog's Give does it: into the inventory, then the
// quest manager's OnReceiveItem (CreateItems alone fulfilled no quest that wants the hero to hold the item, 24.10.).
void ReceiveItem()
{
    char line[200];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    gCQuestManager_PS* quests = gCQuestManager_PS::GetInstance();
    const Template item(argument);
    if (!hero || !quests || !item.IsValid()) { Log("REFUSED receiveitem: no hero, no quest manager or no such item"); return; }
    const GEInt stack = Entity(hero).Inventory.CreateItems(item, 0, 1);
    bCPropertyID const& id = item;
    const GEBool told = quests->OnReceiveItem(nullptr, hero, id, 1);
    Format(line, "RECEIVEITEM item=%s stack=%d told=%d", argument, stack, told ? 1 : 0);
    Log(line);
}

// Codex review 22.10. (R1, R8, R9): a third chain, the other two are at MSVC's limit of nested blocks (C1061).
bool RunReviewAction(const char* action)
{
    if (!std::strcmp(action, "swimstrokeoff")) {
        g3t::body::swimStrokes = false;
        Log("SWIMSTROKEOFF ok=1 (a swimming puppet only glides)");
        return true;
    }
    if (!std::strcmp(action, "swimstrokes")) {
        char line[80];
        Format(line, "SWIMSTROKES on=%d played=%u", g3t::body::swimStrokes ? 1 : 0, g3t::body::swimStrokesPlayed);
        Log(line);
        return true;
    }
    if (!std::strcmp(action, "helloversion")) {  // stage W7: the next hello claims this protocol version
        g3t::net::ClaimVersionForTest(static_cast<uint16_t>(std::atoi(argument)));
        Log("HELLOVERSION ok=1");
        return true;
    }

    static constexpr QaTypedRoute routes[] = {
        {"chapter", g3t::explore::Chapter, nullptr},
        {"politicaloff", g3t::enclaves::PoliticalOff, nullptr},
        {"agressoroff", g3t::combat::AgressorOff, nullptr},
        {"cancelattack", nullptr, g3t::explore::CancelAttack},
        {"sector", nullptr, g3t::explore::Sector},
        {"quickuse", nullptr, g3t::explore::QuickUse},
        {"zoneatt", nullptr, g3t::explore::ZoneAttitude},
        {"attchain", nullptr, g3t::explore::AttitudeChain},
        {"setchapter", nullptr, g3t::explore::SetChapter},
        {"polmem", nullptr, g3t::explore::PolMem},
        {"enclavefame", nullptr, g3t::explore::EnclaveFame},
        {"enclavestatus", nullptr, g3t::explore::EnclaveStatus},
        {"chaptercopyoff", g3t::guards::ChapterCopyOff, nullptr},
        {"mirrorperceiveoff", g3t::witness::MirrorPerceiveOff, nullptr},
        {"scriptwatch", nullptr, g3t::explore::ScriptWatch},
        {"nativewatch", nullptr, g3t::explore::NativeWatch},
        {"sleepers", nullptr, g3t::explore::Sleepers},
        {"waitfree", nullptr, g3t::explore::WaitFree},
        {"animalsleep", nullptr, g3t::explore::AnimalSleep},
        {"swimgoal", nullptr, g3t::explore::SwimGoal},
        {"worldprocess", g3t::world::ProcessingReport, nullptr},
        {"streamprobe", nullptr, g3t::world::StreamProbe},
        {"passiveprobe", nullptr, g3t::npcplayback::Probe},
        {"passivestats", g3t::npcplayback::Stats, nullptr},
        {"movemode", g3t::explore::MoveMode, nullptr},
        {"chaptersyncoff", g3t::story::ChapterOff, nullptr},
        {"ending", g3t::ending::Report, nullptr},
        {"endingsyncoff", g3t::ending::Off, nullptr},
        {"endinghold", nullptr, g3t::ending::Hold},
        {"endinglimit", g3t::ending::Limit, nullptr},
        {"herocut", nullptr, g3t::hero::CutCommand},
        {"recordcheckoff", g3t::hero::CheckOff, nullptr},
        {"recordwritefault", g3t::hero::WriteFault, nullptr},
        {"storyzerosoff", g3t::story::ZerosOff, nullptr},
        {"storyreloadoff", g3t::story::ReloadOff, nullptr},
        {"hitnpcburst", nullptr, g3t::combat::HitNpcBurst},
        {"killonceoff", g3t::combat::KillOnceOff, nullptr},
        {"killconfirmoff", g3t::combat::KillConfirmOff, nullptr},
        {"killreport", g3t::combat::KillReport, nullptr},
        {"recordwaitoff", g3t::persist::RecordWaitOff, nullptr},
        {"recordwait", g3t::persist::Waiting, nullptr},
        {"mirrorretryoff", g3t::npcs::MirrorRetryOff, nullptr},
        {"mirrorstats", g3t::npcs::MirrorStats, nullptr},
        {"summonlatecopy", nullptr, g3t::summon::LateCopyCommand},
        {"skillreplaceoff", g3t::hero::SkillReplaceOff, nullptr},
        {"hasskill", nullptr, g3t::hero::HasSkill},
        {"skilldiag", g3t::hero::SkillDiag, nullptr},
        {"skilltry", g3t::hero::SkillTry, nullptr},
        {"exchangestate", g3t::offer::Report, nullptr},
        {"heroadd", nullptr, g3t::offer::AddCommand},
        {"herocount", nullptr, g3t::offer::CountCommand},
        {"menutext", nullptr, g3t::hud::Probe},
        {"menuline", g3t::hud::MenuLineReport, nullptr},
        {"stacks", nullptr, g3t::containers::Stacks},
        {"holditem", nullptr, g3t::containers::HoldItem},
        {"guide", nullptr, g3t::guide::Report},
        {"guidesyncoff", g3t::guide::Off, nullptr},
        {"farheal", nullptr, g3t::explore::FarHeal},
        {"farxpoff", g3t::npcs::FarXpOff, nullptr},
        {"important", nullptr, g3t::story::Important},
        {"runto", nullptr, g3t::explore::RunTo},
        {"setattr", nullptr, g3t::explore::SetAttribute},
        {"p7probeon", g3t::p7probe::On, nullptr},
        {"p7probe", g3t::p7probe::Report, nullptr},
        {"bothered", nullptr, g3t::social::Bothered},
        {"ownedobj", nullptr, g3t::social::OwnedObjects},
        {"fightmemory", nullptr, g3t::p7probe::FightMemory},
        {"crimeknown", nullptr, g3t::enclaves::SetKnown},
        {"crimesuspect", nullptr, g3t::enclaves::SetSuspect},
        {"fightmem", g3t::fightmem::Report, nullptr},
        {"grudgeoff", g3t::fightmem::ApartOff, nullptr},
        {"grudgeloadoff", g3t::fightmem::KeepOff, nullptr},
        {"formatprobe", nullptr, g3t::FormatProbe},
        {"formatstrict", g3t::FormatStrict, nullptr},
        {"fightmemoff", g3t::fightmem::Off, nullptr},
        {"zonefixoff", g3t::guards::ZoneFixOff, nullptr},
        {"fightsoff", g3t::guards::FightsOff, nullptr},
        {"sneakcheckon", g3t::guards::SneakCheckOn, nullptr},
        {"enterzoneon", g3t::guards::EnterZoneOn, nullptr},
        {"animalwakeoff", g3t::guards::AnimalWakeOff, nullptr},
        {"freedestoff", g3t::guards::FreeDestOff, nullptr},
        {"address", g3t::address::Report, nullptr},
        {"addressoff", g3t::address::Off, nullptr},
        {"addresson", g3t::address::On, nullptr},
    };
    return DispatchTypedRoute(action, argument, &Log, routes);
}

// Commands added after the chain below reached MSVC's limit of nested blocks (C1061).
// Phase 1b/2 (B, 23.10.): glide, what puppets wear and look like, talking and trading with them. Apart from the long chain
// in RunAction, which reached the compiler's nesting limit.
bool RunPlayerAction(const char* action)
{
    if (!std::strcmp(action, "receiveitem")) {
        ReceiveItem();
        return true;
    }
    if (!std::strcmp(action, "questdump")) {
        char path[MAX_PATH] = {};
        if (Path(path, "quests.tsv")) g3t::explore::QuestDump(path, &Log);
        return true;
    }

    static constexpr QaTypedRoute routes[] = {
        {"puppettradeon", nullptr, g3t::remote::PuppetTradeOn},
        {"puppetinfo", nullptr, g3t::remote::PuppetInfo},
        {"lookoff", g3t::remote::LookOff, nullptr},
        {"wear", nullptr, g3t::remote::WearHero},
        {"wornscan", g3t::remote::WornScan, nullptr},
        {"wearoff", g3t::remote::WearOff, nullptr},
        {"glidestopoff", g3t::body::GlideStopOff, nullptr},
        {"glideold", g3t::body::GlideOld, nullptr},
        {"glidereset", g3t::body::GlideReset, nullptr},
        {"glide", g3t::body::GlideReport, nullptr},
        {"tradesync", g3t::trade::Report, nullptr},
        {"autohelpon", g3t::downed::AutoHelpOn, nullptr},
        {"helpoptionoff", g3t::downed::HelpOptionOff, nullptr},
        {"tradeclose", g3t::trade::CloseCommand, nullptr},
        {"murdercand", nullptr, g3t::enclaves::MurderCandidates},
        {"creditqueueoff", g3t::enclaves::CreditQueueOff, nullptr},
        {"credithold", nullptr, g3t::enclaves::CreditHold},
        {"townstatusoff", g3t::enclaves::TownStatusOff, nullptr},
        {"townfarkill", nullptr, g3t::enclaves::TownFarKill},
        {"cheatkill", nullptr, g3t::enclaves::CheatKill},
        {"occupancy", g3t::occupancy::Report, nullptr},
        {"usesyncoff", g3t::occupancy::Off, nullptr},
        {"unuse", g3t::occupancy::StandUp, nullptr},
        {"npcroutine", nullptr, g3t::explore::NpcRoutine},
        {"findents", nullptr, g3t::world::FindEntities},
        {"arenasync", g3t::arena::Report, nullptr},
        {"arenasyncoff", g3t::arena::Off, nullptr},
        {"findall", nullptr, g3t::world::FindAll},
        {"weatherfollowoff", g3t::summon::WeatherFollowOff, nullptr},
        {"weather", g3t::summon::WeatherReport, nullptr},
        {"pickcands", g3t::pickpocket::Candidates, nullptr},
        {"pickinfo", nullptr, g3t::pickpocket::Info},
        {"pickpocket", nullptr, g3t::pickpocket::Pick},
        {"thiefprep", g3t::pickpocket::ThiefPrep, nullptr},
        {"pickpocketoff", g3t::pickpocket::Off, nullptr},
        {"picksync", g3t::pickpocket::Report, nullptr},
        {"remotemurdersoff", g3t::enclaves::RemoteMurdersOff, nullptr},
        {"shapehitsoff", g3t::combat::ShapeHitsOff, nullptr},
        {"remotekooff", g3t::combat::RemoteKnockOutsOff, nullptr},
        {"ransack", g3t::ransack::Report, nullptr},
        {"ransackoff", g3t::ransack::Off, nullptr},
        {"npcgold", nullptr, g3t::ransack::NpcGold},
        {"subtitlesoff", g3t::dialogaudio::SubtitlesOff, nullptr},
        {"witness", nullptr, g3t::witness::Saw},
        {"witnesses", g3t::witness::Report, nullptr},
        {"witnessoff", g3t::witness::Off, nullptr},
        {"murderseenoff", g3t::witness::SeenOff, nullptr},
        {"guards", g3t::guards::Report, nullptr},
        {"trespassoff", g3t::guards::Off, nullptr},
        {"sneak", nullptr, g3t::guards::Sneak},
        {"homes", nullptr, g3t::guards::Homes},
        {"assesshero", nullptr, g3t::explore::AssessHero},
        {"mapmarks", g3t::mapmarks::Report, nullptr},
        {"mapmarksoff", g3t::mapmarks::Off, nullptr},
        {"mapopen", g3t::mapmarks::OpenMap, nullptr},
        {"mapclose", g3t::mapmarks::CloseMap, nullptr},
        {"givequality", nullptr, g3t::trade::GiveQuality},
        {"qualitycount", nullptr, g3t::trade::QualityCount},
        {"tradesell", nullptr, g3t::containers::TradeSell},
        {"tradegift", nullptr, g3t::trade::GiftCommand},
        {"tradeprice", nullptr, g3t::trade::Price},
        {"tradefilloff", g3t::trade::FillOff, nullptr},
        {"playergoods", nullptr, g3t::trade::GoodsReport},
    };
    return DispatchTypedRoute(action, argument, &Log, routes);
}

// P4 hit judgement (25.10.): a fourth chain, RunLaterAction reached C1061 too.
bool RunHitAction(const char* action)
{
    if (!std::strcmp(action, "stanceoff")) {
        g3t::body::stanceSync = false;
        Log("STANCEOFF ok=1 (bodies keep their own AniState)");
        return true;
    }

    static constexpr QaTypedRoute routes[] = {
        {"hitprobe", nullptr, g3t::combat::HitProbe},
        {"puppetparadeoff", g3t::combat::PuppetParadeOff, nullptr},
        {"npcblockoff", g3t::combat::NpcBlockOff, nullptr},
        {"parade", g3t::combat::ParadeReport, nullptr},
        {"npcguard", nullptr, g3t::combat::NpcGuard},
        {"npcstill", nullptr, g3t::combat::NpcStill},
        {"puppetparade", nullptr, g3t::combat::PuppetParade},
        {"aicount", nullptr, g3t::ai::Count},
        {"partyenable", nullptr, g3t::infocmd::PartyEnableQa},
        {"puppetstate", g3t::combat::PuppetState, nullptr},
        {"npcparade", nullptr, g3t::combat::NpcParade},
        {"guard", nullptr, g3t::combat::Guard},
        {"storycompanionoff", g3t::infocmd::StoryCompanionOff, nullptr},
        {"steamstatus", g3t::steam::Status, nullptr},
        {"quickrestoreoff", g3t::hero::QuickRestoreOff, nullptr},
        {"quickassign", nullptr, g3t::hero::QuickAssign},
        {"quickdiag", g3t::hero::QuickDiag, nullptr},
        {"spellrestoreoff", g3t::hero::SpellRestoreOff, nullptr},
        {"spellknow", nullptr, g3t::hero::SpellKnow},
    };
    return DispatchTypedRoute(action, argument, &Log, routes);
}

bool RunLaterAction(const char* action)
{
    if (!std::strcmp(action, "npchp")) {
        // GUID_HP (P3) or Name_HP (stageA4, stageArenaQuest): the GUID form shadowed the name form since P3, Diego kept his
        // 1000 hit points and won the duel (stageA4 red, 25.10.).
        char const* cut = std::strrchr(argument, '_');
        if (cut && cut - argument == 36) g3t::spells::NpcHp(argument, &Log);
        else g3t::combat::NpcHp(argument, &Log);
        return true;
    }
    if (!std::strcmp(action, "scriptanioff")) {
        g3t::body::scriptAniReplay = false;
        Log("SCRIPTANIOFF gestures and knock-downs are replayed with StartPlayAni");
        return true;
    }
    if (!std::strcmp(action, "looprestartoff")) {
        g3t::body::loopRestart = false;
        Log("LOOPRESTARTOFF replayed loops play once");
        return true;
    }
    if (!std::strcmp(action, "spawnnpc")) {
        // "spawnnpc shared <template> <GUID>": the same creature on both machines (this chain is at the compiler's nesting limit)
        if (!std::strncmp(argument, "shared_", 7)) g3t::summon::SpawnShared(argument + 7, &Log);
        else g3t::spells::SpawnNpc(argument, &Log);
        return true;
    }
    if (!std::strcmp(action, "homingoff")) {
        g3t::ranged::homing = false;
        Log("HOMINGOFF shootat keeps the camera's aim");
        return true;
    }
    if (!std::strcmp(action, "walkby")) {
        g3t::transform::Walk(std::atoi(argument), &Log);
        return true;
    }
    if (!std::strcmp(action, "quickloot")) {  // "quickloot" queues the action, "quickloot count" only counts
        if (!std::strcmp(argument, "count")) g3t::containers::QuickLootActions(&Log);
        else g3t::containers::QuickLoot(&Log);
        return true;
    }

    static constexpr QaTypedRoute routes[] = {
        {"world", g3t::persist::WorldReport, nullptr},
        {"worldsyncoff", g3t::persist::WorldSyncOff, nullptr},
        {"respawnsync", g3t::respawn::Report, nullptr},
        {"respawnoff", g3t::respawn::Off, nullptr},
        {"movers", g3t::doors::Movers, nullptr},
        {"revolution", nullptr, g3t::social::Revolution},
        {"storymoveoff", g3t::infocmd::StoryMoveOff, nullptr},
        {"storymoves", g3t::infocmd::StoryMoves, nullptr},
        {"startrevolution", nullptr, g3t::social::StartRevolution},
        {"learnperk", nullptr, g3t::pickpocket::LearnPerk},
        {"gesturesoff", g3t::talk::GesturesOff, nullptr},
        {"optionsoff", g3t::dialogaudio::OptionsOff, nullptr},
        {"clockdays", nullptr, g3t::clock::AddDays},
        {"waitoff", g3t::clock::WaitOff, nullptr},
        {"reconnect", g3t::net::Reconnect, nullptr},
        {"generationoff", g3t::persist::GenerationOff, nullptr},
        {"persistpause", nullptr, g3t::persist::Pause},
        {"worldgatelate", g3t::persist::WorldGateLate, nullptr},
        {"loadticksoff", nullptr, g3t::persist::LoadTicksOff},
        {"dialogxpoff", g3t::infocmd::DialogXpOff, nullptr},
        {"givebackposoff", g3t::infocmd::GivebackPlaceOff, nullptr},
        {"duelhero", nullptr, g3t::explore::DuelHero},
        {"lethalonlyoff", g3t::downed::LethalOnlyOff, nullptr},
        {"arenaruleoff", g3t::combat::ArenaRuleOff, nullptr},
        {"arena", nullptr, g3t::combat::ArenaReport},
        {"xpinfos", g3t::explore::XpInfos, nullptr},
        {"forgetnpclist", g3t::remote::ForgetNpcListOn, nullptr},
        {"contactsoff", g3t::remote::ContactsOff, nullptr},
        {"badtransfer", g3t::persist::BadTransfer, nullptr},
        {"badrecord", g3t::npcs::BadRecord, nullptr},
        {"badstates", nullptr, g3t::remote::BadStates},
        {"reacthitsoff", g3t::combat::ReactHitsOff, nullptr},
        {"keeplocaloff", g3t::enclaves::KeepLocalOff, nullptr},
        {"enclavescanpause", nullptr, g3t::enclaves::PauseScan},
        {"crimeadd", nullptr, g3t::enclaves::AddCrime},
        {"npcsend", nullptr, g3t::npcs::SendInfo},
        {"joingraceoff", g3t::npcs::JoinGraceOff, nullptr},
        {"contestoff", g3t::npcs::ContestOff, nullptr},
        {"claimrecord", nullptr, g3t::npcs::ClaimRecord},
        {"edgenpcs", g3t::npcs::EdgeReport, nullptr},
        {"hostwinsoff", g3t::npcs::HostWinsOff, nullptr},
        {"doors", g3t::doors::Report, nullptr},
        {"doorsyncoff", g3t::doors::SyncOff, nullptr},
        {"owned", nullptr, g3t::social::Owned},
        {"enclavesync", g3t::enclaves::Report, nullptr},
        {"enclavesyncoff", g3t::enclaves::SyncOff, nullptr},
        {"unarm", g3t::social::Unarm, nullptr},
        {"enclaves", g3t::social::Enclaves, nullptr},
        {"crime", nullptr, g3t::social::Crime},
        {"usables", nullptr, g3t::world::Usables},
        {"face", nullptr, g3t::world::Face},
        {"use", nullptr, g3t::world::Use},
        {"unlock", nullptr, g3t::world::Unlock},
        {"useinfo", nullptr, g3t::world::UseInfo},
        {"attitude", nullptr, g3t::explore::Attitude},
        {"continueroutine", g3t::ai::ContinueRoutineReport, nullptr},
        {"continueroutineon", g3t::ai::ContinueRoutineOn, nullptr},
        {"movelength", g3t::ai::MoveLengthReport, nullptr},
        {"movelengthoff", g3t::ai::MoveLengthOff, nullptr},
        {"scriptfind", nullptr, g3t::ai::FindByRva},
        {"heropos", g3t::explore::HeroPos, nullptr},
        {"hitpuppet", nullptr, g3t::combat::HitPuppet},
        {"lookat", nullptr, g3t::explore::LookAt},
        {"deadposeoff", g3t::npcs::DeadPoseOff, nullptr},
        {"deadpose", g3t::npcs::DeadPoseReport, nullptr},
        {"trace", nullptr, g3t::trace::Command},
        {"partnerjump", g3t::partner::JumpQa, nullptr},
        {"partyxpoff", g3t::combat::PartyXpOff, nullptr},
        {"npcrecvon", g3t::npcs::RecvOn, nullptr},
        {"npcrecvoff", g3t::npcs::RecvOff, nullptr},
        {"fleekeepoff", g3t::ai::FleeKeepOff, nullptr},
        {"flee", nullptr, g3t::combat::FleeFrom},
        {"potionoff", g3t::downed::PotionOff, nullptr},
        {"givepotion", nullptr, g3t::downed::GivePotion},
        {"potions", g3t::downed::Potions, nullptr},
        {"transform", nullptr, g3t::transform::Start},
        {"transformback", g3t::transform::Back, nullptr},
        {"transformstate", g3t::transform::Report, nullptr},
        {"inventory", g3t::ranged::Inventory, nullptr},
        {"ready", nullptr, g3t::ranged::Ready},
        {"aimat", nullptr, g3t::ranged::Target},
        {"act", nullptr, g3t::ranged::Act},
        {"finishat", nullptr, g3t::ranged::FinishAt},
        {"shootat", nullptr, g3t::ranged::ShootAt},
        {"summonstate", g3t::summon::Report, nullptr},
        {"summoncopyhero", g3t::summon::CopyHeroOff, nullptr},
        {"creatures", g3t::summon::Creatures, nullptr},
        {"areatickon", g3t::summon::AreaTickOn, nullptr},
        {"summonmirroroff", g3t::summon::MirrorOff, nullptr},
        {"shotstate", g3t::shot::Report, nullptr},
        {"shotcopyhero", g3t::shot::CopyHeroOff, nullptr},
        {"spellstate", g3t::spells::Report, nullptr},
        {"spellmirroroff", g3t::spells::MirrorOff, nullptr},
        {"statusof", nullptr, g3t::spells::StatusOf},
        {"tickguardoff", g3t::spells::TickGuardOff, nullptr},
        {"party", g3t::spells::Party, nullptr},
        {"npcmemory", nullptr, g3t::spells::NpcMemory},
        {"npcgrudge", nullptr, g3t::spells::NpcGrudge},
        {"puppetguid", nullptr, g3t::spells::PuppetGuid},
        {"locks", g3t::spells::Locks, nullptr},
        {"sethp", nullptr, g3t::spells::SetHp},
        {"touchlog", g3t::shot::TouchLog, nullptr},
        {"shotmirroroff", g3t::shot::MirrorOff, nullptr},
        {"summons", g3t::ranged::Summons, nullptr},
        {"lastarrow", g3t::ranged::LastArrow, nullptr},
        {"castat", nullptr, g3t::ranged::CastAt},
        {"mana", g3t::ranged::Mana, nullptr},
        {"flying", g3t::ranged::Flying, nullptr},
        {"shapeoff", g3t::remote::ShapeOff, nullptr},
        {"dialogaudio", g3t::dialogaudio::Report, nullptr},
        {"mutegame", nullptr, g3t::dialogaudio::MuteGame},
        {"dialogaudiooff", g3t::dialogaudio::Disable, nullptr},
        {"spellfunc", nullptr, g3t::spells::SpellFunc},
    };
    return DispatchTypedRoute(action, argument, &Log, routes);
}

void RunConsole(const char* command)
{
    // The game's own console (gCSession::OnCheat: god, fullhealth, invisibility, idkfa, kill <name>, defeat <name>).
    if (!*command) { Log("REFUSED console: no command"); return; }
    if (!gCSession::GetInstance().IsGameRunning()) { Log("REFUSED console: no running game"); return; }
    const bCString answer = eCConsole::AccessConsole().Execute(bCString(command));
    char line[200] = {};
    Format(line, "CONSOLE %s -> %s", command, answer.GetText());
    Log(line);
}

void RunAction(const char* action)
{
    gCSession& session = gCSession::GetInstance();
    char line[256] = {};
    if (!std::strcmp(action, "console")) { RunConsole(argument); return; }
    if (!std::strcmp(action, "tutorialoff") || !std::strcmp(action, "baselineinfo")) {
        eCDynamicEntity* hero = session.GetHero();
        auto* memory = hero ? static_cast<gCPlayerMemory_PS*>(hero->GetPropertySet(eEPropertySetType_PlayerMemory)) : nullptr;
        gCGUIManager* manager = session.GetGUIManager();
        if (!memory || !manager) { Log("REFUSED baseline: no hero or GUI"); return; }
        const bool disable = !std::strcmp(action, "tutorialoff");
        if (disable) {
            memory->AccessHideTips() = GETrue;
            memory->EnableTutorial(static_cast<gETutorialFlag>(-1), GEFalse);
            if (manager->GetActivePage() == gEGamePage_Tutorial) manager->ClosePage();
        }
        Entity self(hero);
        Format(line, "%s hidden=%d page_open=%d menu_open=%d standing=%d falling=%d swimming=%d floor_y=%.1f",
               disable ? "TUTORIALOFF" : "BASELINEINFO", memory->GetHideTips() ? 1 : 0,
               manager->IsAnyPageOpen() ? 1 : 0, manager->IsMenuOpen() ? 1 : 0,
               self.CharacterMovement.IsStanding() ? 1 : 0, self.CharacterMovement.IsFalling() ? 1 : 0,
               self.CharacterMovement.IsSwimming() ? 1 : 0, self.CharacterMovement.GetFloorY());
        Log(line);
        return;
    }
    if (!std::strcmp(action, "consoleopen") || !std::strcmp(action, "consoleclose")) {
        eCConsole& console = eCConsole::AccessConsole();
        if (action[7] == 'o') console.Open(); else console.Close();
        Format(line, "CONSOLE open=%d", console.IsOpen() ? 1 : 0);
        Log(line);
        return;
    }
    if (!std::strcmp(action, "netstats")) { g3t::net::Stats(&Log); return; }
    if (!std::strcmp(action, "syncstats")) {
        g3t::combat::SyncReport(&Log);
        g3t::npcs::SyncReport(&Log);
        g3t::body::SyncReport(&Log);
        return;
    }
    if (!std::strcmp(action, "tradebegin")) { g3t::containers::TradeBegin(argument, &Log); return; }  // C1061: not in the chains
    if (!std::strcmp(action, "rttlogoff")) { g3t::net::RttLogOff(&Log); return; }
    if (!std::strcmp(action, "crashprobe")) { g3t::crashlog::Probe(&Log); return; }  // stageCrashNames
    if (!std::strcmp(action, "crashnamesoff")) { g3t::crashlog::NamesOff(&Log); return; }
    if (!std::strcmp(action, "motiontime")) { g3t::body::MotionTime(g3t::remote::ControlledBody(), &Log); return; }  // stageSeek
    if (!std::strcmp(action, "motionlag")) { g3t::body::MotionLagReport(&Log); return; }
    if (!std::strcmp(action, "seekon")) { g3t::body::motionSeek = 1; Log("SWITCH seekon ok=1"); return; }
    if (!std::strcmp(action, "seekoff")) { g3t::body::motionSeek = 0; Log("SWITCH seekoff ok=1"); return; }
    if (!std::strcmp(action, "interpoff")) { g3t::body::interpolate = false; Log("SWITCH interpoff ok=1"); return; }
    if (!std::strcmp(action, "interpon")) { g3t::body::interpolate = true; Log("SWITCH interpon ok=1"); return; }
    if (!std::strcmp(action, "walkrel")) { g3t::remote::WalkRel(argument, &Log); return; }  // stageWalk
    if (!std::strcmp(action, "npcwalk")) { g3t::combat::NpcWalk(argument, &Log); return; }
    if (!std::strcmp(action, "walking")) { g3t::npcs::WalkingReport(argument, &Log); return; }
    if (!std::strcmp(action, "npclife")) { g3t::npcs::LifeReport(argument, &Log); return; }
    if (!std::strcmp(action, "corpsesleep")) { g3t::npcs::CorpseSleep(argument, &Log); return; }
    if (!std::strcmp(action, "npcattack")) { g3t::combat::NpcAttack(argument, &Log); return; }
    if (!std::strcmp(action, "npcpin")) { g3t::combat::NpcPin(argument, &Log); return; }
    if (!std::strcmp(action, "playerkey")) { g3t::combat::PlayerKey(argument, &Log); return; }
    if (!std::strcmp(action, "npccast")) { g3t::shot::NpcCast(argument, &Log); return; }
    if (!std::strcmp(action, "actionseqon")) { g3t::body::actionSeq = true; Log("SWITCH actionseqon ok=1"); return; }
    if (!std::strcmp(action, "fallmoveon")) { g3t::body::fallMove = true; Log("SWITCH fallmoveon ok=1"); return; }
    if (!std::strcmp(action, "actionseqoff")) { g3t::body::actionSeq = false; Log("SWITCH actionseqoff ok=1"); return; }
    if (!std::strcmp(action, "smoothwindowoff")) { g3t::body::smoothWindow = false; Log("SWITCH smoothwindowoff ok=1"); return; }
    if (!std::strcmp(action, "interpstats")) {
        char interpLine[160];
        Format(interpLine, "INTERP between=%u ahead=%u held=%u on=%d", g3t::body::sampledBetween, g3t::body::sampledAhead,
               g3t::body::sampledHeld, g3t::body::interpolate ? 1 : 0);
        Log(interpLine);
        return;
    }
    if (g3t::npcs::Switch(action, &Log) || g3t::containers::Switch(action, &Log) || g3t::remote::Switch(action, &Log) ||
        g3t::combat::Switch(action, &Log) || g3t::dialogaudio::Switch(action, &Log) ||
        g3t::persist::Switch(action, &Log) || g3t::downed::Switch(action, &Log) || g3t::enclaves::Switch(action, &Log) ||
        g3t::items::Switch(action, &Log))
        return;
    if (RunHitAction(action) || RunLaterAction(action) || RunReviewAction(action) || RunPlayerAction(action)) return;
    if (!std::strcmp(action, "openinventory") || !std::strcmp(action, "closeinventory")) {
        gCGUIManager* manager = session.GetGUIManager();
        if (!session.IsGameRunning() || !manager) { Log("REFUSED inventory page: no game or GUI manager"); return; }
        const bool opening = !std::strcmp(action, "openinventory");
        const bool changed = opening ? manager->OpenInventory() : manager->ClosePage();
        Format(line, "INVENTORYPAGE changed=%d open=%d active=%d", changed ? 1 : 0, hud::InventoryOpen() ? 1 : 0,
               static_cast<int>(manager->GetActivePage()));
        Log(line);
        return;
    }
    if (!std::strcmp(action, "state")) {
        State(session);
    } else if (!std::strcmp(action, "menu")) {
        InspectMenu();
    } else if (!std::strcmp(action, "focusnew")) {
        if (session.IsGameRunning()) { Log("REFUSED focusnew: already running"); return; }
        newGamePending = Focus(20571, "Neues Spiel", nullptr, "New Game");
    } else if (!std::strcmp(action, "focusyes")) {
        if (!newGamePending || session.IsGameRunning()) { Log("REFUSED focusyes: no pending new game"); return; }
        Focus(20711, "Ja", "ein neues Spiel", "Yes", "new game");
    } else if (!std::strcmp(action, "focussave")) {
        if (!session.IsGameRunning() || !session.IsSaveAllowed()) { Log("REFUSED focussave: game not save-ready"); return; }
        Focus(20574, "Spiel speichern");
    } else if (!std::strcmp(action, "focusload")) {
        Focus(20573, "Spiel laden");
    } else if (!std::strcmp(action, "focusquit")) {
        Focus(20720, "Spiel beenden");
    } else if (!std::strcmp(action, "focusquityes")) {
        Focus(20711, "Ja", "beenden");
    } else if (!std::strcmp(action, "pressenter")) {
        // Only for known menu buttons that a previous focus command selected.
        CFFGFCWnd* focus = CFFGFCWnd::FromHandle(CFFGUIModule::GetInstance().GetFocusWnd());
        const unsigned id = focus ? focus->GetDlgCtrlID() : 0;
        if (id != 20571 && id != 20711 && id != 20573 && id != 20574 && id != 20720) {
            Format(line, "REFUSED pressenter: focus id=%u is not an allowed menu button", id); Log(line); return;
        }
        PostKey(VK_RETURN, 0x1C);
        Format(line, "PRESSENTER focus id=%u queued=1 (game window message queue, no OS input)", id);
        Log(line);
    } else if (!std::strcmp(action, "openmenu")) {
        if (!session.IsGameRunning()) { Log("REFUSED openmenu: no game running"); return; }
        session.OpenMenu();
        Log("OPENMENU returned");
    } else if (!std::strcmp(action, "closewindow")) {
        // Same path as the title-bar close button; detach on WM_DESTROY.
        Log("CLOSEWINDOW queue SC_CLOSE; detach on WM_DESTROY");
        PostMessageA(gameWindow, WM_SYSCOMMAND, SC_CLOSE, 0);
    } else if (!std::strcmp(action, "listlocks")) {
        InstanceLocks(nullptr);
    } else if (!std::strcmp(action, "releaselock")) {
        InstanceLocks(L"\\BaseNamedObjects\\GenomeMutex");
    } else if (!std::strcmp(action, "remotes")) {
        g3t::remote::Report(&Log);
        Log("REMOTES end");
    } else if (!std::strcmp(action, "hitnpc")) {
        g3t::combat::HitNpc(argument, &Log);
    } else if (!std::strcmp(action, "gamestr")) {
        g3t::combat::GameString(argument, &Log);
    } else if (!std::strcmp(action, "hitpuppet")) {
        g3t::combat::HitPuppet(std::atoi(argument), &Log);
    } else if (!std::strcmp(action, "arm")) {
        g3t::combat::Arm(argument, &Log);
    } else if (!std::strcmp(action, "combatoff")) {
        g3t::combat::Uninstall(&Log);
    } else if (!std::strcmp(action, "story")) {
        g3t::story::Report(&Log);
    } else if (!std::strcmp(action, "storyoff")) {
        g3t::story::Disable(&Log);
    } else if (!std::strcmp(action, "info") || !std::strcmp(action, "giveinfo")) {
        g3t::story::Info(argument, !std::strcmp(action, "giveinfo"), &Log);
    } else if (!std::strcmp(action, "items")) {
        g3t::items::List(&Log);
    } else if (!std::strcmp(action, "itemtake")) {
        g3t::items::Take(argument, &Log);
    } else if (!std::strcmp(action, "itemdecay")) {
        g3t::items::Decay(argument, &Log);
    } else if (!std::strcmp(action, "itempick")) {
        g3t::items::Pick(argument, &Log);
    } else if (!std::strcmp(action, "npcsummary")) {
        g3t::explore::Summary(&Log);
    } else if (!std::strcmp(action, "teleport")) {
        g3t::explore::Teleport(argument, &Log);
    } else if (!std::strcmp(action, "nativesave")) {
        g3t::explore::NativeSave(argument, &Log);
    } else if (!std::strcmp(action, "nativeload")) {
        g3t::explore::NativeLoad(argument, &Log);
    } else if (!std::strcmp(action, "page")) {
        g3t::explore::Page(argument, &Log);
    } else if (!std::strcmp(action, "teleportto")) {
        g3t::explore::TeleportTo(argument, &Log);
    } else if (!std::strcmp(action, "findnpc")) {
        g3t::explore::Find(argument, &Log);
    } else if (!std::strcmp(action, "farkill")) {
        g3t::explore::FarKill(argument, &Log);
    } else if (!std::strcmp(action, "farmove")) {
        g3t::explore::FarMove(argument, &Log);
    } else if (!std::strcmp(action, "farnpcs")) {
        g3t::explore::FarNpcs(&Log);
    } else if (!std::strcmp(action, "near")) {
        g3t::explore::Near(argument, &Log);
    } else if (!std::strcmp(action, "contents")) {
        g3t::containers::Contents(argument, &Log);
    } else if (!std::strcmp(action, "lootopen")) {
        g3t::containers::Open(argument, &Log);
    } else if (!std::strcmp(action, "loottake")) {
        g3t::containers::Take(argument, &Log);
    } else if (!std::strcmp(action, "tradeopen")) {
        g3t::containers::TradeOpen(argument, &Log);
    } else if (!std::strcmp(action, "tradebuy")) {
        g3t::containers::TradeBuy(argument, &Log);
    } else if (!std::strcmp(action, "stock")) {
        g3t::containers::Stock(argument, &Log);
    } else if (!std::strcmp(action, "goods")) {
        g3t::containers::Goods(argument, &Log);
    } else if (!std::strcmp(action, "invstate")) {
        g3t::containers::InvState(argument, &Log);
    } else if (!std::strcmp(action, "chests")) {
        g3t::containers::Chests(&Log);
    } else if (!std::strcmp(action, "lootsilent")) {
        g3t::containers::Silent(argument, &Log);
    } else if (!std::strcmp(action, "containersync")) {
        g3t::containers::Report(&Log);
    } else if (!std::strcmp(action, "containersyncoff")) {
        g3t::containers::Disable(argument, &Log);
    } else if (!std::strcmp(action, "containers")) {
        g3t::items::Containers(&Log);
    } else if (!std::strcmp(action, "invcount")) {
        g3t::items::Count(argument, &Log);
    } else if (!std::strcmp(action, "downedoff")) {
        g3t::downed::Off(&Log);
    } else if (!std::strcmp(action, "herodump")) {
        g3t::hero::Dump(&Log);
    } else if (!std::strcmp(action, "herosave")) {
        g3t::hero::SaveCommand(argument, &Log);
    } else if (!std::strcmp(action, "heroload")) {
        g3t::hero::LoadCommand(argument, &Log);
    } else if (!std::strcmp(action, "herorestoreoff")) {
        g3t::hero::RestoreOff(&Log);
    } else if (!std::strcmp(action, "persist")) {
        g3t::persist::Report(&Log);
    } else if (!std::strcmp(action, "event") || !std::strcmp(action, "setevent") || !std::strcmp(action, "clearevent")) {
        g3t::story::Event(action, argument, &Log);
    } else if (!std::strcmp(action, "infodesc")) {
        g3t::explore::InfoDescribe(argument, &Log);
    } else if (!std::strcmp(action, "companionsoff")) {
        g3t::npcs::CompanionsOff(&Log);
    } else if (!std::strcmp(action, "questcountoff")) {
        g3t::journal::QuestCountOff(&Log);
    } else if (!std::strcmp(action, "questcounters")) {
        g3t::explore::QuestCounters(argument, &Log);
    } else if (!std::strcmp(action, "styleoff")) {
        g3t::npcs::StyleOff(&Log);
    } else if (!std::strcmp(action, "puppetholdoff")) {
        g3t::remote::PuppetHoldOff(&Log);
    } else if (!std::strcmp(action, "puppetcombatoff")) {
        g3t::remote::PuppetCombatOff(&Log);
    } else if (!std::strcmp(action, "swing")) {
        g3t::combat::Swing(argument, &Log);
    } else if (!std::strcmp(action, "puppetpartyoff")) {
        g3t::remote::PuppetPartyOff(&Log);
    } else if (!std::strcmp(action, "attitudeoff")) {
        g3t::ai::AttitudeOff(&Log);
    } else if (!std::strcmp(action, "combatreplayoff")) {
        g3t::body::combatReplay = false;
        Log("COMBATREPLAYOFF attacks are replayed by motion name");
    } else if (!std::strcmp(action, "motionowner")) {
        g3t::body::motionOwner = std::atoi(argument);
        char text[64];
        Format(text, "MOTIONOWNER %d", g3t::body::motionOwner);
        Log(text);
    } else if (!std::strcmp(action, "nearnpcs")) {
        g3t::explore::NearNpcs(&Log);
    } else if (!std::strcmp(action, "npcfight")) {
        g3t::explore::NpcFight(argument, &Log);
    } else if (!std::strcmp(action, "npchitsoff")) {
        g3t::combat::NpcHitsOff(&Log);
    } else if (!std::strcmp(action, "aggro")) {
        g3t::explore::Aggro(argument, &Log);
    } else if (!std::strcmp(action, "talkinfo")) {
        g3t::talk::ChooseInfo(argument, &Log);
    } else if (!std::strcmp(action, "infoscan")) {
        g3t::explore::InfoScan(argument, &Log);
    } else if (!std::strcmp(action, "join")) {
        g3t::infocmd::JoinQa(argument, &Log);
    } else if (!std::strcmp(action, "infocmdoff")) {
        g3t::infocmd::Off(&Log);
    } else if (!std::strcmp(action, "npcstate") || !std::strcmp(action, "simdialog")) {
        g3t::explore::NpcState(argument, !std::strcmp(action, "simdialog"), &Log);
    } else if (!std::strcmp(action, "infocmd")) {
        g3t::infocmd::Report(&Log);
    } else if (!std::strcmp(action, "clock")) {
        g3t::clock::Report(&Log);
    } else if (!std::strcmp(action, "clockset")) {
        g3t::clock::Set(argument, &Log);
    } else if (!std::strcmp(action, "clockoff")) {
        g3t::clock::Off(&Log);
    } else if (!std::strcmp(action, "campaignoff")) {
        g3t::persist::CampaignOff(&Log);
    } else if (!std::strcmp(action, "persistoff")) {
        g3t::persist::Off(&Log);
    } else if (!std::strcmp(action, "journal")) {
        g3t::journal::Report(&Log);
    } else if (!std::strcmp(action, "journaloff")) {
        g3t::journal::Off(&Log);
    } else if (!std::strcmp(action, "jentry")) {
        g3t::journal::Entry(argument, &Log);
    } else if (!std::strcmp(action, "range")) {
        g3t::range::Report(&Log);
    } else if (!std::strcmp(action, "rangeoff")) {
        g3t::range::Off(&Log);
    } else if (!std::strcmp(action, "holdoff")) {
        g3t::downed::HoldOff(&Log);
    } else if (!std::strcmp(action, "hud")) {
        g3t::hud::Report(&Log);
    } else if (!std::strcmp(action, "downed")) {
        g3t::downed::Report(&Log);
    } else if (!std::strcmp(action, "hitpuppetby")) {
        g3t::combat::HitPuppetBy(argument, &Log);
    } else if (!std::strcmp(action, "killhero")) {
        g3t::combat::KillHero(argument, &Log);
    } else if (!std::strcmp(action, "herostate")) {
        g3t::combat::HeroState(&Log);
    } else if (!std::strcmp(action, "hurthero")) {
        g3t::combat::HurtHero(argument, &Log);
    } else if (!std::strcmp(action, "givexp")) {
        if (!std::strcmp(argument, "report")) g3t::xpshare::Report(&Log);  // XPSHARE line (no further else-if: compiler nesting limit)
        else g3t::combat::GiveXp(argument, &Log);
    } else if (!std::strcmp(action, "ownoff")) {
        g3t::npcs::OwnOff(&Log);
    } else if (!std::strcmp(action, "xpguardoff")) {
        g3t::npcs::XpGuardOff(&Log);
    } else if (!std::strcmp(action, "itemsyncoff")) {
        g3t::items::Disable(&Log);
    } else if (!std::strcmp(action, "itemtook")) {
        g3t::items::TookReport(&Log);
    } else if (!std::strcmp(action, "iteminfo")) {
        g3t::items::Describe(argument, &Log);
    } else if (!std::strcmp(action, "talkchoose")) {
        g3t::talk::Choose(std::atoi(argument), &Log);
    } else if (!std::strcmp(action, "talksync")) {
        g3t::talk::Report(&Log);
    } else if (!std::strcmp(action, "talkholdoff")) {
        g3t::talk::DisableHold(&Log);
    } else if (!std::strcmp(action, "assesstalk")) {
        g3t::story::AssessTalk(argument, &Log);
    } else if (!std::strcmp(action, "approach")) {
        g3t::story::Approach(argument, &Log);
    } else if (!std::strcmp(action, "killraiders")) {
        g3t::story::KillRaiders(&Log);
    } else if (!std::strcmp(action, "talk")) {
        g3t::story::Talk(argument, &Log);
    } else if (!std::strcmp(action, "talkstate")) {
        g3t::story::TalkState(&Log);
    } else if (!std::strcmp(action, "endtalk")) {
        g3t::story::EndTalk(&Log);
    } else if (!std::strcmp(action, "infolist")) {
        g3t::story::InfoCandidates(&Log);
    } else if (!std::strcmp(action, "hitstacks")) {
        g3t::hitprobe::Report(&Log);
    } else if (!std::strcmp(action, "hp")) {
        g3t::combat::Report(&Log);
    } else if (!std::strcmp(action, "names")) {
        g3t::ai::ListNames(argument, &Log);
    } else if (!std::strcmp(action, "scriptdump")) {
        char path[MAX_PATH] = {};
        if (Path(path, "scripts.tsv")) g3t::ai::DumpNames(path, &Log);
    } else if (!std::strcmp(action, "scriptinfo")) {
        g3t::ai::Describe(argument, &Log);
    } else if (!std::strcmp(action, "npcs")) {
        g3t::npcs::Report(&Log);
        Log("NPCS end");
    } else if (!std::strcmp(action, "walkto")) {
        g3t::remote::WalkTo(std::atoi(argument), &Log);
    } else if (!std::strcmp(action, "step")) {
        g3t::remote::Step(std::atoi(argument), &Log);
    } else if (!std::strcmp(action, "quest") || !std::strcmp(action, "runquest") || !std::strcmp(action, "succeedquest")) {
        Quest(session, action);
    } else {
        Log("REFUSED unknown command");
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_DESTROY) {
        Log("DESTROY window close accepted; detach QA callbacks");
        const WNDPROC next = originalWindowProc;
        Detach();
        return CallWindowProcA(next, window, message, wparam, lparam);
    }
    if (message == commandMessage) {
        char action[32] = {};
        strcpy_s(action, pendingAction);
        strcpy_s(argument, pendingArg);
        *pendingAction = '\0';
        *pendingArg = '\0';
        ++commandDepth;
        __try { RunAction(action); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            detached = true;
            Log("FAIL native QA command exception; commands disabled");
        }
        --commandDepth;
        return 0;
    }
    return CallWindowProcA(originalWindowProc, window, message, wparam, lparam);
}
}

bool Enabled() { return enabled && !detached; }

void Init()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&Log), &self))
        return;
    const DWORD length = GetModuleFileNameA(self, root, MAX_PATH);
    if (!length || length >= MAX_PATH) { *root = '\0'; return; }
    for (int part = 0; part != 2; ++part) {
        char* separator = std::strrchr(root, '\\');
        if (!separator) { *root = '\0'; return; }
        *separator = '\0';
    }
    char flag[MAX_PATH] = {};
    enabled = Path(flag, "qa-enable.txt") && GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES;
    if (enabled) Log("INIT G3 Together QA bot: opt-in disposable-install test");
}

void Tick()
{
    if (!Enabled()) return;
    g3t::ranged::Tick();
    g3t::explore::SwimTick();
    const DWORD now = GetTickCount();
    if (now - lastPoll < 250 || commandDepth || *pendingAction) return;
    lastPoll = now;
    if (!originalWindowProc) {
        gameWindow = eCApplication::GetInstance().GetHandle();
        if (!gameWindow) return;
        originalWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
        if (!originalWindowProc) { detached = true; Log("FAIL cannot register deferred command dispatcher"); return; }
        Log("READY deferred engine commands outside the active simulation frame");
    }
    FILE* file = nullptr;
    char path[MAX_PATH] = {};
    if (!Path(path, "qa-command.txt") || fopen_s(&file, path, "rb") || !file) return;
    char text[128] = {};
    const bool got = fgets(text, sizeof(text), file) != nullptr;
    fclose(file);
    if (!got) return;
    unsigned sequence = 0;
    char action[32] = {};
    char arg[64] = {};
    const int fields = sscanf_s(text, "%u %31s %63s", &sequence, action, static_cast<unsigned>(sizeof(action)), arg,
                                static_cast<unsigned>(sizeof(arg)));
    if (fields < 2 || sequence <= lastSequence) return;
    lastSequence = sequence;
    for (const char* c = arg; *c; ++c) {
        if (!(std::isalnum(static_cast<unsigned char>(*c)) || *c == '_' || *c == '-')) { Log("REFUSED argument: only [A-Za-z0-9_-]"); return; }
    }
    char line[160] = {};
    Format(line, "COMMAND sequence=%u action=%s%s%s thread=%lu", sequence, action, *arg ? " " : "", arg, GetCurrentThreadId());
    Log(line);
    strcpy_s(pendingAction, action);
    strcpy_s(pendingArg, arg);
    if (!PostMessageA(gameWindow, commandMessage, 0, 0)) { detached = true; Log("FAIL cannot queue deferred command"); }
}
}
