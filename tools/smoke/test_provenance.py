#!/usr/bin/env python3
"""Compile small source-extracted ownership controls, with independent negative mutations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def default_repo():
    """Work both from tools/smoke and when the three files are copied elsewhere."""
    for directory in (*HERE.parents, HERE, Path.cwd(), *Path.cwd().parents):
        if (directory / 'port/kz3').is_dir() and (directory / 'tools').is_dir():
            return directory
    return Path.cwd()


def between(text, begin, end):
    start = text.index(begin)
    return text[start:text.index(end, start)]


def function(text, signature):
    start = text.index(signature + '\n{')
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def wiring(source):
    # Exact executable sites and preprocessor placement, not a GPU claim.
    checks = {}
    enum = source.index('enum {\n    LD_SURFACE_WRITE_NONE')
    perf = source.index('#if !defined(YZ_PERF_CLEAN)\nstatic u32 g_ld_surface_generation')
    checks['owner-helper-outside-perf-flags'] = source.index('static void ld_ui_note_draw(') < perf and enum < perf
    surface = between(source, 'typedef struct {\n    u32 location, offset;\n    ID3D12Resource* tex;\n    u32 w, h;', '} surface_t;')
    checks['owner-field-outside-perf-flags'] = surface.index('rsx_ui_provenance ui;') < surface.index('#if !defined(YZ_PERF_CLEAN)')
    draw = between(source, '    int h169 = 0, ui_direct = 0;', '\n    if (current_zslot) {\n        render_state_t depth_state;')
    checks['unit-eligibility-before-factor-test'] = draw.index('ui_direct = rsx_ui_direct_candidate') < draw.index('if (ld_h169_factors(')
    checks['draw-commit-after-recorded-command'] = draw.index('ld_ui_note_draw(target,') > draw.index('g.list->lpVtbl->DrawInstanced(')
    checks['applied-factors-not-fresh-globals'] = 'ui_owner_direct, h169 ? h_kx : 1.0f, h169 ? h_ky : 1.0f);' in draw
    checks['owner-extent-checked-without-changing-squeeze'] = 'const int ui_owner_direct = ui_direct && target < g.n_surfaces &&' in draw and 'rsx_ui_owner_extent_matches(' in draw and 'h169 = ui_direct;' in draw
    checks['mrt-independent-mask'] = 'ld_ui_note_draw(mrt[r], (cw.color_mask_mrt >> (4u * r)) & 0xFu, oq_pred,' in draw
    clear = between(source, 'static void sink_clear_impl(void* user, const rsx_dispatch* r, u32 mask)\n{', '\n/*')
    checks['clear-record-before-owner'] = clear.index('ld_ui_note_clear(target,') > clear.index('ClearRenderTargetView(')
    upload = between(source, 'static int ld_gv_upload_rows(', '\n/* Bring surface i up to date')
    checks['upload-invalidation-after-copy'] = upload.index('rsx_ui_unknown(&s->ui)') > upload.index('CopyTextureRegion(')
    checks['all-four-copy-sites-propagate'] = source.count('ld_ui_inherit(') == 6  # definition + four copies + AA
    checks['movie-owner-accepted-with-pixels'] = 'if (g.movie_overlay_valid && overlay_ui_full) rsx_ui_inherit(&g.movie_overlay_ui, &overlay_ui, 1);' in source
    checks['owned-movie-rejects-cropped-footprint'] = 'composite && g.movie_overlay_valid && w == g.width && h == g.height &&' in source
    if not all(checks.values()):
        raise SystemExit('source wiring failed: ' + ', '.join(k for k, value in checks.items() if not value))
    return checks


def fragments(source):
    start = source.index('enum {\n    LD_SURFACE_WRITE_NONE')
    helpers = source[start:source.index('#if !defined(YZ_PERF_CLEAN)\nstatic u32 g_ld_surface_generation', start)]
    normal = between(source,
        '    rsx_present_geometry geometry; memset(&geometry, 0, sizeof geometry);\n    const int present_scaling =',
        '    ld_dm_sync_size();')
    movie = between(source,
        '    rsx_present_geometry geometry; memset(&geometry, 0, sizeof geometry);\n    geometry.hud_kx',
        '    const int present_scaling =')
    macro = between(source, '#define ld_surface_note_write(target, kind)', '\n#endif')
    note = function(source, 'static void ld_surface_note_write(u32 target, u32 kind)')
    eligibility = between(source, '            ui_direct = rsx_ui_direct_candidate(', '\n            if (ld_h169_factors(')
    capture = between(source, '    const rsx_ui_provenance overlay_ui = g.surfaces[target].ui;', '\n    ID3D12Resource* rt =')
    accepted = between(source, '    rsx_ui_unknown(&g.movie_overlay_ui);', '\n    g.movie_overlay_frames++;')
    upload = 'rsx_ui_unknown(&s->ui);' if 'rsx_ui_unknown(&s->ui); /* bytes uploaded' in source else ''
    return dict(helpers=helpers, normal=normal, movie=movie, macro=macro, note=note,
                eligibility=eligibility, capture=capture, accepted=accepted, upload=upload)


def unit(parts):
    return '\n'.join([
        (HERE / 'test_provenance.c').read_text(), parts['helpers'],
        '#if !defined(YZ_PERF_CLEAN)\n' + parts['note'] + '\n#else\n' + parts['macro'] + '\n#endif',
        'static int prepared_eligibility(int display,int full,int is169,int compose,int sampled,int active) { int ui_direct=0;int g_h169_rt_sampled=sampled;if(display){' + parts['eligibility'] + '\n}(void)active;return ui_direct;}',
        'static void actual_guest_upload_owner(surface_t* s) {' + parts['upload'] + '}',
        'static rsx_present_geometry normal_owner(unsigned target) {surface_t* presented_surface=&g.surfaces[target];\n' + parts['normal'] + '\nreturn geometry;}',
        'static rsx_present_geometry movie_owner(unsigned w,unsigned h,int composite) {\n' + parts['movie'] + '\nreturn geometry;}',
        'static void capture_owner(unsigned target,int accepted) {\n' + parts['capture'] + '\ng.movie_overlay_valid=accepted;\n' + parts['accepted'] + '\n}',
        (HERE / 'test_provenance_cases.inc').read_text(),
    ])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=default_repo(), help='repository root (autodetected)')
    parser.add_argument('--candidate-sdk', type=Path, help='assembled SDK (default: REPO/work/ps3recomp-patched)')
    parser.add_argument('--prior-candidate-sdk', type=Path, help='optional older presentation candidate for an additional control')
    parser.add_argument('--out', type=Path, help='evidence directory (default: a fresh directory under /tmp)')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    args = parser.parse_args()
    args.candidate_sdk = (args.candidate_sdk or args.repo / 'work/ps3recomp-patched').resolve()
    if args.prior_candidate_sdk:
        args.prior_candidate_sdk = args.prior_candidate_sdk.resolve()
    input_paths = [args.candidate_sdk / 'libs/video' / name for name in
                   ('rsx_live_draw.c', 'rsx_ui_provenance.h', 'rsx_present_geometry.h',
                    'rsx_hud169.h', 'rsx_display_mode.h')]
    input_paths += [HERE / name for name in ('test_provenance.py', 'test_provenance.c', 'test_provenance_cases.inc')]
    if args.prior_candidate_sdk:
        input_paths.append(args.prior_candidate_sdk / 'libs/video/rsx_live_draw.c')
    missing = [str(path) for path in input_paths if not path.is_file()]
    if missing:
        parser.error('required source missing; assemble the patched SDK or supply --candidate-sdk: ' + ', '.join(missing))
    input_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in input_paths}
    source = (args.candidate_sdk / 'libs/video/rsx_live_draw.c').read_text()
    original = fragments(source)
    checks = wiring(source)
    header = (args.candidate_sdk / 'libs/video/rsx_ui_provenance.h').read_text()
    variants = [('candidate', original, header, set()),
                ('ignore-recorded-owner', dict(original,
                    normal=original['normal'].replace('if (rsx_ui_read(', 'if (0 && rsx_ui_read('),
                    movie=original['movie'].replace('rsx_ui_read(&g.movie_overlay_ui,', '0 && rsx_ui_read(&g.movie_overlay_ui,')), header,
                    {'present-uses-applied-not-current-global', 'unit-applied-survives-later-wide-setting', 'movie-retains-captured-owner'}),
                ('omit-color-mask', dict(original,helpers=original['helpers'].replace('!color_mask || ','')), header, {'masked-color-no-owner-change'}),
                ('omit-cpu-skip', dict(original,helpers=original['helpers'].replace('predication < 0 || ','')), header, {'cpu-skipped-draw-no-owner-change'}),
                ('omit-empty-scissor', dict(original,helpers=original['helpers'].replace(' || right <= left || bottom <= top','')), header, {'empty-scissor-no-owner-change'}),
                ('trust-gpu-predication', dict(original,helpers=original['helpers'].replace('predication > 0 || ','')), header, {'gpu-predication-does-not-mint-owner'}),
                ('unit-eligibility-regression', dict(original,eligibility='if(active){' + original['eligibility'] + '}'), header, {'unit-mode-classification-still-eligible'}),
                ('omit-clean-ownership', dict(original,macro=original['macro'].replace('ld_ui_note_other_write((target), (kind)); ','')), header, {'always-on-generic-write-invalidates'}),
                ('promote-unknown-history', original, header.replace('    p->applied_generation = p->generation;', '    if (p->state == RSX_UI_UNKNOWN) p->state = RSX_UI_EMPTY;\n    p->applied_generation = p->generation;'), {'unknown-history-not-promoted'}),
                ('rewind-copy-version', original, header.replace('dst->generation = version;', 'dst->generation = src->generation;'), {'copy-does-not-rewind-destination-generation'}),
                ('trust-partial-copy', original, header.replace('if (!whole || !dst->generation)', 'if (!dst->generation)'), {'partial-copy-does-not-forge-owner'}),
                ('omit-upload-invalidation', dict(original,upload='(void)s;'), header, {'guest-upload-invalidates'}),
                ('trust-cropped-capture', dict(original,accepted=original['accepted'].replace(' && overlay_ui_full','')), header, {'cropped-readback-not-owned'})]
    if args.prior_candidate_sdk:
        before = (args.prior_candidate_sdk / 'libs/video/rsx_live_draw.c').read_text()
        variants.insert(1, ('prior-global-mapping', dict(original,
            normal=between(before, '    rsx_present_geometry geometry; memset(&geometry, 0, sizeof geometry);\n    const int present_scaling =', '    ld_dm_sync_size();'),
            movie=between(before, '    rsx_present_geometry geometry; memset(&geometry, 0, sizeof geometry);\n    geometry.hud_kx', '    const int present_scaling =')), header,
            {'present-uses-applied-not-current-global', 'unit-applied-survives-later-wide-setting', 'movie-retains-captured-owner'}))
    out = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix='kz3-display-provenance-', dir='/tmp'))
    if args.out:
        out.mkdir(parents=True, exist_ok=False)
    print('evidence:', out, flush=True)
    results = {}
    for label, parts, variant_header, expected in variants:
        directory = out / label
        directory.mkdir(exist_ok=True)
        (directory / 'rsx_ui_provenance.h').write_text(variant_header)
        translation = unit(parts)
        c = directory / 'test.c'
        c.write_text(translation)
        for clean in (False, True):
            suffix = 'clean' if clean else 'profile'
            binary = directory / suffix
            cmd = shlex.split(args.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(directory), '-I', str(args.candidate_sdk / 'libs/video'),
                str(c), '-o', str(binary), '-lm'] + (['-DYZ_PERF_CLEAN=1'] if clean else [])
            compiled = subprocess.run(cmd, capture_output=True, text=True, timeout=45)
            (directory / (suffix + '.compile.stdout')).write_text(compiled.stdout)
            (directory / (suffix + '.compile.stderr')).write_text(compiled.stderr)
            if compiled.returncode:
                raise SystemExit(f'{label}/{suffix}: compile failed: {compiled.stderr}')
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            (directory / (suffix + '.stdout')).write_text(result.stdout)
            (directory / (suffix + '.stderr')).write_text(result.stderr)
            failed = {line[5:] for line in result.stdout.splitlines() if line.startswith('FAIL ')}
            wanted = expected if label != 'omit-clean-ownership' or clean else set()
            if any(diagnostic in result.stderr for diagnostic in ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer')):
                raise SystemExit(f'{label}/{suffix}: sanitizer failure: {result.stderr}')
            if wanted:
                if result.returncode != 1 or not wanted <= failed:
                    raise SystemExit(f'{label}/{suffix}: mutation survived: {wanted-failed}; rc={result.returncode}')
            elif result.returncode or failed:
                raise SystemExit(f'{label}/{suffix}: unexpected failure: {result.stdout}\n{result.stderr}')
            results[label + '/' + suffix] = {'returncode': result.returncode, 'failures': sorted(failed), 'compile_command': cmd,
                'source_sha256': hashlib.sha256(c.read_bytes()).hexdigest(), 'header_sha256': hashlib.sha256(variant_header.encode()).hexdigest()}
            print(label + '/' + suffix, 'PASS' if not wanted else 'mutation rejected', len(failed))
    if any(not Path(path).is_file() or hashlib.sha256(Path(path).read_bytes()).hexdigest() != digest
           for path, digest in input_hashes.items()):
        raise SystemExit('source or test inputs changed during the matrix; results are not accepted')
    (out / 'results.json').write_text(json.dumps({'wiring': checks, 'variants': results, 'inputs': input_hashes,
        'candidate_sdk': str(args.candidate_sdk), 'prior_candidate_sdk': str(args.prior_candidate_sdk) if args.prior_candidate_sdk else None,
        'candidate_source_sha256': hashlib.sha256(source.encode()).hexdigest()}, indent=2) + '\n')


if __name__ == '__main__':
    main()
