// CLI for the C++ rewriter port, so it can be held against the Python original.
//
//   dxrw lower <in.ll> <out.ll>        lower a RayQuery module
//   dxrw analyze <in.ll>               print what the analysis found
//   dxrw rewrite <in.dxil> <out.dxil>  the WHOLE path a proxy would take:
//                                      container in, signed container out
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
#include "../proxy/rewriter/dxc_host.h"

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

        struct Ctx { int pattern = 0; std::string desc; };
        Ctx ctx;
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
    std::printf("usage:\n  dxrw lower <in.ll> <out.ll>\n  dxrw analyze <in.ll>\n"
                "  dxrw rewrite <in.dxil> <out.dxil>\n");
    return 1;
}
