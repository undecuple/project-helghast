static unsigned checks, fails;
static void check(const char* name,int ok) {
    checks++; if(!ok) { fails++;printf("FAIL %s\n",name); }
}
static void make_resource(ID3D12Resource* r,unsigned w,unsigned h,int state) {
    memset(r,0,sizeof *r); r->lpVtbl=&resource_vt; r->state=state;
    r->desc.Width=w;r->desc.Height=h;r->desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    r->desc.DepthOrArraySize=r->desc.MipLevels=r->desc.SampleDesc.Count=1;
    r->desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    r->desc.Flags=(state==D3D12_RESOURCE_STATE_PRESENT||state==D3D12_RESOURCE_STATE_RENDER_TARGET)?D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET:0;
}
static void reset(unsigned bw,unsigned bh) {
    g.width=s_state.width=1280;g.height=s_state.height=720;
    g_dm_bb_w=bw;g_dm_bb_h=bh;client=(RECT){0,0,(int)bw,(int)bh};s_state.hwnd=&client;
    make_resource(&src_resource,1280,720,D3D12_RESOURCE_STATE_RENDER_TARGET);
    make_resource(&gamma_resource,1280,720,D3D12_RESOURCE_STATE_COPY_SOURCE);
    make_resource(&bb_resource,bw,bh,D3D12_RESOURCE_STATE_PRESENT);
    make_resource(&movie_resource,1280,720,D3D12_RESOURCE_STATE_COPY_DEST);
    make_resource(&upload_resource,1280,720,D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.backbuf[0]=&bb_resource;surface.tex=&src_resource;gamma_copy=NULL;
    g.list=&command_list;g.swap=&swap_chain;g.upload=g.movie_upload=&upload_resource;
    copies=textures=clears=blits=invalid_copy=invalid_texture=invalid_barrier=swap_calls=0;
    g.srv_ring_used=0;g.movie_overlay_valid=0;
    pipeline_ok=movie_target_ok=slot_ok=1;present_result=S_OK;
    pipeline_calls=upscale_calls=upscale_enabled=upscale_null=state_invalidations=0;
    memset(&surface.ui,0,sizeof surface.ui);memset(&g.movie_overlay_ui,0,sizeof g.movie_overlay_ui);
    g_dm_present_n=20;g_dm_wide_n=-1000;g_dm_scaling=s_dm_scaling=RSX_DM_FIT;
    g_dm_filter=s_dm_filter=RSX_DM_BILINEAR;g_aspect=RSX_DM_ASPECT_SCREEN;
    rsx_present_geometry empty={0};ld_pg_commit(&empty);
}
static rsx_present_geometry image(unsigned tw,unsigned th,rsx_pg_rect r,float kx,float ky) {
    rsx_present_geometry p={0};p.valid=1;p.logical_w=1280;p.logical_h=720;
    p.target_w=tw;p.target_h=th;p.image=r;p.hud_kx=kx;p.hud_ky=ky;return p;
}
static void routing_tests(void) {
    rsx_present_geometry p;
    reset(960,540);pipeline_ok=0;p=normal_route();
    check("normal-size-failure",copies==0&&invalid_copy==0&&clears==1&&!p.valid&&invalid_barrier==0);
    pipeline_ok=1;p=normal_route();
    check("normal-later-blit-recovery",blits==1&&p.valid&&bb_resource.state==D3D12_RESOURCE_STATE_PRESENT);
    reset(1920,1080);g.srv_ring_used=SRV_RING_TABLES;p=normal_route();
    check("normal-ring-full",!copies&&!invalid_copy&&clears==1&&!p.valid);
    g.srv_ring_used=0;p=normal_route();check("normal-ring-recovery",blits==1&&p.valid);
    reset(1280,720);src_resource.desc.Format=87;pipeline_ok=0;p=normal_route();
    check("normal-format-failure",!copies&&!invalid_copy&&clears==1&&!p.valid);
    const char* mismatch_names[]={"normal-dimension-failure","normal-array-failure","normal-mip-failure","normal-sample-failure","normal-quality-failure"};
    for(unsigned mismatch=0;mismatch<5;mismatch++) {
        reset(1280,720);pipeline_ok=0;
        if(mismatch==0) src_resource.desc.Dimension++;
        if(mismatch==1) src_resource.desc.DepthOrArraySize++;
        if(mismatch==2) src_resource.desc.MipLevels++;
        if(mismatch==3) src_resource.desc.SampleDesc.Count++;
        if(mismatch==4) src_resource.desc.SampleDesc.Quality++;
        p=normal_route();check(mismatch_names[mismatch],!copies&&!invalid_copy&&clears==1&&!p.valid);
    }
    reset(1280,720);src_resource.desc.Width=640;src_resource.desc.Height=360;p=normal_route();
    check("normal-actual-source-dimensions",!copies&&!invalid_copy&&blits==1&&p.valid);
    reset(1280,720);gamma_copy=&gamma_resource;p=normal_route();
    check("normal-compatible-gamma",copies==1&&!invalid_copy&&!invalid_barrier&&bb_resource.state==D3D12_RESOURCE_STATE_PRESENT);
    reset(1280,720);g.backbuf[0]=NULL;p=normal_route();
    check("normal-missing-buffer",!copies&&!clears&&!blits&&!p.valid);
    reset(960,540);pipeline_ok=0;p=movie_route(1280,720,0);
    check("movie-pipeline-failure",!textures&&!invalid_texture&&clears==1&&!p.valid&&!invalid_barrier);
    /* A failed pipeline is latched in production. Recovery at exact dimensions
     * must still be safe without pretending the pipeline itself was retried. */
    make_resource(&bb_resource,1280,720,D3D12_RESOURCE_STATE_PRESENT);g_dm_bb_w=1280;g_dm_bb_h=720;
    textures=invalid_texture=clears=0;p=movie_route(1280,720,0);
    check("movie-compatible-recovery-without-pipeline",textures==1&&!invalid_texture&&!clears);
    reset(1920,1080);movie_target_ok=0;p=movie_route(1280,720,0);
    check("movie-intermediate-failure",!textures&&!invalid_texture&&clears==1&&!p.valid);
    movie_target_ok=1;textures=clears=blits=0;p=movie_route(1280,720,0);
    check("movie-intermediate-recovery",textures==1&&!invalid_texture&&blits==1&&p.valid);
    reset(960,540);g.srv_ring_used=SRV_RING_TABLES;p=movie_route(1280,720,0);
    check("movie-ring-full",textures==1&&!invalid_texture&&clears==1&&!p.valid&&!invalid_barrier);
    reset(1280,720);p=movie_route(1280,720,0);
    check("movie-compatible-direct",textures==1&&!invalid_texture&&!invalid_barrier&&!blits&&!clears);
    reset(1280,720);movie_resource.desc.Width=640;movie_resource.desc.Height=360;p=movie_route(640,360,0);
    check("movie-actual-footprint",textures==1&&!invalid_texture&&blits==1&&p.valid);
    reset(2560,1080);g_dm_present_n=20;g_dm_wide_n=17;p=normal_route();
    check("grace-effective-rect",last_rect.x==0&&last_rect.y==0&&last_rect.w==2560&&last_rect.h==1080);
    /* The mock blit executes the actual source rectangle setup. These globals
     * intentionally disagree to prove it consumes the actual target desc. */
    reset(2560,1080);bb_resource.desc.Width=1920;bb_resource.desc.Height=1200;p=normal_route();
    check("actual-target-rectangle",last_rect.x==0&&last_rect.y==60&&last_rect.w==1920&&last_rect.h==1080);
}
static void eligibility_tests(void) {
    const char* names[]={"dimension","array","msaa","quality","typeless","integer","deny-srv","missing-mips","zero-width"};
    for(unsigned phase=0;phase<2;phase++) for(unsigned kind=0;kind<9;kind++) {
        reset(1920,1080);
        ID3D12Resource* bad=&src_resource;
        if(phase) {
            upscale_enabled=1;
            make_resource(&upscale_resource,1920,1080,D3D12_RESOURCE_STATE_COPY_SOURCE);
            bad=&upscale_resource;
        }
        if(kind==0) bad->desc.Dimension++;
        if(kind==1) bad->desc.DepthOrArraySize++;
        if(kind==2) bad->desc.SampleDesc.Count=4;
        if(kind==3) bad->desc.SampleDesc.Quality=1;
        if(kind==4) bad->desc.Format=27; /* R8G8B8A8_TYPELESS */
        if(kind==5) bad->desc.Format=42; /* R32_UINT */
        if(kind==6) bad->desc.Flags|=D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        if(kind==7) bad->desc.MipLevels=0;
        if(kind==8) bad->desc.Width=0;
        const rsx_present_geometry p=normal_route();
        char name[96];snprintf(name,sizeof name,"%s-upscale-reject-%s",phase?"after":"before",names[kind]);
        check(name,!blits&&!copies&&clears==1&&!p.valid&&
              (phase?(pipeline_calls==1&&upscale_calls==1&&state_invalidations==1):(pipeline_calls==0&&upscale_calls==0&&state_invalidations==0)));
    }
    reset(1920,1080);src_resource.desc.MipLevels=3;
    rsx_present_geometry p=normal_route();check("multi-mip-source-mip0-accepted",blits==1&&p.valid);
    const unsigned formats[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT};
    for(unsigned i=0;i<2;i++) {
        reset(1920,1080);src_resource.desc.Format=formats[i];p=normal_route();
        check(i?"r32-float-source-accepted":"fp16-source-accepted",blits==1&&p.valid);
    }
    reset(1920,1080);upscale_enabled=1;
    make_resource(&upscale_resource,1920,1080,D3D12_RESOURCE_STATE_COPY_SOURCE);
    upscale_resource.desc.Flags=0; /* A source need not be an RTV. */
    p=normal_route();check("supported-upscale-result-accepted",blits==1&&p.valid&&upscale_calls==1);
#if !defined(BASELINE_POLICY_CONTROL)
    /* The old implementation dereferences NULL here. Keep optional baseline
       rejection semantic rather than accepting a sanitizer crash as proof. */
    reset(1920,1080);upscale_enabled=upscale_null=1;
    p=normal_route();check("null-upscale-result-rejected",!blits&&!copies&&clears==1&&!p.valid&&state_invalidations==1);
#endif
    reset(1920,1080);upscale_enabled=1;
    make_resource(&upscale_resource,1920,1080,D3D12_RESOURCE_STATE_COPY_SOURCE);
    upscale_resource.desc.DepthOrArraySize=2;p=normal_route();
    check("rejected-upscale-output-invalidates-bindings",!blits&&state_invalidations==1);
    reset(1920,1080);bb_resource.desc.Format=87; /* BGRA8 RTV is not this PSO's output format. */
    p=normal_route();check("non-rgba8-target-rejected",!blits&&!p.valid&&clears==1&&!pipeline_calls);
}
static void input_tests(void) {
    int x,y,ok;rsx_present_geometry p;
    reset(2560,1080);g_dm_wide_n=g_dm_present_n;
    p=image(2560,1080,(rsx_pg_rect){0,0,2560,1080},.75f,1.f);ld_pg_commit(&p);
    ok=rsx_display_client_to_ui(640,540,&x,&y);
    check("effective-input-x",ok&&x==213&&y==360);
    ok=rsx_display_client_to_ui(480,540,&x,&y);
    check("effective-input-visible",ok&&x==107);
    ok=rsx_display_client_to_guest(640,540,&x,&y);check("guest-map-no-hud-inverse",ok&&x==320);
    reset(2560,1080);g_aspect=RSX_DM_ASPECT_16_9;
    p=image(1280,720,(rsx_pg_rect){0,0,1280,720},1.f,1.f);ld_pg_commit(&p);
    ok=rsx_display_client_to_guest(640,540,&x,&y);check("retained-target-resize",ok&&x==320&&y==360);
    p=image(2560,1080,(rsx_pg_rect){320,0,1920,1080},1.f,1.f);ld_pg_commit(&p);
    check("fit-left-bar",!rsx_display_client_to_ui(319,540,&x,&y));
    check("fit-right-bar",!rsx_display_client_to_ui(2240,540,&x,&y));
    ok=rsx_display_client_to_ui(320,540,&x,&y);check("fit-first-image-pixel",ok&&x==0);
    reset(1920,1080);g_aspect=RSX_DM_ASPECT_16_9;s_dm_filter=RSX_DM_NEAREST;
    p=image(1920,1080,(rsx_pg_rect){320,180,1280,720},1.f,1.f);ld_pg_commit(&p);
    ok=rsx_display_client_to_guest(640,360,&x,&y);check("nearest-integer-fit",ok&&x==320&&y==180);
    p=image(1920,1200,(rsx_pg_rect){0,0,1920,1200},1.f,.9f);
    ok=rsx_pg_map(&p,1920,1200,960,300,1,&x,&y);check("ten-sixteen-hud",ok&&x==640&&y==160);
    p=image(1024,768,(rsx_pg_rect){-171,0,1366,768},1.f,1.f);
    ok=rsx_pg_map(&p,1024,768,0,384,0,&x,&y);check("future-fill-cropped-left",ok&&x==160&&y==360);
    ok=rsx_pg_map(&p,1024,768,1023,384,0,&x,&y);check("future-fill-cropped-right",ok&&x==1119);
    p.valid=0;check("invalid-geometry-rejected",!rsx_pg_map(&p,1024,768,100,100,1,&x,&y));
    p.valid=1;p.hud_kx=NAN;check("malformed-factor-rejected",!rsx_pg_map(&p,1024,768,100,100,1,&x,&y));
    p.hud_kx=1;check("minimized-client-rejected",!rsx_pg_map(&p,0,0,100,100,1,&x,&y));
    p=image(2560,1080,(rsx_pg_rect){0,0,2560,1080},.75f,1.f);
    p.hud_provenance=RSX_PG_HUD_GLOBAL_INFERENCE;
    int inferred_x,cached_x;
    rsx_pg_map(&p,2560,1080,640,540,1,&inferred_x,&y);
    p.hud_kx=1.f;rsx_pg_map(&p,2560,1080,640,540,1,&cached_x,&y);
    check("unknown-fallback-is-not-cached-owner",inferred_x==213&&cached_x==320&&inferred_x!=cached_x);

}
static void own(rsx_ui_provenance* ui,float kx,float ky) {
    rsx_ui_clear(ui,1);rsx_ui_direct(ui,kx,ky,g_ld_frames);
}
static void owner_capture_tests(void) {
    rsx_present_geometry p,out;int x,y;
    reset(2560,1080);own(&surface.ui,.75f,1.f);g_aspect=RSX_DM_ASPECT_16_9;p=normal_route();
    check("normal-captures-surface-owner",p.hud_provenance==RSX_PG_HUD_SURFACE_OWNER&&p.hud_kx==.75f&&
          p.ui_surface==0&&p.ui_generation==surface.ui.generation&&p.ui_state==RSX_UI_KNOWN);
    own(&surface.ui,1.f,1.f);ld_pg_commit(&p);
    check("normal-owner-survives-later-surface-write",rsx_display_client_to_ui(640,540,&x,&y)&&x==71&&p.hud_kx==.75f);
    /* Fit output is 1920 pixels wide at x=320; x=640 first maps to guest 213,
       then the captured .75 factor gives floor(71.55...). */
    reset(2560,1080);g_dm_scaling=RSX_DM_STRETCH;own(&surface.ui,1.f,1.f);p=normal_route();
    check("normal-unit-owner-beats-current-wide-global",p.hud_provenance==RSX_PG_HUD_SURFACE_OWNER&&p.hud_kx==1.f);
    reset(2560,1080);g_dm_scaling=RSX_DM_STRETCH;p=normal_route();
    check("unknown-normal-keeps-explicit-fallback",p.hud_provenance==RSX_PG_HUD_GLOBAL_INFERENCE&&p.hud_kx==.75f&&p.ui_state==RSX_UI_UNKNOWN);
    reset(1280,720);own(&surface.ui,.75f,1.f);gamma_copy=&gamma_resource;p=normal_route();
    check("normal-gamma-keeps-presented-surface-owner",p.hud_provenance==RSX_PG_HUD_SURFACE_OWNER&&p.hud_kx==.75f);
    reset(1280,720);g.movie_overlay_valid=1;own(&g.movie_overlay_ui,.75f,1.f);p=movie_route(1280,720,1);
    check("movie-captures-accepted-overlay-owner",p.hud_provenance==RSX_PG_HUD_MOVIE_OWNER&&p.hud_kx==.75f&&
          p.ui_generation==g.movie_overlay_ui.generation&&p.ui_state==RSX_UI_KNOWN);
    own(&g.movie_overlay_ui,1.f,1.f);present(&p);rsx_live_draw_get_present_geometry(&out);
    check("movie-owner-capture-survives-new-overlay",out.hud_provenance==RSX_PG_HUD_MOVIE_OWNER&&out.hud_kx==.75f);
    reset(1280,720);g.movie_overlay_valid=1;own(&g.movie_overlay_ui,.75f,1.f);
    movie_resource.desc.Width=640;movie_resource.desc.Height=360;p=movie_route(640,360,1);
    check("cropped-movie-does-not-adopt-full-overlay-owner",p.hud_provenance==RSX_PG_HUD_UNKNOWN&&p.hud_kx==1.f);
    reset(1280,720);g.movie_overlay_valid=1;p=movie_route(1280,720,1);
    check("unknown-movie-overlay-keeps-unit-fallback",p.hud_provenance==RSX_PG_HUD_UNKNOWN&&p.hud_kx==1.f);
    reset(1280,720);p=movie_route(1280,720,0);
    check("decoded-movie-has-unit-pixel-transform",p.hud_provenance==RSX_PG_HUD_MOVIE_PIXELS&&p.hud_kx==1.f&&p.ui_state==RSX_UI_EMPTY);
}
static void publication_tests(void) {
    reset(1280,720);rsx_present_geometry a=image(1280,720,(rsx_pg_rect){0,0,1280,720},1.f,1.f),b=a,out;
    a.frame=1;b.frame=2;ld_pg_commit(&a);
    slot_ok=0;present(&b);rsx_live_draw_get_present_geometry(&out);
    check("skipped-present-retains-snapshot",out.frame==1&&swap_calls==0);
    slot_ok=1;present_result=-1;present(&b);rsx_live_draw_get_present_geometry(&out);
    check("failed-present-retains-snapshot",out.frame==1&&swap_calls==1);
    present_result=DXGI_STATUS_OCCLUDED;present(&b);rsx_live_draw_get_present_geometry(&out);
    check("occluded-present-retains-snapshot",out.frame==1);
    present_result=S_OK;present(&b);rsx_live_draw_get_present_geometry(&out);
    check("successful-present-publishes",out.frame==2);
    b.valid=0;present(&b);check("black-present-invalidates-input",!rsx_live_draw_get_present_geometry(&out));
}
static void* publisher(void* unused) {
    (void)unused;rsx_present_geometry p=image(1280,720,(rsx_pg_rect){0,0,1280,720},1.f,1.f);
    for(unsigned i=1;i<=10000;i++) { p.frame=i;p.target_w=i;p.target_h=i;p.image.x=(int)i;p.image.y=-(int)i;ld_pg_commit(&p); }
    return NULL;
}
static void synchronization_test(void) {
    rsx_present_geometry empty={0};ld_pg_commit(&empty);
    pthread_t thread;int failed=0;
    if(pthread_create(&thread,NULL,publisher,NULL)) {check("snapshot-publisher-start",0);return;}
    for(unsigned i=0;i<10000;i++) {rsx_present_geometry p;if(rsx_live_draw_get_present_geometry(&p)&&
        (p.target_w!=p.frame||p.target_h!=p.frame||p.image.x!=(int)p.frame||p.image.y!=-(int)p.frame)) failed=1;}
    pthread_join(thread,NULL);check("snapshot-coherent-host-lock",!failed);
}
int main(void) {
    routing_tests();eligibility_tests();input_tests();owner_capture_tests();publication_tests();synchronization_test();
    printf("checks=%u failures=%u\n",checks,fails);return fails?1:0;
}
