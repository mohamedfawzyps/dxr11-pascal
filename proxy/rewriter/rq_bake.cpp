#include "rq_bake.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>

namespace rq {

const char* const kBakedHitFunctions[4] = {
    "AnyHit", "ClosestHit", "Isect", "ClosestHitProc"
};

std::string BakedName(const std::string& name, size_t k) {
    return name + "_" + std::to_string(k);
}

namespace {

struct Fail { std::string why; };

// Split on '\n' exactly as Python's str.split('\n') does, so a trailing
// newline leaves a trailing empty line. Not SplitLines: that one drops '\r',
// and this has to reproduce the Python byte for byte.
std::vector<std::string> Split(const std::string& t) {
    std::vector<std::string> out;
    size_t s = 0;
    for (;;) {
        const size_t e = t.find('\n', s);
        if (e == std::string::npos) { out.push_back(t.substr(s)); break; }
        out.push_back(t.substr(s, e - s));
        s = e + 1;
    }
    return out;
}

std::string Join(const std::vector<std::string>& ls, const char* sep) {
    std::string out;
    for (size_t i = 0; i < ls.size(); ++i) {
        if (i) out += sep;
        out += ls[i];
    }
    return out;
}

bool StartsWith(const std::string& s, const std::string& p) {
    return s.compare(0, p.size(), p) == 0;
}

bool EndsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string Strip(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> SplitItems(const std::string& s) {
    std::vector<std::string> out;
    size_t b = 0;
    for (;;) {
        const size_t e = s.find(',', b);
        out.push_back(Strip(s.substr(b, e == std::string::npos ? std::string::npos : e - b)));
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return out;
}

std::string EscapeRegex(const std::string& s) {
    static const std::string special = R"RX(\^$.|?*+()[]{})RX";
    std::string out;
    for (char c : s) {
        if (special.find(c) != std::string::npos) out += '\\';
        out += c;
    }
    return out;
}

std::vector<std::string> RecordTags(const std::vector<std::string>& lines) {
    static const std::regex kLoad(R"RX(^\s*%rq\.cbv([\w.]+) = load %rq_record, )RX");
    std::vector<std::string> tags;
    std::smatch m;
    for (const auto& l : lines)
        if (std::regex_search(l, m, kLoad)) tags.push_back(m[1].str());
    return tags;
}

// (first, last) line of `define ... @name(` .. `}`; first == npos when absent.
std::pair<size_t, size_t> FunctionSpan(const std::vector<std::string>& lines,
                                       const std::string& name) {
    const std::string head = "@" + name + "(";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (StartsWith(lines[i], "define ") && lines[i].find(head) != std::string::npos) {
            for (size_t j = i + 1; j < lines.size(); ++j)
                if (lines[j] == "}") return { i, j };
            throw Fail{ "function " + name + " has no closing brace" };
        }
    }
    return { std::string::npos, std::string::npos };
}

std::vector<std::string> BakeBody(const std::vector<std::string>& body,
                                  const std::string& name, size_t k,
                                  const RecordPair& pair,
                                  const std::vector<std::string>& tags) {
    static const std::regex kDef(R"RX(^(\s*)(%[\w.]+) = )RX");
    static const std::regex kExtract(
        R"RX(^(\s*)(%[\w.]+) = extractvalue %dx\.types\.CBufRet\.i32 %rq\.cbr([\w.]+), (\d+)\s*$)RX");
    std::set<std::string> drop;
    const std::set<std::string> tagSet(tags.begin(), tags.end());
    for (const auto& t : tags) {
        drop.insert("%rq.cbv" + t);
        drop.insert("%rq.cbh" + t);
        drop.insert("%rq.cbr" + t);
    }
    std::vector<std::string> out;
    std::smatch m;
    for (const auto& l : body) {
        if (StartsWith(l, "define ")) {
            std::string d = l;
            const std::string from = "@" + name + "(";
            const size_t at = d.find(from);
            d.replace(at, from.size(), "@" + BakedName(name, k) + "(");
            out.push_back(d);
            continue;
        }
        if (std::regex_search(l, m, kDef) && drop.count(m[2].str())) continue;
        if (std::regex_search(l, m, kExtract) && tagSet.count(m[3].str())) {
            const int word = std::stoi(m[4].str());
            if (word != 0 && word != 1)
                throw Fail{ "record read of word " + std::to_string(word) +
                            "; the record has two" };
            const uint32_t v = word == 0 ? pair.first : pair.second;
            out.push_back(m[1].str() + m[2].str() + " = add i32 0, " + std::to_string(v));
            continue;
        }
        out.push_back(l);
    }
    return out;
}

// Drop a declare the baking made dead. The lowered module validated, so any
// unused declaration now is a record read's, and an unused declaration is
// itself a validation error.
std::vector<std::string> PruneDeclares(const std::vector<std::string>& lines) {
    static const std::regex kName(R"RX(@([\w.$"]+)\()RX");
    std::vector<std::string> nonDecl;
    for (const auto& l : lines)
        if (!StartsWith(l, "declare ")) nonDecl.push_back(l);
    const std::string body = Join(nonDecl, "\n");
    std::vector<std::string> out;
    std::smatch m;
    for (const auto& l : lines) {
        if (StartsWith(l, "declare ") && std::regex_search(l, m, kName) &&
            body.find("@" + m[1].str() + "(") == std::string::npos)
            continue;
        out.push_back(l);
    }
    return out;
}

std::vector<std::string> DropRecordResource(const std::vector<std::string>& lines) {
    static const std::regex kRec(R"RX(^!(\d+) = !\{i32 \d+, %rq_record\* @rq_record, )RX");
    static const std::regex kResTuple(R"RX(^!dx\.resources = !\{(!\d+)\}$)RX");
    static const std::regex kList(R"RX(^(!\d+) = !\{((?:!\d+)(?:, !\d+)*)\}$)RX");
    std::smatch m;
    std::string rec, resTuple;
    for (const auto& l : lines)
        if (std::regex_search(l, m, kRec)) rec = "!" + m[1].str();
    if (rec.empty()) throw Fail{ "no resource record for @rq_record" };
    for (const auto& l : lines)
        if (std::regex_search(l, m, kResTuple)) resTuple = m[1].str();

    std::vector<std::string> out;
    std::string emptied;
    int lists = 0;
    for (const auto& l : lines) {
        if (StartsWith(l, rec + " = ")) continue;
        if (std::regex_search(l, m, kList) && m[1].str() != resTuple) {
            std::vector<std::string> items = SplitItems(m[2].str());
            if (std::find(items.begin(), items.end(), rec) != items.end()) {
                ++lists;
                items.erase(std::remove(items.begin(), items.end(), rec), items.end());
                if (items.empty()) { emptied = m[1].str(); continue; }
                out.push_back(m[1].str() + " = !{" + Join(items, ", ") + "}");
                continue;
            }
        }
        out.push_back(l);
    }
    if (lists != 1)
        throw Fail{ "the record cbuffer is in " + std::to_string(lists) +
                    " resource lists, expected 1" };

    if (!emptied.empty()) {
        // The record was the only cbuffer, so the class becomes null, which is
        // how DXC writes a resource class a module does not have.
        const std::string head = resTuple + " = !{";
        for (auto& l : out) {
            if (!resTuple.empty() && StartsWith(l, head) && EndsWith(l, "}")) {
                std::vector<std::string> items =
                    SplitItems(l.substr(head.size(), l.size() - head.size() - 1));
                for (auto& it : items) if (it == emptied) it = "null";
                l = head + Join(items, ", ") + "}";
            }
        }
    }
    return out;
}

std::string DoBake(const std::string& text, const std::vector<RecordPair>& pairs) {
    if (pairs.empty()) throw Fail{ "no pairs to bake" };
    if (pairs.size() > kMaxBakedPairs)
        throw Fail{ std::to_string(pairs.size()) +
                    " distinct (geometry, contribution) pairs; the ceiling is " +
                    std::to_string(kMaxBakedPairs) };
    for (const auto& p : pairs)
        if (p.first >= 0x80000000u || p.second >= 0x80000000u)
            throw Fail{ "pair (" + std::to_string(p.first) + ", " +
                        std::to_string(p.second) + ") does not fit an i32 immediate" };
    if (std::set<RecordPair>(pairs.begin(), pairs.end()).size() != pairs.size())
        throw Fail{ "pairs must be distinct" };

    std::vector<std::string> lines = Split(text);
    const std::vector<std::string> tags = RecordTags(lines);
    if (tags.empty()) throw Fail{ "the module reads no record constants; nothing to bake" };

    // 1. the hit shaders, one copy per pair, in place of the original
    std::vector<std::string> present;
    for (const char* fn : kBakedHitFunctions) {
        const std::string name = fn;
        const auto span = FunctionSpan(lines, name);
        if (span.first == std::string::npos) continue;
        present.push_back(name);
        const std::vector<std::string> body(lines.begin() + span.first,
                                            lines.begin() + span.second + 1);
        std::vector<std::string> copies;
        for (size_t k = 0; k < pairs.size(); ++k) {
            if (k) copies.push_back("");
            const auto b = BakeBody(body, name, k, pairs[k], tags);
            copies.insert(copies.end(), b.begin(), b.end());
        }
        std::vector<std::string> next(lines.begin(), lines.begin() + span.first);
        next.insert(next.end(), copies.begin(), copies.end());
        next.insert(next.end(), lines.begin() + span.second + 1, lines.end());
        lines.swap(next);
    }
    if (present.empty()) throw Fail{ "the module defines none of the hit shaders" };

    // 2. the record itself: its type, its global, its resource record
    {
        std::vector<std::string> kept;
        for (const auto& l : lines)
            if (!StartsWith(l, "%rq_record = type ") && !StartsWith(l, "@rq_record = "))
                kept.push_back(l);
        lines.swap(kept);
    }
    lines = DropRecordResource(lines);
    lines = PruneDeclares(lines);

    // 3. entry points and type annotations, one per copy
    static const std::regex kId(R"RX(^!(\d+) = )RX");
    static const std::regex kTa(R"RX(^!dx\.typeAnnotations = !\{(!\d+))RX");
    std::smatch m;
    long maxId = -1;
    std::string taLine;
    for (const auto& l : lines) {
        if (std::regex_search(l, m, kId)) maxId = std::max(maxId, std::stol(m[1].str()));
        if (std::regex_search(l, m, kTa)) taLine = m[1].str();
    }
    long nxt = maxId + 1;
    std::vector<std::string> added;
    std::map<std::string, std::vector<std::string>> epExtra;
    for (const auto& name : present) {
        const std::regex kEntry("^(!\\d+) = !\\{(.+)\\* @" + EscapeRegex(name) +
                                ", !\"" + EscapeRegex(name) + "\", (.*)\\}$");
        bool found = false;
        for (auto& l : lines) {
            if (!std::regex_search(l, m, kEntry)) continue;
            found = true;
            const std::string node = m[1].str(), ty = m[2].str(), rest = m[3].str();
            const std::string n0 = BakedName(name, 0);
            l = node + " = !{" + ty + "* @" + n0 + ", !\"" + n0 + "\", " + rest + "}";
            std::vector<std::string> extra;
            for (size_t k = 1; k < pairs.size(); ++k) {
                const std::string nk = BakedName(name, k);
                added.push_back("!" + std::to_string(nxt) + " = !{" + ty + "* @" + nk +
                                ", !\"" + nk + "\", " + rest + "}");
                extra.push_back("!" + std::to_string(nxt));
                ++nxt;
            }
            epExtra[node] = extra;
            break;
        }
        if (!found) throw Fail{ "no entry point for " + name };
    }

    const std::string epHead = "!dx.entryPoints = !{";
    for (auto& l : lines) {
        if (StartsWith(l, epHead)) {
            const std::vector<std::string> items =
                SplitItems(l.substr(epHead.size(), l.size() - epHead.size() - 1));
            std::vector<std::string> grown;
            for (const auto& it : items) {
                grown.push_back(it);
                const auto e = epExtra.find(it);
                if (e != epExtra.end())
                    grown.insert(grown.end(), e->second.begin(), e->second.end());
            }
            l = epHead + Join(grown, ", ") + "}";
        } else if (!taLine.empty() && StartsWith(l, taLine + " = !{")) {
            for (const auto& name : present) {
                const std::regex pat(R"RX((void \([^()]*\)\*) @)RX" + EscapeRegex(name) +
                                     R"RX(, (!\d+))RX");
                std::smatch t;
                if (!std::regex_search(l, t, pat))
                    throw Fail{ "no type annotation for " + name };
                std::vector<std::string> rep;
                for (size_t k = 0; k < pairs.size(); ++k)
                    rep.push_back(t[1].str() + " @" + BakedName(name, k) + ", " + t[2].str());
                l = t.prefix().str() + Join(rep, ", ") + t.suffix().str();
            }
        }
    }

    // appended after the last metadata line, before any trailing blank lines
    size_t end = lines.size();
    while (end && lines[end - 1].empty()) --end;
    lines.insert(lines.begin() + end, added.begin(), added.end());

    const std::string out = Join(lines, "\n");
    for (const auto& l : Split(out)) {
        const size_t b = l.find_first_not_of(" \t\r\n\f\v");
        if (b != std::string::npos && l[b] == ';') continue;
        if (l.find("rq_record") != std::string::npos ||
            l.find("%rq.cbr") != std::string::npos)
            throw Fail{ "a record read survived the baking: " + Strip(l) };
    }
    return out;
}

}  // namespace

BakeResult Bake(const std::string& lowered, const std::vector<RecordPair>& pairs) {
    BakeResult r;
    try {
        r.text = DoBake(lowered, pairs);
        r.ok = true;
    } catch (const Fail& f) {
        r.error = f.why;
    } catch (const std::exception& e) {
        // std::regex and std::stoi can throw; a DLL in someone else's process
        // never lets that escape.
        r.error = std::string("bake failed: ") + e.what();
    }
    return r;
}

}  // namespace rq
