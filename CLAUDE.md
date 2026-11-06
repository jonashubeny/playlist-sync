# playlist-sync

Node.js CLI, který stáhne písničky ze Spotify playlistů jako mp3 (přes `yt-dlp`
z YouTube) a otaguje je (Deezer, záložně iTunes). Uživatelská dokumentace je
v `README.md`, tady jsou jen věci pro práci na kódu.

## Struktura

- `playlist-sync.js` je celý klient v jednom souboru (ESM, `"type": "module"`),
  spouští se `node playlist-sync.js [přepínače]`. Přepínače parsuje
  `parseArgs` z `node:util`. Bez přepínačů = stahování z playlistů.
- `server/seznam-server.js` je server se sdíleným seznamem písniček (čistý
  `node:http`, bez Express), `server/docker-compose.yml` ho pouští
  v kontejneru `node:22-alpine` na jonas-serveru. Viz „Aktuální práce“.
- `config.json` (vzor `config-example.json`): `playlist_url` (string nebo pole)
  a `slozka_mp3` (`~` se rozbalí přes `rozbalCestu`).
- `playlist.json`: poslední načtený obsah playlistů (`id`, `nazev`,
  `interpret`, `delka`). `stazeno.json` je pozůstatek, kód ho nepoužívá.
- Závislosti: `spotify-url-info`, `node-id3`. Externí programy: `yt-dlp`,
  `ffmpeg`/`ffprobe`. Node 20.11+ (kvůli `import.meta.dirname`).
- Testy nejsou, ověřuje se ručně, typicky přes `-n` / `--dry-run`.

## Konvence

- Identifikátory, výpisy i komentáře česky. Komentáře bez diakritiky,
  texty pro uživatele s diakritikou.
- 4 mezery, dvojité uvozovky, středníky, `node:` importy, `fs/promises`.
- Async všude, smyčky přes `for...of` + `await` (stahuje se postupně).
- Chyba jedné písničky nesmí shodit celý běh: vypsat a pokračovat.

## Důležité funkce v `playlist-sync.js`

- `jmenoSouboru(p)`: `"Interpret - Název"` bez `.mp3`, zakázané znaky
  `/ \ : * ? " < > |` nahrazuje `_`. **Podle názvu souboru se pozná, jestli
  písnička už je stažená**, takže každá změna tady změní porovnávání
  s existujícími soubory na všech zařízeních.
- `pisnickaZNazvu(jmeno)`: opačný směr, z `"Interpret - Název.mp3"` udělá
  `{ nazev, interpret, delka }`; bez ` - ` dá celé jméno do `nazev`
  a prázdný `interpret`.
- `nactiPlaylist()`, `stahniMp3(p)` (vrací název souboru nebo `null`),
  `otaguj(soubor, p)`, `mp3VeSlozce(slozka)` (rekurzivně, plné cesty).
- Hlavní program je na konci souboru (větev `jenTagovat` vs. stahování).

## Aktuální práce: sdílený seznam písniček na serveru

GitHub issue #1 (`gh issue view 1 --comments`). Platí komentář s plánem,
původní popis issue je zastaralý.

Na jonas-serveru běží jedna oficiální verze seznamu písniček. Každé zařízení
při běžném spuštění nahraje na server všechny svoje lokální písničky, které
tam ještě nejsou. Na požádání (`--server`) stáhne písničky, které jsou na
serveru, ale lokálně chybí. Webové rozhraní není, jen API. Mazání zatím ne.

### Server (`server/seznam-server.js`)

- Zůstává na čistém `node:http`, bez Express a bez závislostí.
- „Databáze“ je `seznam.json` v `DATA_SLOZKA`, zápis přes `.tmp` + `rename`,
  požadavky se zpracovávají ve frontě jeden po druhém.
- `GET /pisnicky` vrátí celý seznam.
- `POST /pisnicky` přijme **pole** písniček, ty už uložené (stejné `id` nebo
  `soubor` bez ohledu na velikost písmen) tiše přeskočí a vrátí
  `{ pridano, celkem }`. Duplicita není chyba.
- Přihlášení: `Authorization: Bearer <token>`, **token pro každého uživatele**.
  `tokeny.json` v `DATA_SLOZKA` mapuje SHA-256 hash tokenu na jméno. Čte se
  při každém požadavku, takže přidání uživatele nevyžaduje restart. Nový
  uživatel: `node seznam-server.js --novy-token JMENO` vypíše token jednou
  a uloží jeho hash. `SEZNAM_TOKEN` odpadá.
- Server sám doplní `pridano` (ISO čas) a `pridal` (jméno podle tokenu),
  hodnotám od klienta v těchto polích nevěří.
