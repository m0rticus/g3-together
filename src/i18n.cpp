// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.h"

#include "config.h"
#include "log.h"

#include <cstring>

namespace g3t::i18n {
namespace {
// Polish: the other player's name always follows "Gracz"/"gracz" (so the verb agrees with that word, not with an unknown
// gender), and the reader's own past actions are worded without a gendered past tense.
struct Translation { wchar_t const* english; wchar_t const* german; wchar_t const* polish; };
constexpr Translation Messages[] = {
    {L"You need a healing potion to help %s up.",
     L"Du brauchst einen Heiltrank, um %s aufzuhelfen.",
     L"Potrzebujesz mikstury leczniczej, aby pomóc graczowi %s wstać."},
    {L"You helped %s up (one healing potion used).",
     L"Du hast %s aufgeholfen (ein Heiltrank verbraucht).",
     L"Pomagasz graczowi %s wstać (zużyto jedną miksturę leczniczą)."},
    {L"You helped %s up.", L"Du hast %s aufgeholfen.", L"Pomagasz graczowi %s wstać."},
    {L"Nobody can help you now.", L"Niemand kann dir mehr helfen.", L"Nikt już nie może ci pomóc."},
    {L"%s is down. Talk to them and choose \"I will help you.\" (costs a healing potion).",
     L"%s liegt am Boden. Sprich ihn an und wähle „Ich werde dir helfen.“ (kostet einen Heiltrank).",
     L"Gracz %s leży na ziemi. Porozmawiaj z nim i wybierz „Pomogę ci.” (kosztuje miksturę leczniczą)."},
    {L"%s is down. Talk to them and choose \"I will help you.\" to help them up.",
     L"%s liegt am Boden. Sprich ihn an und wähle „Ich werde dir helfen.“, um aufzuhelfen.",
     L"Gracz %s leży na ziemi. Porozmawiaj z nim i wybierz „Pomogę ci.”, aby postawić go na nogi."},
    {L"%s has died.", L"%s ist gestorben.", L"Gracz %s zginął."},
    {L"You are knocked out. A partner with a healing potion can help you up.",
     L"Du bist niedergeschlagen. Ein Mitspieler mit Heiltrank kann dir aufhelfen.",
     L"Powalono cię. Drugi gracz z miksturą leczniczą może pomóc ci wstać."},
    {L"You are knocked out. Wait for your partner to help you up.",
     L"Du bist niedergeschlagen. Warte, bis dein Mitspieler dir aufhilft.",
     L"Powalono cię. Poczekaj, aż drugi gracz pomoże ci wstać."},
    {L"%s helped you up.", L"%s hat dir aufgeholfen.", L"Gracz %s pomógł ci wstać."},
    {L"%s has no healing potion to help you up.",
     L"%s hat keinen Heiltrank, um dir aufzuhelfen.",
     L"Gracz %s nie ma mikstury leczniczej, aby pomóc ci wstać."},
    {L"Jumped to your partner.", L"Zum Mitspieler gesprungen.", L"Przeniesiono do drugiego gracza."},
    {L"No partner in this world.", L"Kein Mitspieler in dieser Welt.", L"W tym świecie nie ma drugiego gracza."},
    {L"You cannot jump while knocked out.", L"Nicht, solange du am Boden liegst.",
     L"Nie możesz się przenieść, dopóki leżysz na ziemi."},
    {L"Wait a moment, then try again.", L"Kurz warten, dann noch einmal.", L"Odczekaj chwilę i spróbuj ponownie."},
    {L"No saved hero for this campaign: you are playing a copy of the host's hero.",
     L"Kein eigener Held für diese Kampagne: du spielst mit einer Kopie des Host-Helden.",
     L"Brak zapisanego bohatera dla tej kampanii: grasz kopią bohatera hosta."},
    {L"%s is in another world.", L"%s ist in einer anderen Welt.", L"Gracz %s jest w innym świecie."},
    {L"The host is in the main menu. The game goes on once the host loads a game.",
     L"Der Host ist im Hauptmenü. Es geht weiter, sobald er ein Spiel lädt.",
     L"Host jest w menu głównym. Gra będzie kontynuowana, gdy host wczyta grę."},
    {L"The host plays a new game without a save. Start a new game too, or wait until the host saves.",
     L"Der Host spielt ein neues Spiel ohne Spielstand. Starte auch ein neues Spiel oder warte, bis der Host speichert.",
     L"Host gra w nową grę bez zapisu. Rozpocznij również nową grę albo poczekaj, aż host zapisze grę."},
    {L"The host loaded a save: you will be loaded into the same save.",
     L"Der Host hat einen Spielstand geladen: du wirst in denselben Stand nachgeladen.",
     L"Host wczytał zapis: ten sam zapis zostanie wczytany u ciebie."},
    {L"You will be loaded into the host's save.", L"Du wirst in den Spielstand des Hosts geladen.",
     L"Zapis hosta zostanie wczytany u ciebie."},
    {L"A partner is in another world. Save, and they will be loaded into your save.",
     L"Ein Mitspieler ist in einer anderen Welt. Speichere, dann wird er in deinen Stand geladen.",
     L"Drugi gracz jest w innym świecie. Zapisz grę, a zostanie wczytany do twojego zapisu."},
    {L"Your own hero for this campaign could not be loaded (%s is damaged). You wait outside the host's game until the file is repaired.",
     L"Dein eigener Held für diese Kampagne ließ sich nicht laden (%s ist beschädigt). Du wartest außerhalb des Host-Spiels, bis die Datei repariert ist.",
     L"Nie udało się wczytać twojego bohatera dla tej kampanii (plik %s jest uszkodzony). Czekasz poza grą hosta, dopóki plik nie zostanie naprawiony."},
    {L"Your own hero is back. You are in the host's game.", L"Dein eigener Held ist zurück. Du bist im Spiel des Hosts.",
     L"Twój bohater wrócił. Jesteś w grze hosta."},
    {L"Connection to the host lost. Reconnecting …", L"Verbindung zum Host verloren. Neuer Versuch läuft …",
     L"Utracono połączenie z hostem. Trwa ponowna próba …"},
    {L"Connected to the host again.", L"Wieder mit dem Host verbunden.", L"Ponownie połączono z hostem."},
    {L"The host refused you: another G3 Together version (yours: protocol %u, host: %u). Both players need the same package.",
     L"Der Host hat dich abgewiesen: andere G3-Together-Version (deine: Protokoll %u, Host: %u). Beide brauchen dasselbe Paket.",
     L"Host cię odrzucił: inna wersja G3 Together (twoja: protokół %u, host: %u). Obaj gracze potrzebują tego samego pakietu."},
    {L"The host refused you: your game install differs (%s). Both players need the same game version and patches.",
     L"Der Host hat dich abgewiesen: deine Spielinstallation weicht ab (%s). Beide brauchen dieselbe Spielversion und dieselben Patches.",
     L"Host cię odrzucił: twoja instalacja gry się różni (%s). Obaj gracze potrzebują tej samej wersji gry i tych samych łatek."},
    {L"The host refused you: %s", L"Der Host hat dich abgewiesen: %s", L"Host cię odrzucił: %s"},
    {L"The host's save did not arrive intact and was not loaded.",
     L"Der Spielstand des Hosts kam nicht vollständig an und wurde nicht geladen.",
     L"Zapis hosta nie dotarł w całości i nie został wczytany."},
    {L"%s waits outside your game: their own hero for this campaign could not be loaded (the file is damaged).",
     L"%s wartet außerhalb deines Spiels: der eigene Held für diese Kampagne ließ sich nicht laden (Datei beschädigt).",
     L"Gracz %s czeka poza twoją grą: nie udało się wczytać jego bohatera dla tej kampanii (plik jest uszkodzony)."},
    {L"Connected to the host.", L"Mit dem Host verbunden.", L"Połączono z hostem."},
    {L"%s robbed you of %d gold.", L"%s hat dir %d Gold abgenommen.", L"%s – zabrano ci %d szt. złota."},
    {L"You were robbed of %d gold.", L"Dir wurden %d Gold abgenommen.", L"Zabrano ci %d szt. złota."},
    {L"%s is using this.", L"%s benutzt das gerade.", L"%s właśnie tego używa."},
    {L"%s is talking to %s.", L"%s spricht gerade mit %s.", L"%s rozmawia właśnie z graczem %s."},
    {L"The other player has started the epilogue. It starts here once your dialog, fight or menu is over.",
     L"Der Mitspieler hat den Epilog begonnen. Er beginnt hier, sobald dein Dialog, Kampf oder Menü vorbei ist.",
     L"Drugi gracz rozpoczął epilog. Rozpocznie się on tutaj po zakończeniu twojej rozmowy, walki lub menu."},
    {L"G3 Together: looking for the host among your Steam friends for %u s. The host must be your Steam friend and have its game running.",
     L"G3 Together: Suche den Host seit %u s unter deinen Steam-Freunden. Er muss dein Steam-Freund sein und sein Spiel muss laufen.",
     L"G3 Together: od %u s szukam hosta wśród znajomych na Steam. Host musi być twoim znajomym na Steam i mieć uruchomioną grę."},
    {L"G3 Together: connecting to %s over Steam for %u s. Is its game still loading?",
     L"G3 Together: Verbindung zu %s über Steam seit %u s. Lädt sein Spiel noch?",
     L"G3 Together: łączenie z graczem %s przez Steam od %u s. Czy jego gra nadal się wczytuje?"},
    {L"G3 Together: Steam co-op cannot start (%s). Check Steam and steam_api.dll, then restart the game. No connection was opened.",
     L"G3 Together: Steam-Koop kann nicht starten (%s). Prüfe Steam und steam_api.dll und starte das Spiel neu. Es wurde keine Verbindung geöffnet.",
     L"G3 Together: nie można uruchomić kooperacji przez Steam (%s). Sprawdź Steam i steam_api.dll, potem uruchom grę ponownie. Nie otwarto połączenia."},
    {L"G3 Together: hosting over Steam failed (%s). Check Steam and steam_api.dll, then restart the game. No connection was opened.",
     L"G3 Together: Hosten über Steam fehlgeschlagen (%s). Prüfe Steam und steam_api.dll und starte das Spiel neu. Es wurde keine Verbindung geöffnet.",
     L"G3 Together: nie udało się hostować przez Steam (%s). Sprawdź Steam i steam_api.dll, potem uruchom grę ponownie. Nie otwarto połączenia."},
    {L"G3 Together: starting Steam co-op...", L"G3 Together: Steam-Koop startet...",
     L"G3 Together: uruchamianie kooperacji przez Steam..."},
    {L" | Session ID: %s", L" | Sitzungs-ID: %s", L" | ID sesji: %s"},
    {L"G3 Together: creating a Steam lobby as %s...", L"G3 Together: Steam-Lobby für %s wird erstellt...",
     L"G3 Together: tworzenie lobby Steam dla gracza %s..."},
    {L"G3 Together: hosting as %s | lobby %llu\nClients: %u | ready: %u | Steam peers: %u | ping (max): %s",
     L"G3 Together: Host %s | Lobby %llu\nMitspieler: %u | bereit: %u | Steam-Partner: %u | Ping (max.): %s",
     L"G3 Together: host %s | lobby %llu\nKlienci: %u | gotowi: %u | połączenia Steam: %u | ping (maks.): %s"},
    {L"G3 Together: searching your Steam friends for a host...",
     L"G3 Together: Suche einen Host unter deinen Steam-Freunden...",
     L"G3 Together: szukanie hosta wśród znajomych na Steam..."},
    {L"G3 Together: connecting to lobby %llu of %s\nPing: %s",
     L"G3 Together: Verbindung zur Lobby %llu von %s\nPing: %s",
     L"G3 Together: łączenie z lobby %llu gracza %s\nPing: %s"},
    {L"G3 Together: joined %s | lobby %llu\nWaiting for the host's campaign | ping: %s",
     L"G3 Together: %s beigetreten | Lobby %llu\nWarte auf die Kampagne des Hosts | Ping: %s",
     L"G3 Together: dołączono do gracza %s | lobby %llu\nOczekiwanie na kampanię hosta | ping: %s"},
    {L"G3 Together: joined %s | lobby %llu\nCampaign ready | ping: %s",
     L"G3 Together: %s beigetreten | Lobby %llu\nKampagne bereit | Ping: %s",
     L"G3 Together: dołączono do gracza %s | lobby %llu\nKampania gotowa | ping: %s"},
    {L"G3 Together: Steam lobby error: %s", L"G3 Together: Steam-Lobby-Fehler: %s",
     L"G3 Together: błąd lobby Steam: %s"},
    {L"pending", L"ausstehend", L"oczekiwanie"},
    {L"Exchange not possible: the inventory or connection changed. Your items have not been traded.",
     L"Austausch nicht möglich: Inventar oder Verbindung geändert. Deine Gegenstände wurden nicht getauscht.",
     L"Wymiana niemożliwa: zmienił się ekwipunek lub połączenie. Twoje przedmioty nie zostały wymienione."},
    {L"An exchange could not be restored. Stop and reload the last settled save; see g3together.log.",
     L"Ein Austausch konnte nicht wiederhergestellt werden. Letzten abgeschlossenen Spielstand laden; siehe g3together.log.",
     L"Nie udało się przywrócić wymiany. Wczytaj ostatni zapis po zakończonych wymianach; zobacz g3together.log."},
    {L"Player trading is unavailable for this game build. NPC trading is unchanged; see g3together.log.",
     L"Spielerhandel ist für diese Spielversion nicht verfügbar. NPC-Handel bleibt unverändert; siehe g3together.log.",
     L"Wymiana między graczami jest niedostępna w tej wersji gry. Handel z NPC bez zmian; zobacz g3together.log."},
    {L"An exchange is still synchronizing. Reconnect your partner before saving or loading.",
     L"Ein Austausch wird noch synchronisiert. Mitspieler vor dem Speichern oder Laden wieder verbinden.",
     L"Wymiana jest nadal synchronizowana. Połącz ponownie drugiego gracza przed zapisem lub wczytaniem."},
    {L"Co-op is disabled: this game build does not support passive NPC replay. See g3together.log.",
     L"Koop ist deaktiviert: Diese Spielversion unterstützt keine passive NPC-Wiedergabe. Siehe g3together.log.",
     L"Kooperacja jest wyłączona: ta wersja gry nie obsługuje pasywnego odtwarzania NPC. Zobacz g3together.log."},
    {L"The host cannot load a remote NPC region. Interactions there remain unavailable; retrying. See g3together.log.",
     L"Der Host kann ein entferntes NPC-Gebiet nicht laden. Interaktionen dort bleiben gesperrt; neuer Versuch läuft. Siehe g3together.log.",
     L"Host nie może wczytać odległego regionu NPC. Interakcje tam są niedostępne; trwa ponowna próba. Zobacz g3together.log."},
    {L"Waiting for NPC presentation from the host. Unready NPCs cannot register hits. See g3together.log.",
     L"Warte auf die NPC-Darstellung des Hosts. Nicht bereite NPCs können keine Treffer auslösen. Siehe g3together.log.",
     L"Oczekiwanie na stan NPC od hosta. Niegotowe NPC nie mogą zadawać trafień. Zobacz g3together.log."},
};
static_assert(sizeof(Messages) / sizeof(Messages[0]) == MessageCount, "Missing HUD translation");

char resolved[3] = "";
}

char const* LanguageFor(char const* game)
{
    if (!game) return "en";
    if (!_stricmp(game, "German")) return "de";
    if (!_stricmp(game, "Polish")) return "pl";
    return "en";
}

char const* Language()
{
    char const* configured = GetConfig().language;
    if (std::strcmp(configured, "auto")) return configured;
    // Asked at every message: the player can change the game's text language (subtitle language) in the options.
    char const* game = GameTextLanguage();
    if (!game || !*game) return "en";  // the game has not set its language yet
    char const* language = LanguageFor(game);
    if (std::strcmp(language, resolved)) {
        strcpy_s(resolved, language);
        Logf("i18n: language auto, game text language %s -> %s", game, resolved);
    }
    return language;
}

wchar_t const* Text(Message message)
{
    if (message < 0 || message >= MessageCount) return L"";
    auto const& translation = Messages[message];
    char const* language = Language();
    if (!std::strcmp(language, "de")) return translation.german;
    if (!std::strcmp(language, "pl")) return translation.polish;
    return translation.english;
}
}
