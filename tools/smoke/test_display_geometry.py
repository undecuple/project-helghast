#!/usr/bin/env python3
"""Source-extracted display-policy controls; no game, Windows binary, or GPU.

Defaults to the repository's patched SDK. Independent source mutations must fail
named semantic checks; no archived baseline is needed. --baseline-sdk optionally
checks an older SDK. --baseline-patch reverse-applies a patch to an isolated,
minimal source copy, then runs that baseline as an additional negative control.
The final SRV/barrier/draw body is mocked; this is not native/DPI/GPU validation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE_NAMES = ('rsx_live_draw.c', 'rsx_null_backend.c', 'rsx_display_mode.h',
                'rsx_hud169.h', 'rsx_present_geometry.h', 'rsx_ui_provenance.h')


def repo_root():
    for origin in (HERE, Path.cwd().resolve()):
        for path in (origin, *origin.parents):
            if (path / 'port/patches').is_dir() and (path / 'work').is_dir():
                return path
    return Path.cwd().resolve()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'unique mutation marker required: {old!r}')
    return text.replace(old, new, 1)


def between(text, begin, end):
    if text.count(begin) != 1:
        raise ValueError(f'unique source marker required: {begin!r}')
    start = text.index(begin)
    return text[start:text.index(end, start)]


def function(text, signature):
    start = text.index(signature + '\n{')
    # Selected functions contain no braces inside string/comment literals.
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def sources(sdk):
    live = (sdk / 'libs/video/rsx_live_draw.c').read_text()
    null = (sdk / 'libs/video/rsx_null_backend.c').read_text()
    movie_marker = '    ld_dm_sync_size();   /* patch 2095 */'
    if '    rsx_present_geometry geometry; memset(&geometry, 0, sizeof geometry);\n    geometry.hud_kx' in live:
        movie_marker = '    rsx_present_geometry geometry; memset(&geometry, 0, sizeof geometry);\n    geometry.hud_kx'
    input_marker = 'static int rsx_display_client_map(' if 'static int rsx_display_client_map(' in null else 'int rsx_display_client_to_guest('
    swap_signature = 'static HRESULT ld_present_swap(const rsx_present_geometry* geometry)' if 'ld_present_swap(const' in live else 'static HRESULT ld_present_swap(void)'
    return dict(
        normal=between(live, '    ID3D12Resource* srcimg = presented_surface->tex;', '    ld_flush(LD_FLUSH_PRESENT);'),
        movie=between(live, movie_marker, '    ld_flush(LD_FLUSH_MOVIE_PRESENT);'),
        input=between(null, input_marker, '\nstatic int rsx_dm_wndmsg('),
        blit=between(live, 'static int ld_dm_blit(', '\n    int dm_filter ='),
        upscale=between(live, '    int dm_filter = g_dm_filter;', '    const u32 base = ld_srv_take();'),
        swap=function(live, swap_signature), scaling=function(live, 'static int ld_dm_scaling_now(void)'),
        hud=function(live, 'static int ld_h169_factors(float* kx, float* ky)'))


def translation_unit(selected, helpers):
    normal_init = '' if 'rsx_present_geometry geometry;' in selected['normal'] else 'rsx_present_geometry geometry = {0};'
    movie_init = '' if 'rsx_present_geometry geometry;' in selected['movie'] else 'rsx_present_geometry geometry = {0};'
    blit = selected['blit'].replace('u32 bbi)', 'u32 bbi, int effective_scaling, rsx_present_geometry* geometry)')
    normal = selected['normal'].replace('D3D12_RESOURCE_STATE_RENDER_TARGET, bbi);',
        'D3D12_RESOURCE_STATE_RENDER_TARGET, bbi, ld_dm_scaling_now(), &geometry);')
    if 'D3D12_RESOURCE_DESC bd;' not in blit:
        blit += '\n    D3D12_RESOURCE_DESC bd; bb->lpVtbl->GetDesc(bb,&bd);'  # descriptor query only, for optional old baseline
    movie = selected['movie'].replace('ld_dm_blit(dm_tex, D3D12_RESOURCE_STATE_COPY_DEST, bbi)',
        'ld_dm_blit(dm_tex, D3D12_RESOURCE_STATE_COPY_DEST, bbi, ld_dm_scaling_now(), &geometry)')
    swap_call = 'ld_present_swap(p)' if 'ld_present_swap(const' in selected['swap'] else '(void)p, ld_present_swap()'
    return '\n'.join([
        (HERE / 'test_display_geometry.c').read_text(), selected['scaling'], selected['hud'],
        'int rsx_live_draw_hud169_factors(float* x,float* y) { return ld_h169_factors(x,y); }', helpers,
        blit + '\n' + selected['upscale'] + '\n    (void)dm_filter; last_rect=r;blits++;\n    ld_pg_record(geometry,bb,r);\n    return 1;\n}',
        'static rsx_present_geometry normal_route(void) { const u32 target=0; ' + normal_init + '\n' + normal + '\nreturn geometry;\n}',
        'static rsx_present_geometry movie_route(u32 w,u32 h,int composite) { const u32 pitch=(w*4+255)&~255u; ' + movie_init + '\n' + movie + '\nreturn geometry;\n}',
        selected['input'], selected['swap'],
        'static HRESULT present(const rsx_present_geometry* p) { return ' + swap_call + '; }',
        (HERE / 'test_display_cases.inc').read_text()])


def mutate(parts, key, old, new):
    return dict(parts, **{key: replace_once(parts[key], old, new)})


def variants(parts, helpers):
    cases = [('candidate', parts, helpers, set())]
    cases += [('permit-incompatible-copy', mutate(parts, 'normal',
        'if (!dm_scaled && bb && copy_ok)', 'if (!dm_scaled && bb)'), helpers, {'normal-size-failure'})]
    direct_start = '    const int direct_copy = bb && bd.width == w'
    start = parts['movie'].index(direct_start)
    end = parts['movie'].index(';', start) + 1
    movie = parts['movie'][:start] + '    const int direct_copy = bb != NULL;' + parts['movie'][end:]
    cases += [('permit-incompatible-movie-upload', dict(parts, movie=movie), helpers, {'movie-pipeline-failure'})]
    no_snapshot = replace_once(parts['input'],
        '    if (rsx_live_draw_get_present_geometry(&geometry))\n        return rsx_pg_map(&geometry, rc.right, rc.bottom, cx, cy, ui, gx, gy);',
        '    (void)geometry; (void)ui;')
    no_snapshot = replace_once(no_snapshot, '    if (s_present_suppressed) return 0;', '    (void)s_present_suppressed;')
    cases += [('ignore-submitted-input-geometry', dict(parts, input=no_snapshot), helpers, {'retained-target-resize'})]
    cases += [('ignore-effective-scaling', mutate(parts, 'blit', '(int)bd.Height, effective_scaling,',
        '(int)bd.Height, g_dm_scaling,'), helpers, {'grace-effective-rect'})]
    cases += [('publish-failed-present', mutate(parts, 'swap',
        'if (SUCCEEDED(hr) && hr != DXGI_STATUS_OCCLUDED) ld_pg_commit(geometry);',
        'ld_pg_commit(geometry);'), helpers, {'failed-present-retains-snapshot'})]
    cases += [('publish-skipped-present', mutate(parts, 'swap',
        'if (!ld_present_slot()) return S_OK;', 'if (!ld_present_slot()) { ld_pg_commit(geometry); return S_OK; }'),
        helpers, {'skipped-present-retains-snapshot'})]
    cases += [('omit-source-eligibility', mutate(parts, 'blit', '!ld_pg_blit_eligible(&sd, &bd) || ', ''),
        helpers, {'before-upscale-reject-typeless'})]
    cases += [('omit-upscale-eligibility', mutate(parts, 'upscale',
        '                if (!ld_pg_blit_eligible(&sd, &bd)) return 0;\n', ''), helpers, {'after-upscale-reject-array'})]
    cases += [('omit-upscale-state-invalidation', mutate(parts, 'upscale', '                LD_SM_INVALIDATE();',
        '                (void)0;'), helpers, {'rejected-upscale-output-invalidates-bindings'})]
    cases += [('ignore-surface-owner', mutate(parts, 'normal',
        '    if (rsx_ui_read(&presented_surface->ui, &geometry.hud_kx, &geometry.hud_ky))\n        geometry.hud_provenance = RSX_PG_HUD_SURFACE_OWNER;', ''),
        helpers, {'normal-captures-surface-owner', 'normal-unit-owner-beats-current-wide-global'})]
    movie_owner = '    if (composite && g.movie_overlay_valid && w == g.width && h == g.height &&\n        rsx_ui_read(&g.movie_overlay_ui, &geometry.hud_kx, &geometry.hud_ky))\n        geometry.hud_provenance = RSX_PG_HUD_MOVIE_OWNER;'
    cases += [('ignore-movie-owner', mutate(parts, 'movie', movie_owner, ''), helpers, {'movie-captures-accepted-overlay-owner'})]
    cases += [('ignore-copy-descriptors', parts, replace_once(helpers,
        'return rsx_pg_copy_compatible(&a, &b);', '(void)a;(void)b;return 1;'), {'normal-format-failure'})]
    return cases


def reverse_baseline(candidate, patch, out):
    target = out / 'baseline-sdk'
    target.mkdir(exist_ok=False)
    video = target / 'libs/video'
    video.mkdir(parents=True)
    for name in SOURCE_NAMES:
        shutil.copy2(candidate / 'libs/video' / name, video / name)
    command = ['patch', '--batch', '--fuzz=0', '-R', '-p1', '-i', str(patch.resolve())]
    run = subprocess.run(command, cwd=target, capture_output=True, text=True, timeout=30)
    (out / 'reverse-baseline.stdout').write_text(run.stdout)
    (out / 'reverse-baseline.stderr').write_text(run.stderr)
    if run.returncode:
        raise SystemExit(f'baseline reverse patch failed: {run.stdout}\n{run.stderr}')
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate-sdk', type=Path, default=repo_root() / 'work/ps3recomp-patched')
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--baseline-sdk', type=Path)
    group.add_argument('--baseline-patch', type=Path, help='reverse-apply this patch to an isolated minimal source copy')
    parser.add_argument('--out', type=Path, help='fresh output directory; default is a temporary directory')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--sanitize', action='store_true', help='enable AddressSanitizer and UBSan; host compiler must support them')
    args = parser.parse_args()
    candidate = args.candidate_sdk.resolve()
    out = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix='kz3-display-host-'))
    if args.out:
        out.mkdir(parents=True, exist_ok=False)
    input_paths = [candidate / 'libs/video' / name for name in SOURCE_NAMES]
    input_paths += [HERE / name for name in ('test_display_geometry.py', 'test_display_geometry.c', 'test_display_cases.inc')]
    before = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in input_paths}
    source = (candidate / 'libs/video/rsx_live_draw.c').read_text()
    helpers = between(source, 'static SRWLOCK g_pg_lock = SRWLOCK_INIT;', '\nstatic int ld_dm_blit(')
    controls = variants(sources(candidate), helpers)
    baseline = reverse_baseline(candidate, args.baseline_patch, out) if args.baseline_patch else args.baseline_sdk
    if baseline:
        controls += [('baseline', sources(baseline.resolve()), helpers,
            {'normal-size-failure', 'movie-pipeline-failure', 'effective-input-x', 'grace-effective-rect', 'successful-present-publishes'})]
    environment = os.environ.copy()
    # Present-interval diagnostic is intentionally read once by extracted code;
    # do not inherit it from a caller's game environment into deterministic tests.
    environment.pop('LD_PRESENT_INTERVAL', None)
    results = {}
    for label, parts, selected_helpers, expected in controls:
        c = out / (label + '.c')
        binary = out / label
        c.write_text(translation_unit(parts, selected_helpers))
        command = shlex.split(args.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-function',
            '-Wno-unused-variable', '-Wno-unused-parameter', '-D_XOPEN_SOURCE=700', '-I', str(candidate / 'libs/video'),
            str(c), '-o', str(binary), '-pthread', '-lm']
        if label == 'baseline':
            command += ['-DBASELINE_POLICY_CONTROL=1']
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=45)
        (out / (label + '.compile.stdout')).write_text(compiled.stdout)
        (out / (label + '.compile.stderr')).write_text(compiled.stderr)
        if compiled.returncode:
            raise SystemExit(f'{label}: host compile failed: {compiled.stderr}')
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30, env=environment)
        (out / (label + '.stdout')).write_text(run.stdout)
        (out / (label + '.stderr')).write_text(run.stderr)
        failures = {line[5:] for line in run.stdout.splitlines() if line.startswith('FAIL ')}
        if any(word in run.stderr for word in ('AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:')):
            raise SystemExit(f'{label}: sanitizer failed: {run.stderr}')
        if expected:
            if run.returncode != 1 or not expected <= failures:
                raise SystemExit(f'{label}: negative control survived; missing={sorted(expected-failures)}, rc={run.returncode}')
        elif run.returncode or failures:
            raise SystemExit(f'{label}: unexpected failure: {run.stdout}\n{run.stderr}')
        results[label] = {'returncode': run.returncode, 'expected_failures': sorted(expected), 'failures': sorted(failures),
            'source_sha256': hashlib.sha256(c.read_bytes()).hexdigest(), 'compile_command': command}
        print(label, 'PASS' if not expected else 'negative control rejected', 'failures=', len(failures), flush=True)
    if any(hashlib.sha256(Path(path).read_bytes()).hexdigest() != digest for path, digest in before.items()):
        raise SystemExit('source or harness changed during the matrix')
    inputs = {'candidate': {name: hashlib.sha256((candidate / 'libs/video' / name).read_bytes()).hexdigest() for name in SOURCE_NAMES}}
    if baseline:
        inputs['baseline'] = {name: hashlib.sha256((baseline / 'libs/video' / name).read_bytes()).hexdigest()
            for name in SOURCE_NAMES if (baseline / 'libs/video' / name).is_file()}
    inputs['harness'] = {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
        for name in ('test_display_geometry.py', 'test_display_geometry.c', 'test_display_cases.inc')}
    (out / 'results.json').write_text(json.dumps({'variants': results, 'inputs': inputs, 'sanitized': args.sanitize,
        'candidate_sdk': str(candidate), 'baseline_sdk': str(baseline) if baseline else None,
        'limits': 'Host extraction mocks final GPU SRV/barrier/draw body; no native/GPU/DPI/visual/FPS proof.'}, indent=2) + '\n')
    print('Evidence:', out / 'results.json')


if __name__ == '__main__':
    main()
