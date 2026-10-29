import { readFile, writeFile, appendFile, mkdir, access, readdir, stat } from "node:fs/promises";
import { execFile } from "node:child_process";
import { promisify, parseArgs } from "node:util";
import { homedir } from "node:os";
import path from "node:path";
import spotifyUrlInfo from "spotify-url-info";
import NodeID3 from "node-id3";

const spust = promisify(execFile);
const { getTracks } = spotifyUrlInfo(fetch);

const SLOZKA_SCRIPTU = import.meta.dirname;
const PLAYLIST_JSON = path.join(SLOZKA_SCRIPTU, "playlist.json");
const CONFIG_JSON = path.join(SLOZKA_SCRIPTU, "config.json");
const PRESKOCIT_TXT = path.join(SLOZKA_SCRIPTU, "preskocit.txt");

// prepinace se kontroluji jeste pred nactenim configu, aby --help fungoval vzdy
const NAPOVEDA = `Použití: node playlist-sync.js [přepínače]

Stáhne nové písničky ze Spotify playlistů jako mp3 (přes yt-dlp z YouTube)
a otaguje je: název, interpret, album, rok, žánr, číslo stopy/disku a cover
alba. Metadata dohledává na Deezeru, záložně na iTunes.

Přepínače:
  -h, --help               vypíše tuto nápovědu a skončí
  --otagovat               po stažení projde všechna mp3 ve slozka_mp3
                           (i podsložky) a otaguje ty, kterým chybí album
                           nebo cover, třeba ručně stažené písničky
  -d, --dir SLOŽKA         nic nestahuj, jen otaguj mp3 v téhle složce
                           (i podsložky); lze zadat víckrát
  -s, --pisnicka NÁZEV     nic nestahuj, jen otaguj tuhle písničku: cesta
                           k souboru nebo část názvu (diakritika a velikost
                           písmen nevadí). Vždy znovu, seznam přeskakovaných
                           neplatí. Lze zadat víckrát. Hledá v -d, jinak
                           ve slozka_mp3.
  -x, --preskocit SOUBOR…  přidej soubory nebo vzory (*, ?) do seznamu
                           přeskakovaných a skonči
  -n, --dry-run            jen vypiš, co by se stalo, nic nestahuj
                           ani nezapisuj
  -f, --force              přetaguj i soubory, které už mají album i cover
  --save-cover             ulož cover i jako "Soubor.jpg" vedle mp3
  --min-score ČÍSLO        minimální shoda pro přijetí výsledku hledání
                           (0-1, výchozí 0.75)

Bez -d a -s:
  - načte playlisty a uloží jejich obsah do playlist.json
  - projde složku slozka_mp3 a stáhne písničky, jejichž soubor
    "Interpret - Název.mp3" tam ještě není (složku tak můžeš sdílet
    mezi víc zařízeními, každé dotáhne jen to, co chybí)
  - každou nově staženou písničku hned otaguje

Tagování:
  Interpret a název se berou ze Spotify, když je písnička v playlistu (u -d
  a -s z posledního playlist.json), jinak z názvu souboru
  "Interpret1, Interpret2 - Název.mp3". Délku skladby zjistí ffprobe.
  Soubory, které už mají album i cover, se přeskočí (pokud není --force),
  stejně jako soubory ze seznamu přeskakovaných. Nenalezené písničky
  dostanou aspoň interpreta a název a vypíšou se na konci.

Jak se hledá správná skladba:
  Hledá se na Deezeru, když nic dost dobrého nenajde (nebo najde jen
  kompilaci typu "Indie Hits"), zkusí ještě iTunes. Každý výsledek dostane
  skóre: 60 % podobnost názvu + 40 % podobnost interpreta, +0.05 když sedí
  délka (±3 s), -0.1 když se liší o víc než 20 s, -0.12 za kompilaci a
  -0.15 když nesedí remix/live/acoustic/... Výsledek pod --min-score se
  nepoužije. Skóre je vidět ve výpisu, např. [deezer, 0.95].

Co se zapíše do mp3 (ID3v2.3):
  název, interpret, interpret alba, album, rok, žánr, číslo stopy/počet
  stop, číslo disku a cover 1000x1000. Po otagování v Clementine:
  Nástroje → Prohledat celou sbírku znovu.

Příklady:
  node playlist-sync.js                   stáhni nové písničky a otaguj je
  node playlist-sync.js --otagovat        navíc otaguj i ručně přidané
  node playlist-sync.js -n                jen ukaž, co by se stáhlo
  node playlist-sync.js -d ~/Hudba        otaguj jinou složku, nic nestahuj
  node playlist-sync.js -s faded          přetaguj jednu písničku
  node playlist-sync.js -x "Alex G - *"   tyhle soubory příště přeskoč

Konfigurace (${CONFIG_JSON}):
  playlist_url   URL playlistu/alba na Spotify, nebo pole URL. Spotify vrací
                 nejvýš 100 skladeb na playlist, větší je potřeba rozdělit.
  slozka_mp3     kam ukládat mp3, ~ = domovská složka

Soubory vedle skriptu:
  playlist.json  poslední načtený obsah playlistů
  preskocit.txt  seznam přeskakovaných souborů, jeden název nebo vzor
                 na řádek (řádky začínající # se ignorují)

Písnička se pozná podle názvu souboru. Když soubor přejmenuješ nebo smažeš,
stáhne se znovu.

Vyžaduje: Node.js 20.11+, yt-dlp a ffmpeg (s ffprobe) v PATH, npm install.
Podrobný popis: README.md vedle skriptu.`;

