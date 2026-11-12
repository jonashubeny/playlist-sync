// playlist-sync: stahne pisnicky ze Spotify playlistu jako mp3 (pres yt-dlp
// z YouTube) a otaguje je (Deezer, zalozne iTunes).
// Prepis puvodniho playlist-sync.js, chovani i vypisy jsou stejne.
//
// Sestaveni:  cmake -B build && cmake --build build
// Binarka ./playlist-sync vznikne vedle tohoto souboru a config.json,
// playlist.json a preskocit.txt hleda vedle sebe.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <utf8proc.h>

#include <attachedpictureframe.h>
#include <id3v2tag.h>
#include <mpegfile.h>
#include <textidentificationframe.h>

extern char** environ;

namespace fs = std::filesystem;
// ordered_json drzi poradi klicu, aby playlist.json vypadal jako z JSON.stringify
using json = nlohmann::ordered_json;

const std::string PROGRAM = "./playlist-sync";

// ---------- cesty ----------

// slozka, ve ktere lezi binarka (config.json a spol. jsou vedle ni)
fs::path slozkaProgramu() {
    std::error_code chyba;
    fs::path exe = fs::read_symlink("/proc/self/exe", chyba);
    if (chyba) return fs::current_path();
    return exe.parent_path();
}

const fs::path SLOZKA_PROGRAMU = slozkaProgramu();
const fs::path PLAYLIST_JSON = SLOZKA_PROGRAMU / "playlist.json";
const fs::path CONFIG_JSON = SLOZKA_PROGRAMU / "config.json";
const fs::path PRESKOCIT_TXT = SLOZKA_PROGRAMU / "preskocit.txt";

std::string napoveda() {
    return R"(Použití: ./playlist-sync [přepínače]

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
  ./playlist-sync                   stáhni nové písničky a otaguj je
  ./playlist-sync --otagovat        navíc otaguj i ručně přidané
  ./playlist-sync -n                jen ukaž, co by se stáhlo
  ./playlist-sync -d ~/Hudba        otaguj jinou složku, nic nestahuj
  ./playlist-sync -s faded          přetaguj jednu písničku
  ./playlist-sync -x "Alex G - *"   tyhle soubory příště přeskoč

Konfigurace ()" + CONFIG_JSON.string() + R"():
  playlist_url   URL playlistu/alba na Spotify, nebo pole URL. Spotify vrací
                 nejvýš 100 skladeb na playlist, větší je potřeba rozdělit.
  slozka_mp3     kam ukládat mp3, ~ = domovská složka

Soubory vedle programu:
  playlist.json  poslední načtený obsah playlistů
  preskocit.txt  seznam přeskakovaných souborů, jeden název nebo vzor
                 na řádek (řádky začínající # se ignorují)

Písnička se pozná podle názvu souboru. Když soubor přejmenuješ nebo smažeš,
stáhne se znovu.

Vyžaduje: yt-dlp a ffmpeg (s ffprobe) v PATH.
Sestavení a podrobný popis: README.md vedle programu.)";
}

// ---------- globalni nastaveni ----------

double MIN_SHODA = 0.75;
bool zkouska = false;
bool ulozitCover = false;
bool jenTagovat = false;
fs::path SLOZKA_MP3;
json config;

// ---------- vypisy ----------

template <class... A>
void vypis(std::format_string<A...> f, A&&... a) {
    std::cout << std::format(f, std::forward<A>(a)...) << '\n';
}

template <class... A>
void vypisChybu(std::format_string<A...> f, A&&... a) {
    std::cerr << std::format(f, std::forward<A>(a)...) << '\n';
}

// ---------- prace s textem ----------

bool zacinaNa(std::string_view s, std::string_view zacatek) {
    return s.substr(0, zacatek.size()) == zacatek;
}

bool konciNa(std::string_view s, std::string_view konec) {
    return s.size() >= konec.size() && s.substr(s.size() - konec.size()) == konec;
}

std::string maleAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// jako /\.mp3$/i
bool konciNaMp3(const std::string& s) {
    return konciNa(maleAscii(s), ".mp3");
}

std::string bezMp3(const std::string& s) {
    return konciNaMp3(s) ? s.substr(0, s.size() - 4) : s;
}

std::string nahradVse(std::string s, std::string_view co, std::string_view cim) {
    for (size_t i = s.find(co); i != std::string::npos; i = s.find(co, i + cim.size())) {
        s.replace(i, co.size(), cim);
    }
    return s;
}

// jako JS text.split(oddelovac): vzdy vrati aspon jednu polozku
std::vector<std::string> rozdel(const std::string& s, const std::string& oddelovac) {
    std::vector<std::string> casti;
    size_t zacatek = 0;
    for (size_t i = s.find(oddelovac); i != std::string::npos; i = s.find(oddelovac, zacatek)) {
        casti.push_back(s.substr(zacatek, i - zacatek));
        zacatek = i + oddelovac.size();
    }
    casti.push_back(s.substr(zacatek));
    return casti;
}

// jako JS text.split(regex); hleda se v kdeHledat, ktery musi mit stejne
// pozice znaku jako text (treba text prevedeny na mala pismena)
std::vector<std::string> rozdelRegexem(const std::string& text, const std::regex& re,
                                       const std::string& kdeHledat) {
    std::vector<std::string> casti;
    size_t zacatek = 0;
    for (auto it = std::sregex_iterator(kdeHledat.begin(), kdeHledat.end(), re);
         it != std::sregex_iterator(); ++it) {
        size_t pozice = static_cast<size_t>(it->position(0));
        casti.push_back(text.substr(zacatek, pozice - zacatek));
        zacatek = pozice + static_cast<size_t>(it->length(0));
    }
    casti.push_back(text.substr(zacatek));
    return casti;
}

std::string spoj(const std::vector<std::string>& casti, std::string_view oddelovac) {
    std::string vysledek;
    for (size_t i = 0; i < casti.size(); i++) {
        if (i > 0) vysledek += oddelovac;
        vysledek += casti[i];
    }
    return vysledek;
}

bool obsahuje(const std::vector<std::string>& seznam, const std::string& hodnota) {
    return std::find(seznam.begin(), seznam.end(), hodnota) != seznam.end();
}

// ---------- Unicode (utf8proc) ----------

// jeden znak (Unicode code point) a kde v textu lezi; neplatny bajt ma kod -1
struct Znak {
    int32_t kod;
    size_t od;
    size_t delka;
};

std::vector<Znak> naZnaky(const std::string& s) {
    std::vector<Znak> znaky;
    size_t i = 0;
    while (i < s.size()) {
        utf8proc_int32_t kod;
        auto n = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(s.data() + i),
                                  static_cast<utf8proc_ssize_t>(s.size() - i), &kod);
        if (n <= 0) {
            znaky.push_back({-1, i, 1});
            i++;
        } else {
            znaky.push_back({kod, i, static_cast<size_t>(n)});
            i += static_cast<size_t>(n);
        }
    }
    return znaky;
}

void pridejZnak(std::string& s, int32_t kod) {
    utf8proc_uint8_t buf[4];
    auto n = utf8proc_encode_char(kod, buf);
    s.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
}

// jako JS text.normalize("NFKD")
std::string nfkd(const std::string& s) {
    utf8proc_uint8_t* vystup = utf8proc_NFKD(reinterpret_cast<const utf8proc_uint8_t*>(s.c_str()));
    if (!vystup) return s;  // neplatne UTF-8, nechat jak je
    std::string vysledek(reinterpret_cast<char*>(vystup));
    std::free(vystup);
    return vysledek;
}

// znaky, ktere odstrani JS trim()
bool jeMezera(int32_t k) {
    return (k >= 0x09 && k <= 0x0d) || k == 0x20 || k == 0xa0 || k == 0x1680
        || (k >= 0x2000 && k <= 0x200a) || k == 0x2028 || k == 0x2029 || k == 0x202f
        || k == 0x205f || k == 0x3000 || k == 0xfeff;
}

