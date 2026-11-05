# G3 Together: dołączanie do hosta

[English](JOINING.md) · [Deutsch](MITSPIELER.md) · **Polski**

## Przed startem

- Oba komputery powinny mieć przetestowaną podstawę: Gothic 3 ze Steama, **Community Patch 1.75.14**
  i **Parallel Universe Patch 1.1.1**. Używajcie tej samej biblioteki DLL G3 Together i tych samych plików gry.
  Działanie bez PUP nie zostało potwierdzone.
- Poziom trudności, alternatywny balans, alternatywna SI, mnożnik doświadczenia, minimalny czas trafienia, Quick Loot
  i sekcja `[Game]` w `Ini\ge3.ini` muszą być takie same jak u hosta.
- W folderze `scripts` nie może być innego moda kooperacji.
- Steam musi działać i być zalogowany na obu komputerach; obaj gracze muszą być znajomymi na Steam.

## Instalacja

**Instalator:** gdy Gothic 3 i obie łatki są zainstalowane, rozpakuj cały pakiet w dowolnym miejscu i kliknij dwukrotnie
`Instalacja.cmd` (po angielsku `Setup.cmd`, po niemiecku `Einrichten.cmd`). Instalator zrobi resztę: znajdzie Gothic 3
(Steam, GOG albo podany folder), sprawdzi Community Patch 1.75.14 i Parallel Universe Patch 1.1.1, przeniesie inny mod
kooperacji ze `scripts`, skopiuje mod do `scripts` i zapisze `g3together.ini` na podstawie twoich odpowiedzi (host czy
dołączający gracz). Nazwa pochodzi automatycznie ze Steam. Steam i sprawdzony `steam_api.dll` są wymagane; bez tej DLL instalator kończy pracę.
Zastąpione pliki trafiają do `G3Together-backup` w folderze gry. Instalator
sprawdzono na folderach testowych, jeszcze nie u graczy; poniższe kroki robią to samo ręcznie.

1. Zainstaluj Gothic 3 ze Steama, potem **Community Patch 1.75.14**, potem
   [Parallel Universe Patch **1.1.1**](https://www.worldofgothic.de/dl/download_678.htm)
   do tej samej instalacji gry.
2. Skopiuj `Script_G3Together.dll` z tego samego pakietu co host do `<folder Gothic 3>\scripts\`
   i sprawdzony 32-bitowy `steam_api.dll` do `<folder Gothic 3>\G3Together\`, albo użyj instalatora.
3. Skopiuj `g3together.ini.client-example` z pakietu do `<folder Gothic 3>\`, zmień nazwę na `g3together.ini` i wpisz
   swoje dane:

   ```ini
   role = client
   transport = steam
   language = auto
   ```

Nazwa gracza pochodzi automatycznie ze Steam. Twój komputer musi mieć
`role = client`. Nie zmieniaj swojej nazwy na Steam: host po niej przywraca twoich towarzyszy po wczytaniu zapisu.

`language = auto` pokazuje komunikaty moda w języku tekstów twojego Gothic 3 (polska gra: po polsku). Na stałe możesz
ustawić `language = pl`, `de` albo `en`; bez tego wiersza komunikaty są po angielsku. Każdy gracz wybiera niezależnie;
ustawienie nie zmienia języka samego Gothic 3.

## Dołączanie

Host uruchamia Gothic 3 i zaczyna nową kampanię albo wczytuje kampanię kooperacji. Ty uruchamiasz grę potem. Jeśli host
wczytał albo zapisał kampanię, zostań w menu głównym i poczekaj, aż kampania zostanie przesłana i wczytana. Jeśli host
zaczyna zupełnie nową grę, ty też zacznij nową grę.

Jeśli dołączenie zostanie odrzucone, otwórz `g3together.log` w folderze Gothic 3 i znajdź wiersz z `REFUSED by host`.
Porównaj wersje i opcje gry z hostem. Powód zostaje też w górnym wierszu menu głównego.

**Steam jest jedyną drogą połączenia** (zob. [GRANIE.md](GRANIE.md#przez-steam-eksperymentalnie)).
Twoja gra szuka hosta wśród twoich znajomych na Steam; dopóki go nie znajdzie, po około 15 sekundach
mówi o tym górny wiersz menu głównego. Host musi być na twojej liście znajomych na Steam i mieć uruchomiony Steam i grę.
Jeśli Steam nie może się uruchomić, pojawia się błąd. Sprawdź Steam i wymaganą DLL, potem uruchom grę ponownie.
Nie ma połączenia zastępczego przez IP.

## Podczas gry

- Gdy leżysz powalony, poczekaj na drugiego gracza. Połączony gracz w tym samym świecie może pomóc ci wstać: rozmawia
  z tobą i wybiera „Pomogę ci.”. Kosztuje go to miksturę leczniczą. Bez mikstury leżysz, dopóki jej nie zdobędzie. Jeśli
  wszyscy leżą albo jesteś sam, giniesz jak w grze dla jednego gracza: koniec gry, potem wczytaj zapis.
- Noś mikstury lecznicze dla drugiego gracza.
- Okolica każdego gracza działa na jego komputerze i jest taka sama dla obu, więc odległość od hosta nie wywołuje
  komunikatu. Gdy drugi gracz jest w innym świecie, widzisz **„Gracz <nazwa> jest w innym świecie.”**
- **Page Down** stawia cię tuż za drugim graczem, także gdy leży powalony. Można go użyć raz na pięć sekund. Aby wybrać
  inny klawisz, ustaw w INI `partner_key = F9` (F1–F24, PAGEUP, HOME, END, INSERT, DELETE albo litera).
- Drugi gracz jest zaznaczony na twojej mapie i kompasie.
- Drugi gracz widzi twoją zbroję i hełm. `look = Diego` w twoim INI pokazuje cię jako inną postać.
- Handel z drugim graczem: „Pokaż mi swoje towary.”, wybierz przedmioty lub złoto i użyj przycisku wymiany.
  Przedmioty są darmowe; wybrane złoto jest prezentem. Bez zatwierdzania przez partnera i bez zamykania okna.
  Założony sprzęt, przedmioty fabularne i umiejętności pozostają chronione. Testy ręczne są jeszcze wymagane.
- Kampanię zapisuje host.
- Problemy i obejścia: **Znane problemy** w [pełnym poradniku](GRANIE.md).

## Gdy coś pójdzie nie tak

Otwórz PowerShell w folderze rozpakowanego pakietu, tam gdzie leży ten plik, i uruchom poniższe polecenie. Potrzebny jest
Python:

```powershell
python "tools\collect_logs.py" "<folder Gothic 3>"
```

Wyślij hostowi powstały plik `g3together-logs-<czas>.zip`. Powstaje w folderze pakietu. Bez Pythona zbierz ręcznie
`g3together.log` z folderu Gothic 3 oraz pliki `.log` z `Dokumenty\gothic3`, łącznie z podfolderem `paru`.
