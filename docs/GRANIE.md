# Gra w G3 Together

[English](PLAYING.md) · [Deutsch](SPIELEN.md) · **Polski**

G3 Together dodaje do Gothic 3 tryb kooperacji dla dwóch graczy. Więcej graczy jest planowanych, ale nie zostało to
przetestowane. Jeden gracz jest **hostem**: jego świat jest wspólną kampanią, a jego zapis gry jest zapisem kampanii
wszystkich. Drugi gracz do niego dołącza.

To wersja testowa. Znane problemy i ograniczenia opisano niżej.

Ten fork obsługuje **tylko Steam**. Dołączanie przez IP, LAN/VPN i automatyczne połączenie zastępcze przez IP nie są obsługiwane.

## Wymagania (takie same na obu komputerach)

- Gothic 3 ze Steama z **Community Patch 1.75.14**.
- **Parallel Universe Patch 1.1.1**. Steam, CP 1.75.14 i PUP 1.1.1 to przetestowana podstawa; działanie bez PUP nie
  zostało potwierdzone.
- Ta sama wersja G3 Together (ten sam plik `Script_G3Together.dll`) i te same pliki gry.
- Zgodne ustawienia: poziom trudności, alternatywny balans, alternatywna SI, mnożnik doświadczenia, minimalny czas
  trafienia, Quick Loot oraz cała sekcja `[Game]` w `Ini\ge3.ini`. Mod sprawdza to przy dołączaniu i odrzuca niezgodne
  instalacje. Plik `<Gothic 3>\g3together.log` dołączającego gracza podaje powód w wierszu z `REFUSED by host`. Luźne pliki
  w podfolderach Data nie są sprawdzane.
- Żadnych innych modów kooperacji w `scripts`, np. `Script_G3Coop.dll` z Myrtana Accord.
- Najlepiej używać do kooperacji osobnej **kopii** instalacji Gothic 3.
- Steam musi działać i być zalogowany na obu komputerach; obaj gracze muszą być znajomymi na Steam.

## Instalacja

**Instalator:** gdy Gothic 3 i obie łatki są zainstalowane, rozpakuj cały pakiet w dowolnym miejscu i kliknij dwukrotnie
`Instalacja.cmd` (po angielsku `Setup.cmd`, po niemiecku `Einrichten.cmd`). Instalator zrobi resztę: znajdzie Gothic 3
(Steam, GOG albo podany folder), sprawdzi Community Patch 1.75.14 i Parallel Universe Patch 1.1.1, przeniesie inny mod
kooperacji ze `scripts`, skopiuje mod do `scripts` i zapisze `g3together.ini` na podstawie twoich odpowiedzi (host czy
dołączający gracz). Nazwa gracza pochodzi automatycznie ze Steam. Steam i sprawdzony `steam_api.dll` są wymagane; instalator kończy pracę, jeśli DLL nie może być zainstalowany.
Zastąpione pliki trafiają do `G3Together-backup` w folderze gry. Instalator
sprawdzono na folderach testowych, jeszcze nie u graczy; poniższe kroki robią to samo ręcznie.

