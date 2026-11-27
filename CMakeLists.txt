cmake_minimum_required(VERSION 3.24)
project(G3Together LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Use MSVC with -A Win32 for Gothic 3.")
endif()
# The unofficial Gothic 3 SDK (georgeto/gothic3sdk) is not part of this repository.
set(G3SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/workspace/tools/gothic3sdk" CACHE PATH "Gothic 3 SDK checkout")
if(NOT EXISTS "${G3SDK_DIR}/cmake/Setup.cmake")
  message(FATAL_ERROR "Gothic 3 SDK not found at ${G3SDK_DIR}; pass -DG3SDK_DIR=<path>.")
endif()
set(G3SDK_BINDING_ONLY ON CACHE BOOL "Use only the SDK bindings" FORCE)
add_subdirectory("${G3SDK_DIR}" sdk)
include("${G3SDK_DIR}/cmake/Setup.cmake")

file(GLOB ENET_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/enet/*.c")
add_library(enet STATIC ${ENET_SOURCES})
target_include_directories(enet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/enet/include")
target_compile_definitions(enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
# Gothic script targets default to __stdcall (/Gz); ENet must match so the import names agree.
target_compile_options(enet PRIVATE /Gz)
# Codec entry points explicitly use ENET_CALLBACK (__cdecl); its allocator/host calls still match ENet's /Gz ABI.
target_link_libraries(enet PUBLIC ws2_32 winmm)

add_library(Script_G3Together SHARED src/Script_G3Together.cpp src/log.cpp src/guid_text.cpp src/guid.cpp src/config.cpp src/net.cpp src/net_direct.cpp src/net_session.cpp src/net_metrics.cpp src/net_npc.cpp src/net_npc_pose.cpp src/net_messages.cpp src/npclease_runtime.cpp src/npcwire_bones.cpp src/npcwire_control.cpp src/npcwire_adapter.cpp src/steam.cpp src/lobby.cpp src/snapshot.cpp src/npcplayback.cpp
            src/remote.cpp src/remote_equip.cpp src/remote_qa.cpp src/body.cpp src/body_qa.cpp src/npcs.cpp src/ai.cpp src/combat.cpp src/story.cpp src/talk.cpp src/items.cpp src/containers.cpp src/explore.cpp src/parity.cpp src/downed.cpp src/hud.cpp src/range.cpp src/journal.cpp src/persist.cpp src/hero.cpp src/clock.cpp src/infocmd.cpp src/transform.cpp src/ranged.cpp src/shot.cpp src/partner.cpp src/summon.cpp src/spells.cpp src/crashlog.cpp src/trace.cpp src/world.cpp src/social.cpp src/enclaves.cpp src/doors.cpp src/dialogaudio.cpp src/offer.cpp src/trade.cpp src/tradeview.cpp src/mapmarks.cpp src/ransack.cpp src/witness.cpp src/guards.cpp src/pickpocket.cpp src/respawn.cpp src/occupancy.cpp src/address.cpp src/fightmem.cpp src/guide.cpp src/arena.cpp src/ending.cpp src/xpshare.cpp src/i18n.cpp src/exports.def)
target_gothic_script_options(Script_G3Together)
target_compile_definitions(Script_G3Together PRIVATE G3T_NET_SEPARATE_TUS=1)
target_sources(Script_G3Together PRIVATE src/npcremoval.cpp)
target_sources(Script_G3Together PRIVATE src/relation.cpp)
target_sources(Script_G3Together PRIVATE src/patch.cpp)
target_sources(Script_G3Together PRIVATE src/npc_targets.cpp)
target_sources(Script_G3Together PRIVATE src/npclease_host.cpp src/npclease_client.cpp)
target_sources(Script_G3Together PRIVATE src/ai_diag.cpp)
target_sources(Script_G3Together PRIVATE src/enclaves_murder.cpp src/enclaves_qa.cpp)
target_sources(Script_G3Together PRIVATE src/player_immunity.cpp)
target_sources(Script_G3Together PRIVATE src/downed_terminal.cpp)
target_sources(Script_G3Together PRIVATE src/combat_melee.cpp)
target_sources(Script_G3Together PRIVATE src/combat_claims.cpp src/combat_kill_xp.cpp)
target_sources(Script_G3Together PRIVATE src/combat_hostility.cpp)
target_sources(Script_G3Together PRIVATE src/ranged_overlay.cpp)
target_sources(Script_G3Together PRIVATE src/movement.cpp)
target_sources(Script_G3Together PRIVATE src/options.cpp)
target_sources(Script_G3Together PRIVATE src/steam_published.cpp src/steam_p2p.cpp src/steam_flow.cpp src/steam_callbacks.cpp src/steam_legacy_auto.cpp src/steam_diagnostics.cpp)
target_sources(Script_G3Together PRIVATE
    src/npcwire_pose.cpp
    src/npcplayback_skeleton.cpp
    src/npcplayback_hooks.cpp
    src/npcplayback_hit.cpp
    src/npc_corpse.cpp
    src/npc_companions.cpp
    src/npc_identity.cpp
    src/npc_contact.cpp
    src/npc_send.cpp
    src/npc_qa.cpp
)
target_compile_definitions(Script_G3Together PRIVATE G3T_NPCWIRE_POSE_SEPARATE_TU=1)
target_sources(Script_G3Together PRIVATE src/world_pvs.cpp src/world_collision.cpp src/navzone_guard.cpp)
target_sources(Script_G3Together PRIVATE src/hero_codec.cpp)
target_sources(Script_G3Together PRIVATE src/personal_ledger.cpp)
target_sources(Script_G3Together PRIVATE src/persist_hooks.cpp)
target_sources(Script_G3Together PRIVATE src/persist_storage.cpp)
target_sources(Script_G3Together PRIVATE src/persist_client.cpp)
target_sources(Script_G3Together PRIVATE src/client_tips.cpp)
target_sources(Script_G3Together PRIVATE src/symbols.cpp)
target_sources(Script_G3Together PRIVATE src/shutdown_watchdog.cpp)
target_sources(Script_G3Together PRIVATE
    src/story_domains.cpp src/story_qa.cpp
    src/infocmd_progression.cpp src/infocmd_sector.cpp src/infocmd_qa.cpp
    src/world_sectors.cpp
    src/trade_qa.cpp)
target_sources(Script_G3Together PRIVATE src/worldentry_diag.cpp)
target_sources(Script_G3Together PRIVATE src/focus_diag.cpp)
target_sources(Script_G3Together PRIVATE src/frame_scan.cpp)
target_sources(Script_G3Together PRIVATE src/telemetry.cpp src/telemetry_events.cpp)
target_sources(Script_G3Together PRIVATE src/explore_watch.cpp src/explore_dialog.cpp src/explore_npc.cpp)
# The standalone codec rolls back partially installed baselines on allocation failure.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/npcwire_control.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/net.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/net_direct.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/net_session.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/net_metrics.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/net_npc.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/net_npc_pose.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/net_messages.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/steam.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/steam_published.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/steam_p2p.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/steam_flow.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/steam_callbacks.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/steam_legacy_auto.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/steam_diagnostics.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/focus_diag.cpp" PROPERTIES COMPILE_OPTIONS /EHsc)
# Sources are UTF-8: without /utf-8 MSVC reads them as Windows-1252 and HUD texts show "lÃ¤uft" (plan 3.5).
target_compile_options(Script_G3Together PRIVATE /utf-8 /wd4530)
# A map file, to turn crash addresses from the engine's report into functions.
target_link_options(Script_G3Together PRIVATE /MAP)
target_link_libraries(Script_G3Together PRIVATE g3sdk::SDK enet bcrypt)
set_target_properties(Script_G3Together PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts")