// jako JS trim(), vcetne Unicode mezer (kvuli jmenoSouboru, ktere se nesmi zmenit)
std::string orizni(const std::string& s) {
    auto znaky = naZnaky(s);
    size_t a = 0, b = znaky.size();
    while (a < b && jeMezera(znaky[a].kod)) a++;
    while (b > a && jeMezera(znaky[b - 1].kod)) b--;
    if (a == b) return "";
    return s.substr(znaky[a].od, znaky[b - 1].od + znaky[b - 1].delka - znaky[a].od);
}

// pro hledani v -s: bez diakritiky a velikosti pismen
std::string bezDiakritiky(const std::string& text) {
    std::string rozlozeny = nfkd(text);
    std::string vysledek;
    for (const Znak& z : naZnaky(rozlozeny)) {
        if (z.kod < 0) {
            vysledek += rozlozeny.substr(z.od, z.delka);
        } else if (z.kod < 0x300 || z.kod > 0x36f) {
            pridejZnak(vysledek, utf8proc_tolower(z.kod));
        }
    }
    return vysledek;
}

// ---------- JSON ----------

// bezpecny pristup k j[klic]: kdyz j neni objekt nebo klic chybi, vrati null
const json& polozka(const json& j, const char* klic) {
    static const json nic;
    if (j.is_object()) {
        auto it = j.find(klic);
        if (it != j.end()) return *it;
    }
    return nic;
}

std::optional<std::string> jakoText(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    return std::nullopt;
}

// neprazdny text (jako JS `x || ...`)
std::optional<std::string> neprazdnyText(const json& j) {
    auto t = jakoText(j);
    if (t && !t->empty()) return t;
    return std::nullopt;
}

std::optional<double> jakoCislo(const json& j) {
    if (j.is_number()) return j.get<double>();
    return std::nullopt;
}

// Deezer ID muze byt cislo i text, do URL ho potrebujeme jako text
std::string idText(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number_integer()) return std::to_string(j.get<long long>());
    if (j.is_number()) return j.dump();
    return "";
}

// cele cislo zapsat bez ".0", aby playlist.json vypadal jako z JS
json cisloDoJson(std::optional<double> x) {
    if (!x) return nullptr;
    if (std::floor(*x) == *x && std::fabs(*x) < 1e15) return static_cast<long long>(*x);
    return *x;
}

// ---------- soubory ----------

std::optional<std::string> prectiSoubor(const fs::path& soubor) {
    std::ifstream f(soubor, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream obsah;
    obsah << f.rdbuf();
    if (f.bad()) return std::nullopt;
    return obsah.str();
}

void zapisSoubor(const fs::path& soubor, const std::string& obsah, bool pripojit = false) {
    std::ofstream f(soubor, std::ios::binary | (pripojit ? std::ios::app : std::ios::trunc));
    f << obsah;
    if (!f) throw std::runtime_error(std::format("nejde zapsat {}", soubor.string()));
}

json nactiJson(const fs::path& soubor, json vychozi) {
    auto text = prectiSoubor(soubor);
    if (!text) return vychozi;
    json data = json::parse(*text, nullptr, false);
    return data.is_discarded() ? vychozi : data;
}

void ulozJson(const fs::path& soubor, const json& data) {
    zapisSoubor(soubor, data.dump(2, ' ', false, json::error_handler_t::replace));
}

fs::path domov() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/";
}

// ~ na zacatku = domovska slozka, vysledek je absolutni cesta (jako path.resolve)
fs::path rozbalCestu(const std::string& cesta) {
    std::string c = cesta;
    if (c == "~" || zacinaNa(c, "~/")) c = domov().string() + c.substr(1);
    std::string vysledek = fs::absolute(c).lexically_normal().string();
    while (vysledek.size() > 1 && vysledek.back() == '/') vysledek.pop_back();
    return vysledek;
}

bool jeSoubor(const fs::path& cesta) {
    std::error_code chyba;
    return fs::is_regular_file(cesta, chyba);
}

bool jeSlozka(const fs::path& cesta) {
    std::error_code chyba;
    return fs::is_directory(cesta, chyba);
}

std::string zakladniJmeno(const std::string& cesta) {
    return fs::path(cesta).filename().string();
}

// vsechna mp3 ve slozce vcetne podslozek, jako plne cesty
std::vector<std::string> mp3VeSlozce(const fs::path& slozka) {
    std::vector<std::string> vysledek;
    std::error_code chyba;
    for (auto it = fs::recursive_directory_iterator(slozka, fs::directory_options::skip_permission_denied, chyba);
         !chyba && it != fs::recursive_directory_iterator(); it.increment(chyba)) {
        if (konciNaMp3(it->path().filename().string())) vysledek.push_back(it->path().string());
    }
    return vysledek;
}

// ---------- spousteni programu ----------

struct Vysledek {
    int kod = -1;
    std::string vystup;
    std::string chyby;
};

// spusti program bez shellu (jako execFile v Node) a pocka, az skonci
Vysledek spust(const std::string& program, const std::vector<std::string>& argumenty) {
    Vysledek v;
    int vystup[2], chyby[2];
    if (pipe2(vystup, O_CLOEXEC) != 0) {
        v.chyby = std::strerror(errno);
        return v;
    }
    if (pipe2(chyby, O_CLOEXEC) != 0) {
        v.chyby = std::strerror(errno);
        close(vystup[0]);
        close(vystup[1]);
        return v;
    }

    posix_spawn_file_actions_t akce;
    posix_spawn_file_actions_init(&akce);
    posix_spawn_file_actions_addopen(&akce, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&akce, vystup[1], 1);
    posix_spawn_file_actions_adddup2(&akce, chyby[1], 2);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& a : argumenty) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid;
    int r = posix_spawnp(&pid, program.c_str(), &akce, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&akce);
    close(vystup[1]);
    close(chyby[1]);
    if (r != 0) {
        close(vystup[0]);
        close(chyby[0]);
        v.chyby = std::format("{} nejde spustit: {}", program, std::strerror(r));
        return v;
    }

    // stdout i stderr cist zaroven, jinak se program muze zaseknout na plne roure
    pollfd roury[2] = {{vystup[0], POLLIN, 0}, {chyby[0], POLLIN, 0}};
    std::string* cile[2] = {&v.vystup, &v.chyby};
    int otevrene = 2;
    char buf[65536];
    while (otevrene > 0) {
        if (poll(roury, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (roury[i].fd < 0 || !(roury[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(roury[i].fd, buf, sizeof buf);
            if (n > 0) {
                cile[i]->append(buf, static_cast<size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                close(roury[i].fd);
                roury[i].fd = -1;
                otevrene--;
            }
        }
    }
    for (auto& roura : roury) {
        if (roura.fd >= 0) close(roura.fd);
    }

    int stav = 0;
    while (waitpid(pid, &stav, 0) < 0 && errno == EINTR) {
    }
    v.kod = WIFEXITED(stav) ? WEXITSTATUS(stav) : -1;
    return v;
}

// delka mp3 v sekundach pres ffprobe (kvuli porovnani s nalezenou skladbou)
std::optional<double> delkaMp3(const fs::path& soubor) {
    Vysledek v = spust("ffprobe", {"-v", "error", "-show_entries", "format=duration",
                                   "-of", "csv=p=0", soubor.string()});
    if (v.kod != 0) return std::nullopt;
    char* konec = nullptr;
    double delka = std::strtod(v.vystup.c_str(), &konec);
    if (konec == v.vystup.c_str() || !std::isfinite(delka)) return std::nullopt;
    return delka;
}

// ---------- HTTP (libcurl) ----------

const std::string UA = "playlist-sync/1.0";

struct Odpoved {
    bool spojeni = false;  // false = sitova chyba nebo timeout
    long kod = 0;
    std::string telo;
    std::string typ;
};

size_t pridejData(char* data, size_t velikost, size_t pocet, void* cil) {
    static_cast<std::string*>(cil)->append(data, velikost * pocet);
    return velikost * pocet;
}

Odpoved stahni(const std::string& adresa, long casovyLimitMs) {
    Odpoved o;
    CURL* curl = curl_easy_init();
    if (!curl) return o;
    curl_easy_setopt(curl, CURLOPT_URL, adresa.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UA.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, casovyLimitMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, pridejData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &o.telo);
    if (curl_easy_perform(curl) == CURLE_OK) {
        o.spojeni = true;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &o.kod);
        char* typ = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &typ);
        if (typ) o.typ = typ;
    }
    curl_easy_cleanup(curl);
    return o;
}

std::string zakoduj(const std::string& s) {
    std::string vysledek;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            vysledek += static_cast<char>(c);
        } else {
            vysledek += std::format("%{:02X}", c);
        }
    }
    return vysledek;
}

