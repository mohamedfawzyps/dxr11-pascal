// CLI for the C++ rewriter port, so it can be held against the Python original.
//
//   dxrw lower <in.ll> <out.ll>        lower a RayQuery module
//   dxrw analyze <in.ll>               print what the analysis found
//   dxrw rewrite <in.dxil> <out.dxil> [g:c,...]
//                                      the WHOLE path a proxy would take:
//                                      container in, signed container out,
//                                      record constants baked for the given
//                                      (geometry, contribution) pairs, 0:0 by
//                                      default, exactly as at pipeline creation
//   dxrw bake <lowered.ll> <out.ll> <g:c,...>
//                                      bake record constants into hit shader
//                                      copies, see proxy/rewriter/rq_bake.h
//
// The port's correctness bar is BYTE-IDENTICAL output to
// phase5/rewriter/dxrewrite.py on every case in the regression. That is a far
// stronger oracle than "it renders correctly", and it is why this exists.
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../proxy/rewriter/ll_model.h"
#include "../proxy/rewriter/rq_analyze.h"
#include "../proxy/rewriter/rq_lower.h"
#include "../proxy/rewriter/rq_bake.h"
#include "../proxy/rewriter/dxc_host.h"

// "g:c,g:c" -> pairs. False on anything malformed, which the Python rejects
// too, so the two refuse the same inputs.
static bool ParsePairs(const std::string& s, std::vector<rq::RecordPair>* out) {
    out->clear();
    size_t b = 0;
    for (;;) {
        const size_t e = s.find(',', b);
        const std::string item = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
        const size_t colon = item.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= item.size()) return false;
        char* end = nullptr;
        const unsigned long g = std::strtoul(item.c_str(), &end, 10);
        if (end != item.c_str() + colon) return false;
        const unsigned long c = std::strtoul(item.c_str() + colon + 1, &end, 10);
        if (*end != '\0') return false;
        out->push_back({ static_cast<uint32_t>(g), static_cast<uint32_t>(c) });
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return true;
}