let volby, zbytek;
try {
    ({ values: volby, positionals: zbytek } = parseArgs({
        allowPositionals: true,
        options: {
            help: { type: "boolean", short: "h" },
            otagovat: { type: "boolean" },
            dir: { type: "string", short: "d", multiple: true },
            pisnicka: { type: "string", short: "s", multiple: true },
            preskocit: { type: "string", short: "x", multiple: true },
            "dry-run": { type: "boolean", short: "n" },
            force: { type: "boolean", short: "f" },
            "save-cover": { type: "boolean" },
            "min-score": { type: "string" },
        },
    }));
} catch (chyba) {
    console.error(`Špatné přepínače: ${chyba.message}\nNápověda: node playlist-sync.js --help`);
    process.exit(1);
}

if (volby.help) {
    console.log(NAPOVEDA);
    process.exit(0);
}

const MIN_SHODA = volby["min-score"] === undefined ? 0.75 : Number(volby["min-score"]);
if (!(MIN_SHODA >= 0 && MIN_SHODA <= 1)) {
    console.error(`--min-score musí být číslo od 0 do 1, ne "${volby["min-score"]}"`);
    process.exit(1);
}

const zkouska = Boolean(volby["dry-run"]);

// -x a b c: prvni polozka je hodnota prepinace, dalsi jsou volne argumenty
if (volby.preskocit) {
    await pridejPreskakovane([...volby.preskocit, ...zbytek]);
    process.exit(0);
}

// volne argumenty bez prepinace se berou jako dalsi slozky (jako v opravit-tagy.py)
const slozkyKTagovani = [...(volby.dir ?? []), ...zbytek];
const jenTagovat = slozkyKTagovani.length > 0 || Boolean(volby.pisnicka);

// config je potreba na stahovani, nebo kdyz se taguje bez -d (vychozi slozka)
let config = null;
let SLOZKA_MP3 = null;
if (!jenTagovat || slozkyKTagovani.length === 0) {
    let configText;
    try {
        configText = await readFile(CONFIG_JSON, "utf8");
    } catch {
        console.error(`Chybí ${CONFIG_JSON}. Popis konfigurace: node playlist-sync.js --help`);
        process.exit(1);
    }
    config = JSON.parse(configText);
    SLOZKA_MP3 = rozbalCestu(config.slozka_mp3);
}