using Parametry = std::vector<std::pair<std::string, std::string>>;

void pockej(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::optional<json> stahniJson(std::string adresa, const Parametry& parametry = {}) {
    for (size_t i = 0; i < parametry.size(); i++) {
        adresa += (i == 0 ? "?" : "&") + zakoduj(parametry[i].first) + "=" + zakoduj(parametry[i].second);
    }
    for (int pokus = 0; pokus < 3; pokus++) {
        Odpoved o = stahni(adresa, 15000);
        json data = o.spojeni ? json::parse(o.telo, nullptr, false) : json(json::value_t::discarded);
        if (data.is_discarded()) {
            pockej(2000 * (pokus + 1));
            continue;
        }
        // Deezer vraci prekroceni kvoty jako error s kodem 4
        if (jakoCislo(polozka(polozka(data, "error"), "code")) == 4.0) {
            pockej(5000);
            continue;
        }
        return data;
    }
    return std::nullopt;
}

struct Obrazek {
    std::string data;
    std::string mime;
};

std::optional<Obrazek> stahniObrazek(const std::string& adresa) {
    Odpoved o = stahni(adresa, 20000);
    if (!o.spojeni || o.kod < 200 || o.kod > 299) return std::nullopt;
    std::string mime = orizni(o.typ.substr(0, o.typ.find(';')));
    return Obrazek{std::move(o.telo), mime.empty() ? "image/jpeg" : mime};
}

// ---------- Spotify ----------

struct Pisnicka {
    std::string id;  // prazdne = neni ze Spotify
    std::string nazev;
    std::string interpret;
    std::optional<double> delka;
};

Pisnicka pisnickaZJson(const json& j) {
    return {
        jakoText(polozka(j, "id")).value_or(""),
        jakoText(polozka(j, "nazev")).value_or(""),
        jakoText(polozka(j, "interpret")).value_or(""),
        jakoCislo(polozka(j, "delka")),
    };
}

json pisnickaDoJson(const Pisnicka& p) {
    return {
        {"id", p.id.empty() ? json(nullptr) : json(p.id)},
        {"nazev", p.nazev},
        {"interpret", p.interpret},
        {"delka", cisloDoJson(p.delka)},
    };
}

std::string jmenoSouboru(const Pisnicka& p) {
    std::string jmeno = p.interpret + " - " + p.nazev;
    for (char& c : jmeno) {
        if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) c = '_';
    }
    return orizni(jmeno);
}

struct SkladbaSpotify {
    std::string uri;
    std::string nazev;
    std::string interpret;
    std::optional<double> delkaMs;
};

// interpreti jako ve spotify-url-info: "A, B & C", jinak podtitulek
SkladbaSpotify naSkladbu(const json& s) {
    std::vector<std::string> jmena;
    const json& interpreti = polozka(s, "artists");
    if (interpreti.is_array()) {
        for (const json& a : interpreti) {
            if (auto jmeno = jakoText(polozka(a, "name"))) jmena.push_back(*jmeno);
        }
    }
    std::string interpret;
    for (size_t i = 0; i < jmena.size(); i++) {
        if (i > 0) interpret += (i == jmena.size() - 1) ? " & " : ", ";
        interpret += jmena[i];
    }
    if (interpret.empty()) interpret = jakoText(polozka(s, "subtitle")).value_or("");
    return {
        jakoText(polozka(s, "uri")).value_or(""),
        jakoText(polozka(s, "title")).value_or(jakoText(polozka(s, "name")).value_or("")),
        interpret,
        jakoCislo(polozka(s, "duration")),
    };
}

// skladby z embed stranky Spotify (stejne jako getTracks ze spotify-url-info)
std::vector<SkladbaSpotify> nactiSkladby(const std::string& adresa) {
    static const std::regex reAdresa(R"((album|playlist|track|artist|episode)[/:]([A-Za-z0-9]{22}))");
    std::smatch m;
    if (adresa.find("spotify") == std::string::npos || !std::regex_search(adresa, m, reAdresa)) {
        throw std::runtime_error(std::format("Nerozumím adrese playlistu: {}", adresa));
    }
    std::string embed = std::format("https://open.spotify.com/embed/{}/{}", m[1].str(), m[2].str());

    std::string html;
    for (int pokus = 0; pokus < 3; pokus++) {
        Odpoved o = stahni(embed, 30000);
        if (!o.spojeni) {
            if (pokus == 2) throw std::runtime_error(std::format("Spotify neodpovídá: {}", embed));
            continue;
        }
        html = std::move(o.telo);
        if (html.find("<html") != std::string::npos) break;
    }

    // data jsou jako JSON v <script id="__NEXT_DATA__" type="application/json">
    size_t znacka = html.find("id=\"__NEXT_DATA__\"");
    size_t od = znacka == std::string::npos ? znacka : html.find('>', znacka);
    size_t konec = od == std::string::npos ? od : html.find("</script>", od);
    if (konec == std::string::npos) {
        throw std::runtime_error(std::format(
            "Na stránce {} nejsou data playlistu, Spotify asi změnil stránku.", embed));
    }
    json data = json::parse(html.substr(od + 1, konec - od - 1), nullptr, false);
    const json& entita = polozka(polozka(polozka(polozka(polozka(data, "props"), "pageProps"),
                                                 "state"), "data"), "entity");
    if (!jakoText(polozka(entita, "type")) || !jakoText(polozka(entita, "name"))) {
        throw std::runtime_error(std::format("Data ze stránky {} nemají očekávaný tvar.", embed));
    }

    std::vector<SkladbaSpotify> skladby;
    const json& seznam = polozka(entita, "trackList");
    if (seznam.is_array()) {
        for (const json& s : seznam) skladby.push_back(naSkladbu(s));
    } else {
        skladby.push_back(naSkladbu(entita));
    }
    return skladby;
}

