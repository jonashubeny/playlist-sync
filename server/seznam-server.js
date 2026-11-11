// Malý server, který drží seznam písniček (seznam.json).
// Spouští se na jonas-server:  node seznam-server.js
//
// Nastavení přes proměnné prostředí:
//   PORT         - port, výchozí 3456
//   DATA_SLOZKA   - kam ukládat seznam.json, výchozí složka vedle skriptu
//
// API:
//   GET  /pisnicky          -> celý seznam (pole písniček)
//   POST /pisnicky  [ ... ] -> přidá písničky, které v seznamu ještě nejsou

import { createServer } from "node:http";
import { readFile, writeFile, rename, mkdir } from "node:fs/promises";
import { createHash, randomBytes } from "node:crypto";
import path from "node:path";

const PORT = Number(process.env.PORT ?? 3456);
const DATA_SLOZKA = process.env.DATA_SLOZKA ?? import.meta.dirname;
const SEZNAM_JSON = path.join(DATA_SLOZKA, "seznam.json");
const TOKENY_JSON = path.join(DATA_SLOZKA, "tokeny.json");

await mkdir(DATA_SLOZKA, { recursive: true });

async function nactiSeznam() {
    try {
        return JSON.parse(await readFile(SEZNAM_JSON, "utf8"));
    } catch {
        return [];
    }
}

// zapis pres docasny soubor, aby se pri vypadku proudu nerozbil seznam napul
async function ulozSeznam(seznam) {
    const docasny = SEZNAM_JSON + ".tmp";
    await writeFile(docasny, JSON.stringify(seznam, null, 2), "utf8");
    await rename(docasny, SEZNAM_JSON);
}

function hashTokenu(token) {
    return createHash("sha256").update(token).digest("hex");
}

// cte se pri kazdem pozadavky, aby novy uzivatel nepotreboval restart
async function nactiTokeny() {
    try {
        return JSON.parse(await readFile(TOKENY_JSON, "utf8"));
    } catch {
        return {};
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
            await ulozSeznam(seznam);
            console.log(`Přidáno ${pridane.length}: ${pridane.map((p) => p.soubor).join(", ")}`);
        }
        return odpovez(res, 200, { pridano: pridane.length, celkem: seznam.length });
    }

    odpovez(res, 404, { chyba: "neznámá adresa" });
}

server.listen(PORT, () => {
    console.log(`Seznam písniček běží na portu ${PORT}, data: ${SEZNAM_JSON}`);
});
