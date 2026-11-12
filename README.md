# playlist-sync

Stáhne písničky ze Spotify playlistů jako mp3 a otaguje je: název, interpret,
album, rok, žánr, číslo stopy a cover alba.

Spotify slouží jen jako seznam písniček. Samotné mp3 se stahuje z YouTube
přes `yt-dlp` a tagy se dohledávají na Deezeru, záložně na iTunes.

Program je napsaný v C++ (`playlist-sync.cpp`), takže ho na každém zařízení
nejdřív zkompiluješ.

## Instalace

### 1. Programy, které playlist-sync spouští

`yt-dlp` stahuje z YouTube, `ffmpeg` převádí do mp3 a `ffprobe` zjišťuje délku
skladby.

```bash
# Fedora
sudo dnf install ffmpeg
# yt-dlp radši přímo od autorů, verze v distribuci často nestíhá změny YouTube
mkdir -p ~/.local/bin
curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp -o ~/.local/bin/yt-dlp
chmod +x ~/.local/bin/yt-dlp
```

Když yt-dlp přestane stahovat, aktualizuj ho: `yt-dlp -U`.

### 2. Kompilátor a knihovny

Potřebuješ kompilátor s C++20 a `std::format` (GCC 13+ nebo Clang 17+), CMake
a vývojové balíčky těchto knihoven:

| Knihovna | K čemu |
|---|---|
| libcurl | stahování (Spotify, Deezer, iTunes, covery) |
| TagLib | čtení a zápis ID3 tagů v mp3 |
| utf8proc | odstranění diakritiky při porovnávání názvů |
| nlohmann/json | čtení a zápis JSON |

```bash
# Fedora
sudo dnf install gcc-c++ cmake libcurl-devel taglib-devel utf8proc-devel json-devel

# Debian 13 / Ubuntu 24.04 a novější (názvy balíčků neověřené)
sudo apt install g++ cmake pkg-config libcurl4-openssl-dev libtag-dev libutf8proc-dev nlohmann-json3-dev

# Arch
sudo pacman -S base-devel cmake curl taglib libutf8proc nlohmann-json
```

Debian 12 má jen GCC 12, které `std::format` neumí.

### 3. Kompilace

V kořeni projektu:

```bash
cmake -B build             # jednou: připraví složku build/
cmake --build build        # zkompiluje
```

Výsledek je spustitelný soubor `./playlist-sync` v kořeni projektu, vedle
`playlist-sync.cpp`. Složka `build/` obsahuje jen mezivýsledky kompilace a
`build/` i `playlist-sync` jsou v `.gitignore`.

Po každé změně kódu nebo `git pull` stačí znovu `cmake --build build`. Když se
něco zasekne (třeba po instalaci nové verze knihovny), smaž `build/` a začni
od `cmake -B build`.

### 4. Nastavení

```bash
cp config-example.json config.json
```