// spotify vraci max 100 skladeb na playlist, proto jde zadat vic playlistu
// v config.playlist_url (jedna URL nebo pole URL) a skladby se spoji
std::vector<Pisnicka> nactiPlaylist() {
    std::vector<std::string> adresy;
    const json& url = polozka(config, "playlist_url");
    if (url.is_string()) adresy.push_back(url.get<std::string>());
    if (url.is_array()) {
        for (const json& x : url) {
            if (x.is_string()) adresy.push_back(x.get<std::string>());
        }
    }
    if (adresy.empty()) {
        throw std::runtime_error(std::format("V {} chybí playlist_url.", CONFIG_JSON.string()));
    }

    std::vector<Pisnicka> vysledek;
    std::unordered_map<std::string, size_t> poradi;  // id -> index ve vysledku
    for (const auto& adresa : adresy) {
        auto skladby = nactiSkladby(adresa);
        if (skladby.size() >= 100) {
            vypis("Pozor: playlist {} má aspoň 100 skladeb, zbytek se nenačte. Rozděl ho.", adresa);
        }
        for (const auto& s : skladby) {
            if (!zacinaNa(s.uri, "spotify:track")) continue;
            auto casti = rozdel(s.uri, ":");
            std::string id = casti.size() > 2 ? casti[2] : "";
            std::optional<double> delka;
            if (s.delkaMs && *s.delkaMs != 0) delka = std::round(*s.delkaMs / 1000);
            Pisnicka p{id, s.nazev, s.interpret, delka};
            // stejna skladba ve dvou playlistech: misto prvni, udaje posledni (jako JS Map)
            if (auto it = poradi.find(id); it != poradi.end()) {
                vysledek[it->second] = p;
            } else {
                poradi[id] = vysledek.size();
                vysledek.push_back(p);
            }
        }
    }
    return vysledek;
}

// % je v sablone nazvu yt-dlp specialni znak, %% je obycejne %
std::string escapujSablonu(const std::string& s) {
    return nahradVse(s, "%", "%%");
}

std::optional<std::string> stahniMp3(const Pisnicka& p) {
    std::string jmeno = jmenoSouboru(p);
    fs::path cilovySoubor = SLOZKA_MP3 / (jmeno + ".mp3");

    Vysledek v = spust("yt-dlp", {
        std::format("ytsearch1:{} - {} audio", p.interpret, p.nazev),
        "--extract-audio",
        "--audio-format", "mp3",
        "--audio-quality", "192K",
        "--no-playlist",
        "--quiet",
        "--no-warnings",
        "-o", escapujSablonu((SLOZKA_MP3 / jmeno).string()) + ".%(ext)s",
    });

    if (v.kod != 0) {
        std::string duvod = orizni(v.chyby);
        if (duvod.empty()) duvod = std::format("yt-dlp skončil s kódem {}", v.kod);
        vypis("nepodařilo se stáhnout: {}", duvod);
        return std::nullopt;
    }

    if (fs::exists(cilovySoubor)) return jmeno + ".mp3";
    return std::nullopt;
}

// ---------- tagy (prevzato z opravit-tagy.py) ----------
// dohleda skladbu na Deezeru (zaloha iTunes) a zapise nazev, interpreta, album,
// rok, zanr, cislo stopy/disku a cover alba

// hleda se v textu prevedenem na mala pismena (misto priznaku /i)
const std::regex KOMPILACE(
    R"(\b(hits|now that|compilation|best of|greatest|top \d+|party|summer \d{4}|workout|playlist|various|mix \d{4}|karaoke|tribute)\b)");

bool jeKompilace(const std::string& album) {
    return std::regex_search(maleAscii(album), KOMPILACE);
}

// porovnavani nazvu

std::string normalizuj(const std::string& text) {
    static const std::regex reZavorkaFeat(R"([\(\[].*?(feat|ft\.|with|prod).*?[\)\]])");
    static const std::regex reFeat(R"(\b(feat|ft)\.? .*)");
    static const std::regex reZnaky("[^a-z0-9 ]+");
    static const std::regex reMezery(R"(\s+)");

    std::string s;
    for (char c : nfkd(text)) {
        if (static_cast<unsigned char>(c) < 0x80) s += c;
    }
    s = maleAscii(s);
    s = nahradVse(s, "_", " ");
    s = nahradVse(s, "&", " and ");
    s = std::regex_replace(s, reZavorkaFeat, " ");
    s = std::regex_replace(s, reFeat, " ", std::regex_constants::format_first_only);
    s = std::regex_replace(s, reZnaky, " ");
    s = std::regex_replace(s, reMezery, " ");
    return orizni(s);
}

// pocet shodnych znaku jako v pythonim difflib.SequenceMatcher:
// najdi nejdelsi spolecny usek a totez opakuj vlevo a vpravo od nej
size_t pocetShod(std::string_view a, std::string_view b) {
    size_t delka = 0, ia = 0, ib = 0;
    for (size_t i = 0; i < a.size(); i++) {
        for (size_t j = 0; j < b.size(); j++) {
            size_t k = 0;
            while (i + k < a.size() && j + k < b.size() && a[i + k] == b[j + k]) k++;
            if (k > delka) {
                delka = k;
                ia = i;
                ib = j;
            }
        }
    }
    if (delka == 0) return 0;
    return delka + pocetShod(a.substr(0, ia), b.substr(0, ib))
        + pocetShod(a.substr(ia + delka), b.substr(ib + delka));
}

double podobnost(const std::string& x, const std::string& y) {
    std::string a = normalizuj(x);
    std::string b = normalizuj(y);
    if (a.empty() || b.empty()) return 0;
    if (a == b) return 1;
    double pomer = 2.0 * static_cast<double>(pocetShod(a, b)) / static_cast<double>(a.size() + b.size());
    // jeden obsahuje druhy ("Dancin" vs "Dancin - Krono Remix")
    if (a.find(b) != std::string::npos || b.find(a) != std::string::npos) {
        double kratsi = static_cast<double>(std::min(a.size(), b.size()));
        double delsi = static_cast<double>(std::max(a.size(), b.size()));
        pomer = std::max(pomer, 0.85 * kratsi / delsi + 0.15);
    }
    return pomer;
}

std::vector<std::string> rozdelInterprety(const std::string& text) {
    static const std::regex re(R"(\s*(?:,|&|/|\bx\b|\bfeat\.?|\bft\.?|\band\b)\s*)");
    auto casti = rozdelRegexem(text, re, maleAscii(text));
    std::erase(casti, "");
    return casti;
}

std::vector<std::string> slova(const std::string& text) {
    return rozdel(normalizuj(text), " ");
}

struct Kandidat {
    std::string zdroj;
    std::string id;       // deezer
    std::string nazev;
    std::string interpret;
    std::string album;
    std::string albumId;  // deezer
    std::optional<double> delka;
    std::string cover;    // deezer
    json surova;          // itunes: cely zaznam
};

double shoda(const Kandidat& kandidat, const std::vector<std::string>& interpreti,
             const std::string& nazev, std::optional<double> delka) {
    double t = podobnost(nazev, kandidat.nazev);
    std::vector<std::string> jehoInterpreti{kandidat.interpret};
    for (auto& x : rozdelInterprety(kandidat.interpret)) jehoInterpreti.push_back(x);
    double a = 0.5;
    if (!interpreti.empty()) {
        a = -INFINITY;
        for (const auto& x : interpreti) {
            for (const auto& y : jehoInterpreti) a = std::max(a, podobnost(x, y));
        }
    }
    double s = 0.6 * t + 0.4 * a;
    if (delka && *delka != 0 && kandidat.delka && *kandidat.delka != 0) {
        double rozdil = std::fabs(*delka - *kandidat.delka);
        if (rozdil <= 3) s += 0.05;
        else if (rozdil > 20) s -= 0.1;
    }
    if (jeKompilace(kandidat.album)) {
        s -= 0.12;
    }
    // remix/live/acoustic musi sedet s nazvem ze Spotify
    auto mojeSlova = slova(nazev);
    auto jehoSlova = slova(kandidat.nazev);
    for (const char* slovo : {"remix", "live", "acoustic", "instrumental", "remaster", "version", "mix"}) {
        if (obsahuje(mojeSlova, slovo) != obsahuje(jehoSlova, slovo)) {
            s -= 0.15;
        }
    }
    return s;
}

// zdroje

std::string dotaz(const std::vector<std::string>& interpreti, const std::string& nazev) {
    static const std::regex reZavorky(R"([\(\[].*?[\)\]])");
    return orizni((interpreti.empty() ? "" : interpreti[0]) + " " + std::regex_replace(nazev, reZavorky, ""));
}

