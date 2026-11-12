static unsigned checks,failures;
static void check(const char* name,int ok) {checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static int known(unsigned i,float x,float y) {float kx=0,ky=0;return rsx_ui_read(&g.surfaces[i].ui,&kx,&ky)&&kx==x&&ky==y;}
static void reset(void) {memset(&g,0,sizeof g);g.width=1280;g.height=720;g_dm_bb_w=2560;g_dm_bb_h=1080;
    for(unsigned i=0;i<MAX_SURFACES;i++){g.surfaces[i].w=1280;g.surfaces[i].h=720;}}
static void clear_full(unsigned i) {ld_surface_note_write(i,LD_SURFACE_WRITE_CLEAR);ld_ui_note_clear(i,1);}
static void draw(unsigned i,float kx,float ky) {ld_surface_note_write(i,LD_SURFACE_WRITE_DRAW);ld_ui_note_draw(i,15,0,0,0,1280,720,1,kx,ky);}
static void owner_tests(void) {
    reset();draw(0,.75f,1.f);check("unknown-history-not-promoted",g.surfaces[0].ui.state==RSX_UI_UNKNOWN&&!known(0,.75f,1.f));
    clear_full(0);check("full-clear-empty-base",g.surfaces[0].ui.state==RSX_UI_EMPTY);
    float xf[8]={1,-1,1,0,0,0,0,0};rsx_h169_xf(xf,.75f,1.f);
    check("prepared-xf-does-not-own-pixels",g.surfaces[0].ui.state==RSX_UI_EMPTY);
    draw(0,.75f,1.f);check("actual-recorded-direct-owner",known(0,.75f,1.f));
    uint64_t version=g.surfaces[0].ui.generation;
    ld_surface_note_write(0,LD_SURFACE_WRITE_DRAW);ld_ui_note_draw(0,0,0,0,0,1280,720,1,.5f,1.f);
    check("masked-color-no-owner-change",known(0,.75f,1.f)&&g.surfaces[0].ui.generation==version);
    ld_surface_note_write(0,LD_SURFACE_WRITE_DRAW);ld_ui_note_draw(0,15,-1,0,0,1280,720,1,.5f,1.f);
    check("cpu-skipped-draw-no-owner-change",known(0,.75f,1.f)&&g.surfaces[0].ui.generation==version);
    ld_surface_note_write(0,LD_SURFACE_WRITE_DRAW);ld_ui_note_draw(0,15,0,100,0,100,720,1,.5f,1.f);
    check("empty-scissor-no-owner-change",known(0,.75f,1.f)&&g.surfaces[0].ui.generation==version);
    ld_ui_note_draw(0,15,1,0,0,1280,720,1,.75f,1.f);
    check("gpu-predication-does-not-mint-owner",g.surfaces[0].ui.state==RSX_UI_UNKNOWN);
    clear_full(0);draw(0,.75f,1.f);ld_ui_note_clear(0,0);
    check("partial-constant-clear-preserves-factor",known(0,.75f,1.f));
    draw(0,.5f,1.f);check("conflicting-direct-factors-mixed",g.surfaces[0].ui.state==RSX_UI_MIXED&&!known(0,.5f,1.f));
    clear_full(0);draw(0,1.f,1.f);check("unit-factor-is-real-owner",known(0,1.f,1.f));
    check("unit-mode-classification-still-eligible",prepared_eligibility(1,1,1,0,0,0));
    check("composite-excluded",!prepared_eligibility(1,1,1,0,1,1));
    check("split-viewport-excluded",!prepared_eligibility(1,0,1,0,0,1));
    check("owner-covers-actual-resource",rsx_ui_owner_extent_matches(2560,1440,2560,1440,1280,720,1280,720));
    check("partial-resource-viewport-not-owner",!rsx_ui_owner_extent_matches(1280,720,2560,1440,1280,720,1280,720));
    check("different-logical-basis-not-owner",!rsx_ui_owner_extent_matches(1280,720,1280,720,640,360,1280,720));
    clear_full(0);draw(0,.75f,1.f);ld_ui_note_draw(0,15,0,0,0,1280,720,0,1.f,1.f);
    check("arbitrary-composite-invalidates",g.surfaces[0].ui.state==RSX_UI_UNKNOWN);
    clear_full(0);draw(0,.75f,1.f);ld_surface_note_write(0,LD_SURFACE_WRITE_OTHER);
    check("always-on-generic-write-invalidates",g.surfaces[0].ui.state==RSX_UI_UNKNOWN&&!known(0,.75f,1.f));
    clear_full(0);draw(0,.75f,1.f);ld_surface_note_write(0,LD_SURFACE_WRITE_CREATE);
    check("resource-replacement-invalidates",g.surfaces[0].ui.state==RSX_UI_UNKNOWN);
    clear_full(0);draw(0,.75f,1.f);actual_guest_upload_owner(&g.surfaces[0]);
    check("guest-upload-invalidates",g.surfaces[0].ui.state==RSX_UI_UNKNOWN);
    ld_ui_note_draw(MAX_SURFACES,15,0,0,0,1280,720,1,1.f,1.f);
    check("invalid-target-bounded",g.surfaces[0].ui.state==RSX_UI_UNKNOWN);
}
static void transfer_tests(void) {
    reset();clear_full(0);draw(0,.75f,1.f);
    for(unsigned i=0;i<8;i++)ld_surface_note_write(1,LD_SURFACE_WRITE_OTHER);
    rsx_ui_provenance source=g.surfaces[0].ui;
    ld_surface_note_write(1,LD_SURFACE_WRITE_COPY);uint64_t version=g.surfaces[1].ui.generation;
    ld_ui_inherit(1,&source,1);
    check("whole-copy-preserves-factor",known(1,.75f,1.f));
    check("copy-does-not-rewind-destination-generation",g.surfaces[1].ui.generation==version&&version>source.generation);
    ld_surface_note_write(1,LD_SURFACE_WRITE_COPY);ld_ui_inherit(1,&source,0);
    check("partial-copy-does-not-forge-owner",g.surfaces[1].ui.state==RSX_UI_UNKNOWN);
    rsx_ui_unknown(&source);ld_surface_note_write(1,LD_SURFACE_WRITE_COPY);ld_ui_inherit(1,&source,1);
    check("unknown-copy-remains-unknown",g.surfaces[1].ui.state==RSX_UI_UNKNOWN);
    source.state=RSX_UI_KNOWN;source.generation=0;source.applied_generation=1;source.kx=.5f;source.ky=1.f;
    ld_ui_inherit(1,&source,1);check("unversioned-source-rejected",!known(1,.5f,1.f));
    source=g.surfaces[0].ui;source.applied_generation=source.generation+1;
    ld_ui_inherit(1,&source,1);check("future-owner-generation-rejected",!known(1,.75f,1.f));
    source=g.surfaces[0].ui;source.generation=UINT64_MAX;
    rsx_ui_clear(&source,0);float kx,ky;
    check("partial-clear-version-wrap-not-empty",source.state==RSX_UI_UNKNOWN&&!rsx_ui_read(&source,&kx,&ky));
}
static void publication_tests(void) {
    reset();clear_full(0);draw(0,.75f,1.f);
    g_dm_bb_w=1920;g_dm_bb_h=1080;rsx_present_geometry p=normal_owner(0);
    p.valid=1;p.logical_w=1280;p.logical_h=720;p.target_w=2560;p.target_h=1080;p.image=(rsx_pg_rect){0,0,2560,1080};
    int x,y,ok=rsx_pg_map(&p,2560,1080,640,540,1,&x,&y);
    check("present-uses-applied-not-current-global",p.hud_provenance==RSX_PG_HUD_SURFACE_OWNER&&ok&&x==213);
    check("present-tags-selected-version",p.ui_generation==g.surfaces[0].ui.generation&&p.ui_state==RSX_UI_KNOWN);
    clear_full(1);draw(1,1.f,1.f);g_dm_bb_w=2560;g_dm_bb_h=1080;p=normal_owner(1);
    check("unit-applied-survives-later-wide-setting",p.hud_provenance==RSX_PG_HUD_SURFACE_OWNER&&p.hud_kx==1.f);
    check("selected-surface-not-current-surface",p.ui_surface==1&&p.ui_generation==g.surfaces[1].ui.generation);
    ld_surface_note_write(1,LD_SURFACE_WRITE_OTHER);p=normal_owner(1);
    check("unknown-still-explicit-fallback",p.hud_provenance==RSX_PG_HUD_GLOBAL_INFERENCE&&p.ui_state==RSX_UI_UNKNOWN);
    capture_owner(0,1);draw(0,.5f,1.f);p=movie_owner(1280,720,1);
    check("movie-retains-captured-owner",p.hud_provenance==RSX_PG_HUD_MOVIE_OWNER&&p.hud_kx==.75f);
    capture_owner(0,0);p=movie_owner(1280,720,1);
    check("rejected-overlay-not-owned",p.hud_provenance!=RSX_PG_HUD_MOVIE_OWNER);
    clear_full(0);draw(0,.75f,1.f);capture_owner(0,1);p=movie_owner(640,360,1);
    check("cropped-movie-overlay-not-forged",p.hud_provenance==RSX_PG_HUD_UNKNOWN);
    g.surfaces[0].w=2560;g.surfaces[0].h=1440;capture_owner(0,1);p=movie_owner(1280,720,1);
    check("cropped-readback-not-owned",p.hud_provenance==RSX_PG_HUD_UNKNOWN);
    p=movie_owner(1280,720,0);check("decoded-movie-unit-unmodified",p.hud_kx==1.f&&p.hud_ky==1.f&&p.hud_provenance==RSX_PG_HUD_MOVIE_PIXELS);
}
int main(void) {owner_tests();transfer_tests();publication_tests();printf("checks=%u failures=%u\n",checks,failures);return failures?1:0;}
