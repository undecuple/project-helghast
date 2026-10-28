// Model of the FIOS identity rules behind the Botzone follow failure (RE pass 165, the RE notes s.48).
// Not the real FIOS: a small model of the three rules read from the 1.14 EBOOT, driven by the sequence the port logged.
//   R1 executor file entry (0xE1EE20 creates it, size from the path stat at the first open; 0xE18C10 reopens): every
//      reopen does open + fstat and compares the size with the entry's; a mismatch closes the fd and sets it to -1
//      ("Error, size of file '%s' after reopen (%lld) doesn't match original size (%lld)").
//   R2 one path stat per path per run (log: 323 stats over 304 paths, 16 opens of one path -> 1 stat): the stat op the
//      cache layer issues to validate a file (0xDF7D74) is answered from the entry once it exists. ASSUMPTION: the code
//      path that answers it was not decompiled, the log is the evidence.
//   R3 cache layer (0xDF62E4, TOP of the stack: cache -> patch layer 0xE0C228 -> 0xB48 -> 0xC08 -> executor) keys records
//      by the path hash and drops them when the stat op's mtime differs from the oldest record ("mod date changed").
// Scenario A = the port today (patched bytes served under the same path, host side). Scenario B = the PS3 / native
// mode: the executor always sees the base file, the patch layer (hash table = the active set's directory listing)
// answers the stat op with the patch's time and size and composes the bytes above the executor.
// Needs no game data and no toolkit. Exit 0 = PASS.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using Bytes = std::vector<uint8_t>;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("[FAIL] "); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct File { Bytes b; int64_t mtime; };
static Bytes make(size_t n, uint8_t seed) { Bytes v(n); for (size_t i = 0; i < n; i++) v[i] = (uint8_t)(seed + i * 7 + (i >> 8)); return v; }

// ---- host side (what cellFs sees) ----
struct Host {
    std::map<std::string, File> sp, mp;          // served version of a guest path, per set
    int set = 0; int pathStats = 0, fstats = 0, opens = 0, closes = 0;
    File& cur(const std::string& p) { auto& m = set ? mp : sp; return m.count(p) ? m[p] : sp[p]; }
};
// ---- FIOS executor (media layer) ----
struct Executor {
    struct Entry { int64_t size, mtime; };
    std::map<std::string, Entry> entries; Host* h; int reopenErrors = 0;
    int open(const std::string& p, const File** fp) {          // returns fd (>0) or -1
        h->opens++; File& f = h->cur(p); h->fstats++;
        auto it = entries.find(p);
        if (it == entries.end()) { h->pathStats++; entries[p] = { (int64_t)f.b.size(), f.mtime }; *fp = &f; return 5; }
        if ((int64_t)f.b.size() != it->second.size) { reopenErrors++; h->closes++; *fp = nullptr; return -1; }   // R1
        *fp = &f; return 5;
    }
    int64_t statMtime(const std::string& p) {                  // the stat op below the cache layer (R2)
        auto it = entries.find(p);
        if (it != entries.end()) return it->second.mtime;
        h->pathStats++; File& f = h->cur(p); entries[p] = { (int64_t)f.b.size(), f.mtime }; return f.mtime;
    }
};
// ---- patch layer (0xE0C228): hash table of the active set, composes patch + base above the executor ----
struct PatchLayer {
    std::map<std::string, File> patched;    // active set's table: path -> composed view (patch time, composed bytes)
    bool has(const std::string& p) const { return patched.count(p) != 0; }
};
// ---- cache layer (top) ----
struct Cache {
    struct Rec { int64_t mtime; Bytes data; };
    std::map<std::string, Rec> recs; Executor* ex; PatchLayer* pl = nullptr; int flushes = 0;
    int64_t statOp(const std::string& p) { return pl && pl->has(p) ? pl->patched[p].mtime : ex->statMtime(p); }
    // open + read the whole file as the lump loader does; returns false on a failed refill
    bool readAll(const std::string& p, Bytes& out) {
        auto it = recs.find(p);
        if (it != recs.end() && statOp(p) != it->second.mtime) { recs.erase(it); flushes++; it = recs.end(); }   // R3
        if (it != recs.end()) { out = it->second.data; return true; }       // served from cache.dat
        if (pl && pl->has(p)) { const File* f; if (ex->open(p, &f) < 0) return false; out = pl->patched[p].b; recs[p] = { pl->patched[p].mtime, out }; return true; }
        const File* f = nullptr; int fd = ex->open(p, &f);
        if (fd < 0) return false;
        out = f->b; recs[p] = { f->mtime, out }; return true;
    }
};

