// SPDX-License-Identifier: GPL-3.0-or-later
// G3 Together: co-op for Gothic 3, built on the unofficial Gothic 3 SDK.
// Runs once per game frame on the engine thread: reports the local hero, exchanges player states
// with the other players and shows them as puppets.
#include "ai.h"
#include "combat.h"
#include "combat_hostility.h"
#include "net.h"
#include "npcremoval.h"
#include "story.h"
#include "talk.h"
#include "downed.h"
#include "range.h"
#include "journal.h"
#include "persist.h"
#include "clock.h"
#include "guide.h"
#include "arena.h"
#include "ending.h"
#include "infocmd.h"
#include "partner.h"
#include "items.h"
#include "containers.h"
#include "trade.h"
#include "mapmarks.h"
#include "ransack.h"
#include "witness.h"
#include "guards.h"
#include "pickpocket.h"
#include "respawn.h"
#include "occupancy.h"
#include "address.h"
#include "fightmem.h"
#include "dialogaudio.h"
#include "config.h"
#include "trace.h"
#include "doors.h"
#include "enclaves.h"
#include "log.h"
#include "npcs.h"
#include "relation.h"
#include "npcplayback.h"
#include "npclease_runtime.h"
#include "offer.h"
#include "remote.h"
#include "ranged.h"
#include "crashlog.h"
#include "shot.h"
#include "spells.h"
#include "summon.h"
#include "transform.h"
#include "world.h"
#include "hud.h"
#include "i18n.h"
#include "xpshare.h"
#include "movement.h"
#include "body.h"
#include "guid.h"
#include "focus_diag.h"
#include "host_frame_diag.h"
#include "telemetry.h"

#include <Game.h>
#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
unsigned long long frames = 0;
DWORD lastReport = 0;
bool faulted = false;
bool playbackErrorShown = false;
unsigned moduleProcessDepth = 0;

void ResetFocusObservation() { g3t::focus_diag::Reset(); }

void ObserveFocus()
{
    const uint32_t now = GetTickCount();
    const bool focused = g3t::partner::Focused();
    auto& session = gCSession::GetInstance();
    const bool paused = session.IsPaused();
    const bool inWorld = session.IsGameRunning() && !g3t::persist::NativeLoading() && !g3t::persist::HeroPending();
    const auto header = g3t::focus_diag::AdvanceFrame(now, focused, paused, inWorld);
    if (header.lost || header.regained || header.resumed || header.longGap) {
        eCEntity* instance = session.GetHero();
        Entity hero(instance);
        const bool routine = instance && instance->GetPropertySet(eEPropertySetType_ScriptRoutine);
        g3t::Logf("focus: %s role=%s serial=%u gap_ms=%u away_ms=%u native_dt_ms=%.1f paused=%d "
                  "task=%.48s state=%.48s ai=%d ani=%d",
                  header.lost ? "lost" : header.regained ? "regained" : header.resumed ? "pause-resumed" : "frame-gap",
                  g3t::net::IsHost() ? "host" : "client", header.serial, header.gap, header.awayMs,
                  eCApplication::GetInstance().GetScaledFrameTimeInSeconds() * 1000.0f, paused ? 1 : 0,
                  routine ? hero.Routine.GetCurrentTask().GetText() : "-",
                  routine ? hero.Routine.GetCurrentState().GetText() : "-",
                  routine ? static_cast<int>(hero.Routine.AIMode) : -1,
                  routine ? static_cast<int>(hero.Routine.AniState) : -1);
        if (header.regained || header.resumed || header.longGap) g3t::downed::OnFrameResume();
    }
    // Sample world NPCs only on foreground transition edges, post-resume watch windows, or sparse baseline refresh.
    if (!header.sampleDue) return;
    g3t::focus_diag::BeginSample(now);
    const auto all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity npc = all.GetAt(i);
        eCEntity* entity = npc.GetInstance();
        if (!entity || entity == session.GetHero() || g3t::remote::IsPuppet(entity) ||
            !entity->GetPropertySet(eEPropertySetType_ScriptRoutine)) continue;
        char identity[40] = {}, motion[256] = {};
        g3t::guid::FormatEntity(entity, identity, sizeof(identity));
        g3t::body::CurrentMotion(entity, motion, sizeof(motion));
        const g3t::focus_diag::NpcSample sample{
            reinterpret_cast<uintptr_t>(entity),
            static_cast<int>(gCEntity::GetCombatMode(entity)),
            static_cast<int>(npc.Routine.AIMode),
            g3t::body::MotionFraction(entity),
            identity,
            motion,
        };
        const auto change = g3t::focus_diag::RecordNpc(sample);
        if (change.logDetail)
            g3t::Logf("focus: npc serial=%u guid=%s name=%.48s weapon=%d->%d ai=%d->%d draw=%d motion=%.96s",
                      header.serial, identity, npc.GetName().GetText(), change.prevWeapon, sample.weapon,
                      change.prevAi, sample.ai, change.draw ? 1 : 0, motion);
    }
    g3t::focus_diag::SettleReport settled{};
    if (g3t::focus_diag::FinishSample(now, settled))
        g3t::Logf("focus: settled serial=%u window_ms=%u sampled_npcs=%zu weapon_changed=%zu ai_changed=%zu "
                  "draw_motion=%zu actor_replacements=%u held=%d",
                  settled.serial, settled.windowMs, settled.sampledNpcs,
                  settled.weaponChanged, settled.aiChanged, settled.drawMotion,
                  settled.actorReplacements, g3t::downed::Held() ? 1 : 0);
}

