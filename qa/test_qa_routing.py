"""Regression checks for QA command route names and dispatch precedence."""
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
QA_CPP = ROOT / 'src' / 'qa.cpp'
QA_DISPATCH_H = ROOT / 'src' / 'qa_dispatch.h'


EXPECTED = {
    'RunReviewAction': set('''
        chapter politicaloff swimstrokeoff swimstrokes agressoroff cancelattack sector quickuse zoneatt attchain setchapter
        polmem enclavefame enclavestatus chaptercopyoff mirrorperceiveoff scriptwatch nativewatch sleepers waitfree
        animalsleep swimgoal worldprocess streamprobe passiveprobe passivestats movemode chaptersyncoff ending endingsyncoff
        endinghold endinglimit herocut recordcheckoff recordwritefault storyzerosoff storyreloadoff hitnpcburst killonceoff
        killconfirmoff killreport recordwaitoff recordwait helloversion mirrorretryoff mirrorstats summonlatecopy
        skillreplaceoff hasskill skilldiag skilltry exchangestate heroadd herocount menutext menuline stacks holditem guide
        guidesyncoff farheal farxpoff important runto setattr p7probeon p7probe bothered ownedobj fightmemory crimeknown
        crimesuspect fightmem grudgeoff grudgeloadoff formatprobe formatstrict fightmemoff zonefixoff fightsoff sneakcheckon
        enterzoneon animalwakeoff freedestoff address addressoff addresson
    '''.split()),
    'RunPlayerAction': set('''
        puppettradeon puppetinfo lookoff wear wornscan wearoff glidestopoff glideold glidereset glide tradesync autohelpon
        helpoptionoff tradeclose murdercand creditqueueoff credithold townstatusoff townfarkill cheatkill occupancy
        usesyncoff unuse npcroutine findents arenasync arenasyncoff findall weatherfollowoff weather receiveitem pickcands
        pickinfo pickpocket thiefprep pickpocketoff picksync remotemurdersoff shapehitsoff remotekooff questdump ransack
        ransackoff npcgold subtitlesoff witness witnesses witnessoff murderseenoff guards trespassoff sneak homes assesshero
        mapmarks mapmarksoff mapopen mapclose givequality qualitycount tradesell tradegift tradeprice tradefilloff playergoods
    '''.split()),
    'RunHitAction': set('''
        hitprobe puppetparadeoff npcblockoff parade npcguard npcstill puppetparade stanceoff aicount partyenable
        puppetstate npcparade guard storycompanionoff steamstatus quickrestoreoff quickassign quickdiag spellrestoreoff spellknow
    '''.split()),
    'RunLaterAction': set('''
        world worldsyncoff npchp respawnsync respawnoff movers revolution storymoveoff storymoves startrevolution learnperk
        gesturesoff optionsoff clockdays waitoff reconnect generationoff persistpause worldgatelate loadticksoff dialogxpoff
        givebackposoff duelhero lethalonlyoff arenaruleoff arena xpinfos forgetnpclist contactsoff badtransfer badrecord
        badstates reacthitsoff keeplocaloff enclavescanpause crimeadd npcsend joingraceoff contestoff claimrecord edgenpcs
        hostwinsoff doors doorsyncoff owned enclavesync enclavesyncoff unarm enclaves crime usables face use unlock useinfo
        attitude continueroutine continueroutineon movelength movelengthoff scriptfind heropos hitpuppet lookat deadposeoff
        deadpose trace partnerjump partyxpoff npcrecvon npcrecvoff fleekeepoff flee scriptanioff looprestartoff potionoff
        givepotion potions transform transformback transformstate inventory ready aimat act finishat shootat summonstate
        summoncopyhero creatures areatickon summonmirroroff shotstate shotcopyhero spellstate spellmirroroff statusof
        tickguardoff spawnnpc party npcmemory npcgrudge puppetguid locks sethp touchlog shotmirroroff summons lastarrow
        homingoff castat mana flying shapeoff walkby dialogaudio mutegame dialogaudiooff spellfunc quickloot
    '''.split()),
}