std::vector<Kandidat> hledejDeezer(const std::vector<std::string>& interpreti, const std::string& nazev) {
    std::vector<Kandidat> kandidati;
    auto data = stahniJson("https://api.deezer.com/search", {{"q", dotaz(interpreti, nazev)}, {"limit", "15"}});
    if (!data) return kandidati;
    const json& seznam = polozka(*data, "data");
    if (!seznam.is_array()) return kandidati;
    for (const json& t : seznam) {
        const json& album = polozka(t, "album");
        Kandidat k;
        k.zdroj = "deezer";
        k.id = idText(polozka(t, "id"));
        k.nazev = jakoText(polozka(t, "title")).value_or("");
        k.interpret = jakoText(polozka(polozka(t, "artist"), "name")).value_or("");
        k.album = jakoText(polozka(album, "title")).value_or("");
        k.albumId = idText(polozka(album, "id"));
        k.delka = jakoCislo(polozka(t, "duration"));
        k.cover = jakoText(polozka(album, "cover_xl")).value_or("");
        kandidati.push_back(std::move(k));
    }
    return kandidati;
}

// udaje, ktere se zapisou do mp3; co chybi, to se v souboru nemeni
struct Info {
    std::string nazev;
    std::string interpret;
    std::optional<std::string> album;
    std::optional<std::string> interpretAlba;
    std::optional<std::string> datum;
    std::optional<std::string> zanr;
    std::optional<double> stopa;
    std::optional<double> stop;
    std::optional<double> disk;
    std::optional<std::string> cover;
};

std::map<std::string, json> albaDeezer;

Info detailDeezer(const Kandidat& k) {
    json skladba = stahniJson("https://api.deezer.com/track/" + k.id).value_or(json::object());
    if (!albaDeezer.contains(k.albumId)) {
        albaDeezer[k.albumId] = stahniJson("https://api.deezer.com/album/" + k.albumId).value_or(json::object());
    }
    const json& album = albaDeezer[k.albumId];

    std::vector<std::string> interpreti;
    const json& prispevatele = polozka(skladba, "contributors");
    if (prispevatele.is_array()) {
        for (const json& x : prispevatele) {
            auto jmeno = jakoText(polozka(x, "name"));
            if (jmeno && !obsahuje(interpreti, *jmeno)) interpreti.push_back(*jmeno);
        }
    }
    const json& zanry = polozka(polozka(album, "genres"), "data");

    Info info;
    info.nazev = jakoText(polozka(skladba, "title")).value_or(k.nazev);
    info.interpret = interpreti.empty() ? k.interpret : spoj(interpreti, ", ");
    info.album = jakoText(polozka(album, "title")).value_or(k.album);
    info.interpretAlba = jakoText(polozka(polozka(album, "artist"), "name")).value_or(k.interpret);
    info.datum = neprazdnyText(polozka(skladba, "release_date"))
        .value_or(neprazdnyText(polozka(album, "release_date")).value_or(""))
        .substr(0, 10);
    if (zanry.is_array() && !zanry.empty()) info.zanr = jakoText(polozka(zanry[0], "name"));
    info.stopa = jakoCislo(polozka(skladba, "track_position"));
    info.stop = jakoCislo(polozka(album, "nb_tracks"));
    info.disk = jakoCislo(polozka(skladba, "disk_number"));
    info.cover = neprazdnyText(polozka(album, "cover_xl")).value_or(k.cover);
    return info;
}

std::chrono::steady_clock::time_point posledniITunes{};

std::vector<Kandidat> hledejITunes(const std::vector<std::string>& interpreti, const std::string& nazev) {
    // iTunes povoluje cca 20 dotazu za minutu
    auto cekej = std::chrono::milliseconds(3200) - (std::chrono::steady_clock::now() - posledniITunes);
    if (cekej > std::chrono::nanoseconds::zero()) std::this_thread::sleep_for(cekej);
    posledniITunes = std::chrono::steady_clock::now();

    std::vector<Kandidat> kandidati;
    auto data = stahniJson("https://itunes.apple.com/search", {
        {"term", dotaz(interpreti, nazev)}, {"entity", "song"}, {"limit", "15"},
    });
    if (!data) return kandidati;
    const json& seznam = polozka(*data, "results");
    if (!seznam.is_array()) return kandidati;
    for (const json& t : seznam) {
        if (jakoText(polozka(t, "kind")) != "song") continue;
        Kandidat k;
        k.zdroj = "itunes";
        k.nazev = jakoText(polozka(t, "trackName")).value_or("");
        k.interpret = jakoText(polozka(t, "artistName")).value_or("");
        k.album = jakoText(polozka(t, "collectionName")).value_or("");
        auto ms = jakoCislo(polozka(t, "trackTimeMillis"));
        if (ms && *ms != 0) k.delka = *ms / 1000;
        k.surova = t;
        kandidati.push_back(std::move(k));
    }
    return kandidati;
}

Info detailITunes(const Kandidat& k) {
    static const std::regex reEpSingl(" - (EP|Single)$");
    const json& t = k.surova;
    std::string obrazek = jakoText(polozka(t, "artworkUrl100")).value_or("");

    Info info;
    info.nazev = jakoText(polozka(t, "trackName")).value_or(k.nazev);
    info.interpret = jakoText(polozka(t, "artistName")).value_or(k.interpret);
    info.album = std::regex_replace(jakoText(polozka(t, "collectionName")).value_or(""), reEpSingl, "");
    info.interpretAlba = neprazdnyText(polozka(t, "collectionArtistName")).value_or(info.interpret);
    info.datum = jakoText(polozka(t, "releaseDate")).value_or("").substr(0, 10);
    info.zanr = jakoText(polozka(t, "primaryGenreName"));
    info.stopa = jakoCislo(polozka(t, "trackNumber"));
    info.stop = jakoCislo(polozka(t, "trackCount"));
    info.disk = jakoCislo(polozka(t, "discNumber"));
    if (!obrazek.empty()) {
        size_t i = obrazek.find("100x100bb");
        if (i != std::string::npos) obrazek.replace(i, 9, "1000x1000bb");
        info.cover = obrazek;
    }
    return info;
}

struct Nalez {
    std::optional<Info> info;  // nullopt = nic dost dobreho
    double shoda = 0;
    std::optional<Kandidat> kandidat;
};

Nalez dohledej(const std::vector<std::string>& interpreti, const std::string& nazev,
               std::optional<double> delka) {
    using Hledani = std::vector<Kandidat> (*)(const std::vector<std::string>&, const std::string&);
    const Hledani zdroje[] = {hledejDeezer, hledejITunes};

    std::optional<Kandidat> nejlepsi;
    double nejlepsiShoda = 0;
    for (Hledani hledej : zdroje) {
        auto kandidati = hledej(interpreti, nazev);
        for (size_t i = 0; i < kandidati.size(); i++) {
            double s = shoda(kandidati[i], interpreti, nazev, delka) - 0.005 * static_cast<double>(i);
            if (s > nejlepsiShoda) {
                nejlepsi = kandidati[i];
                nejlepsiShoda = s;
            }
        }
        // kompilace ("Indie Hits" apod.) zkus jeste vylepsit v dalsim zdroji
        if (nejlepsi && nejlepsiShoda >= MIN_SHODA && !jeKompilace(nejlepsi->album)) break;
    }
    if (!nejlepsi || nejlepsiShoda < MIN_SHODA) {
        return {std::nullopt, nejlepsiShoda, nejlepsi};
    }
    Info info = nejlepsi->zdroj == "deezer" ? detailDeezer(*nejlepsi) : detailITunes(*nejlepsi);
    return {info, nejlepsiShoda, nejlepsi};
}

// zapis (TagLib)