// pomocne funkce
function rozbalCestu(cesta) {
    return path.resolve(cesta.replace(/^~(?=$|\/)/, homedir()));
}

async function nactiJson(soubor, vychozi) {
    try {
        return JSON.parse(await readFile(soubor, "utf8"));
    } catch {
        return vychozi;
    }
}

async function ulozJson(soubor, data) {
    const text = JSON.stringify(data, null, 2);
    await writeFile(soubor, text, "utf8");
}

async function existuje(soubor) {
    try {
        await access(soubor);
        return true;
    } catch {
        return false;
    }
}

async function jeSoubor(cesta) {
    try {
        return (await stat(cesta)).isFile();
    } catch {
        return false;
    }
}

async function jeSlozka(cesta) {
    try {
        return (await stat(cesta)).isDirectory();
    } catch {
        return false;
    }
}

// vsechna mp3 ve slozce vcetne podslozek, jako plne cesty
async function mp3VeSlozce(slozka) {
    const vse = await readdir(slozka, { recursive: true });
    return vse
        .filter((x) => x.toLowerCase().endsWith(".mp3"))
        .map((x) => path.join(slozka, x));
}

// pro hledani v -s: bez diakritiky a velikosti pismen
function bezDiakritiky(text) {
    return text.normalize("NFKD").replace(/[̀-ͯ]/g, "").toLowerCase();
}

// delka mp3 v sekundach pres ffprobe (kvuli porovnani s nalezenou skladbou)
async function delkaMp3(soubor) {
    try {
        const { stdout } = await spust("ffprobe", [
            "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", soubor,
        ]);
        const delka = parseFloat(stdout);
        return Number.isFinite(delka) ? delka : null;
    } catch {
        return null;
    }
}

