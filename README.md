# playlist-sync

Stáhne písničky ze Spotify playlistů jako mp3 a otaguje je: název, interpret,
album, rok, žánr, číslo stopy a cover alba.

Spotify slouží jen jako seznam písniček. Samotné mp3 se stahuje z YouTube
přes `yt-dlp` a tagy se dohledávají na Deezeru, záložně na iTunes.

## Instalace

Potřebuješ Node.js 20.11 nebo novější, `yt-dlp` a `ffmpeg` (včetně `ffprobe`).

```bash
# Fedora
sudo dnf install ffmpeg
# yt-dlp radši přímo od autorů, verze v distribuci často nestíhá změny YouTube
curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp -o ~/.local/bin/yt-dlp
chmod +x ~/.local/bin/yt-dlp

cd playlist-sync
npm install
cp config-example.json config.json
```

Když yt-dlp přestane stahovat, aktualizuj ho: `yt-dlp -U`.

## Nastavení

`config.json` vedle skriptu:

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
node playlist-sync.js                   # stáhni nové písničky a otaguj je
node playlist-sync.js --otagovat        # navíc otaguj i ručně přidané písničky
node playlist-sync.js -n                # jen ukaž, co by se stáhlo
node playlist-sync.js -d ~/Hudba        # otaguj jinou složku, nic nestahuj
node playlist-sync.js -s faded          # přetaguj jednu písničku
node playlist-sync.js -x "Alex G - *"   # tyhle soubory příště přeskoč
node playlist-sync.js --help            # všechny přepínače
```

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
nebo smažeš, při dalším spuštění se stáhne znovu.

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
node playlist-sync.js -x "Neznámá Kapela - Písnička.mp3"
node playlist-sync.js -x "Alex G - *"
```

Seznam je v `preskocit.txt` vedle skriptu, jeden název souboru nebo vzor na
řádek. Řádky začínající `#` se ignorují.

## Soubory

| Soubor | Co v něm je |
|---|---|
| `playlist-sync.js` | celý program |
| `config.json` | nastavení (není v gitu), vzor v `config-example.json` |
| `playlist.json` | poslední načtený obsah playlistů (není v gitu) |
| `preskocit.txt` | seznam přeskakovaných souborů |

## Časté problémy

- **`spawn yt-dlp ENOENT`**: yt-dlp není nainstalovaný nebo není v PATH, viz Instalace.
- **`… is not valid JSON`** při startu: chyba v `config.json`, nejčastěji čárka
  za posledním prvkem pole.
- **Stáhla se špatná verze z YouTube**: smaž soubor a stáhni ho ručně. Pak ho
  otaguj přes `-s` a název souboru nech `Interpret - Název.mp3`, aby ho
  program příště nestahoval znovu.
- **Našlo to špatné album**: zkus vyšší `--min-score`, nebo písničku přidej do
  seznamu přeskakovaných a otaguj ji ručně.
