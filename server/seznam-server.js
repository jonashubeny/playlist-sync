// Malý server, který drží seznam písniček (seznam.json).
// Spouští se na jonas-server:  node seznam-server.js
//
// Nastavení přes proměnné prostředí:
//   PORT          - port, výchozí 3456
//   DATA_SLOZKA   - kam ukládat seznam.json a tokeny.json, výchozí složka vedle skriptu
//
// Uživatelé: tokeny.json v DATA_SLOZKA (hash tokenu -> jméno),
//   nový uživatel:  node seznam-server.js --novy-token JMENO
//
// API:
//   GET  /pisnicky          -> celý seznam (pole písniček)
//   POST /pisnicky  [ ... ] -> přidá písničky, které v seznamu ještě nejsou

import { createServer } from "node:http";
import { readFile, writeFile, rename, mkdir } from "node:fs/promises";
import { createHash, randomBytes } from "node:crypto";
import { parseArgs } from "node:util";
import path from "node:path";

const PORT = Number(process.env.PORT ?? 3456);
const DATA_SLOZKA = process.env.DATA_SLOZKA ?? import.meta.dirname;
const SEZNAM_JSON = path.join(DATA_SLOZKA, "seznam.json");
const TOKENY_JSON = path.join(DATA_SLOZKA, "tokeny.json");

await mkdir(DATA_SLOZKA, { recursive: true });

async function nactiSeznam() {
    try {
        return JSON.parse(await readFile(SEZNAM_JSON, "utf8"));
    } catch (chyba) {
        if (chyba.code === "ENOENT") return [];
        throw chyba;
    }
}

// zapis pres docasny soubor, aby se pri vypadku proudu nerozbil soubor napul
async function ulozJson(soubor, data) {
    const docasny = soubor + ".tmp";
    await writeFile(docasny, JSON.stringify(data, null, 2), "utf8");
    await rename(docasny, soubor);
}

function hashTokenu(token) {
    return createHash("sha256").update(token).digest("hex");
}

// cte se pri kazdem pozadavku, aby novy uzivatel nepotreboval restart
async function nactiTokeny() {
    try {
        return JSON.parse(await readFile(TOKENY_JSON, "utf8"));
    } catch (chyba) {
        if (chyba.code === "ENOENT") return {};
        throw chyba;
    }
}

// pisnicka je v seznamu, kdyz sedi Spotify ID nebo nazev souboru
function klicSouboru(soubor) {
    return soubor.toLowerCase();
}

function pridej(seznam, nove) {
    const ids = new Set(seznam.filter((p) => p.id).map((p) => p.id));
    const soubory = new Set(seznam.map((p) => klicSouboru(p.soubor)));
    const pridane = [];

    for (const p of nove) {
        // bez nazvu souboru nevime, jak ji ulozit -> preskocit
        if (typeof p?.soubor !== "string" || !p.soubor.endsWith(".mp3")) continue;
        if ((p.id && ids.has(p.id)) || soubory.has(klicSouboru(p.soubor))) continue;

        const pisnicka = {
            id: p.id ?? null,
            nazev: p.nazev ?? null,
            interpret: p.interpret ?? null,
            delka: p.delka ?? null,
            soubor: p.soubor,
            odkud: p.odkud ?? null,
        };
        seznam.push(pisnicka);
        pridane.push(pisnicka);
        if (pisnicka.id) ids.add(pisnicka.id);
        soubory.add(klicSouboru(pisnicka.soubor));
    }
    return pridane;
}

function odpovez(res, kod, data) {
    res.writeHead(kod, { "Content-Type": "application/json; charset=utf-8" });
    res.end(JSON.stringify(data));
}

async function prectiTelo(req) {
    let text = "";
    for await (const kus of req) {
        text += kus;
        if (text.length > 5_000_000) throw new Error("moc velký požadavek");
    }
    return JSON.parse(text);
}

// node seznam-server.js --novy-token JMENO: vytvori token, vypise ho a skonci,
// server se v tom pripade nespousti
let prepinace;
try {
    ({ values: prepinace } = parseArgs({
        options: { "novy-token": { type: "string" } },
    }));
} catch (chyba) {
    console.error(chyba.message);
    console.error("Použití: node seznam-server.js [--novy-token JMENO]");
    process.exit(1);
}

if (prepinace["novy-token"] !== undefined) {
    const jmeno = prepinace["novy-token"].trim();
    if (!jmeno) {
        console.error("Zadej jméno: node seznam-server.js --novy-token JMENO");
        process.exit(1);
    }
    const token = randomBytes(32).toString("base64url");
    const tokeny = await nactiTokeny();
    tokeny[hashTokenu(token)] = jmeno;
    await ulozJson(TOKENY_JSON, tokeny);
    console.log(`Token pro uživatele ${jmeno} (víckrát se nezobrazí, ulož si ho):`);
    console.log(token);
    process.exit(0);
}

// pozadavky zpracovavame jeden po druhem, aby se dva soucasne POSTy neprepsaly
let fronta = Promise.resolve();

const server = createServer((req, res) => {
    fronta = fronta.then(() => zpracuj(req, res)).catch((chyba) => {
        console.error(chyba);
        odpovez(res, 500, { chyba: "interní chyba serveru" });
    });
});

async function zpracuj(req, res) {
    const { pathname } = new URL(req.url, "http://localhost");

    if (pathname === "/pisnicky" && req.method === "GET") {
        return odpovez(res, 200, await nactiSeznam());
    }

    if (pathname === "/pisnicky" && req.method === "POST") {
        let nove;
        try {
            nove = await prectiTelo(req);
        } catch {
            return odpovez(res, 400, { chyba: "tělo musí být JSON pole písniček" });
        }
        if (!Array.isArray(nove)) {
            return odpovez(res, 400, { chyba: "tělo musí být JSON pole písniček" });
        }
        const seznam = await nactiSeznam();
        const pridane = pridej(seznam, nove);
        if (pridane.length > 0) {
            await ulozJson(SEZNAM_JSON, seznam);
            console.log(`Přidáno ${pridane.length}: ${pridane.map((p) => p.soubor).join(", ")}`);
        }
        return odpovez(res, 200, { pridano: pridane.length, celkem: seznam.length });
    }

    odpovez(res, 404, { chyba: "neznámá adresa" });
}

server.listen(PORT, () => {
    console.log(`Seznam písniček běží na portu ${PORT}, data: ${SEZNAM_JSON}`);
});