// Yaw in degrees around the up axis (Y in Gothic 3).
float Yaw(bCQuaternion const& q)
{
    const float x = q.GetX(), y = q.GetY(), z = q.GetZ(), w = q.GetW();
    return std::atan2(2.0f * (w * y + x * z), 1.0f - 2.0f * (y * y + x * x)) * 57.2957795f;
}

void Report()
{
    gCSession& session = gCSession::GetInstance();
    gCWorld* currentWorld = gCWorld::GetCurrentWorld();
    eCDynamicEntity* hero = session.GetHero();
    const bCString worldName = currentWorld ? currentWorld->GetFileBaseName() : bCString("none");
    if (!hero || !session.IsGameRunning()) {
        g3t::Logf("frame=%llu world=%s running=%d hero=none", frames, worldName.GetText(), session.IsGameRunning());
        return;
    }
    const bCVector& p = hero->GetWorldPosition();
    g3t::Logf("frame=%llu world=%s running=1 hero=%s pos=%.1f,%.1f,%.1f yaw=%.1f", frames, worldName.GetText(),
              hero->GetName().GetText(), p.GetX(), p.GetY(), p.GetZ(), Yaw(hero->GetWorldRotation()));
}

void Frame()
{
    if (faulted) return;
    __try {
        ObserveFocus();
        ++frames;
        const DWORD now = GetTickCount();
        if (now - lastReport >= 1000) {
            lastReport = now;
            Report();
        }
        if (g3t::GetLocalConfig().host) {
            g3t::host_frame_diag::Summary perf{};
            if (g3t::host_frame_diag::Global().PollHeartbeat(g3t::net::Now(), perf)) {
                const auto sim = g3t::world::HostSimulationStatus();
                char line[512] = {};
                g3t::host_frame_diag::FormatHeartbeat(line, sizeof(line), perf, sim.spheres, sim.processed);
                g3t::Log(line);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        g3t::Log("FAIL exception in frame callback; mod disabled for this session");
    }
}

template <typename Fn>
void Step(char const* name, Fn&& tick)
{
    g3t::movement::Stage stage(name);
    tick();
}

template <typename Fn>
void TimedStep(char const* name, g3t::host_frame_diag::Phase phase, Fn&& tick)
{
    g3t::movement::Stage stage(name);
    const long long t0 = g3t::GetLocalConfig().host ? g3t::movement::EntryTime() : 0;
    tick();
    if (t0) g3t::host_frame_diag::Global().AddPhase(phase, t0, g3t::movement::EntryTime());
}

// One frame of the module for persist (BeginFrame/EndFrame), closed on every way out of Process.
struct FrameScope {
    FrameScope() : entered(g3t::movement::EntryTime()), run(g3t::persist::BeginFrame())
    {
        if (run) ++moduleProcessDepth;
        if (run && moduleProcessDepth == 1) {
            g3t::movement::Begin(entered);
            if (g3t::GetLocalConfig().host) {
                auto& session = gCSession::GetInstance();
                const bool inWorld = session.IsGameRunning() && !g3t::persist::NativeLoading() &&
                    !g3t::persist::HeroPending();
                g3t::host_frame_diag::Global().BeginFrame(entered, g3t::net::Now(), g3t::partner::Focused(), inWorld);
            }
        }
    }
    ~FrameScope()
    {
        if (run) {
            TimedStep("net flush", g3t::host_frame_diag::Phase::Net, [] { g3t::net::Flush(); });
            Step("persist end", [] { g3t::persist::EndFrame(); });
            if (--moduleProcessDepth == 0) g3t::hud::Tick();
            if (moduleProcessDepth == 0) {
                g3t::movement::End();
                if (g3t::GetLocalConfig().host)
                    g3t::host_frame_diag::Global().EndFrame(g3t::movement::EntryTime());
            }
        }
    }
    FrameScope(FrameScope const&) = delete;
    FrameScope& operator=(FrameScope const&) = delete;
    const long long entered;
    const bool run;
};

// The engine's own extension point: a module registered with eCModuleAdmin gets Process()
// every frame and world activation/shutdown callbacks.
// Every virtual is overridden and no base version is called: the base versions log "not implemented"
// through the engine's property type registry, which knows nothing about this class. In engine shutdown
// that lookup crashed the game (Engine.dll eCEngineComponentBase::PreShutdown).
class Module : public eCEngineComponentBase
{
  public:
    bCGuid const& GetID() const override
    {
        // {6F3A1C52-8E2B-4D7A-9B51-3C0E2A7F4D19}
        static const _GUID raw = {0x6f3a1c52, 0x8e2b, 0x4d7a, {0x9b, 0x51, 0x3c, 0x0e, 0x2a, 0x7f, 0x4d, 0x19}};
        static const bCGuid id(raw);
        return id;
    }
    bEResult Initialize() override
    {
        g3t::world::Start();
        return bEResult_Ok;
    }
    bEResult PostInitialize() override { return bEResult_Ok; }
    void OnAppInitialized() override {}
    // Forget every entity the mod holds. Loading a save (gCHUDFileManager::LoadGame) replaces the world's
    // entities and only activates the world again, without deactivating it first: the puppets kept from before
    // were deleted and the next frame crashed on them. So forget on both.
    struct NamedCall {
        char const* name;
        void (*call)();
    };
    static void ForgetStep(char const* name, void (*forget)())
    {
        g3t::crashlog::WorldEntryScope operation(name);
        forget();
    }
    static void ForgetWorld()
    {
        constexpr NamedCall earlySteps[] = {
            {"world cleanup / native prefetch queues", &g3t::world::OnWorldDeactivate},
            {"NPC playback cleanup / source-options mutex", &g3t::npcplayback::OnWorldDeactivate},
            {"remote puppet cleanup", &g3t::remote::OnWorldDeactivate},
            {"NPC cleanup / replication reset", &g3t::npcs::OnWorldDeactivate},
            {"Ardea removal cleanup", &g3t::npcremoval::OnWorldDeactivate},
            {"combat cleanup", &g3t::combat::OnWorldChange},
            {"combat hostility cleanup", &g3t::combat_hostility::OnWorldChange},
            {"relation cleanup", &g3t::relation::OnWorldChange},
            {"guards cleanup", &g3t::guards::OnWorldChange},
            {"shot cleanup", &g3t::shot::OnWorldChange},
            {"ranged cleanup", &g3t::ranged::OnWorldChange},
            {"transform cleanup", &g3t::transform::OnWorldChange},
            {"summon cleanup", &g3t::summon::OnWorldChange},
            {"fight memory cleanup", &g3t::fightmem::OnWorldChange},
            {"XP share cleanup", &g3t::xpshare::OnWorldChange},
            {"talk cleanup", &g3t::talk::OnWorldChange},
            {"journal cleanup", &g3t::journal::OnWorldChange},
            {"story cleanup", &g3t::story::OnWorldChange},
            {"info command cleanup", &g3t::infocmd::OnWorldChange},
            {"guide cleanup", &g3t::guide::OnWorldChange},
            {"arena cleanup", &g3t::arena::OnWorldChange},
            {"ending cleanup", &g3t::ending::OnWorldChange},
        };
        constexpr NamedCall lateSteps[] = {
            {"door cleanup", &g3t::doors::OnWorldChange},
            {"enclave cleanup", &g3t::enclaves::OnWorldChange},
            {"occupancy cleanup", &g3t::occupancy::OnWorldChange},
            {"address cleanup", &g3t::address::OnWorldChange},
            {"dialog audio cleanup / native channels", &g3t::dialogaudio::OnWorldChange},
            {"container cleanup", &g3t::containers::OnWorldChange},
            {"ransack cleanup", &g3t::ransack::OnWorldChange},
            {"trade cleanup", &g3t::trade::OnWorldChange},  // forget the old hero before retiring its automatic transactions
            {"offer cleanup", &g3t::offer::OnWorldChange},
            {"telemetry cleanup", &g3t::telemetry::OnWorldChange},
        };
        ResetFocusObservation();
        CaptureCacheInventory("before world cleanup");
        for (auto const& step : earlySteps) ForgetStep(step.name, step.call);
        ForgetStep("clock cleanup", &g3t::clock::OnWorldChange);
        ForgetStep("HUD observation cleanup", &g3t::hud::OnWorldChange);
        ForgetStep("movement observation cleanup", &g3t::movement::OnWorldChange);
        g3t::host_frame_diag::Global().Reset();
        {
            g3t::crashlog::WorldEntryScope operation("downed cleanup");
            g3t::downed::OnWorldChange();
        }
        for (auto const& step : lateSteps) ForgetStep(step.name, step.call);
        CaptureCacheInventory("after world cleanup");
    }
    static void CaptureCacheInventory(char const* phase)
    {
        g3t::world::CaptureCacheInventory();
        g3t::remote::CaptureCacheInventory();
        g3t::npcremoval::CaptureCacheInventory();
        g3t::npclease::CaptureCacheInventory();
        g3t::summon::CaptureCacheInventory();
        g3t::crashlog::LogCacheInventory(phase);
    }
    bEResult WorldActivate() override
    {
        if (g3t::ai::ShuttingDown()) return bEResult_Ok;
        playbackErrorShown = false;
        g3t::crashlog::WorldEntryWait("world activation cleanup");
        g3t::crashlog::WorldEntryScope operation("WorldActivate callback");
        g3t::Log("world activated");
        ForgetWorld();
        g3t::persist::OnWorldActivate();
        g3t::world::OnWorldActivate();
        g3t::telemetry::OnWorldActivate();
        g3t::Log("world-entry: activation cleanup returned");
        g3t::crashlog::WorldEntryWait("engine first in-world frame");
        return bEResult_Ok;
    }
    bEResult WorldDeactivate() override
    {
        if (g3t::ai::ShuttingDown()) return bEResult_Ok;
        g3t::crashlog::WorldEntryWait("engine world activation");
        g3t::crashlog::WorldEntryScope operation("WorldDeactivate callback");
        g3t::Log("world deactivated");
        ForgetWorld();
        g3t::downed::OnWorldDeactivate();
        g3t::persist::OnWorldDeactivate();
        g3t::telemetry::OnWorldDeactivate();
        g3t::Log("world-entry: deactivation cleanup returned");
        return bEResult_Ok;
    }
    bEResult PreShutdown() override
    {
        g3t::ai::BeginShutdown();
        g3t::crashlog::BeginShutdown();
        g3t::crashlog::StartWatchdog();
        const bool savesQuiescent = g3t::persist::PrepareShutdown();
        ForgetWorld();
        g3t::crashlog::Stage("world deactivated");
        g3t::crashlog::Stage("forget done");
        g3t::net::Stop();
        g3t::crashlog::Stage("steam stopped");
        const auto patches = g3t::ai::RestorePatches();
        g3t::crashlog::Stage("hooks removed");
        const bool threadsJoined = g3t::crashlog::Shutdown();
        if (threadsJoined) g3t::crashlog::Stage("threads joined");
        g3t::crashlog::Stage("pre-shutdown done");
        g3t::crashlog::CompletePreShutdown(savesQuiescent && threadsJoined && !patches.failed);
        return bEResult_Ok;
    }
    bEResult Shutdown() override
    {
        g3t::ai::BeginShutdown();
        g3t::crashlog::Shutdown();
        g3t::crashlog::Stage("shutdown called");
        return bEResult_Ok;
    }
    bEResult ResetResources(GEBool full) override
    {
        const unsigned count = g3t::focus_diag::CountResourceReset();
        if (count <= 4 || count % 100 == 0)
            g3t::Logf("focus: resource-reset count=%u full=%d focused=%d", count, full ? 1 : 0, g3t::partner::Focused());
        return bEResult_Ok;
    }
    bEResult RestoreResources(GEBool full) override
    {
        const unsigned count = g3t::focus_diag::CountResourceRestore();
        if (count <= 4 || count % 100 == 0)
            g3t::Logf("focus: resource-restored count=%u full=%d focused=%d", count, full ? 1 : 0, g3t::partner::Focused());
        g3t::downed::OnFrameResume();
        return bEResult_Ok;
    }
    // Also a frame the game runs inside its own load (Session::Start, C 22.10.): there nothing of the mod runs, from the
    // first line on (C 23.10.); the world is half made and the load has not returned.
    void Process() override
    {
        if (g3t::ai::ShuttingDown()) return;
        if (g3t::persist::ProcessClientTransition()) return;
        g3t::crashlog::WorldEntryScope entryProcess("engine module Process");
        FrameScope frame;
        if (!frame.run) return;
        {
            g3t::crashlog::WorldEntryScope operation("frame report");
            Step("frame report", &Frame);
        }
        if (!g3t::npcplayback::Available()) {
            Step("crashlog", &g3t::crashlog::Tick);
            Step("persist", &g3t::persist::Tick);
            g3t::hud::MenuLine(g3t::i18n::Text(g3t::i18n::NpcReplayUnavailable));
            if (!playbackErrorShown && gCSession::GetInstance().IsGameRunning()) {
                g3t::hud::Message(g3t::hud::Bad, g3t::i18n::Text(g3t::i18n::NpcReplayUnavailable));
                playbackErrorShown = true;
            }
            return;
        }
        Step("crashlog", &g3t::crashlog::Tick);
        TimedStep("world", g3t::host_frame_diag::Phase::World, &g3t::world::Tick);
        Step("NPC removal", &g3t::npcremoval::Tick);
        Step("NPC identity", &g3t::npcs::ReconcileClientEntities);
        Step("NPC playback", &g3t::npcplayback::Tick);
        {
            g3t::crashlog::WorldEntryScope operation("remote tick / ENet service");
            TimedStep("remote / ENet service", g3t::host_frame_diag::Phase::Net, &g3t::remote::Tick);
        }
        if (g3t::persist::ClientTransitionPending()) {
            g3t::persist::Tick();
            g3t::talk::Tick();
            g3t::trade::Tick();
            g3t::offer::Tick();
            g3t::telemetry::Tick();
            return;
        }
        Step("AI", &g3t::ai::Tick);
        TimedStep("NPC / lease", g3t::host_frame_diag::Phase::Npcs, &g3t::npcs::Tick);
        Step("combat", &g3t::combat::Tick);
        constexpr NamedCall beforeDowned[] = {
            {"summon", &g3t::summon::Tick},
            {"story", &g3t::story::Tick},
            {"talk", &g3t::talk::Tick},
        };
        for (auto const& step : beforeDowned) Step(step.name, step.call);
        { g3t::movement::Stage stage("downed"); g3t::downed::Tick(); }
        constexpr NamedCall beforePersist[] = {
            {"range", &g3t::range::Tick},
            {"partner jump", &g3t::partner::Tick},
            {"doors", &g3t::doors::Tick},
            {"enclaves", &g3t::enclaves::Tick},
            {"journal", &g3t::journal::Tick},
        };
        for (auto const& step : beforePersist) Step(step.name, step.call);
        {
            g3t::crashlog::WorldEntryScope operation("persist tick");
            g3t::movement::Stage stage("persist");
            g3t::persist::Tick();
        }
        constexpr NamedCall beforeRelations[] = {
            {"clock", &g3t::clock::Tick},
            {"info command", &g3t::infocmd::Tick},
            {"guide", &g3t::guide::Tick},
            {"arena", &g3t::arena::Tick},
            {"ending", &g3t::ending::Tick},
            {"containers", &g3t::containers::Tick},
            {"trade", &g3t::trade::Tick},
            {"map marks", &g3t::mapmarks::Tick},
            {"ransack", &g3t::ransack::Tick},
            {"witness", &g3t::witness::Tick},
            {"guards", &g3t::guards::Tick},
            {"pickpocket", &g3t::pickpocket::Tick},
            {"respawn", &g3t::respawn::Tick},
            {"occupancy", &g3t::occupancy::Tick},
            {"address", &g3t::address::Tick},
        };
        for (auto const& step : beforeRelations) Step(step.name, step.call);
        { g3t::movement::Stage stage("fight memory"); g3t::fightmem::Tick(); }
        { g3t::movement::Stage stage("relation"); g3t::relation::Tick(); }
        constexpr NamedCall tailSteps[] = {
            {"XP share", &g3t::xpshare::Tick},
            {"dialog audio", &g3t::dialogaudio::Tick},
            {"offer", &g3t::offer::Tick},
            {"shot", &g3t::shot::Tick},
            {"spells", &g3t::spells::Tick},
        };
        for (auto const& step : tailSteps) Step(step.name, step.call);
        Step("telemetry", &g3t::telemetry::Tick);
    }
    void PostProcess() override {}
    void OnBeginRenderSolid() override {}
    void OnEndRenderSolid() override {}
    void OnBeginRenderAlpha() override {}
    void OnEndRenderAlpha() override {}
    void OnPostRender() override {}
    void CreateMenu(eCMenu*) override {}
    void GetDebugLines(bTObjArray<eSModuleDebugLine>&) const override {}
};

Module* module = nullptr;
}

extern "C" __declspec(dllexport) gSScriptInit const* GE_STDCALL ScriptInit()
{
    static gSScriptInit scripts;
    g3t::OpenLog("g3together.log");
    g3t::LoadConfig();
    g3t::crashlog::Install();
    const bool playbackReady = g3t::npcplayback::Start(!g3t::GetConfig().host);
    g3t::trace::Configure(g3t::GetConfig().trace);
    if (playbackReady) {
        g3t::remote::Start();
    } else {
        g3t::Log("FAIL co-op disabled: this game build does not support authoritative NPC playback");
    }
    g3t::npcs::Start();
    g3t::npcremoval::Start();
    g3t::combat::Start();
    g3t::summon::Start();
    g3t::shot::Start();
    g3t::spells::Start();
    g3t::story::Start();
    g3t::talk::Start();
    g3t::items::Start();
    g3t::enclaves::Start();
    g3t::containers::Start();
    g3t::trade::Start();
    g3t::mapmarks::Start();
    g3t::ransack::Start();
    g3t::witness::Start();
    g3t::dialogaudio::Start();
    g3t::offer::Start();
    g3t::journal::Start();
    g3t::persist::SetWorldCleanup(&Module::ForgetWorld);
    g3t::persist::Install();
    g3t::clock::Start();
    g3t::infocmd::Start();
    // The game unloads script DLLs before eCModuleAdmin::DoShutdownModules calls every registered
    // module, which then jumped into unmapped memory. Keep this DLL loaded until the process ends.
    HMODULE pinned = nullptr;
    const BOOL pin = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                        reinterpret_cast<LPCSTR>(&ScriptInit), &pinned);
    if (!pin) g3t::Log("WARN could not pin the mod DLL; engine shutdown may crash");
    module = new Module;
    module->Enable(GETrue);
    const GEBool registered = eCModuleAdmin::GetInstance().RegisterModule(*module);
    g3t::Logf("READY G3 Together stage 1: module registered=%d enabled=%d", registered, module->IsEnabled());
    return &scripts;
}
