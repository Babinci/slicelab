#include <rubberband/RubberBandStretcher.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cerrno>

extern "C" void* __libc_malloc(size_t);
extern "C" void* __libc_memalign(size_t, size_t);
extern "C" void* __libc_calloc(size_t, size_t);
extern "C" void* __libc_realloc(void*, size_t);
extern "C" void __libc_free(void*);

static bool watching = false;
static unsigned allocations = 0, frees = 0;
static unsigned failures = 0;
extern "C" void* malloc(size_t n) { if (watching) ++allocations; return __libc_malloc(n); }
extern "C" void* calloc(size_t n, size_t s) { if (watching) ++allocations; return __libc_calloc(n, s); }
extern "C" void* realloc(void* p, size_t n) { if (watching) ++allocations; return __libc_realloc(p, n); }
extern "C" void free(void* p) { if (watching && p) ++frees; __libc_free(p); }

extern "C" int posix_memalign(void** ptr, size_t alignment, size_t size) { if(watching) ++allocations; auto* p=__libc_memalign(alignment,size); if(!p) return ENOMEM; *ptr=p; return 0; }
extern "C" void* aligned_alloc(size_t alignment, size_t size) { if(watching) ++allocations; return __libc_memalign(alignment,size); }

using R = RubberBand::RubberBandStretcher;
constexpr auto options = R::OptionProcessRealTime | R::OptionThreadingNever | R::OptionWindowShort | R::OptionPitchHighConsistency | R::OptionEngineFiner | R::OptionChannelsTogether;
constexpr std::array<double, 6> points { .25, .5, .8, 1.0, 2.0, 4.0 };

void feed(R& r) {
    std::array<float, 1024> left {}, right {};
    const float* input[] = { left.data(), right.data() };
    float* output[] = { left.data(), right.data() };
    for (int i = 0; i < 8; ++i) {
        const auto count = std::clamp<size_t>(r.getSamplesRequired(), 1, 1024);
        r.process(input, count, false);
        while (r.available() > 0) r.retrieve(output, (size_t)std::min(1024, r.available()));
    }
}

void measured(R& r, const char* label, double ratio, double pitch, bool reset, bool process) {
    allocations = frees = 0;
    watching = true;
    if (reset) r.reset();
    r.setTimeRatio(ratio);
    r.setPitchScale(pitch);
    if (process) feed(r);
    watching = false;
    if (allocations || frees) ++failures;
    if (allocations || frees || label[0] == 'c')
        std::printf("%s ratio=%g pitch=%g reset=%d process=%d allocations=%u frees=%u\n",
                    label, ratio, pitch, reset, process, allocations, frees);
}

int main() {
    allocations=frees=0; watching=true; auto* positive=malloc(17); void* aligned=nullptr; const int alignedStatus=posix_memalign(&aligned,32,64); free(positive); free(aligned); watching=false; std::printf("positive-control allocations=%u frees=%u\n",allocations,frees); if(allocations!=2 || frees!=2 || alignedStatus!=0) return 1;
    std::puts("Rubber Band allocation probe. Flags match SamplerEngine prepare. Sample rate 44100, stereo, max process size 1024.");
    for (double pitch : points) {
        R r(44100, 2, options);
        r.setMaxProcessSize(1024);
        measured(r, "cold", 1.0, pitch, true, false);
    }
    {
        R r(44100, 2, options);
        r.setMaxProcessSize(1024);
        for (double pitch : points) { r.reset(); r.setPitchScale(pitch); feed(r); }
        std::puts("After one pitch sweep, report every observed allocation or free in the same sweep and reverse sweep.");
        for (double pitch : points) measured(r, "warm-forward", 1.0, pitch, true, true);
        for (auto it = points.rbegin(); it != points.rend(); ++it) measured(r, "warm-reverse", 1.0, *it, true, true);
    }
    for (bool reset : { false, true }) {
        R r(44100, 2, options, 4.0, .25);
        r.setMaxProcessSize(1024);
        feed(r);
        std::printf("Constructed at ratio=4 pitch=.25. Grid reset=%d. Report every observed allocation or free.\n", reset);
        for (double ratio : points) for (double pitch : points)
            measured(r, "extreme-constructed-grid", ratio, pitch, reset, true);
    }
    std::printf("Allocation grid failures: %u\n",failures);
    return failures==0 ? 0 : 1;
}