TYPED_HANDLER_BASELINE = {
    'RunReviewAction': '''
        chapter=g3t::explore::Chapter/noarg|politicaloff=g3t::enclaves::PoliticalOff/noarg|agressoroff=g3t::combat::AgressorOff/noarg
        |cancelattack=g3t::explore::CancelAttack/arg|sector=g3t::explore::Sector/arg|quickuse=g3t::explore::QuickUse/arg
        |zoneatt=g3t::explore::ZoneAttitude/arg|attchain=g3t::explore::AttitudeChain/arg|setchapter=g3t::explore::SetChapter/arg
        |polmem=g3t::explore::PolMem/arg|enclavefame=g3t::explore::EnclaveFame/arg|enclavestatus=g3t::explore::EnclaveStatus/arg
        |chaptercopyoff=g3t::guards::ChapterCopyOff/noarg|mirrorperceiveoff=g3t::witness::MirrorPerceiveOff/noarg
        |scriptwatch=g3t::explore::ScriptWatch/arg|nativewatch=g3t::explore::NativeWatch/arg|sleepers=g3t::explore::Sleepers/arg
        |waitfree=g3t::explore::WaitFree/arg|animalsleep=g3t::explore::AnimalSleep/arg|swimgoal=g3t::explore::SwimGoal/arg
        |worldprocess=g3t::world::ProcessingReport/noarg|streamprobe=g3t::world::StreamProbe/arg
        |passiveprobe=g3t::npcplayback::Probe/arg|passivestats=g3t::npcplayback::Stats/noarg
        |movemode=g3t::explore::MoveMode/noarg|chaptersyncoff=g3t::story::ChapterOff/noarg
        |ending=g3t::ending::Report/noarg|endingsyncoff=g3t::ending::Off/noarg|endinghold=g3t::ending::Hold/arg
        |endinglimit=g3t::ending::Limit/noarg|herocut=g3t::hero::CutCommand/arg|recordcheckoff=g3t::hero::CheckOff/noarg
        |recordwritefault=g3t::hero::WriteFault/noarg|storyzerosoff=g3t::story::ZerosOff/noarg
        |storyreloadoff=g3t::story::ReloadOff/noarg|hitnpcburst=g3t::combat::HitNpcBurst/arg
        |killonceoff=g3t::combat::KillOnceOff/noarg|killconfirmoff=g3t::combat::KillConfirmOff/noarg
        |killreport=g3t::combat::KillReport/noarg|recordwaitoff=g3t::persist::RecordWaitOff/noarg
        |recordwait=g3t::persist::Waiting/noarg|mirrorretryoff=g3t::npcs::MirrorRetryOff/noarg
        |mirrorstats=g3t::npcs::MirrorStats/noarg|summonlatecopy=g3t::summon::LateCopyCommand/arg
        |skillreplaceoff=g3t::hero::SkillReplaceOff/noarg|hasskill=g3t::hero::HasSkill/arg
        |skilldiag=g3t::hero::SkillDiag/noarg|skilltry=g3t::hero::SkillTry/noarg|exchangestate=g3t::offer::Report/noarg
        |heroadd=g3t::offer::AddCommand/arg|herocount=g3t::offer::CountCommand/arg|menutext=g3t::hud::Probe/arg
        |menuline=g3t::hud::MenuLineReport/noarg|stacks=g3t::containers::Stacks/arg|holditem=g3t::containers::HoldItem/arg
        |guide=g3t::guide::Report/arg|guidesyncoff=g3t::guide::Off/noarg|farheal=g3t::explore::FarHeal/arg
        |farxpoff=g3t::npcs::FarXpOff/noarg|important=g3t::story::Important/arg|runto=g3t::explore::RunTo/arg
        |setattr=g3t::explore::SetAttribute/arg|p7probeon=g3t::p7probe::On/noarg|p7probe=g3t::p7probe::Report/noarg
        |bothered=g3t::social::Bothered/arg|ownedobj=g3t::social::OwnedObjects/arg
        |fightmemory=g3t::p7probe::FightMemory/arg|crimeknown=g3t::enclaves::SetKnown/arg
        |crimesuspect=g3t::enclaves::SetSuspect/arg|fightmem=g3t::fightmem::Report/noarg
        |grudgeoff=g3t::fightmem::ApartOff/noarg|grudgeloadoff=g3t::fightmem::KeepOff/noarg
        |formatprobe=g3t::FormatProbe/arg|formatstrict=g3t::FormatStrict/noarg|fightmemoff=g3t::fightmem::Off/noarg
        |zonefixoff=g3t::guards::ZoneFixOff/noarg|fightsoff=g3t::guards::FightsOff/noarg
        |sneakcheckon=g3t::guards::SneakCheckOn/noarg|enterzoneon=g3t::guards::EnterZoneOn/noarg
        |animalwakeoff=g3t::guards::AnimalWakeOff/noarg|freedestoff=g3t::guards::FreeDestOff/noarg
        |address=g3t::address::Report/noarg|addressoff=g3t::address::Off/noarg|addresson=g3t::address::On/noarg
    ''',
    'RunPlayerAction': '''
        puppettradeon=g3t::remote::PuppetTradeOn/arg|puppetinfo=g3t::remote::PuppetInfo/arg|lookoff=g3t::remote::LookOff/noarg
        |wear=g3t::remote::WearHero/arg|wornscan=g3t::remote::WornScan/noarg|wearoff=g3t::remote::WearOff/noarg
        |glidestopoff=g3t::body::GlideStopOff/noarg|glideold=g3t::body::GlideOld/noarg|glidereset=g3t::body::GlideReset/noarg
        |glide=g3t::body::GlideReport/noarg|tradesync=g3t::trade::Report/noarg|autohelpon=g3t::downed::AutoHelpOn/noarg
        |helpoptionoff=g3t::downed::HelpOptionOff/noarg|tradeclose=g3t::trade::CloseCommand/noarg
        |murdercand=g3t::enclaves::MurderCandidates/arg|creditqueueoff=g3t::enclaves::CreditQueueOff/noarg
        |credithold=g3t::enclaves::CreditHold/arg|townstatusoff=g3t::enclaves::TownStatusOff/noarg
        |townfarkill=g3t::enclaves::TownFarKill/arg|cheatkill=g3t::enclaves::CheatKill/arg
        |occupancy=g3t::occupancy::Report/noarg|usesyncoff=g3t::occupancy::Off/noarg|unuse=g3t::occupancy::StandUp/noarg
        |npcroutine=g3t::explore::NpcRoutine/arg|findents=g3t::world::FindEntities/arg
        |arenasync=g3t::arena::Report/noarg|arenasyncoff=g3t::arena::Off/noarg|findall=g3t::world::FindAll/arg
        |weatherfollowoff=g3t::summon::WeatherFollowOff/noarg|weather=g3t::summon::WeatherReport/noarg
        |pickcands=g3t::pickpocket::Candidates/noarg|pickinfo=g3t::pickpocket::Info/arg|pickpocket=g3t::pickpocket::Pick/arg
        |thiefprep=g3t::pickpocket::ThiefPrep/noarg|pickpocketoff=g3t::pickpocket::Off/noarg
        |picksync=g3t::pickpocket::Report/noarg|remotemurdersoff=g3t::enclaves::RemoteMurdersOff/noarg
        |shapehitsoff=g3t::combat::ShapeHitsOff/noarg|remotekooff=g3t::combat::RemoteKnockOutsOff/noarg
        |ransack=g3t::ransack::Report/noarg|ransackoff=g3t::ransack::Off/noarg|npcgold=g3t::ransack::NpcGold/arg
        |subtitlesoff=g3t::dialogaudio::SubtitlesOff/noarg|witness=g3t::witness::Saw/arg
        |witnesses=g3t::witness::Report/noarg|witnessoff=g3t::witness::Off/noarg|murderseenoff=g3t::witness::SeenOff/noarg
        |guards=g3t::guards::Report/noarg|trespassoff=g3t::guards::Off/noarg|sneak=g3t::guards::Sneak/arg
        |homes=g3t::guards::Homes/arg|assesshero=g3t::explore::AssessHero/arg|mapmarks=g3t::mapmarks::Report/noarg
        |mapmarksoff=g3t::mapmarks::Off/noarg|mapopen=g3t::mapmarks::OpenMap/noarg|mapclose=g3t::mapmarks::CloseMap/noarg
        |givequality=g3t::trade::GiveQuality/arg|qualitycount=g3t::trade::QualityCount/arg
        |tradesell=g3t::containers::TradeSell/arg|tradegift=g3t::trade::GiftCommand/arg|tradeprice=g3t::trade::Price/arg
        |tradefilloff=g3t::trade::FillOff/noarg|playergoods=g3t::trade::GoodsReport/arg
    ''',
    'RunHitAction': '''
        hitprobe=g3t::combat::HitProbe/arg|puppetparadeoff=g3t::combat::PuppetParadeOff/noarg
        |npcblockoff=g3t::combat::NpcBlockOff/noarg|parade=g3t::combat::ParadeReport/noarg
        |npcguard=g3t::combat::NpcGuard/arg|npcstill=g3t::combat::NpcStill/arg
        |puppetparade=g3t::combat::PuppetParade/arg|aicount=g3t::ai::Count/arg
        |partyenable=g3t::infocmd::PartyEnableQa/arg|puppetstate=g3t::combat::PuppetState/noarg
        |npcparade=g3t::combat::NpcParade/arg|guard=g3t::combat::Guard/arg
        |storycompanionoff=g3t::infocmd::StoryCompanionOff/noarg|steamstatus=g3t::steam::Status/noarg
        |quickrestoreoff=g3t::hero::QuickRestoreOff/noarg|quickassign=g3t::hero::QuickAssign/arg
        |quickdiag=g3t::hero::QuickDiag/noarg|spellrestoreoff=g3t::hero::SpellRestoreOff/noarg
        |spellknow=g3t::hero::SpellKnow/arg
    ''',
    'RunLaterAction': '''
        world=g3t::persist::WorldReport/noarg|worldsyncoff=g3t::persist::WorldSyncOff/noarg
        |respawnsync=g3t::respawn::Report/noarg|respawnoff=g3t::respawn::Off/noarg|movers=g3t::doors::Movers/noarg
        |revolution=g3t::social::Revolution/arg|storymoveoff=g3t::infocmd::StoryMoveOff/noarg
        |storymoves=g3t::infocmd::StoryMoves/noarg|startrevolution=g3t::social::StartRevolution/arg
        |learnperk=g3t::pickpocket::LearnPerk/arg|gesturesoff=g3t::talk::GesturesOff/noarg
        |optionsoff=g3t::dialogaudio::OptionsOff/noarg|clockdays=g3t::clock::AddDays/arg
        |waitoff=g3t::clock::WaitOff/noarg|reconnect=g3t::net::Reconnect/noarg
        |generationoff=g3t::persist::GenerationOff/noarg|persistpause=g3t::persist::Pause/arg
        |worldgatelate=g3t::persist::WorldGateLate/noarg|loadticksoff=g3t::persist::LoadTicksOff/arg
        |dialogxpoff=g3t::infocmd::DialogXpOff/noarg|givebackposoff=g3t::infocmd::GivebackPlaceOff/noarg
        |duelhero=g3t::explore::DuelHero/arg|lethalonlyoff=g3t::downed::LethalOnlyOff/noarg
        |arenaruleoff=g3t::combat::ArenaRuleOff/noarg|arena=g3t::combat::ArenaReport/arg
        |xpinfos=g3t::explore::XpInfos/noarg|forgetnpclist=g3t::remote::ForgetNpcListOn/noarg
        |contactsoff=g3t::remote::ContactsOff/noarg|badtransfer=g3t::persist::BadTransfer/noarg
        |badrecord=g3t::npcs::BadRecord/noarg|badstates=g3t::remote::BadStates/arg
        |reacthitsoff=g3t::combat::ReactHitsOff/noarg|keeplocaloff=g3t::enclaves::KeepLocalOff/noarg
        |enclavescanpause=g3t::enclaves::PauseScan/arg|crimeadd=g3t::enclaves::AddCrime/arg
        |npcsend=g3t::npcs::SendInfo/arg|joingraceoff=g3t::npcs::JoinGraceOff/noarg
        |contestoff=g3t::npcs::ContestOff/noarg|claimrecord=g3t::npcs::ClaimRecord/arg
        |edgenpcs=g3t::npcs::EdgeReport/noarg|hostwinsoff=g3t::npcs::HostWinsOff/noarg
        |doors=g3t::doors::Report/noarg|doorsyncoff=g3t::doors::SyncOff/noarg|owned=g3t::social::Owned/arg
        |enclavesync=g3t::enclaves::Report/noarg|enclavesyncoff=g3t::enclaves::SyncOff/noarg
        |unarm=g3t::social::Unarm/noarg|enclaves=g3t::social::Enclaves/noarg|crime=g3t::social::Crime/arg
        |usables=g3t::world::Usables/arg|face=g3t::world::Face/arg|use=g3t::world::Use/arg
        |unlock=g3t::world::Unlock/arg|useinfo=g3t::world::UseInfo/arg|attitude=g3t::explore::Attitude/arg
        |continueroutine=g3t::ai::ContinueRoutineReport/noarg|continueroutineon=g3t::ai::ContinueRoutineOn/noarg
        |movelength=g3t::ai::MoveLengthReport/noarg|movelengthoff=g3t::ai::MoveLengthOff/noarg
        |scriptfind=g3t::ai::FindByRva/arg|heropos=g3t::explore::HeroPos/noarg
        |hitpuppet=g3t::combat::HitPuppet/arg|lookat=g3t::explore::LookAt/arg
        |deadposeoff=g3t::npcs::DeadPoseOff/noarg|deadpose=g3t::npcs::DeadPoseReport/noarg
        |trace=g3t::trace::Command/arg|partnerjump=g3t::partner::JumpQa/noarg
        |partyxpoff=g3t::combat::PartyXpOff/noarg|npcrecvon=g3t::npcs::RecvOn/noarg|npcrecvoff=g3t::npcs::RecvOff/noarg
        |fleekeepoff=g3t::ai::FleeKeepOff/noarg|flee=g3t::combat::FleeFrom/arg
        |potionoff=g3t::downed::PotionOff/noarg|givepotion=g3t::downed::GivePotion/arg
        |potions=g3t::downed::Potions/noarg|transform=g3t::transform::Start/arg
        |transformback=g3t::transform::Back/noarg|transformstate=g3t::transform::Report/noarg
        |inventory=g3t::ranged::Inventory/noarg|ready=g3t::ranged::Ready/arg|aimat=g3t::ranged::Target/arg
        |act=g3t::ranged::Act/arg|finishat=g3t::ranged::FinishAt/arg|shootat=g3t::ranged::ShootAt/arg
        |summonstate=g3t::summon::Report/noarg|summoncopyhero=g3t::summon::CopyHeroOff/noarg
        |creatures=g3t::summon::Creatures/noarg|areatickon=g3t::summon::AreaTickOn/noarg
        |summonmirroroff=g3t::summon::MirrorOff/noarg|shotstate=g3t::shot::Report/noarg
        |shotcopyhero=g3t::shot::CopyHeroOff/noarg|spellstate=g3t::spells::Report/noarg
        |spellmirroroff=g3t::spells::MirrorOff/noarg|statusof=g3t::spells::StatusOf/arg
        |tickguardoff=g3t::spells::TickGuardOff/noarg|party=g3t::spells::Party/noarg
        |npcmemory=g3t::spells::NpcMemory/arg|npcgrudge=g3t::spells::NpcGrudge/arg
        |puppetguid=g3t::spells::PuppetGuid/arg|locks=g3t::spells::Locks/noarg|sethp=g3t::spells::SetHp/arg
        |touchlog=g3t::shot::TouchLog/noarg|shotmirroroff=g3t::shot::MirrorOff/noarg
        |summons=g3t::ranged::Summons/noarg|lastarrow=g3t::ranged::LastArrow/noarg
        |castat=g3t::ranged::CastAt/arg|mana=g3t::ranged::Mana/noarg|flying=g3t::ranged::Flying/noarg
        |shapeoff=g3t::remote::ShapeOff/noarg
        |dialogaudio=g3t::dialogaudio::Report/noarg|mutegame=g3t::dialogaudio::MuteGame/arg
        |dialogaudiooff=g3t::dialogaudio::Disable/noarg|spellfunc=g3t::spells::SpellFunc/arg
    ''',
}


