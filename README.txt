AmongUs-like multiplayer prototype
==================================

Projekt sklada sie z backendu w C oraz frontendu SDL klienta.
Serwer jest autorytatywny: przechowuje globalny stan gry, waliduje akcje
graczy i rozsyla aktualizacje do wszystkich klientow.

Budowanie:
  mingw32-make

Uruchomienie:
  .\server.exe 5050
  .\sdl_client.exe 127.0.0.1 5050

Opcjonalnie nadal mozna uruchomic terminalowego klienta testowego:
  .\client.exe 127.0.0.1 5050

Klient SDL po uruchomieniu pyta w terminalu, czy stworzyc lokalne lobby, czy
dolaczyc do istniejacego lobby przez host i port. Przy tworzeniu lokalnego lobby
klient probuje automatycznie uruchomic .\server.exe na wybranym porcie.

Klient SDL pokazuje lobby i mape gry w osobnym oknie. Po wpisaniu nazwy gracza
w terminalu sterowanie odbywa sie juz w oknie gry.

Lobby:
  Jeden proces server.exe obsluguje wiele lobby jednoczesnie.

  Klient najpierw pyta o nickname, adres serwera i port. Potem mozna wybrac:
  1. Stworz nowe lobby - klient wysyla do serwera CREATE i dolacza jako pierwszy
     gracz. Jesli lokalny serwer na 127.0.0.1 nie dziala, klient sprobuje
     automatycznie uruchomic .\server.exe.
  2. Dolacz do istniejacego lobby - klient wysyla LIST, wyswietla dostepne lobby
     z ID, nazwa, liczba graczy i faza gry, a potem pyta o ID lobby.

  Opcja 2 dolacza do istniejacego lobby: klient nie tworzy serwera, tylko laczy sie
  z podanym hostem i portem. Nie ma globalnego matchmakingu przez internet; lista
  lobby pochodzi z konkretnego serwera, z ktorym klient jest polaczony.

Sterowanie w kliencie SDL:
  X           rozpoczecie gry z lobby
  W/A/S/D     ruch po mapie bez wciskania Enter
  E           rozpoczecie minitaska przez crewmate stojacego na polu T
  K           zabicie najblizszego crewmate'a przez impostora
  R           report tylko po podejsciu do ciala
  V           glosowanie odbywa sie w oknie spotkania przez wpisanie ID i Enter
  L           powrot do lobby po zakonczeniu gry
  Q           wyjscie

Glosowanie:
  Podczas meetingu zywy gracz wpisuje w oknie ID zywego gracza i wciska Enter.
  Wpisanie 0 oznacza skip. Martwi gracze nie moga glosowac, nie da sie glosowac
  na martwych graczy, a kazdy zywy gracz ma tylko jeden glos na spotkanie.

Taski:
  Sa dwa oddzielne typy taskow.
  LOAD: czekanie, az pasek postepu sie wypelni.
  CODE: trzy serie kodow; gra pokazuje 4 cyfry na chwile, a gracz musi je wpisac
  z pamieci. Dopiero po trzech poprawnych seriach serwer dostaje informacje
  o ukonczeniu taska.
  CARD: przesuwanie karty; naciskaj kilka razy strzalke w prawo, az karta
  przejedzie przez czytnik i pasek postepu sie wypelni.

Widocznosc:
  Crewmate ma male, okragle pole widzenia wokol postaci.
  Impostor widzi cala widoczna czesc mapy bez zaciemnienia.

Mapa:
  Mapa ma rozmiar 20x14, sciany i kolizje po stronie serwera. Ruch w sciane jest
  odrzucany przez backend, wiec klienci nie moga przechodzic przez przeszkody.

Legenda mapy:
  T           pole zadania
  I           zywy impostor
  C           zywy crewmate
  x           martwy gracz
  1-9         gracz w lobby, zanim role zostana przydzielone

Struktura:
  include/game_state.h + src/game_state.c
    logika gry, lobby, role, ruch, zadania, spotkania, glosowanie

  include/event_queue.h + src/event_queue.c
    centralna kolejka zdarzen chroniona sekcja krytyczna i condition variables

  include/net.h + src/net.c
    funkcje sieciowe Winsock: serwer, klient, wysylanie i odbieranie linii

  src/server.c
    akceptowanie klientow, watki klientow, przetwarzanie kolejki zdarzen

  src/client.c
    prosty frontend terminalowy, zostawiony jako narzedzie testowe

  src/sdl_client.c
    frontend SDL: lobby, okno gry, render mapy, graczy, zadan i realtime input
