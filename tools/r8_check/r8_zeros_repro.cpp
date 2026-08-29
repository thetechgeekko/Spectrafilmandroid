/*
 * Spektrafilm for Android — host reproduction of the R8 param-zeroing defect. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * PURE LOCAL TOOL. Not a parity gate, not in CI.
 *
 * WHAT IT SHOWS. R8 removed kotlin.Triple/Pair getters from the release dex, so
 * every Triple/Pair-valued engine param marshalled as 0.0 (see
 * tools/r8_check/check_release_dex.sh). This zeroes exactly those params on the
 * host and renders both routes, which does two things the device could not:
 *
 *   1. It reproduces the flat render EXACTLY. The slide route goes constant at
 *      8-bit [220 134 106]; the device reported [220 135 106] from a different
 *      scene through a different JPEG path. One code apart in green.
 *
 *   2. It answers "why does the print route survive the same zeros?" — IT DOES
 *      NOT. Print is not flat, but its spread collapses 0.830 -> 0.036 and its
 *      mean 0.477 -> 0.054: a near-black, nearly-flat frame. It was only judged
 *      healthy because it still compressed to megabytes rather than kilobytes,
 *      and a non-constant image passes a file-size check. BOTH routes were
 *      broken in every release build; only one was broken visibly.
 *
 * Measured output with the zeros applied (512x512, portra_400):
 *
 *   slide  CORRECT     spread 0.470 0.385 0.383   mean 0.520 0.300 0.220
 *   slide  R8-ZEROED   spread 0.000 0.000 0.000   mean 0.863 0.525 0.416  FLAT
 *   print  CORRECT     spread 0.830 0.830 0.811   mean 0.477 0.361 0.345
 *   print  R8-ZEROED   spread 0.036 0.035 0.031   mean 0.054 0.057 0.069
 *
 * Build (run from engine/spektra-core/src/main/cpp):
 *   g++ -std=c++17 -O2 -pthread -I. ../../../../../tools/r8_check/r8_zeros_repro.cpp \
 *     spektra.cpp gpu/ *.cpp kernels/ *.cpp io/ *.cpp model/ *.cpp \
 *     profiles/ *.cpp runtime/ *.cpp runtime/stages/ *.cpp -o /tmp/r8repro
 *   /tmp/r8repro ../assets/spektra
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "spektra.h"

static std::vector<float> make(int w, int h) {
    std::vector<float> v((size_t)w*h*3);
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        double fx=(double)x/(w-1), fy=(double)y/(h-1);
        double b = 0.184*std::pow(2.0,-4.0+7.0*fx)*(0.85+0.3*fy);
        size_t i=((size_t)y*w+x)*3; v[i]=(float)b; v[i+1]=(float)(b*(0.55+0.4*fy)); v[i+2]=(float)(b*(0.3+0.55*fx));
    }
    return v;
}

// Zero every Triple/Pair-valued param, exactly as the shrunk dex did.
static void apply_r8_zeros(spk_params* p) {
    for (int i=0;i<3;++i) {
        p->grain_particle_scale[i]=0.f; p->grain_particle_scale_layers[i]=0.f;
        p->grain_density_min[i]=0.f;    p->grain_uniformity[i]=0.f;
        p->halation_scatter_core_um[i]=0.f; p->halation_scatter_tail_um[i]=0.f;
        p->halation_scatter_tail_weight[i]=0.f; p->halation_strength[i]=0.f;
        p->camera_filter_uv[i]=0.f; p->camera_filter_ir[i]=0.f;
    }
    p->grain_micro_structure[0]=0.f; p->grain_micro_structure[1]=0.f;
}

static void run(const char* dir, int scan_film, int r8, const char* label) {
    spk_engine* e=nullptr;
    if (spk_engine_create(dir,&e)!=SPK_OK){ std::fprintf(stderr,"engine fail\n"); return; }
    spk_params p{}; p.film_profile="kodak_portra_400"; p.print_profile="kodak_portra_endura";
    spk_default_params(&p);
    p.auto_exposure=1; p.scan_film=scan_film; p.grain_active=1;
    p.halation_active=1; p.dir_couplers_active=1;
    p.output_color_space=SPK_CS_SRGB; p.output_cctf_encoding=1;
    p.rgb_to_raw_method=SPK_RGB2RAW_HANATOS2025; p.preview_max_size=0;
    if (r8) apply_r8_zeros(&p);
    int w=512,h=512;
    std::vector<float> sc=make(w,h);
    spk_image in{sc.data(),w,h,SPK_CS_PROPHOTO}, out{};
    spk_status st=spk_simulate(e,&in,&p,&out);
    if (st!=SPK_OK){ std::printf("%-34s SIMULATE FAILED (%s)\n",label,spk_status_str(st)); spk_engine_destroy(e); return; }
    double mn[3]={1e300,1e300,1e300},mx[3]={-1e300,-1e300,-1e300},sum[3]={0,0,0};
    size_t n=(size_t)out.width*out.height, nn=0;
    for(size_t i=0;i<n;++i) for(int c=0;c<3;++c){ double v=out.data[i*3+c];
        if(std::isnan(v)){++nn;continue;} mn[c]=std::min(mn[c],v); mx[c]=std::max(mx[c],v); sum[c]+=v; }
    bool flat=(mx[0]-mn[0]<1e-6)&&(mx[1]-mn[1]<1e-6)&&(mx[2]-mn[2]<1e-6);
    std::printf("%-34s spread %.6f %.6f %.6f  mean %.3f %.3f %.3f  nan=%zu %s\n",
        label, mx[0]-mn[0],mx[1]-mn[1],mx[2]-mn[2], sum[0]/n,sum[1]/n,sum[2]/n, nn,
        flat?"  <<<< FLAT CONSTANT":"");
    if (flat) std::printf("%-34s   constant value = [%.4f %.4f %.4f] -> 8-bit [%d %d %d]\n",
        "", mn[0],mn[1],mn[2], (int)std::lround(mn[0]*255),(int)std::lround(mn[1]*255),(int)std::lround(mn[2]*255));
    spk_image_free(&out); spk_engine_destroy(e);
}

int main(int argc,char**argv){
    const char* dir=argc>1?argv[1]:".";
    run(dir,1,0,"slide  CORRECT params");
    run(dir,1,1,"slide  R8-ZEROED params");
    run(dir,0,0,"print  CORRECT params");
    run(dir,0,1,"print  R8-ZEROED params");
    return 0;
}