static bool ReadAll(const char* path, std::string& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    size_t got = out.empty() ? 0 : std::fread(&out[0], 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

static bool WriteAll(const char* path, const std::string& s) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    size_t put = std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    return put == s.size();
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "analyze") {
        std::string raw;
        if (!ReadAll(argv[2], raw)) { std::printf("cannot read %s\n", argv[2]); return 1; }
        llm::Module m(llm::Normalize(raw));
        auto a = rq::Analyze(m);
        std::printf("\n-- %s --\n", argv[2]);
        if (!a.ok) { std::printf("   UNSUPPORTED            : %s\n", a.error.c_str()); return 2; }
        const auto& q = a.query;
        std::printf("   entry function         : %s\n", q.fn->name.c_str());
        std::printf("   query handle           : %s\n", q.handle.c_str());
        std::printf("   combined ray flags     : %d (%s)\n", q.RayFlags(),
                    rq::FlagNames(q.RayFlags()).c_str());
        std::printf("   Proceed calls          : %zu\n", q.proceeds.size());
        if (q.hasLoop)
            std::printf("   rotated Proceed loop   : header %s, latch %s\n",
                        q.loop.header.c_str(), q.loop.latch.c_str());
        else
            std::printf("   rotated Proceed loop   : none\n");
        std::printf("   PATTERN                : %d, %s\n", q.patternNum,
                    q.patternDesc.c_str());
        return 0;
    }
    if (argc >= 4 && std::string(argv[1]) == "lower") {
        std::string raw;
        if (!ReadAll(argv[2], raw)) { std::printf("cannot read %s\n", argv[2]); return 1; }
        llm::Module m(llm::Normalize(raw));
        auto a = rq::Analyze(m);
        if (!a.ok) { std::fprintf(stderr, "REFUSED: %s\n", a.error.c_str()); return 2; }
        auto l = rq::Lower(m, a.query);
        if (!l.ok) { std::fprintf(stderr, "REFUSED: %s\n", l.error.c_str()); return 2; }
        if (!WriteAll(argv[3], l.text)) { std::printf("cannot write %s\n", argv[3]); return 1; }
        std::printf("lowered %s -> %s (pattern %d, %s)\n", argv[2], argv[3],
                    a.query.patternNum, a.query.patternDesc.c_str());
        return 0;
    }
    if (argc >= 4 && std::string(argv[1]) == "rewrite") {
        // The path the proxy will take: a container arrives, a signed
        // container comes back, with DXC doing the text conversion at both
        // ends and nothing touching the filesystem in between.
        std::string raw;
        if (!ReadAll(argv[2], raw)) { std::printf("cannot read %s\n", argv[2]); return 1; }
        std::string err;
        if (!dxch::Available(&err)) { std::printf("DXC unavailable: %s\n", err.c_str()); return 1; }

        struct Ctx { int pattern = 0; std::string desc; std::vector<rq::RecordPair> pairs; };
        Ctx ctx;
        ctx.pairs = { { 0u, 0u } };
        if (argc >= 5 && !ParsePairs(argv[4], &ctx.pairs)) {
            std::printf("bad pair list %s, expected g:c,g:c,...\n", argv[4]);
            return 1;
        }
        auto xform = [](const std::string& in, std::string* out, std::string* why,
                        void* c) -> bool {
            Ctx* x = static_cast<Ctx*>(c);
            llm::Module m(llm::Normalize(in));
            auto a = rq::Analyze(m);
            if (!a.ok) { *why = a.error; return false; }
            auto l = rq::Lower(m, a.query);
            if (!l.ok) { *why = l.error; return false; }
            x->pattern = a.query.patternNum;
            x->desc = a.query.patternDesc;
            *out = l.text;
            // The proxy never emits a record READ: see rq_bake.h.
            if (a.query.needsRecordConstants) {
                auto b = rq::Bake(l.text, x->pairs);
                if (!b.ok) { *why = b.error; return false; }
                *out = b.text;
            }
            return true;
        };
        std::vector<uint8_t> outBytes;
        if (!dxch::RewriteContainer(raw.data(), raw.size(), xform, &ctx, &outBytes, &err)) {
            std::fprintf(stderr, "REFUSED: %s\n", err.c_str());
            return 2;
        }
        std::string blob(reinterpret_cast<const char*>(outBytes.data()), outBytes.size());
        if (!WriteAll(argv[3], blob)) { std::printf("cannot write %s\n", argv[3]); return 1; }
        std::printf("rewrote %s -> %s (%zu -> %zu bytes, pattern %d, %s)\n",
                    argv[2], argv[3], raw.size(), outBytes.size(), ctx.pattern,
                    ctx.desc.c_str());
        return 0;
    }
    if (argc >= 5 && std::string(argv[1]) == "bake") {
        std::string raw;
        if (!ReadAll(argv[2], raw)) { std::printf("cannot read %s\n", argv[2]); return 1; }
        std::vector<rq::RecordPair> pairs;
        if (!ParsePairs(argv[4], &pairs)) {
            std::fprintf(stderr, "REFUSED: bad pair list %s\n", argv[4]);
            return 2;
        }
        auto b = rq::Bake(raw, pairs);
        if (!b.ok) { std::fprintf(stderr, "REFUSED: %s\n", b.error.c_str()); return 2; }
        if (!WriteAll(argv[3], b.text)) { std::printf("cannot write %s\n", argv[3]); return 1; }
        std::printf("baked %s -> %s (%s)\n", argv[2], argv[3], argv[4]);
        return 0;
    }
    std::printf("usage:\n  dxrw lower <in.ll> <out.ll>\n  dxrw analyze <in.ll>\n"
                "  dxrw rewrite <in.dxil> <out.dxil> [g:c,...]\n"
                "  dxrw bake <lowered.ll> <out.ll> <g:c,...>\n");
    return 1;
}