bool maDobreTagy(const fs::path& soubor) {
    TagLib::MPEG::File f(soubor.c_str(), false);
    if (!f.isValid() || !f.hasID3v2Tag()) return false;
    TagLib::ID3v2::Tag* tag = f.ID3v2Tag();
    return !tag->album().isEmpty() && !tag->frameList("APIC").isEmpty();
}

// prazdna hodnota = ramec nechat, jak je (jako undefined v node-id3)
void nastavText(TagLib::ID3v2::Tag* tag, const char* ramec, const std::string& hodnota) {
    if (hodnota.empty()) return;
    tag->removeFrames(ramec);
    auto* f = new TagLib::ID3v2::TextIdentificationFrame(ramec, TagLib::String::UTF8);
    f->setText(TagLib::String(hodnota, TagLib::String::UTF8));
    tag->addFrame(f);
}

std::string celeCislo(std::optional<double> x) {
    if (!x || *x == 0) return "";
    return std::to_string(static_cast<long long>(*x));
}

bool zapisTagy(const fs::path& soubor, const Info& info, const std::optional<Obrazek>& cover, std::string& chyba) {
    TagLib::MPEG::File f(soubor.c_str(), false);
    if (!f.isValid()) {
        chyba = "soubor nejde otevřít jako mp3";
        return false;
    }
    TagLib::ID3v2::Tag* tag = f.ID3v2Tag(true);
    std::string datum = info.datum.value_or("");
    std::string stopa = celeCislo(info.stopa);
    std::string stop = celeCislo(info.stop);

    nastavText(tag, "TIT2", info.nazev);
    nastavText(tag, "TPE1", info.interpret);
    nastavText(tag, "TPE2", info.interpretAlba.value_or(""));
    nastavText(tag, "TALB", info.album.value_or(""));
    // pri ulozeni jako ID3v2.3 z TDRC udela TagLib TYER
    nastavText(tag, "TDRC", datum.substr(0, 4));
    nastavText(tag, "TCON", info.zanr.value_or(""));
    nastavText(tag, "TRCK", stopa.empty() ? "" : stopa + (stop.empty() ? "" : "/" + stop));
    nastavText(tag, "TPOS", celeCislo(info.disk));

    if (cover) {
        tag->removeFrames("APIC");
        auto* obrazek = new TagLib::ID3v2::AttachedPictureFrame;
        obrazek->setMimeType(TagLib::String(cover->mime, TagLib::String::UTF8));
        obrazek->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
        obrazek->setDescription("Cover");
        obrazek->setPicture(TagLib::ByteVector(cover->data.data(), static_cast<unsigned int>(cover->data.size())));
        tag->addFrame(obrazek);
    }

    if (!f.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, TagLib::ID3v2::v3,
                TagLib::File::DoNotDuplicate)) {
        chyba = "TagLib soubor neuložil";
        return false;
    }
    return true;
}

struct Statistika {
    int stazeno = 0;
    int nalezeno = 0;
    std::vector<std::string> nenalezeno;
    int uzOtagovanych = 0;
    int preskoceno = 0;
} statistika;

void otaguj(const fs::path& soubor, const Pisnicka& p) {
    static const std::regex reOddelovac(", | & ");
    std::vector<std::string> interpreti;
    for (const auto& x : rozdelRegexem(p.interpret, reOddelovac, p.interpret)) {
        std::string jmeno = orizni(x);
        if (!jmeno.empty()) interpreti.push_back(jmeno);
    }
    std::optional<double> delka = p.delka ? p.delka : delkaMp3(soubor);
    Nalez nalez = dohledej(interpreti, p.nazev, delka);

    Info info;
    if (!nalez.info) {
        std::string tip = nalez.kandidat
            ? std::format(" (nejbližší: {} - {}, {:.2f})", nalez.kandidat->interpret, nalez.kandidat->nazev, nalez.shoda)
            : "";
        vypis("    ✗ tagy nenalezeny{}", tip);
        statistika.nenalezeno.push_back(soubor.filename().string());
        info.nazev = p.nazev;
        info.interpret = spoj(interpreti, ", ");
    } else {
        info = *nalez.info;
        vypis("    ✓ {} - {} | {} ({}) [{}, {:.2f}]", info.interpret, info.nazev, info.album.value_or(""),
              info.datum.value_or("").substr(0, 4), nalez.kandidat->zdroj, nalez.shoda);
        statistika.nalezeno++;
        // u vice interpretu je seznam ze Spotify / nazvu souboru casto uplnejsi
        if (interpreti.size() > rozdel(info.interpret, ", ").size()) {
            info.interpret = spoj(interpreti, ", ");
        }
    }

    if (zkouska) return;

    std::optional<Obrazek> cover;
    if (info.cover && !info.cover->empty()) {
        cover = stahniObrazek(*info.cover);
        if (!cover) vypis("    ! cover se nepodařilo stáhnout");
    }
    std::string chyba;
    if (!zapisTagy(soubor, info, cover, chyba)) {
        vypis("    ! zápis tagů selhal: {}", chyba);
        return;
    }
    if (cover && ulozitCover) {
        zapisSoubor(bezMp3(soubor.string()) + ".jpg", cover->data);
    }
}

// chyba jedne pisnicky nesmi shodit cely beh
void otagujBezpecne(const fs::path& soubor, const Pisnicka& p) {
    try {
        otaguj(soubor, p);
    } catch (const std::exception& e) {
        vypis("    ! chyba: {}", e.what());
    }
}

// ---------- seznam preskakovanych ----------

std::vector<std::string> nactiPreskakovane() {
    std::vector<std::string> vzory;
    auto text = prectiSoubor(PRESKOCIT_TXT);
    if (!text) return vzory;
    for (const auto& radek : rozdel(*text, "\n")) {
        std::string r = orizni(radek);
        if (!r.empty() && r[0] != '#') vzory.push_back(r);
    }
    return vzory;
}

void pridejPreskakovane(const std::vector<std::string>& polozky) {
    auto seznam = nactiPreskakovane();
    std::set<std::string> existujici(seznam.begin(), seznam.end());
    std::vector<std::string> pridat;
    for (const auto& x : polozky) {
        // existujici soubor se zapise jen jmenem, vzor ("Alex G - *") tak jak je
        std::string polozka = jeSoubor(rozbalCestu(x)) ? zakladniJmeno(x) : x;
        if (!existujici.contains(polozka) && !obsahuje(pridat, polozka)) {
            pridat.push_back(polozka);
        }
    }
    if (!fs::exists(PRESKOCIT_TXT)) {
        zapisSoubor(PRESKOCIT_TXT,
                    "# Soubory, které playlist-sync při tagování přeskočí.\n"
                    "# Jeden název souboru nebo vzor (*, ?) na řádek.\n");
    }
    std::string text;
    for (const auto& x : pridat) text += x + "\n";
    zapisSoubor(PRESKOCIT_TXT, text, true);
    for (const auto& x : pridat) {
        vypis("+ {}", x);
    }
    vypis("Přidáno {} položek do {}", pridat.size(), PRESKOCIT_TXT.string());
}

// vzor s * a ? -> regularni vyraz; ? je jeden znak, i kdyz ma v UTF-8 vic bajtu
std::regex vzorNaRegex(const std::string& vzor) {
    std::string re = "^";
    for (char c : vzor) {
        if (c == '*') {
            re += ".*";
        } else if (c == '?') {
            re += R"((?:[\x01-\x7F]|[\xC0-\xFF][\x80-\xBF]*))";
        } else {
            if (std::string_view(".+^${}()|[]\\").find(c) != std::string_view::npos) re += '\\';
            re += c;
        }
    }
    re += '$';
    return std::regex(re);
}

bool jePreskocen(const std::string& soubor, const std::vector<std::regex>& vzory) {
    std::string jmeno = zakladniJmeno(soubor);
    return std::any_of(vzory.begin(), vzory.end(), [&](const std::regex& re) {
        return std::regex_match(jmeno, re) || std::regex_match(soubor, re);
    });
}