def function_block(source, name):
    start = source.index(f'bool {name}(')
    ends = [source.find(marker, start + 1) for marker in ('\nbool ', '\nvoid ') if source.find(marker, start + 1) >= 0]
    end = min(ends) if ends else len(source)
    return source[start:end]


class QaRoutingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = QA_CPP.read_text()

    def test_every_original_command_name_remains_in_its_chain(self):
        for name, expected in EXPECTED.items():
            with self.subTest(chain=name):
                block = function_block(self.source, name)
                routes = set(re.findall(r'\{"([^"]+)",', block))
                routes.update(re.findall(r'if\s*\(!std::strcmp\(action,\s*"([^"]+)"\)', block))
                self.assertEqual(routes, expected)

    def test_unknown_command_flows_through_dispatch_false_result(self):
        for name in EXPECTED:
            with self.subTest(chain=name):
                block = function_block(self.source, name)
                self.assertIn('return DispatchTypedRoute(action, argument, &Log, routes);', block)

    def test_dispatch_helper_preserves_match_order_and_unknown_result(self):
        source = QA_DISPATCH_H.read_text()
        self.assertIn('if (std::strcmp(action, route.name)) continue;', source)
        self.assertIn('if (route.withArgument) route.withArgument(argument, log);', source)
        self.assertIn('else if (route.withoutArgument) route.withoutArgument(log);', source)
        self.assertIn('return true;', source)
        self.assertIn('return false;', source)

    def test_every_typed_route_preserves_original_handler_and_argument_flow(self):
        for name, baseline_text in TYPED_HANDLER_BASELINE.items():
            with self.subTest(chain=name):
                new_block = function_block(self.source, name)
                expected = {}
                for item in baseline_text.split('|'):
                    item = item.strip()
                    if not item:
                        continue
                    route, expected_value = item.split('=', 1)
                    callback, kind = expected_value.rsplit('/', 1)
                    expected[route] = (callback, kind)

                actual = {}
                for match in re.finditer(r'\{"([^"]+)",\s*([^,]+),\s*([^}]+)\}', new_block):
                    route, no_arg, with_arg = match.groups()
                    callback = with_arg.strip() if with_arg.strip() != 'nullptr' else no_arg.strip()
                    kind = 'arg' if with_arg.strip() != 'nullptr' else 'noarg'
                    actual[route] = (callback, kind)
                self.assertEqual(actual, expected)

    def test_special_actions_remain_explicit_and_preserve_callbacks(self):
        checks = {
            'RunReviewAction': (
                'swimStrokes = false',
                'swimStrokesPlayed',
                'g3t::net::ClaimVersionForTest',
            ),
            'RunPlayerAction': (
                'ReceiveItem();',
                'g3t::explore::QuestDump(path, &Log)',
            ),
            'RunHitAction': (
                'g3t::body::stanceSync = false',
            ),
            'RunLaterAction': (
                'g3t::spells::NpcHp(argument, &Log)',
                'g3t::combat::NpcHp(argument, &Log)',
                'g3t::body::scriptAniReplay = false',
                'g3t::body::loopRestart = false',
                'g3t::summon::SpawnShared(argument + 7, &Log)',
                'g3t::spells::SpawnNpc(argument, &Log)',
                'g3t::ranged::homing = false',
                'g3t::transform::Walk(std::atoi(argument), &Log)',
                'g3t::containers::QuickLootActions(&Log)',
                'g3t::containers::QuickLoot(&Log)',
            ),
        }
        for name, snippets in checks.items():
            with self.subTest(chain=name):
                block = function_block(self.source, name)
                for snippet in snippets:
                    self.assertIn(snippet, block)

    def test_chain_order_precedes_remaining_run_action_routes(self):
        start = self.source.index('void RunAction(')
        body = self.source[start:]
        switch_position = body.index('g3t::npcs::Switch(action, &Log)')
        chain_position = body.index('if (RunHitAction(action) || RunLaterAction(action) || '
                                   'RunReviewAction(action) || RunPlayerAction(action)) return;')
        remaining_position = body.index('if (!std::strcmp(action, "openinventory")')
        self.assertLess(switch_position, chain_position)
        self.assertLess(chain_position, remaining_position)
        hit = body.index('RunHitAction(action)')
        later = body.index('RunLaterAction(action)')
        review = body.index('RunReviewAction(action)')
        player = body.index('RunPlayerAction(action)')
        self.assertLess(hit, later)
        self.assertLess(later, review)
        self.assertLess(review, player)


if __name__ == '__main__':
    unittest.main()