a uprav `config.json`, viz [Nastavení](#nastavení).

## Spouštění

Program hledá `config.json`, `playlist.json` a `preskocit.txt` **ve složce,
kde leží spustitelný soubor**, ne v aktuální složce. Spouštět ho jde odkudkoli:

```bash
cd ~/0_PROJEKTY/playlist-sync && ./playlist-sync
~/0_PROJEKTY/playlist-sync/playlist-sync          # odjinud
```

Jestli ho chceš spouštět jen jako `playlist-sync`, udělej si symlink do
`~/.local/bin`. Symlink funguje, protože program si dohledá, kde skutečně leží.
**Kopie by nefungovala**, hledala by `config.json` vedle sebe.

```bash
ln -s ~/0_PROJEKTY/playlist-sync/playlist-sync ~/.local/bin/playlist-sync
```

## Nastavení

`config.json` vedle programu:

```json
{
    "playlist_url": [
        "https://open.spotify.com/playlist/...",
        "https://open.spotify.com/album/..."
    ],
    "slozka_mp3": "~/Hudba/playlist"
}
```

- `playlist_url`: jedna URL, nebo pole URL playlistů či alb. Spotify vrací
  nejvýš 100 skladeb na playlist, takže větší playlist rozděl na víc menších.
  Za poslední URL v poli nesmí být čárka.
- `slozka_mp3`: kam se ukládají mp3, `~` znamená domovskou složku.

`config.json` je v `.gitignore`, takže si ho na každém zařízení nastavíš zvlášť,
třeba s jinou cestou ke složce.

## Použití

```bash
./playlist-sync                   # stáhni nové písničky a otaguj je
./playlist-sync --otagovat        # navíc otaguj i ručně přidané písničky
./playlist-sync -n                # jen ukaž, co by se stáhlo
./playlist-sync -d ~/Hudba        # otaguj jinou složku, nic nestahuj
./playlist-sync -s faded          # přetaguj jednu písničku
./playlist-sync -x "Alex G - *"   # tyhle soubory příště přeskoč
./playlist-sync --help            # všechny přepínače
```

Před prvním skutečným během se vyplatí `-n`: ukáže, co by se stáhlo a jaké
tagy by se našly, a nic nezmění.

### Přepínače

| Přepínač | Co dělá |
|---|---|
| `-h`, `--help` | vypíše nápovědu |
| `--otagovat` | po stažení projde všechna mp3 ve `slozka_mp3` (i podsložky) a otaguje ty, kterým chybí album nebo cover |
| `-d`, `--dir SLOŽKA` | nic nestahuje, jen otaguje mp3 v téhle složce (i podsložky). Lze zadat víckrát. |
| `-s`, `--pisnicka NÁZEV` | nic nestahuje, přetaguje jednu písničku. Stačí cesta k souboru nebo část názvu, na diakritice a velikosti písmen nezáleží. Vždy znovu, seznam přeskakovaných neplatí. Lze zadat víckrát. |
| `-x`, `--preskocit SOUBOR…` | přidá soubory nebo vzory (`*`, `?`) do `preskocit.txt` a skončí |
| `-n`, `--dry-run` | jen vypíše, co by se stáhlo a jaké tagy by se našly, nic nezapisuje |
| `-f`, `--force` | přetaguje i soubory, které už album i cover mají |
| `--save-cover` | uloží cover i jako `Soubor.jpg` vedle mp3 |
| `--min-score ČÍSLO` | minimální shoda pro přijetí výsledku hledání (0–1, výchozí 0.75) |

Krátké přepínače jde spojovat (`-nf` = `-n -f`) a hodnotu napsat i jako
`--dir=SLOŽKA`. Slova bez přepínače se berou jako další složky pro `-d`.

S `-d` nebo `-s` program pracuje jen se soubory na disku a Spotify vůbec
nenačítá. Ostatní přepínače platí ve všech režimech.

## Jak to funguje

### Stahování

1. Načte všechny playlisty z `config.json` a uloží jejich obsah do `playlist.json`.
2. Podívá se do `slozka_mp3`, které soubory už tam jsou.
3. Každou písničku, jejíž soubor `Interpret - Název.mp3` ve složce chybí,
   najde na YouTube (`ytsearch1:Interpret - Název audio`), stáhne jako mp3
   192 kb/s a hned otaguje.

Písnička se pozná **jen podle názvu souboru**, žádný seznam stažených se nevede.
Díky tomu můžeš složku sdílet mezi víc zařízeními (Syncthing, flashka…)
a každé zařízení dotáhne jen to, co ve složce chybí. Když soubor přejmenuješ
nebo smažeš, při dalším spuštění se stáhne znovu. Znaky `/ \ : * ? " < > |`
se v názvu souboru nahrazují `_`.

### Tagování

Interpret a název se berou:

- ze Spotify, když je písnička v playlistu (v režimu `-d`/`-s` z posledního
  `playlist.json`),
- jinak z názvu souboru `Interpret1, Interpret2 - Název.mp3`.

Délku skladby zjistí `ffprobe` a pomáhá vybrat správnou verzi.

Hledá se nejdřív na Deezeru. Když tam není nic dost dobrého, nebo jen
kompilace typu „Indie Hits“, zkusí se ještě iTunes. Každý výsledek dostane skóre:

- 60 % podobnost názvu + 40 % podobnost interpreta,
- +0.05, když délka sedí na ±3 s, −0.1, když se liší o víc než 20 s,
- −0.12 za kompilaci,
- −0.15 za každé nesedící slovo remix / live / acoustic / instrumental /
  remaster / version / mix.

Výsledek se skóre pod `--min-score` se nepoužije a písnička dostane aspoň
interpreta a název. Na konci se vypíše seznam nenalezených. Skóre je vidět ve
výpisu, např. `✓ Alan Walker - Faded | Faded (2015) [deezer, 1.05]`.

Do mp3 se zapíše (ID3v2.3): název, interpret, interpret alba, album, rok, žánr,
číslo stopy / počet stop, číslo disku a cover 1000×1000.

Soubory, které už mají album i cover, se přeskakují (pokud není `--force`).
Po otagování v Clementine: Nástroje → Prohledat celou sbírku znovu.

### Seznam přeskakovaných

Písničky, které se nedaří dohledat, by se zkoušely při každém `--otagovat`
znovu. Přidej je do seznamu přeskakovaných:

```bash
./playlist-sync -x "Neznámá Kapela - Písnička.mp3"
./playlist-sync -x "Alex G - *"
```

Seznam je v `preskocit.txt` vedle programu, jeden název souboru nebo vzor na
řádek. Řádky začínající `#` se ignorují.

## Práce na kódu

Celý klient je v jednom souboru `playlist-sync.cpp`, rozdělený komentáři
`// ---------- … ----------` na části: cesty a nápověda, práce s textem
a Unicode, JSON a soubory, spouštění programů, HTTP, Spotify, hledání tagů,
zápis tagů, seznam přeskakovaných, přepínače a hlavní program (`hlavni`
a `main` na konci).

Běžný postup:

```bash
# uprav playlist-sync.cpp
cmake --build build        # zkompiluj, varování (-Wall -Wextra) ber vážně
./playlist-sync -n         # vyzkoušej bez zápisu
./playlist-sync -n -s faded   # vyzkoušej tagování jedné písničky
```

Testy nejsou, ověřuje se ručně, typicky přes `-n`.

Užitečné varianty kompilace:

```bash
# ladicí verze (pro gdb, bez optimalizací)
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build

# compile_commands.json pro clangd / napovídání v editoru
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && ln -sf build/compile_commands.json .
```

Na co dát pozor:

- **`jmenoSouboru`** určuje název mp3 a podle něj se pozná, jestli je písnička
  stažená. Jakákoli změna (i jiné ořezání mezer) způsobí, že se na všech
  zařízeních začnou stahovat znovu písničky, které už tam jsou.
- Chyba jedné písničky nesmí shodit celý běh: vypsat a pokračovat
  (viz `otagujBezpecne`).
- Spotify data se čtou z embed stránky (`open.spotify.com/embed/...`, blok
  `__NEXT_DATA__`). Když Spotify stránku změní, program skončí hláškou „nejsou
  data playlistu“ a je potřeba upravit `nactiSkladby`.
- Identifikátory, výpisy i komentáře česky. Komentáře bez diakritiky, texty
  pro uživatele s diakritikou.

## Soubory

| Soubor | Co v něm je |
|---|---|
| `playlist-sync.cpp` | celý program |
| `CMakeLists.txt` | jak program zkompilovat |
| `playlist-sync` | zkompilovaný program (není v gitu) |
| `config.json` | nastavení (není v gitu), vzor v `config-example.json` |
| `playlist.json` | poslední načtený obsah playlistů (není v gitu) |
| `preskocit.txt` | seznam přeskakovaných souborů |
| `server/` | server se sdíleným seznamem písniček (rozpracovaný) |
| `playlist-sync.js`, `package.json` | původní verze v Node.js, zatím ponechaná pro porovnání |

## Časté problémy

- **`Could not find a package configuration file provided by "nlohmann_json"`**
  nebo **`No package 'taglib' found`** při `cmake -B build`: chybí vývojový
  balíček knihovny, viz [Kompilátor a knihovny](#2-kompilátor-a-knihovny).
- **`fatal error: format: No such file or directory`**: kompilátor je moc starý,
  potřebuješ GCC 13+ nebo Clang 17+.
- **`nepodařilo se stáhnout: yt-dlp nejde spustit`**: yt-dlp není nainstalovaný
  nebo není v PATH, viz Instalace.
- **`config.json není platný JSON objekt`**: chyba v `config.json`, nejčastěji
  čárka za posledním prvkem pole.
- **`Chybí …/config.json`**, i když config máš: program ho hledá vedle sebe,
  ne v aktuální složce. Nespouštíš kopii programu z jiné složky?
- **Stáhla se špatná verze z YouTube**: smaž soubor a stáhni ho ručně. Pak ho
  otaguj přes `-s` a název souboru nech `Interpret - Název.mp3`, aby ho
  program příště nestahoval znovu.
- **Našlo to špatné album**: zkus vyšší `--min-score`, nebo písničku přidej do
  seznamu přeskakovaných a otaguj ji ručně.