// ---------- vyber a tagovani souboru ----------

// -s: cesta k souboru nebo cast nazvu
std::vector<std::string> vyberPisnicky(const std::vector<std::string>& dotazy, const std::vector<std::string>& vsechny) {
    std::vector<std::string> vybrane;
    for (const auto& d : dotazy) {
        fs::path cesta = rozbalCestu(d);
        if (jeSoubor(cesta)) {
            vybrane.push_back(cesta.string());
            continue;
        }
        std::string hledany = bezDiakritiky(d);
        size_t predtim = vybrane.size();
        for (const auto& s : vsechny) {
            if (bezDiakritiky(bezMp3(zakladniJmeno(s))).find(hledany) != std::string::npos) {
                vybrane.push_back(s);
            }
        }
        if (vybrane.size() == predtim) {
            vypisChybu("Žádná písnička neodpovídá: {}", d);
            std::exit(1);
        }
    }
    // bez duplicit, poradi zustane
    std::vector<std::string> unikatni;
    std::set<std::string> videne;
    for (const auto& s : vybrane) {
        if (videne.insert(s).second) unikatni.push_back(s);
    }
    return unikatni;
}

// pisnicka stazena rucne (neni v playlistu): "Interpret1, Interpret2 - Nazev.mp3"
Pisnicka pisnickaZNazvu(const std::string& jmeno) {
    std::string bezPripony = bezMp3(jmeno);
    size_t oddelovac = bezPripony.find(" - ");
    if (oddelovac == std::string::npos) {
        return {"", orizni(bezPripony), "", std::nullopt};
    }
    return {"", orizni(bezPripony.substr(oddelovac + 3)), orizni(bezPripony.substr(0, oddelovac)), std::nullopt};
}

void otagujSoubory(const std::vector<std::string>& soubory, const std::vector<Pisnicka>& playlist,
                   bool vynutit, bool preskakovat) {
    std::map<std::string, Pisnicka> zPlaylistu;
    for (const auto& p : playlist) zPlaylistu[jmenoSouboru(p) + ".mp3"] = p;
    std::vector<std::regex> vzory;
    if (preskakovat) {
        for (const auto& v : nactiPreskakovane()) vzory.push_back(vzorNaRegex(v));
    }

    std::vector<std::string> kOtagovani;
    for (const auto& soubor : soubory) {
        if (jePreskocen(soubor, vzory)) {
            statistika.preskoceno++;
        } else if (!vynutit && maDobreTagy(soubor)) {
            statistika.uzOtagovanych++;
        } else {
            kOtagovani.push_back(soubor);
        }
    }
    vypis("Písniček k otagování: {}", kOtagovani.size());
    for (size_t i = 0; i < kOtagovani.size(); i++) {
        std::string jmeno = zakladniJmeno(kOtagovani[i]);
        vypis("[{}/{}] Taguji: {}", i + 1, kOtagovani.size(), jmeno);
        auto it = zPlaylistu.find(jmeno);
        otagujBezpecne(kOtagovani[i], it != zPlaylistu.end() ? it->second : pisnickaZNazvu(jmeno));
    }
}

void vypisShrnuti() {
    std::vector<std::string> casti;
    if (!jenTagovat) casti.push_back(std::format("{} staženo", statistika.stazeno));
    casti.push_back(std::format("{} s nalezenými tagy", statistika.nalezeno));
    casti.push_back(std::format("{} nenalezeno", statistika.nenalezeno.size()));
    casti.push_back(std::format("{} už otagovaných", statistika.uzOtagovanych));
    casti.push_back(std::format("{} na seznamu přeskakovaných", statistika.preskoceno));
    std::string pozn = zkouska ? " (jen zkouška, nic se nezapsalo)" : "";
    vypis("\nHotovo{}: {}.", pozn, spoj(casti, ", "));
    if (!statistika.nenalezeno.empty()) {
        vypis("Nenalezené (mají jen interpreta a název):");
        for (const auto& jmeno : statistika.nenalezeno) {
            vypis("  - {}", jmeno);
        }
        vypis("Nechceš je zkoušet znovu? Přidej je: {} -x \"název souboru.mp3\"", PROGRAM);
    }
}

// ---------- prepinace ----------
// stejna pravidla jako parseArgs z node:util (strict, volne argumenty povolene)

struct Volby {
    bool help = false;
    bool otagovat = false;
    bool dryRun = false;
    bool force = false;
    bool saveCover = false;
    std::vector<std::string> dir;
    std::vector<std::string> pisnicka;
    std::vector<std::string> preskocit;
    std::optional<std::string> minScore;
    std::vector<std::string> zbytek;  // volne argumenty
};

struct DefinicePrepinace {
    std::string dlouhy;
    char kratky;  // 0 = nema
    bool sHodnotou;
};

const std::vector<DefinicePrepinace> PREPINACE = {
    {"help", 'h', false},
    {"otagovat", 0, false},
    {"dir", 'd', true},
    {"pisnicka", 's', true},
    {"preskocit", 'x', true},
    {"dry-run", 'n', false},
    {"force", 'f', false},
    {"save-cover", 0, false},
    {"min-score", 0, true},
};

void nastav(Volby& v, const std::string& prepinac, const std::string& hodnota) {
    if (prepinac == "help") v.help = true;
    else if (prepinac == "otagovat") v.otagovat = true;
    else if (prepinac == "dir") v.dir.push_back(hodnota);
    else if (prepinac == "pisnicka") v.pisnicka.push_back(hodnota);
    else if (prepinac == "preskocit") v.preskocit.push_back(hodnota);
    else if (prepinac == "dry-run") v.dryRun = true;
    else if (prepinac == "force") v.force = true;
    else if (prepinac == "save-cover") v.saveCover = true;
    else if (prepinac == "min-score") v.minScore = hodnota;
}

// hodnota prepinace z dalsiho argumentu; hodnota s pomlckou na zacatku je
// nejspis zapomenuta hodnota, parseArgs ji taky odmitne
std::string hodnotaZa(const std::vector<std::string>& argumenty, size_t& i, const std::string& prepinac) {
    if (i + 1 >= argumenty.size()) {
        throw std::runtime_error(std::format("u přepínače '{}' chybí hodnota", prepinac));
    }
    const std::string& hodnota = argumenty[i + 1];
    if (hodnota.size() > 1 && hodnota[0] == '-') {
        throw std::runtime_error(std::format(
            "u přepínače '{}' chybí hodnota ('{}' vypadá jako další přepínač)", prepinac, hodnota));
    }
    i++;
    return hodnota;
}