int main() {
    const std::string P = "/dev_bdvd/PS3_GAME/USRDIR/LocalCachePS3/lumps/defaultshared.core";
    File disc{ make(30950, 1), 1000 };                              // SP: the disc entry (MP-only patch)
    File mpv{ make(30950, 1), 2000 }; mpv.b.resize(31010); for (size_t i = 30950; i < 31010; i++) mpv.b[i] = (uint8_t)(0xA0 + i);   // +60 bytes, newer file

    // A: the port today
    { Host h; h.sp[P] = disc; h.mp[P] = mpv; Executor ex{ {}, &h }; Cache c; c.ex = &ex;
      Bytes a, b; bool ok1 = c.readAll(P, a);
      printf("A boot: ok=%d size=%zu pathStats=%d fstats=%d\n", ok1, a.size(), h.pathStats, h.fstats);
      CHECK(ok1 && a == disc.b, "A: boot read must return the disc bytes");
      h.set = 1;                                                    // the game selects the MP set; the overlay now serves mpv under P
      bool ok2 = c.readAll(P, b);
      printf("A after switch: ok=%d size=%zu reopenErrors=%d flushes=%d pathStats=%d\n", ok2, b.size(), ex.reopenErrors, c.flushes, h.pathStats);
      // the model must reproduce the log: no second path stat, the stale disc bytes come back (the real run failed in the refill)
      CHECK(h.pathStats == 1, "A: model must show one path stat per path (the log's rule), got %d", h.pathStats);
      CHECK(!(ok2 && b == mpv.b), "A: model must reproduce the bug (stale view after the switch)");
      Bytes c2; File* dummy = nullptr; (void)dummy;
      bool ok3 = ex.open(P, (const File**)&dummy) >= 0;             // a direct reopen shows the size rule
      CHECK(!ok3 && ex.reopenErrors >= 1, "A: reopen with a changed size must hit the executor's size error"); }

    // B: native mode: executor and entry always see the base file; the patch layer carries the set
    { Host h; h.sp[P] = disc; h.mp[P] = disc; Executor ex{ {}, &h }; PatchLayer pl; Cache c; c.ex = &ex; c.pl = &pl;
      Bytes a, b, d;                                               // SP: no patch for this path
      bool ok1 = c.readAll(P, a); CHECK(ok1 && a == disc.b, "B: SP read must return the base bytes");
      h.set = 1; pl.patched[P] = mpv;                              // SelectPatchSetMP: the layer re-lists the directory and finds the hash
      bool ok2 = c.readAll(P, b);
      printf("B after switch: ok=%d size=%zu reopenErrors=%d flushes=%d pathStats=%d\n", ok2, b.size(), ex.reopenErrors, c.flushes, h.pathStats);
      CHECK(ok2 && b == mpv.b, "B: MP read must return the MP bytes (size %zu)", mpv.b.size());
      CHECK(ex.reopenErrors == 0, "B: the executor must never see a size change (got %d errors)", ex.reopenErrors);
      CHECK(c.flushes == 1, "B: the cache must drop the SP record once (mod date changed), got %d", c.flushes);
      h.set = 0; pl.patched.erase(P);                              // back to SP (menu): the layer no longer patches it
      bool ok3 = c.readAll(P, d); CHECK(ok3 && d == disc.b, "B: back in SP the base bytes must return");
      CHECK(h.pathStats == 1, "B: one executor path stat for the base path, got %d", h.pathStats); }

    if (g_fail) { printf("FAIL (%d failed)\n", g_fail); return 1; }
    printf("PASS (0 failed)\n"); return 0;
}