- Dostupný jen přes Tailscale: v `docker-compose.yml` publikovat port jen na
  Tailscale IP serveru (`"100.x.y.z:3456:3456"`). HTTPS netřeba, Tailscale
  provoz šifruje.

Záznam v `seznam.json` (české klíče jako v `playlist.json`):

```json
{
    "id": "3eibuD91myd82Uc8jruuzM",
    "nazev": "Stereoid",
    "interpret": "Vypsana Fixa",
    "delka": 220,
    "soubor": "Vypsana Fixa - Stereoid.mp3",
    "odkud": "spotify",
    "pridano": "2026-11-05T10:00:00.000Z",
    "pridal": "jonas"
}
```

`soubor` je klíč pro porovnání (vždy přes `.toLowerCase()`). `id` je Spotify
ID nebo `null`. `odkud` je `"spotify"` nebo `"local"`.

### Klient (`playlist-sync.js`)

Přepínače:

| Přepínač | Co dělá |
|---|---|
| `--server-url URL`, `--token TOKEN` | zapíše `server` / `token` do `config.json` (ostatní klíče zachová) a skončí, stejně jako `-x` |
| *(bez přepínačů)* | stáhne nové ze Spotify a pak nahraje lokální písničky na server, když je server nastavený |
| `--server` | navíc stáhne písničky, které jsou na serveru a lokálně chybí |
| `-n`, `--dry-run` | jen vypíše, co by se nahrálo / stáhlo |

Pořadí běžného běhu:

1. Spotify → stáhnout nové (jako dnes).
2. Je-li v configu `server`: `GET /pisnicky`.
3. S `--server`: záznamy, jejichž `soubor` lokálně chybí, stáhnout
   a otagovat. Postupně (`for...of` + `await`), chyba jedné písničky =
   vypsat a pokračovat.
4. Lokální `.mp3`, které na serveru nejsou, poslat **jedním** `POST`.
   Když název souboru odpovídá písničce z `playlist.json`, poslat ji
   s `id` a `odkud: "spotify"`, jinak `odkud: "local"` a `nazev`/`interpret`
   z `pisnickaZNazvu`.

Pravidla:

- Krok 4 nahrazuje „přidat po každém stažení“: nahrává se na konci běhu podle
  toho, co je ve složce, takže se přenese i to, co se stáhlo právě teď. Když
  běh spadne, další ho doplní. Opakované spuštění nevytvoří duplicity.
- Lokální písničky = jen kořen `slozka_mp3` (`readdir`), ne podsložky, stejně
  jako kontrola při stahování ze Spotify.
- Server nedostupný: jen varování, stahování ze Spotify proběhne normálně.
  `401` → srozumitelná hláška, ať si uživatel nastaví `--token`. `--server`
  bez nastaveného serveru = chyba.
- `-d` a `-s` (jen tagování) se serveru nedotýkají.
- Názvy souborů dál dělá `jmenoSouboru` (zakázané znaky → `_`). Neměnit, jinak
  se rozbije porovnání s už staženými soubory.
- `stahniMp3` musí umět uložit soubor **přesně pod názvem `soubor` ze
  serveru**. Jinak se lokální písnička bez ` - ` v názvu uloží pod jiným
  jménem a bude se stahovat při každém běhu znovu. Hledá se na YouTube
  podle `interpret - nazev`, bez interpreta jen podle `nazev`.
- Token je v `config.json`, takže `config.json` nesmí být v gitu.

### Mimo rozsah

Mazání písniček (lokálně i na serveru, později s potvrzením), sync na flashku,
webové rozhraní.

### Hotovo, když

- `-n` vypíše, co by se nahrálo a stáhlo, a nic nezmění.
- Po běžném běhu je každá lokální písnička na serveru a další běh už nic
  nenahraje.
- Ručně vložený soubor se po běhu objeví v `GET /pisnicky` s `odkud: "local"`
  a správným `pridal`.
- Písnička stažená ze Spotify je po běhu na serveru s `id`.
- Soubor smazaný ze složky se po `--server` znovu stáhne pod stejným názvem
  a další `--server` už nic nestahuje.
- Bez tokenu nebo se špatným tokenem server vrací `401`.
- Dvojí spuštění čehokoli nevytvoří v seznamu duplicity.

## Pozor v repu

- README tvrdí, že `config.json` a `playlist.json` jsou v `.gitignore`, ale
  `.gitignore` neexistuje a `config.json` i `node_modules/` jsou commitnuté.
- `deno.lock` s projektem nesouvisí.