Volby nactiPrepinace(int argc, char** argv) {
    Volby v;
    std::vector<std::string> argumenty(argv + 1, argv + argc);
    for (size_t i = 0; i < argumenty.size(); i++) {
        const std::string& a = argumenty[i];
        if (a == "--") {
            v.zbytek.insert(v.zbytek.end(), argumenty.begin() + static_cast<long>(i) + 1, argumenty.end());
            break;
        }
        if (zacinaNa(a, "--")) {
            size_t rovnase = a.find('=');
            std::string jmeno = a.substr(2, rovnase == std::string::npos ? std::string::npos : rovnase - 2);
            auto d = std::find_if(PREPINACE.begin(), PREPINACE.end(),
                                  [&](const DefinicePrepinace& x) { return x.dlouhy == jmeno; });
            if (d == PREPINACE.end()) {
                throw std::runtime_error(std::format("neznámý přepínač '--{}'", jmeno));
            }
            if (rovnase != std::string::npos) {
                if (!d->sHodnotou) throw std::runtime_error(std::format("přepínač '--{}' nebere hodnotu", jmeno));
                nastav(v, jmeno, a.substr(rovnase + 1));
            } else {
                nastav(v, jmeno, d->sHodnotou ? hodnotaZa(argumenty, i, "--" + jmeno) : "");
            }
            continue;
        }
        if (a.size() > 1 && a[0] == '-') {
            // skupina kratkych prepinacu (-nf); prepinac s hodnotou si vezme
            // zbytek skupiny (-dSLOZKA), nebo dalsi argument
            for (size_t k = 1; k < a.size(); k++) {
                auto d = std::find_if(PREPINACE.begin(), PREPINACE.end(),
                                      [&](const DefinicePrepinace& x) { return x.kratky != 0 && x.kratky == a[k]; });
                if (d == PREPINACE.end()) {
                    throw std::runtime_error(std::format("neznámý přepínač '-{}'", a[k]));
                }
                if (!d->sHodnotou) {
                    nastav(v, d->dlouhy, "");
                    continue;
                }
                if (k + 1 < a.size()) {
                    nastav(v, d->dlouhy, a.substr(k + 1));
                } else {
                    nastav(v, d->dlouhy, hodnotaZa(argumenty, i, std::string("-") + a[k]));
                }
                break;
            }
            continue;
        }
        v.zbytek.push_back(a);
    }
    return v;
}

// jako JS Number(): cely text musi byt cislo, prazdny text je 0
std::optional<double> naCislo(const std::string& text) {
    std::string t = orizni(text);
    if (t.empty()) return 0.0;
    char* konec = nullptr;
    double x = std::strtod(t.c_str(), &konec);
    if (konec != t.c_str() + t.size()) return std::nullopt;
    return x;
}

// ---------- hlavni program ----------

int hlavni(const Volby& volby) {
    // -x a b c: prvni polozka je hodnota prepinace, dalsi jsou volne argumenty
    if (!volby.preskocit.empty()) {
        std::vector<std::string> polozky = volby.preskocit;
        polozky.insert(polozky.end(), volby.zbytek.begin(), volby.zbytek.end());
        pridejPreskakovane(polozky);
        return 0;
    }

    // volne argumenty bez prepinace se berou jako dalsi slozky (jako v opravit-tagy.py)
    std::vector<std::string> slozkyKTagovani = volby.dir;
    slozkyKTagovani.insert(slozkyKTagovani.end(), volby.zbytek.begin(), volby.zbytek.end());
    jenTagovat = !slozkyKTagovani.empty() || !volby.pisnicka.empty();

    // config je potreba na stahovani, nebo kdyz se taguje bez -d (vychozi slozka)
    if (!jenTagovat || slozkyKTagovani.empty()) {
        auto text = prectiSoubor(CONFIG_JSON);
        if (!text) {
            vypisChybu("Chybí {}. Popis konfigurace: {} --help", CONFIG_JSON.string(), PROGRAM);
            return 1;
        }
        config = json::parse(*text, nullptr, false);
        if (!config.is_object()) {
            vypisChybu("{} není platný JSON objekt.", CONFIG_JSON.string());
            return 1;
        }
        auto slozka = jakoText(polozka(config, "slozka_mp3"));
        if (!slozka) {
            vypisChybu("V {} chybí slozka_mp3.", CONFIG_JSON.string());
            return 1;
        }
        SLOZKA_MP3 = rozbalCestu(*slozka);
    }

    if (jenTagovat) {
        // jen tagovani (-d / -s), nic se nestahuje
        std::vector<fs::path> slozky;
        if (slozkyKTagovani.empty()) {
            slozky.push_back(SLOZKA_MP3);
        } else {
            for (const auto& s : slozkyKTagovani) slozky.push_back(rozbalCestu(s));
        }
        for (const auto& slozka : slozky) {
            if (!jeSlozka(slozka)) {
                vypisChybu("Složka neexistuje: {}", slozka.string());
                return 1;
            }
        }
        std::vector<std::string> vsechny;
        for (const auto& slozka : slozky) {
            auto mp3 = mp3VeSlozce(slozka);
            vsechny.insert(vsechny.end(), mp3.begin(), mp3.end());
        }
        std::sort(vsechny.begin(), vsechny.end());
        // udaje ze Spotify z posledniho behu, pokud jsou
        std::vector<Pisnicka> playlist;
        json ulozeny = nactiJson(PLAYLIST_JSON, json::array());
        if (ulozeny.is_array()) {
            for (const json& p : ulozeny) playlist.push_back(pisnickaZJson(p));
        }

        if (!volby.pisnicka.empty()) {
            auto vybrane = vyberPisnicky(volby.pisnicka, vsechny);
            otagujSoubory(vybrane, playlist, true, false);
        } else {
            if (vsechny.empty()) {
                vypisChybu("Žádné mp3 soubory.");
                return 1;
            }
            otagujSoubory(vsechny, playlist, volby.force, true);
        }
    } else {
        fs::create_directories(SLOZKA_MP3);

        std::vector<Pisnicka> playlist = nactiPlaylist();
        if (!zkouska) {
            json data = json::array();
            for (const auto& p : playlist) data.push_back(pisnickaDoJson(p));
            ulozJson(PLAYLIST_JSON, data);
        }

        std::set<std::string> soubory;
        for (const auto& polozka : fs::directory_iterator(SLOZKA_MP3)) {
            soubory.insert(polozka.path().filename().string());
        }
        // pro --otagovat: seznam pred stahovanim, prave stazene se otaguji hned
        std::vector<std::string> predStazenim;
        if (volby.otagovat) {
            predStazenim = mp3VeSlozce(SLOZKA_MP3);
            std::sort(predStazenim.begin(), predStazenim.end());
        }
        vypis("Playlist má {} písniček, ve složce je {} souborů", playlist.size(), soubory.size());

        std::vector<Pisnicka> nove;
        for (const auto& p : playlist) {
            if (!soubory.contains(jmenoSouboru(p) + ".mp3")) nove.push_back(p);
        }
        vypis("Nových písniček: {}", nove.size());

        for (size_t i = 0; i < nove.size(); i++) {
            const Pisnicka& p = nove[i];
            std::string poradi = std::format("[{}/{}]", i + 1, nove.size());
            if (zkouska) {
                vypis("{} Stáhl bych: {} - {}", poradi, p.interpret, p.nazev);
                continue;
            }
            vypis("{} Stahuji: {} - {}", poradi, p.interpret, p.nazev);
            auto soubor = stahniMp3(p);
            if (soubor) {
                statistika.stazeno++;
                otagujBezpecne(SLOZKA_MP3 / *soubor, p);
            }
        }

        if (volby.otagovat) {
            otagujSoubory(predStazenim, playlist, volby.force, true);
        }
    }

    vypisShrnuti();
    return 0;
}

int main(int argc, char** argv) {
    // prepinace se kontroluji jeste pred nactenim configu, aby --help fungoval vzdy
    Volby volby;
    try {
        volby = nactiPrepinace(argc, argv);
    } catch (const std::exception& e) {
        vypisChybu("Špatné přepínače: {}\nNápověda: {} --help", e.what(), PROGRAM);
        return 1;
    }

    if (volby.help) {
        std::cout << napoveda() << '\n';
        return 0;
    }

    if (volby.minScore) {
        auto x = naCislo(*volby.minScore);
        if (!x || !(*x >= 0 && *x <= 1)) {
            vypisChybu("--min-score musí být číslo od 0 do 1, ne \"{}\"", *volby.minScore);
            return 1;
        }
        MIN_SHODA = *x;
    }

    zkouska = volby.dryRun;
    ulozitCover = volby.saveCover;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    int kod;
    try {
        kod = hlavni(volby);
    } catch (const std::exception& e) {
        vypisChybu("Chyba: {}", e.what());
        kod = 1;
    }
    curl_global_cleanup();
    return kod;
}