function jmenoSouboru(p) {
    const jmeno = `${p.interpret} - ${p.nazev}`;
    return jmeno.replace(/[\/\\:*?"<>|]/g, "_").trim();
}

// spotify vraci max 100 skladeb na playlist, proto jde zadat vic playlistu
// v config.playlist_url (jedna URL nebo pole URL) a skladby se spoji
async function nactiPlaylist() {
    const adresy = [config.playlist_url].flat();
    const vysledek = new Map();

    for (const adresa of adresy) {
        const skladby = await getTracks(adresa);
        if (skladby.length >= 100) {
            console.log(`Pozor: playlist ${adresa} má aspoň 100 skladeb, zbytek se nenačte. Rozděl ho.`);
        }

        const jenPisnicky = skladby.filter((s) => s.uri.startsWith("spotify:track"));

        for (const s of jenPisnicky) {
            const id = s.uri.split(":")[2];
            vysledek.set(id, {
                id,
                nazev: s.name,
                interpret: s.artist,
                delka: s.duration ? Math.round(s.duration / 1000) : null,
            });
        }
    }

    return [...vysledek.values()];
}

async function stahniMp3(p) {
    const jmeno = jmenoSouboru(p);
    const cilovySoubor = path.join(SLOZKA_MP3, `${jmeno}.mp3`);

    const argumenty = [
        `ytsearch1:${p.interpret} - ${p.nazev} audio`,
        "--extract-audio",
        "--audio-format", "mp3",
        "--audio-quality", "192K",
        "--no-playlist",
        "--quiet",
        "--no-warnings",
        "-o", path.join(SLOZKA_MP3, `${jmeno}.%(ext)s`),
    ];

    try {
        await spust("yt-dlp", argumenty);
    } catch (chyba) {
        console.log(`nepodarilo se stáhnout: ${chyba.message}`);
        return null;
    }

    if (await existuje(cilovySoubor)) {
        return `${jmeno}.mp3`
    }

    return null;
}

// ---------- tagy (prevzato z opravit-tagy.py) ----------
// dohleda skladbu na Deezeru (zaloha iTunes) a zapise nazev, interpreta, album,
// rok, zanr, cislo stopy/disku a cover alba

const UA = "playlist-sync/1.0";
const KOMPILACE = /\b(hits|now that|compilation|best of|greatest|top \d+|party|summer \d{4}|workout|playlist|various|mix \d{4}|karaoke|tribute)\b/i;

function pockej(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

async function stahniJson(adresa, parametry) {
    if (parametry) {
        adresa += "?" + new URLSearchParams(parametry);
    }
    for (let pokus = 0; pokus < 3; pokus++) {
        try {
            const odpoved = await fetch(adresa, {
                headers: { "User-Agent": UA },
                signal: AbortSignal.timeout(15000),
            });
            const data = await odpoved.json();
            // Deezer vraci prekroceni kvoty jako error s kodem 4
            if (data?.error?.code === 4) {
                await pockej(5000);
                continue;
            }
            return data;
        } catch {
            await pockej(2000 * (pokus + 1));
        }
    }
    return null;
}

async function stahniObrazek(adresa) {
    try {
        const odpoved = await fetch(adresa, {
            headers: { "User-Agent": UA },
            signal: AbortSignal.timeout(20000),
        });
        if (!odpoved.ok) return null;
        return {
            data: Buffer.from(await odpoved.arrayBuffer()),
            mime: odpoved.headers.get("content-type")?.split(";")[0] || "image/jpeg",
        };
    } catch {
        return null;
    }
}

// porovnavani nazvu

function normalizuj(s) {
    s = s.normalize("NFKD").replace(/[^\x00-\x7f]/g, "").toLowerCase();
    s = s.replaceAll("_", " ").replaceAll("&", " and ");
    s = s.replace(/[\(\[].*?(feat|ft\.|with|prod).*?[\)\]]/g, " ");
    s = s.replace(/\b(feat|ft)\.? .*/, " ");
    s = s.replace(/[^a-z0-9 ]+/g, " ");
    return s.replace(/\s+/g, " ").trim();
}

// pocet shodnych znaku jako v pythonim difflib.SequenceMatcher:
// najdi nejdelsi spolecny usek a totez opakuj vlevo a vpravo od nej
function pocetShod(a, b) {
    let delka = 0, ia = 0, ib = 0;
    for (let i = 0; i < a.length; i++) {
        for (let j = 0; j < b.length; j++) {
            let k = 0;
            while (i + k < a.length && j + k < b.length && a[i + k] === b[j + k]) k++;
            if (k > delka) {
                delka = k; ia = i; ib = j;
            }
        }
    }
    if (delka === 0) return 0;
    return delka
        + pocetShod(a.slice(0, ia), b.slice(0, ib))
        + pocetShod(a.slice(ia + delka), b.slice(ib + delka));
}

function podobnost(a, b) {
    a = normalizuj(a);
    b = normalizuj(b);
    if (!a || !b) return 0;
    if (a === b) return 1;
    let pomer = 2 * pocetShod(a, b) / (a.length + b.length);
    // jeden obsahuje druhy ("Dancin" vs "Dancin - Krono Remix")
    if (a.includes(b) || b.includes(a)) {
        const kratsi = Math.min(a.length, b.length);
        const delsi = Math.max(a.length, b.length);
        pomer = Math.max(pomer, 0.85 * kratsi / delsi + 0.15);
    }
    return pomer;
}

function rozdelInterprety(text) {
    return text.split(/\s*(?:,|&|\/|\bx\b|\bfeat\.?|\bft\.?|\band\b)\s*/i).filter(Boolean);
}

function slova(text) {
    return normalizuj(text).split(" ");
}

function shoda(kandidat, interpreti, nazev, delka) {
    const t = podobnost(nazev, kandidat.nazev);
    const jehoInterpreti = [kandidat.interpret, ...rozdelInterprety(kandidat.interpret)];
    let a = 0.5;
    if (interpreti.length > 0) {
        a = Math.max(...interpreti.flatMap((x) => jehoInterpreti.map((y) => podobnost(x, y))));
    }
    let s = 0.6 * t + 0.4 * a;
    if (delka && kandidat.delka) {
        const rozdil = Math.abs(delka - kandidat.delka);
        if (rozdil <= 3) s += 0.05;
        else if (rozdil > 20) s -= 0.1;
    }
    if (KOMPILACE.test(kandidat.album || "")) {
        s -= 0.12;
    }
    // remix/live/acoustic musi sedet s nazvem ze Spotify
    for (const slovo of ["remix", "live", "acoustic", "instrumental", "remaster", "version", "mix"]) {
        if (slova(nazev).includes(slovo) !== slova(kandidat.nazev).includes(slovo)) {
            s -= 0.15;
        }
    }
    return s;
}

// zdroje

function dotaz(interpreti, nazev) {
    return `${interpreti[0] ?? ""} ${nazev.replace(/[\(\[].*?[\)\]]/g, "")}`.trim();
}

async function hledejDeezer(interpreti, nazev) {
    const data = await stahniJson("https://api.deezer.com/search", { q: dotaz(interpreti, nazev), limit: 15 });
    return (data?.data ?? []).map((t) => ({
        zdroj: "deezer",
        id: t.id,
        nazev: t.title,
        interpret: t.artist.name,
        album: t.album.title,
        albumId: t.album.id,
        delka: t.duration,
        cover: t.album.cover_xl,
    }));
}

const albaDeezer = new Map();

async function detailDeezer(k) {
    const skladba = (await stahniJson(`https://api.deezer.com/track/${k.id}`)) ?? {};
    if (!albaDeezer.has(k.albumId)) {
        albaDeezer.set(k.albumId, (await stahniJson(`https://api.deezer.com/album/${k.albumId}`)) ?? {});
    }
    const album = albaDeezer.get(k.albumId);
    const interpreti = [...new Set((skladba.contributors ?? []).map((x) => x.name))];
    const zanry = (album.genres?.data ?? []).map((g) => g.name);
    return {
        nazev: skladba.title ?? k.nazev,
        interpret: interpreti.join(", ") || k.interpret,
        album: album.title ?? k.album,
        interpretAlba: album.artist?.name ?? k.interpret,
        datum: (skladba.release_date || album.release_date || "").slice(0, 10),
        zanr: zanry[0],
        stopa: skladba.track_position,
        stop: album.nb_tracks,
        disk: skladba.disk_number,
        cover: album.cover_xl || k.cover,
    };
}

let posledniITunes = 0;

async function hledejITunes(interpreti, nazev) {
    // iTunes povoluje cca 20 dotazu za minutu
    const cekej = 3200 - (Date.now() - posledniITunes);
    if (cekej > 0) await pockej(cekej);
    posledniITunes = Date.now();
    const data = await stahniJson("https://itunes.apple.com/search", {
        term: dotaz(interpreti, nazev), entity: "song", limit: 15,
    });
    return (data?.results ?? [])
        .filter((t) => t.kind === "song")
        .map((t) => ({
            zdroj: "itunes",
            nazev: t.trackName,
            interpret: t.artistName,
            album: t.collectionName ?? "",
            delka: t.trackTimeMillis ? t.trackTimeMillis / 1000 : null,
            surova: t,
        }));
}

function detailITunes(k) {
    const t = k.surova;
    const obrazek = t.artworkUrl100 ?? "";
    return {
        nazev: t.trackName,
        interpret: t.artistName,
        album: (t.collectionName ?? "").replace(/ - (EP|Single)$/, ""),
        interpretAlba: t.collectionArtistName || t.artistName,
        datum: (t.releaseDate ?? "").slice(0, 10),
        zanr: t.primaryGenreName,
        stopa: t.trackNumber,
        stop: t.trackCount,
        disk: t.discNumber,
        cover: obrazek ? obrazek.replace("100x100bb", "1000x1000bb") : null,
    };
}

async function dohledej(interpreti, nazev, delka) {
    let nejlepsi = null, nejlepsiShoda = 0;
    for (const hledej of [hledejDeezer, hledejITunes]) {
        const kandidati = await hledej(interpreti, nazev);
        for (const [i, k] of kandidati.entries()) {
            const s = shoda(k, interpreti, nazev, delka) - 0.005 * i;
            if (s > nejlepsiShoda) {
                nejlepsi = k;
                nejlepsiShoda = s;
            }
        }
        // kompilace ("Indie Hits" apod.) zkus jeste vylepsit v dalsim zdroji
        if (nejlepsiShoda >= MIN_SHODA && !KOMPILACE.test(nejlepsi.album)) break;
    }
    if (!nejlepsi || nejlepsiShoda < MIN_SHODA) {
        return { info: null, shoda: nejlepsiShoda, kandidat: nejlepsi };
    }
    const info = nejlepsi.zdroj === "deezer" ? await detailDeezer(nejlepsi) : detailITunes(nejlepsi);
    return { info, shoda: nejlepsiShoda, kandidat: nejlepsi };
}

// zapis

async function maDobreTagy(soubor) {
    try {
        const tagy = await NodeID3.Promise.read(soubor);
        return Boolean(tagy.album && tagy.image);
    } catch {
        return false;
    }
}

async function zapisTagy(soubor, info, cover) {
    const tagy = {
        title: info.nazev,
        artist: info.interpret,
        performerInfo: info.interpretAlba,
        album: info.album,
        year: info.datum ? info.datum.slice(0, 4) : undefined,
        genre: info.zanr,
        trackNumber: info.stopa ? String(info.stopa) + (info.stop ? `/${info.stop}` : "") : undefined,
        partOfSet: info.disk ? String(info.disk) : undefined,
    };
    if (cover) {
        tagy.image = {
            mime: cover.mime,
            type: { id: 3, name: "front cover" },
            description: "Cover",
            imageBuffer: cover.data,
        };
    }
    await NodeID3.Promise.update(tagy, soubor);
}

const statistika = { stazeno: 0, nalezeno: 0, nenalezeno: [], uzOtagovanych: 0, preskoceno: 0 };

async function otaguj(soubor, p) {
    const interpreti = p.interpret.split(/, | & /).map((x) => x.trim()).filter(Boolean);
    const delka = p.delka ?? (await delkaMp3(soubor));
    let { info, shoda: s, kandidat } = await dohledej(interpreti, p.nazev, delka);

    if (!info) {
        const tip = kandidat ? ` (nejbližší: ${kandidat.interpret} - ${kandidat.nazev}, ${s.toFixed(2)})` : "";
        console.log(`    ✗ tagy nenalezeny${tip}`);
        statistika.nenalezeno.push(path.basename(soubor));
        info = { nazev: p.nazev, interpret: interpreti.join(", ") };
    } else {
        console.log(`    ✓ ${info.interpret} - ${info.nazev} | ${info.album} (${info.datum.slice(0, 4)}) [${kandidat.zdroj}, ${s.toFixed(2)}]`);
        statistika.nalezeno++;
        // u vice interpretu je seznam ze Spotify / nazvu souboru casto uplnejsi
        if (interpreti.length > info.interpret.split(", ").length) {
            info.interpret = interpreti.join(", ");
        }
    }

    if (zkouska) return;

    let cover = null;
    if (info.cover) {
        cover = await stahniObrazek(info.cover);
        if (!cover) console.log("    ! cover se nepodařilo stáhnout");
    }
    try {
        await zapisTagy(soubor, info, cover);
    } catch (chyba) {
        console.log(`    ! zápis tagů selhal: ${chyba.message}`);
        return;
    }
    if (cover && volby["save-cover"]) {
        await writeFile(soubor.replace(/\.mp3$/i, ".jpg"), cover.data);
    }
}

// ---------- seznam preskakovanych ----------

async function nactiPreskakovane() {
    let text;
    try {
        text = await readFile(PRESKOCIT_TXT, "utf8");
    } catch {
        return [];
    }
    return text.split("\n").map((r) => r.trim()).filter((r) => r && !r.startsWith("#"));
}

async function pridejPreskakovane(polozky) {
    const existujici = new Set(await nactiPreskakovane());
    const pridat = [];
    for (const x of polozky) {
        // existujici soubor se zapise jen jmenem, vzor ("Alex G - *") tak jak je
        const polozka = (await jeSoubor(rozbalCestu(x))) ? path.basename(x) : x;
        if (!existujici.has(polozka) && !pridat.includes(polozka)) {
            pridat.push(polozka);
        }
    }
    if (!(await existuje(PRESKOCIT_TXT))) {
        await writeFile(PRESKOCIT_TXT,
            "# Soubory, které playlist-sync.js při tagování přeskočí.\n"
            + "# Jeden název souboru nebo vzor (*, ?) na řádek.\n", "utf8");
    }
    await appendFile(PRESKOCIT_TXT, pridat.map((x) => x + "\n").join(""), "utf8");
    for (const x of pridat) {
        console.log(`+ ${x}`);
    }
    console.log(`Přidáno ${pridat.length} položek do ${PRESKOCIT_TXT}`);
}

// vzor s * a ? -> regularni vyraz
function vzorNaRegex(vzor) {
    const escapovany = vzor.replace(/[.+^${}()|[\]\\]/g, "\\$&");
    return new RegExp(`^${escapovany.replaceAll("*", ".*").replaceAll("?", ".")}$`);
}

function jePreskocen(soubor, vzory) {
    return vzory.some((v) => {
        const re = vzorNaRegex(v);
        return re.test(path.basename(soubor)) || re.test(soubor);
    });
}

// ---------- vyber a tagovani souboru ----------

// -s: cesta k souboru nebo cast nazvu
async function vyberPisnicky(dotazy, vsechny) {
    const vybrane = [];
    for (const dotaz of dotazy) {
        const cesta = rozbalCestu(dotaz);
        if (await jeSoubor(cesta)) {
            vybrane.push(cesta);
            continue;
        }
        const hledany = bezDiakritiky(dotaz);
        const shody = vsechny.filter((s) => bezDiakritiky(path.basename(s).replace(/\.mp3$/i, "")).includes(hledany));
        if (shody.length === 0) {
            console.error(`Žádná písnička neodpovídá: ${dotaz}`);
            process.exit(1);
        }
        vybrane.push(...shody);
    }
    return [...new Set(vybrane)];
}

async function otagujSoubory(soubory, playlist, { vynutit, preskakovat }) {
    const zPlaylistu = new Map(playlist.map((p) => [`${jmenoSouboru(p)}.mp3`, p]));
    const vzory = preskakovat ? await nactiPreskakovane() : [];
    const kOtagovani = [];
    for (const soubor of soubory) {
        if (jePreskocen(soubor, vzory)) {
            statistika.preskoceno++;
        } else if (!vynutit && (await maDobreTagy(soubor))) {
            statistika.uzOtagovanych++;
        } else {
            kOtagovani.push(soubor);
        }
    }
    console.log(`Písniček k otagování: ${kOtagovani.length}`);
    for (const [i, soubor] of kOtagovani.entries()) {
        const jmeno = path.basename(soubor);
        console.log(`[${i + 1}/${kOtagovani.length}] Taguji: ${jmeno}`);
        await otaguj(soubor, zPlaylistu.get(jmeno) ?? pisnickaZNazvu(jmeno));
    }
}

// pisnicka stazena rucne (neni v playlistu): "Interpret1, Interpret2 - Nazev.mp3"
function pisnickaZNazvu(jmeno) {
    const bezPripony = jmeno.replace(/\.mp3$/i, "");
    const oddelovac = bezPripony.indexOf(" - ");
    if (oddelovac === -1) {
        return { nazev: bezPripony.trim(), interpret: "", delka: null };
    }
    return {
        nazev: bezPripony.slice(oddelovac + 3).trim(),
        interpret: bezPripony.slice(0, oddelovac).trim(),
        delka: null,
    };
}

function vypisShrnuti() {
    const casti = [];
    if (!jenTagovat) casti.push(`${statistika.stazeno} staženo`);
    casti.push(
        `${statistika.nalezeno} s nalezenými tagy`,
        `${statistika.nenalezeno.length} nenalezeno`,
        `${statistika.uzOtagovanych} už otagovaných`,
        `${statistika.preskoceno} na seznamu přeskakovaných`,
    );
    const pozn = zkouska ? " (jen zkouška, nic se nezapsalo)" : "";
    console.log(`\nHotovo${pozn}: ${casti.join(", ")}.`);
    if (statistika.nenalezeno.length > 0) {
        console.log("Nenalezené (mají jen interpreta a název):");
        for (const jmeno of statistika.nenalezeno) {
            console.log(`  - ${jmeno}`);
        }
        console.log(`Nechceš je zkoušet znovu? Přidej je: node playlist-sync.js -x "název souboru.mp3"`);
    }
}

// hlavni program
if (jenTagovat) {
    // jen tagovani (-d / -s), nic se nestahuje
    const slozky = slozkyKTagovani.length > 0 ? slozkyKTagovani.map(rozbalCestu) : [SLOZKA_MP3];
    for (const slozka of slozky) {
        if (!(await jeSlozka(slozka))) {
            console.error(`Složka neexistuje: ${slozka}`);
            process.exit(1);
        }
    }
    const vsechny = (await Promise.all(slozky.map(mp3VeSlozce))).flat().sort();
    // udaje ze Spotify z posledniho behu, pokud jsou
    const playlist = await nactiJson(PLAYLIST_JSON, []);

    if (volby.pisnicka) {
        const vybrane = await vyberPisnicky(volby.pisnicka, vsechny);
        await otagujSoubory(vybrane, playlist, { vynutit: true, preskakovat: false });
    } else {
        if (vsechny.length === 0) {
            console.error("Žádné mp3 soubory.");
            process.exit(1);
        }
        await otagujSoubory(vsechny, playlist, { vynutit: volby.force, preskakovat: true });
    }
} else {
    await mkdir(SLOZKA_MP3, { recursive: true });

    const playlist = await nactiPlaylist();
    if (!zkouska) {
        await ulozJson(PLAYLIST_JSON, playlist);
    }

    const soubory = new Set(await readdir(SLOZKA_MP3));
    // pro --otagovat: seznam pred stahovanim, prave stazene se otaguji hned
    const predStazenim = volby.otagovat ? (await mp3VeSlozce(SLOZKA_MP3)).sort() : [];
    console.log(`Playlist má ${playlist.length} písniček, ve složce je ${soubory.size} souborů`);

    const nove = playlist.filter((p) => !soubory.has(`${jmenoSouboru(p)}.mp3`));
    console.log(`Nových písniček: ${nove.length}`);

    for (const [i, p] of nove.entries()) {
        const poradi = `[${i + 1}/${nove.length}]`;
        if (zkouska) {
            console.log(`${poradi} Stáhl bych: ${p.interpret} - ${p.nazev}`);
            continue;
        }
        console.log(`${poradi} Stahuji: ${p.interpret} - ${p.nazev}`);
        const soubor = await stahniMp3(p);
        if (soubor !== null) {
            statistika.stazeno++;
            await otaguj(path.join(SLOZKA_MP3, soubor), p);
        }
    }

    if (volby.otagovat) {
        await otagujSoubory(predStazenim, playlist, { vynutit: volby.force, preskakovat: true });
    }
}

vypisShrnuti();