1. Zainstaluj Gothic 3 ze Steama, potem **Community Patch 1.75.14**.
2. Zainstaluj [Parallel Universe Patch **1.1.1**](https://www.worldofgothic.de/dl/download_678.htm) do tej samej
   instalacji gry. Obaj gracze muszą mieć te same wersje łatek.
3. Używaj tego samego pakietu na obu komputerach. Skopiuj `Script_G3Together.dll` do `<Gothic 3>\scripts\`
   i sprawdzony 32-bitowy `steam_api.dll` do `<Gothic 3>\G3Together\`, albo użyj instalatora.
4. Skopiuj z pakietu odpowiedni plik `g3together.ini.host-example` albo `g3together.ini.client-example` do `<Gothic 3>\`,
   zmień jego nazwę na `g3together.ini` i edytuj go:

   Host:

   ```ini
   role = host
   transport = steam
   language = auto
   ```

   Dołączający gracz:

   ```ini
   role = client
   transport = steam
   language = auto
   ```

   Dołączający gracz musi mieć `role = client`. Jeśli brakuje pliku INI albo rola ma inną wartość, gra startuje jako host.

### Język komunikatów moda

`language = auto` (tak zapisuje instalator i przykładowe pliki INI) pokazuje komunikaty moda w języku tekstów twojego
Gothic 3: po polsku, gdy gra jest po polsku, po niemiecku, gdy jest po niemiecku, w innych przypadkach po angielsku.
Na stałe możesz wybrać `language = pl`, `language = de` albo `language = en` we własnym `g3together.ini` i uruchomić grę
ponownie. Bez wiersza `language` komunikaty są po angielsku. Każdy gracz wybiera niezależnie; ustawienie nie zmienia
języka samego Gothic 3. Przykłady komunikatów w tym poradniku są po polsku.

## Połączenie przez internet

Steam jest jedyną drogą połączenia. Adres hosta, VPN i przekierowanie portów na routerze nie są potrzebne.

### Przez Steam (eksperymentalnie)

Z `transport = steam` w obu plikach INI gra dołączającego gracza sama znajduje hosta wśród jego znajomych na Steam, a cały
ruch idzie przez Steam, w razie potrzeby przez przekaźniki Valve.

- Obaj gracze muszą posiadać Gothic 3 na Steam, mieć uruchomiony i zalogowany Steam oraz być znajomymi.
  Nazwy graczy pochodzą automatycznie ze Steam; nazwa w INI jest ignorowana.
- Mod potrzebuje 32-bitowego pliku `steam_api.dll` od Valve w `<Gothic 3>\G3Together\`. Instalator umieszcza go tam (z
  paczki albo, po pytaniu, pobrany z wydania Steamworks.NET 2025.164.1 i sprawdzony sumą SHA-256).
- Mod używa Gothic 3 (AppID 39500), więc aktywność Steam pokazuje Gothic 3 zamiast Spacewar.
- Obaj gracze wybierają w instalatorze ten sam `session_id` (także w `g3together.ini`). Dla trzech osobnych par
  użyj `pair-red`, `pair-green`, `pair-blue`, po jednym ID na hosta i jego klienta. ID zawiera 1-63 litery ASCII,
  cyfry, `_` lub `-`; wielkość liter ma znaczenie. Brak wpisu oznacza `default`.
  Menu główne i log pokazują ID. Zaproszenia i numery lobby nie omijają sprawdzania ID.
  Nieprawidłowe ID blokuje uruchomienie sieci. To publiczna nazwa grupy, nie hasło.
  Wszyscy potrzebują nowej wersji; starsze przekaźniki Steam są niezgodne. Każda równoczesna instancja wymaga
  osobnej tożsamości Steam oraz oddzielnych folderów gry, profilu i zapisów.
- Gdy hostuje kilku znajomych naraz, dołączający gracz wpisuje `steam_host = <nazwa hosta na Steam>`. Plik
  `g3together.log` hosta podaje też numer lobby; `steam_lobby = <numer>` dołącza dokładnie do niego.
- Bez Steam albo wymaganego `steam_api.dll` kooperacja nie uruchamia się; menu główne i gra pokazują błąd.
  Nie ma połączenia zastępczego. Gniazda UDP ENet są przypisane tylko do localhost i akceptują wyłącznie połączone przekaźniki Steam.
- Zaproszenie przez nakładkę Steam może się w Gothic 3 nie otworzyć. Nie jest potrzebne: gra dołączającego gracza sama
  znajduje hosta, a „Dołącz do gry” na liście znajomych Steam też działa, gdy jego gra jest uruchomiona.

To połączenie przetestowano z zastępnikiem Steam na jednym komputerze; gra między dwoma prawdziwymi kontami Steam nie została
jeszcze sprawdzona.

## Start i zapisywanie

1. Host uruchamia Gothic 3 i zaczyna nową grę albo wczytuje kampanię kooperacji.
2. Drugi gracz uruchamia Gothic 3.
   - Jeśli host wczytał albo zapisał kampanię, zostań w menu głównym. Kampania zostanie przesłana i wczytana automatycznie
     po dołączeniu.
   - Jeśli host zaczął nową grę, ty też zacznij nową grę.
3. Kampanię zapisuje **host**. Każdy dołączający gracz automatycznie zachowuje do tego zapisu własny stan bohatera: poziom,
   doświadczenie, atrybuty, ekwipunek itd. Zostaje on przywrócony, gdy gracz dołączy ponownie. Przy pierwszym dołączeniu
   z menu głównego gracz dostaje kopię bohatera hosta. Jeśli chcesz zachować własnego nowego bohatera, zacznij nową grę
   przed dołączeniem.

## Co jest wspólne

- Gracze widzą się nawzajem, łącznie z ruchem, animacjami walki i upadkami.
- Protokół 69 jest nadal eksperymentalny: host symuluje NPC przy wszystkich graczach, również w odległych regionach.
  Klienci odtwarzają buforowane pozycje i animacje bez własnej sztucznej inteligencji ani zadań NPC.
- Każdy gracz ocenia widoczne trafienia, a host stosuje obrażenia NPC. Doświadczenie dostaje gracz, który ją zabije.
  Zabójstwa przez towarzyszy albo przywołane istoty gracza nagradzają tego gracza; zabójstwa przez inne postacie nie
  nagradzają nikogo. Jak w grze dla jednego gracza, ostatni cios w człowieka albo orka powala go (doświadczenie, bez
  morderstwa); dopiero cios w leżącego zabija.
- Towarzysze idą za graczem, który ich zwerbował, także daleko od hosta. Zawsze symuluje ich host;
  werbowanie i odprawianie zmieniają przynależność do drużyny, nie władzę nad symulacją.
- Postać, która cię prowadzi albo którą eskortujesz (zadania prowadzenia i eskorty), idzie z graczem, który przyjął
  zadanie, także z dołączającym graczem, a po wczytaniu zapisu przez hosta ten gracz znowu prowadzi. Gdy eskorta kończy się
  daleko od hosta, postać stoi w tym samym miejscu na obu komputerach.
- Stan zadań i wcześniej ukończone dialogi są wspólne. Nagrody za zadania dostają wszyscy. Zadania polegające na zabiciu
  liczą też wrogów zabitych w regionie dołączającego gracza.
- Główna fabuła jest wspólna do końca: gdy jeden gracz odda artefakty w świątyni boga, rozdział zmienia się dla obu, a gdy
  jeden gracz rozpocznie epilog, drugi widzi te same obrazy i zakończenie gry w tym samym czasie. Jeśli drugi gracz
  akurat rozmawia, ma otwarte menu albo walczy, jego epilog rozpocznie się po zakończeniu rozmowy, walki lub menu (mówi
  o tym komunikat).
- Stołek, ławka, ognisko albo inne miejsce ma jednego użytkownika: dopóki używa go drugi gracz, ty nie możesz („<Nazwa>
  właśnie tego używa.”), a postacie też go nie zajmą.
- Przedmioty, skrzynie, zwłoki i handel są wspólne: co jeden gracz zabierze, znika dla wszystkich. Postaci okradzionej
  przez jednego gracza drugi nie okradnie ponownie.
- Wszyscy mają tę samą porę dnia. Sen przesuwa czas dla wszystkich graczy, tak samo czekanie do rana: ten, kto czeka,
  przeskakuje od razu na godzinę 9.
- Zwierzęta, które wracają po kilku dniach (ścierwojady, jelenie, wilki i podobne), wracają na obu komputerach albo na
  żadnym.
- Podniesienie powalonego gracza kosztuje jedną miksturę leczniczą. Gracz, który by zginął, zostaje na ziemi, dopóki
  połączony drugi gracz w tym samym świecie, który sam nie leży, może pomóc. Porozmawiaj z postacią powalonego gracza
  i wybierz „Pomogę ci.”, aby pomóc mu wstać. Zużyta zostaje najtańsza mikstura lecznicza z ekwipunku pomagającego.
  Bez mikstury obaj gracze widzą komunikat, a powalony gracz czeka, aż drugi wróci z miksturą.
- Jeśli nikt nie może pomóc, bo wszyscy leżą, albo drugi gracz jest rozłączony, w innym świecie lub poza grą dłużej niż
  pięć sekund, bohater ginie jak w grze dla jednego gracza. Host wczytuje zapis; drugi gracz zostaje do niego wczytany
  automatycznie.
- Powalenie przez samą grę działa jak w grze dla jednego gracza: gdy strażnik albo inna osoba pokona cię w walce (bez
  zabijania), po chwili wstajesz sam, nikt nie musi pomagać, a zwycięzca może zabrać ci połowę złota. Gdy zrobi to postać
  z gry drugiego gracza, widzisz „<Nazwa> – zabrano ci N szt. złota.” Drugi gracz nie może cię przeszukać.
- Postać, która widzi kradzież gracza, reaguje jak w grze dla jednego gracza, także gdy symuluje ją komputer drugiego
  gracza: idzie za złodziejem. Tak samo widziane są walki i morderstwa obu graczy: gapie i strażnicy przychodzą podczas
  bójki, a ścigany jest tylko morderca. Strażnicy ostrzegają, a potem atakują gracza, który dobędzie broni w ich pobliżu
  albo wejdzie do strzeżonego miejsca. Morderstwo popełnione przez towarzysza gracza zostaje zauważone jak w grze dla
  jednego gracza.
- Gapie podchodzą do walki każdego z graczy i widzą powalenie, niezależnie od tego, kto powalił. Przeciwnik i gapie
  pamiętają, jak skończyła się walka, a postać, która wciąż jest zła na jednego gracza, ma pretensje tylko do niego.
- Blokowanie działa dla obu graczy jak w grze dla jednego gracza: blok kosztuje wytrzymałość, a talent tarczy się liczy.
  Postać, która paruje, blokuje ciosy obu graczy tak samo.
- Ogień przyjacielski jest domyślnie wyłączony (`friendly_fire = 0`): gracze i ich towarzysze nie mogą się wzajemnie
  zranić, powalić, ogłuszyć, zatruć ani zamrozić. Leczenie i podnoszenie nadal działają; obrażenia od wrogów i otoczenia
  pozostają bez zmian. Ustaw tę samą wartość na obu komputerach i uruchom grę ponownie. Wartość od 1 do 100 zachowuje
  dotychczasowe skalowanie obrażeń między graczami; instalator zachowuje istniejące ustawienia.
- Gracz w promieniu około 20 m słyszy dialog drugiego gracza. Przy włączonych napisach gry widać też tekst kwestii,
  w języku twojej gry, chyba że sam prowadzisz rozmowę. Widzisz również odpowiedzi, które drugi gracz może wybrać, a potem
  tę wybraną; wybiera ten, kto rozmawia.
- Postać rozmawia z jednym graczem naraz: dopóki rozmawia z nią drugi gracz, jest dla ciebie zajęta („<Postać> rozmawia
  właśnie z graczem <gracz>.”). Gestykuluje i zwraca się do rozmówcy na obu komputerach.
- Postać z ważną wiadomością dla gracza (strażnik, zleceniodawca) podchodzi do tego gracza i zagaduje go jak w grze dla
  jednego gracza, także do dołączającego gracza.
- Martwe postacie pozostają martwe, także te zabite daleko od hosta.
- Otwarcie drzwi albo zamka wytrychem otwiera je dla wszystkich, także dla graczy, którzy dołączą później.
- Postacie reagują na ataki obu graczy jak w grze dla jednego gracza: walczą, uciekają albo wołają o pomoc.
- Gracze widzą nawzajem swoje strzały, bełty i kule ognia w locie; każde trafienie liczy się raz. Czary leczenia działają
  też na drugiego gracza.
- Czary postaci, takie jak sen, strach, urok, trucizna i choroba, działają na obu komputerach. Przywołane istoty są
  widoczne dla drugiego gracza i włączają się do walki.

## Zgłaszanie błędu

- Zapisz godzinę, kto był hostem, co robiliście i co widział każdy gracz.
- Obaj gracze uruchamiają `tools\collect_logs.py` z folderu rozpakowanego pakietu i wysyłają powstały plik ZIP. Polecenia
  są w [liście kontrolnej testu](PLAYTEST.md) (po angielsku).
- Przy powtarzalnych problemach z postaciami albo wrogami, np. martwym orku, który na drugim komputerze stoi, dopisz do INI
  `trace = on` i uruchom grę ponownie. Gra zapisuje `g3trace.log` w swoim folderze instalacji: animacje postaci, zadania
  i kod gry, który je wywołał. Plik ma najwyżej około 50 000 wierszy i jest tworzony od nowa przy każdym starcie. Narzędzie
  do zbierania logów go dołącza. Po zebraniu zgłoszenia usuń to ustawienie.

## Znane problemy (21 października 2026)

| Co może się zdarzyć | Co zrobić |
|---|---|
| Gra może się zawiesić podczas walki albo krótko po śmierci wroga. Znany błąd przy napadzie orków jest zabezpieczony, ale inne awarie są możliwe. | Host powinien zapisywać przed trudnymi walkami. Zapisz godzinę i wroga, zachowaj logi obu graczy przed ponownym uruchomieniem. Host wczytuje ostatni dobry zapis; drugi gracz zostaje do niego wczytany automatycznie (jeśli zawiesiła się jego gra, uruchamia ją ponownie i czeka w menu głównym). |
| Postać drugiego gracza przez chwilę zostaje w tyle podczas trafienia albo niedokładnie pokazuje ruch. | Zbierzcie się i chwilę poczekajcie; w razie potrzeby użyj **Page Down**. Jeśli wrogowie, zdrowie albo pozycje wyraźnie się różnią, przerwij walkę i zapisz problem. |
| Wrogowie albo towarzysze zachowują się inaczej na obu komputerach po dołączeniu. | Sprawdź, czy obaj gracze widzą te same postacie i ruchy w pobliżu hosta. Jeśli różnica się utrzymuje, zachowaj logi i zacznijcie razem od ostatniego dobrego zapisu. |
| „<Nazwa> właśnie tego używa.”, gdy chcesz usiąść albo użyć ogniska, kowadła lub innego miejsca | Drugi gracz właśnie tego używa; poczekaj, aż wstanie. Boty sprawdziły tylko stołki; jeśli ktoś mimo to utknie w takim miejscu, zgłoś błąd i wczytajcie grę razem. |
| Zapisywanie w postaci zwierzęcia nie zostało sprawdzone. | Przed zapisem wróć do ludzkiej postaci. |
| Kontrolowana albo oswojona postać nie idzie niezawodnie za graczem albo opuszcza drużynę. | Nie polegaj jeszcze na tych umiejętnościach. Zapisz postać i godzinę; ponowne rzucenie czaru nie jest sprawdzonym obejściem. |
| Ogłuszenie i hipnoza rzucone przez dołączającego gracza działają tylko przez hosta. | Zapisz nieudane próby; czar może rzucić host. |
| Przywołana istota przestaje się ruszać na drugim komputerze, gdy gracze bardzo się oddalą. | Używaj przywołań, gdy jesteście razem. Jeśli to się zdarzy, zapisz miejsce i istotę. |
| Trucizna albo choroba nie jest w pełni widoczna na drugim komputerze, a śmierć od trucizny może nie dać doświadczenia. | Zapisz rozbieżność. |
| Strażnik, który rozmawia z tobą o przestępstwach, liczy cały rejestr miasta (przy `shared` albo `together` także czyny drugiego gracza). | Jeśli ci to przeszkadza, unikaj kradzieży i celowych ataków na spokojnych mieszkańców. Zapisz, kto działał i do kogo strażnik się zwrócił albo kogo zaatakował. |

## Znane ograniczenia

- Okolica każdego gracza działa na jego komputerze i jest taka sama dla obu; odległość nie wywołuje komunikatu. Host i
  dołączający gracz widzą tylko „Gracz <nazwa> jest w innym świecie.”, gdy drugi gracz jest w innym świecie.
- **Skok do drugiego gracza:** **Page Down** stawia cię 1,5 m za drugim graczem w tym samym świecie, także za powalonym
  graczem, któremu chcesz pomóc. Można go użyć raz na pięć sekund. Lepiej trzymać się razem, ale możecie się rozdzielić
  i potem zebrać. Aby zmienić klawisz, wpisz w INI `partner_key = F9` (F1–F24, PAGEUP, HOME, END, INSERT, DELETE albo
  litera; Insert i Delete to klawisze handlu opisane niżej).
- **Mapa i kompas:** drugi gracz jest zaznaczony na twojej mapie, z nazwą, i na kompasie w swoim kierunku względem tego,
  gdzie patrzysz.
- **Twój wygląd:** drugi gracz widzi twoją zbroję i hełm. Aby wyglądać jak inna postać, ustaw we własnym INI
  `look = Diego` (szablon postaci z gry); bez tego decyduje `puppet_template` drugiego gracza (domyślnie: bohater).
- **Handel z drugim graczem:** porozmawiaj z postacią drugiego gracza i wybierz „Pokaż mi swoje towary.”. Otworzy się okno
  handlu gry z jego niezałożonymi przedmiotami i złotem. Wybierz, co chcesz wziąć lub przekazać, i użyj zwykłego
  przycisku wymiany. Przedmioty są darmowe: nie potrzeba zapłaty ani równej wartości towarów. Złoto przekazujesz
  osobno, wybierając jego ilość, także bez przedmiotów. Każda wymiana synchronizuje się automatycznie przy otwartym
  oknie, bez zatwierdzania przez partnera. Założony/trzymany sprzęt, przedmioty fabularne i umiejętności pozostają
  chronione; jakość przedmiotów jest zachowana. Pierwszą wskazówkę gry zamknij przez Ok.
  Dawne klawisze ofert i ceny już nie działają; handel z NPC pozostaje bez zmian. Przed zapisem lub wczytaniem
  poczekaj na synchronizację, a po rozłączeniu połącz partnera ponownie.
  Nowy przepływ zbudowano do testów ręcznych; obsługa myszą i przekazywanie samego złota wymagają sprawdzenia w grze.
- Zmiany postaci przez dialogi, np. ustawienia handlarza, nauczyciela, strażnika i rozkładu dnia, obowiązują wszystkich,
  podobnie jak pojedynki, ataki i werbowanie towarzyszy wywołane dialogiem. Przedmioty z dialogów dostaje gracz, który
  rozmawia.
- Zapis hosta pamięta, do którego gracza należy każdy towarzysz. Po wczytaniu towarzysz wraca do swojego gracza, gdy ten
  dołączy ponownie; gracz jest rozpoznawany po nazwie na Steam. Nie zmieniaj tej nazwy. Po rozłączeniu bez wczytywania
  towarzysze należą najpierw do hosta.
- Jeśli host wczyta zapis podczas sesji, pozostali gracze zostają do niego wczytani automatycznie. Dopóki to trwa, nie
  widzicie się nawzajem.
- Dawne testy handlu używały pomocników ekwipunku, nie przycisku wymiany. Nie potwierdzają nowego przepływu;
  wymagane są ręczne testy okna handlu.
- Sen przesuwa zegar dla wszystkich, ale leczy tylko śpiącego gracza.
- Przemiany: drugi gracz widzi poruszające się zwierzę, a po końcu przemiany znowu bohatera. Wilka i krwiopijcę sprawdzono
  w takim samym układzie jak czar gry. W postaci zwierzęcia walczysz jako ty: twoje trafienia liczą się jako twoje
  (powalenia, zabójstwa, doświadczenie, morderstwa), a trafienia wroga ranią zwierzę, nie twoje ukryte ludzkie ciało
  (wilka sprawdziły boty). Zapisywanie w postaci zwierzęcia pozostaje nieprzetestowane.
- Burza gradowa drugiego gracza: jej pogoda zostaje teraz u drugiego gracza na twoim komputerze (wcześniej zaciemniała
  niebo i uderzała wokół twojego bohatera; boty sprawdziły to po pozycji i zrzucie ekranu). Obrażenia zadaje tylko burza
  rzucającego, jak w grze dla jednego gracza.
- Wrogowie atakują obu graczy. Ich nastawienie do dołączającego gracza pochodzi od bohatera na komputerze, który akurat
  symuluje daną postać. Na przykład złe stosunki hosta z orkami dotyczą też drugiego gracza w przypadku postaci
  symulowanych przez hosta.
- Miasto albo obóz wyzwolony (albo zdobyty) przez któregokolwiek gracza jest wyzwolony dla obu: stan miasta pochodzi
  od hosta.
- Reputacja w miastach i obozach, wybierana przez `reputation` w INI hosta:
  - `shared` (domyślnie): jeden rejestr na miasto dla obu graczy; liczą się kradzieże obu graczy, a strażnik, który
    rozmawia z tobą o przestępstwach, liczy czyny wszystkich. Postać, która widziała przestępstwo, ma pretensje do tego,
    kto je popełnił.
  - `together`: jak `shared`, a postać, która widziała przestępstwo jednego z was, ma pretensje do was obu (obaj
    zostajecie odprawieni).
  - `personal`: każdy gracz ma własny rejestr; to, co robi drugi gracz, nigdy nie zmienia twojego.
  Morderstwo liczy się dla miasta ofiary jak w grze dla jednego gracza, także gdy ofiarę symuluje komputer drugiego
  gracza (`personal`: w rejestrze mordercy). Bicie człowieka albo orka najpierw go powala; dopiero cios w leżącego zabija.
- Podróż duszy: drugi gracz widzi poruszającego się ducha, a nie ciało, które zostawiłeś.
- Czary lodu i ognia, które gaszą płonięcie albo zamrożenie (i rozbijają takie pociski), działają na postacie drugiego
  gracza tylko na twoim komputerze. Rzadko to zauważysz.
- Postać z wiadomością przestaje do ciebie podchodzić, gdy oddalisz się od niej na więcej niż około 25 m; porozmawiaj
  z nią sam.
- Jeszcze niesprawdzone, bo boty nie potrafią przygotować tych scen: powstanie w Cape Dun, wyzwolenie miasta w środku gry
  (boty sprawdziły Ardeę na początku), zadania polegające na przegnaniu kogoś (walka w dialogu) i plądrowanie. Są na
  liście kontrolnej testu.
- Główną fabułę boty przeszły do zakończenia Innosa (z ognistymi pucharami z Nordmaru). Zakończeń Adanosa i Beliara,
  planu Xardasa i piątej świątyni nikt jeszcze nie przeszedł wspólnie, a nie wiadomo jeszcze, co dzieje się
  z połączeniem po zakończeniu gry.
- Pływanie do przodu nie było jeszcze próbowane przez człowieka (boty nie potrafią płynąć do przodu).
- Nieprzetestowane: potwory, które dalej atakują powalonego bohatera, gra przez prawdziwy internet i więcej niż dwóch
  graczy. Powyższe funkcje sprawdzono botami. Pierwszy krótki test ludzi z dwoma oknami na jednym komputerze znalazł dwa
  błędy: wrogowie ignorowali dołączającego gracza, a walka nie była widoczna na jego komputerze. Oba naprawiono
  i sprawdzono botami, ale ludzie jeszcze ich ponownie nie sprawdzili.
