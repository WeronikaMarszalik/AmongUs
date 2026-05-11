Bundled SDL dependencies
========================

Ten katalog zawiera pliki potrzebne do zbudowania i uruchomienia klienta SDL
bez instalowania SDL2 osobno:

  include/   naglowki SDL2 i SDL2_ttf
  lib/       biblioteki importu dla MinGW-w64
  bin/       DLL-e wymagane przy uruchamianiu sdl_client.exe
  licenses/  licencje dolaczonych bibliotek

Makefile uzywa tych plikow przez sciezki wzgledne:

  SDL_DIR=third_party/SDL2

Do kompilacji nadal wymagany jest toolchain MinGW-w64 z programem
mingw32-make.
