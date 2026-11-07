#!/usr/bin/env python3
"""Synthetic, game-data-free checks for the boot and versioned benchmark parsers."""

import io
import json
from pathlib import Path
import subprocess
import sys
import unittest
from contextlib import redirect_stdout
from unittest import mock
from uuid import uuid4

import kz3_metrics as metrics

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tools" / "kz3_metrics.py"


def frame_log(durations, clock="benchmark"):
    end = 0
    lines = []
    for duration in durations:
        end += duration
        lines.append(f"[benchmark-frame] clock={clock} end_ms={end} duration_ms={duration}")
    return "\n".join(lines) + "\n"


def analyze(text, window=(0, 0.1), alive=True):
    with mock.patch.object(metrics, "blocks", return_value=iter([text])):
        return metrics.measure_benchmark("synthetic.log", alive, window)


def codes(result):
    return {entry["code"] for entry in result["failures"]}


FRAMES = frame_log([10, 20, 30, 40])
GPU_TAIL = (" (copy MB/frame 1.0): draw 11.0 (2/frame) clear 1.0 (1/frame)"
            " copy 0.5 (1/frame) tail 0.0 (0/frame) vbcopy-list 0.0 (0/frame)")


class BenchmarkFramesTest(unittest.TestCase):
    def test_exact_distributions_and_mean_rate(self):
        r = analyze(FRAMES)
        self.assertEqual(r["status"], "pass")
        f = r["frames"]
        self.assertEqual((f["source"], f["clock"], f["quality"]), ("benchmark-frame", "benchmark", "exact"))
        self.assertEqual(f["sample_count"], 4)
        self.assertEqual(f["fps_mean"], 40)
        self.assertAlmostEqual(f["fps_median"], (50 + 1000 / 30) / 2)
        self.assertEqual(f["frame_time_median_ms"], 25)
        self.assertEqual(f["frame_time_p95_ms"], 40)
        self.assertEqual(f["frame_time_p99_ms"], 40)
        self.assertEqual(f["fps_low_1pct"], 25)
        self.assertIsNone(r["cpu"])
        self.assertIsNone(r["walker"])
        self.assertIsNone(r["candidates"])
        self.assertEqual(r["gpu"]["status"], "unavailable")
        self.assertIsNone(r["gpu"]["metrics"])

    def test_one_percent_low_uses_slowest_ceiling_not_average_of_fps(self):
        r = analyze(frame_log([10] * 198 + [50, 100, 200]), window=(0, 2.33))
        f = r["frames"]
        self.assertEqual(r["status"], "pass")
        self.assertEqual(f["frame_time_p95_ms"], 10)
        self.assertEqual(f["frame_time_p99_ms"], 50)
        self.assertAlmostEqual(f["fps_low_1pct"], 1000 / ((50 + 100 + 200) / 3))

    def test_partial_boundary_frames_are_not_cut_or_counted(self):
        r = analyze(FRAMES, window=(0.005, 0.09))
        f = r["frames"]
        self.assertEqual(r["status"], "pass")
        self.assertEqual((f["actual_start_ms"], f["actual_end_ms"], f["sample_count"]), (10, 60, 2))
        self.assertEqual(f["fps_mean"], 40)
        self.assertIn("partial boundary frames are excluded", f["notes"])

    def test_incomplete_overlapping_and_mixed_frames_fail(self):
        cases = [
            frame_log([10, 20, 30]),
            "[benchmark-frame] end_ms=10 duration_ms=10\n"
            "[benchmark-frame] end_ms=40 duration_ms=20\n"
            "[benchmark-frame] end_ms=100 duration_ms=60\n",
            "[benchmark-frame] end_ms=30 duration_ms=30\n"
            "[benchmark-frame] end_ms=100 duration_ms=80\n",
            "[benchmark-frame] clock=first end_ms=50 duration_ms=50\n"
            "[benchmark-frame] clock=second end_ms=100 duration_ms=50\n",
        ]
        for text in cases:
            with self.subTest(text=text):
                r = analyze(text)
                self.assertEqual(r["status"], "fail")
                self.assertIsNone(r["frames"]["fps_mean"])
                self.assertTrue({"incomplete_frame_window", "mixed_frame_clocks"} & codes(r))

    def test_window_with_no_whole_frame_fails(self):
        r = analyze(frame_log([100]), window=(0.01, 0.02))
        self.assertEqual(r["status"], "fail")
        self.assertIn("incomplete_frame_window", codes(r))

    def test_contiguous_guest_flips_are_approximate_not_present_timestamps(self):
        log = "\n".join(
            f"[flip] {n} at {t} ms host={1000000 + t} ms"
            for n, t in enumerate([0, 10, 30, 60, 100], 1)
        )
        r = analyze(log + "\n")
        self.assertEqual(r["status"], "pass")
        self.assertEqual(r["frames"]["source"], "guest_flip")
        self.assertEqual(r["frames"]["clock"], "flip")
        self.assertEqual(r["frames"]["quality"], "approximate")
        self.assertEqual(r["frames"]["fps_mean"], 40)
        self.assertEqual(r["frames"]["frame_time_p95_ms"], 40)
        self.assertIn("not present timestamps", r["frames"]["notes"][0])

    def test_sampled_or_same_millisecond_flips_never_invent_percentiles(self):
        sparse = "[flip] 1 at 0 ms\n[flip] 11 at 1000 ms\n[flip] 21 at 2000 ms\n[flip] 31 at 3000 ms\n"
        r = analyze(sparse, window=(0.5, 2.5))
        self.assertEqual(r["status"], "pass")
        self.assertEqual(r["frames"]["fps_mean"], 10)
        self.assertEqual((r["frames"]["actual_start_ms"], r["frames"]["actual_end_ms"]), (1000, 2000))
        self.assertIsNone(r["frames"]["sample_count"])
        self.assertIsNone(r["frames"]["frame_time_p95_ms"])
        self.assertIsNone(r["frames"]["fps_low_1pct"])
        self.assertEqual(analyze(sparse, window=(0.5, 1.5))["status"], "fail")
        same = "[flip] 1 at 0 ms\n[flip] 2 at 10 ms\n[flip] 3 at 100 ms\n[flip] 4 at 100 ms\n"
        r = analyze(same)
        self.assertEqual(r["status"], "pass")
        self.assertEqual(r["frames"]["source"], "sampled_guest_flip")
        self.assertIsNone(r["frames"]["frame_time_p99_ms"])

    def test_pace_summary_does_not_invent_p95_or_pooled_quantiles(self):
        first = "[pace] t=10000 n=600 p50=16.62 p99=20.00 max=25.00 sd=2.00 low1=45.00\n"
        r = analyze(first, window=(0, 10))
        self.assertEqual(r["status"], "pass")
        self.assertEqual(r["frames"]["fps_mean"], 60)
        self.assertIsNone(r["frames"]["frame_time_median_ms"])
        self.assertIsNone(r["frames"]["fps_median"])
        self.assertEqual(r["frames"]["frame_time_p99_ms"], 20)
        self.assertEqual(r["frames"]["fps_low_1pct"], 45)
        self.assertIsNone(r["frames"]["frame_time_p95_ms"])
        odd = analyze(first.replace("n=600", "n=601"), window=(0, 10))
        self.assertEqual(odd["frames"]["frame_time_median_ms"], 16.62)
        self.assertAlmostEqual(odd["frames"]["fps_median"], 1000 / 16.62)
        second = "[pace] t=20000 n=500 p50=20 p99=30 max=35 sd=3 low1=25\n"
        r = analyze(first + second, window=(0, 20))
        self.assertEqual(r["status"], "pass")
        self.assertEqual(r["frames"]["fps_mean"], 55)
        for name in ("fps_median", "frame_time_median_ms", "frame_time_p95_ms",
                     "frame_time_p99_ms", "fps_low_1pct"):
            self.assertIsNone(r["frames"][name], name)
        self.assertEqual(analyze(first + second, window=(0, 15))["status"], "fail")

    def test_malformed_pace_or_nonmonotonic_flips_fail(self):
        logs = [
            "[pace] t=10000 n=0\n",
            "[pace] t=10000 n=600 p50=oops p99=20 max=25 sd=2 low1=45\n",
            "[pace] p50=16 p99=20 max=25 sd=2 low1=45\n",
            "[pace] t=20000 n=600 p50=16 p99=20 max=25 sd=2 low1=45\n",
            "[flip] 1 at 0 ms\n[flip] 2 at 100 ms\n[flip] 1 at 101 ms\n",
        ]
        for log in logs:
            with self.subTest(log=log):
                self.assertEqual(analyze(FRAMES + log)["status"], "fail")


class BenchmarkMetricsTest(unittest.TestCase):
    def test_window_metrics_cpu_gpu_and_generic_candidates(self):
        log = FRAMES + """
[benchmark-metric] category=walker start_ms=0 end_ms=50 op=sum run_ms=3 wait_ms=0 park_ms=1
[benchmark-metric] category=walker start_ms=50 end_ms=100 op=sum run_ms=4 wait_ms=1 park_ms=2
[benchmark-metric] category=draw start_ms=0 end_ms=100 op=sum draw_ms=12 draws=4
[benchmark-metric] category=submit start_ms=0 end_ms=100 op=sum submit_ms=3 chunks=2
[benchmark-metric] category=fence start_ms=0 end_ms=100 op=sum wait_ms=2 waits=3
[benchmark-metric] category=writeback start_ms=0 end_ms=100 op=sum bytes=512 writeback_ms=4
[benchmark-metric] category=spu start_ms=0 end_ms=100 op=sum enqueued=6 dispatches=5 run_ms=9
[benchmark-metric] category=spu start_ms=0 end_ms=50 op=max wake_delay_us=2
[benchmark-metric] category=spu start_ms=50 end_ms=100 op=max wake_delay_us=4
[benchmark-metric] category=cpu name=process start_ms=0 end_ms=50 op=sum cpu_ms=20
[benchmark-metric] category=cpu name=process start_ms=50 end_ms=100 op=sum cpu_ms=30
[benchmark-metric] category=cpu name=process start_ms=0 end_ms=50 op=last thread_count=10
[benchmark-metric] category=cpu name=process start_ms=50 end_ms=100 op=last thread_count=11
[benchmark-metric] category=cpu name=rsx start_ms=0 end_ms=100 op=sum cpu_ms=15
[benchmark-metric] category=gpu method=timestamp start_ms=0 end_ms=50 op=sum gpu_ms=7 dropped=0
[benchmark-metric] category=gpu method=timestamp start_ms=50 end_ms=100 op=sum gpu_ms=8 dropped=0
[benchmark-metric] category=candidate name=custom_thing start_ms=0 end_ms=50 op=sum hit=0 tail_miss=1
[benchmark-metric] category=candidate name=custom_thing start_ms=50 end_ms=100 op=sum hit=4 tail_miss=2
"""
        r = analyze(log)
        self.assertEqual(r["status"], "pass")
        self.assertEqual(r["walker"]["metrics"], {"run_ms": 7, "wait_ms": 1, "park_ms": 3})
        self.assertEqual(r["draw"]["metrics"]["draw_ms"], 12)
        self.assertEqual(r["submit"]["metrics"]["chunks"], 2)
        self.assertEqual(r["fence"]["metrics"]["wait_ms"], 2)
        self.assertEqual(r["writeback"]["metrics"]["bytes"], 512)
        self.assertEqual(r["spu"]["metrics"]["dispatches"], 5)
        self.assertEqual(r["spu"]["metrics"]["wake_delay_us"], 4)
        self.assertEqual(r["spu"]["aggregation"]["wake_delay_us"], "max")
        self.assertEqual(r["cpu"]["process"]["metrics"], {"cpu_ms": 50, "thread_count": 11})
        self.assertEqual(r["cpu"]["threads"]["rsx"]["metrics"]["cpu_ms"], 15)
        self.assertEqual(r["gpu"]["status"], "exact")
        self.assertEqual(r["gpu"]["metrics"], {"gpu_ms": 15, "dropped": 0})
        self.assertEqual(r["candidates"]["custom_thing"]["metrics"], {"hit": 4, "tail_miss": 3})

    def test_recorded_zero_is_not_missing_and_large_counters_are_exact(self):
        log = FRAMES + ("[benchmark-metric] category=candidate name=custom start_ms=0 end_ms=100 op=sum "
                        "zero=0 huge=18446744073709551615\n")
        r = analyze(log)
        self.assertEqual(r["status"], "pass")
        self.assertEqual(r["candidates"]["custom"]["metrics"]["zero"], 0)
        self.assertEqual(r["candidates"]["custom"]["metrics"]["huge"], 18446744073709551615)

    def test_missing_metric_in_one_interval_is_null_and_fails(self):
        log = FRAMES + """
[benchmark-metric] category=walker start_ms=0 end_ms=50 op=sum run_ms=4 park_ms=0
[benchmark-metric] category=walker start_ms=50 end_ms=100 op=sum run_ms=5
"""
        r = analyze(log)
        self.assertEqual(r["status"], "fail")
        self.assertEqual(r["walker"]["metrics"]["run_ms"], 9)
        self.assertIsNone(r["walker"]["metrics"]["park_ms"])
        self.assertFalse(r["walker"]["complete"])
        self.assertIn("incomplete_metric_window", codes(r))

    def test_gaps_overlaps_clipped_intervals_and_changed_operations_fail(self):
        cases = [
            ("0", "40", "60", "100", "sum", "sum"),
            ("0", "60", "50", "100", "sum", "sum"),
            ("0", "50", "50", "150", "sum", "sum"),
            ("0", "50", "50", "100", "sum", "max"),
        ]
        for a, b, c, d, first_op, second_op in cases:
            with self.subTest(case=(a, b, c, d, first_op, second_op)):
                log = FRAMES + f"""
[benchmark-metric] category=spu start_ms={a} end_ms={b} op={first_op} run_ms=1
[benchmark-metric] category=spu start_ms={c} end_ms={d} op={second_op} run_ms=2
"""
                r = analyze(log)
                self.assertEqual(r["status"], "fail")
                self.assertIsNone(r["spu"]["metrics"]["run_ms"])

    def test_other_clock_is_unscoped_not_a_falsely_aligned_window(self):
        log = FRAMES + "[benchmark-metric] clock=rsx_perf category=cpu name=process start_ms=0 end_ms=100 op=sum cpu_ms=60\n"
        r = analyze(log)
        self.assertEqual(r["status"], "pass")
        self.assertIsNone(r["cpu"])
        self.assertEqual(r["unscoped_metrics"][0]["clock"], "rsx_perf")
        self.assertEqual(r["unscoped_metrics"][0]["metrics"]["cpu_ms"], 60)

    def test_gpu_fence_poll_is_approximate_and_loss_is_not_zero_by_default(self):
        record = "[benchmark-metric] category=gpu method=fence_poll start_ms=0 end_ms=100 op=sum gpu_ms=14 missed=0\n"
        r = analyze(FRAMES + record)
        self.assertEqual((r["status"], r["gpu"]["status"]), ("pass", "approximate"))
        exact = "[benchmark-metric] category=gpu method=timestamp start_ms=0 end_ms=100 op=sum gpu_ms=10 dropped=0\n"
        r = analyze(FRAMES + record + exact)
        self.assertEqual(r["gpu"]["status"], "exact")
        self.assertEqual(r["gpu"]["alternatives"][0]["status"], "approximate")
        self.assertEqual(r["gpu"]["alternatives"][0]["metrics"]["gpu_ms"], 14)
        r = analyze(FRAMES + record.replace(" missed=0", ""))
        self.assertEqual((r["status"], r["gpu"]["status"]), ("fail", "unavailable"))
        self.assertIn("incomplete_gpu_window", codes(r))
        r = analyze(FRAMES + record.replace("missed=0", "missed=1"))
        self.assertEqual((r["status"], r["gpu"]["status"]), ("fail", "unavailable"))

    def test_timestamp_loss_and_nonadditive_timing_are_not_exact(self):
        bad_loss = ("[benchmark-metric] category=gpu method=timestamp start_ms=0 end_ms=100 op=sum "
                    "gpu_ms=5 dropped=0 dropped_samples=2\n")
        self.assertEqual(analyze(FRAMES + bad_loss)["gpu"]["status"], "unavailable")
        last_only = """
[benchmark-metric] category=gpu method=timestamp start_ms=0 end_ms=50 op=last gpu_ms=2 dropped=0
[benchmark-metric] category=gpu method=timestamp start_ms=50 end_ms=100 op=last gpu_ms=3 dropped=0
"""
        r = analyze(FRAMES + last_only)
        self.assertEqual(r["status"], "fail")
        self.assertEqual(r["gpu"]["status"], "unavailable")
        per_frame = ("[benchmark-metric] category=gpu method=timestamp start_ms=0 end_ms=100 op=last "
                     "gpu_ms_per_frame=12 dropped=0")
        self.assertEqual(analyze(FRAMES + per_frame + "\n")["gpu"]["status"], "unavailable")
        self.assertEqual(analyze(FRAMES + per_frame + " frames=4\n")["gpu"]["status"], "exact")

    def test_loss_in_any_gpu_interval_cannot_be_hidden_by_last_zero(self):
        for method, loss in (("timestamp", "dropped"), ("fence_poll", "missed")):
            with self.subTest(method=method):
                log = FRAMES + f"""
[benchmark-metric] category=gpu method={method} start_ms=0 end_ms=50 op=sum gpu_ms=2
[benchmark-metric] category=gpu method={method} start_ms=50 end_ms=100 op=sum gpu_ms=3
[benchmark-metric] category=gpu method={method} start_ms=0 end_ms=50 op=last {loss}=1
[benchmark-metric] category=gpu method={method} start_ms=50 end_ms=100 op=last {loss}=0
"""
                r = analyze(log)
                self.assertEqual((r["status"], r["gpu"]["status"]), ("fail", "unavailable"))
                self.assertIn("incomplete_gpu_window", codes(r))
                negative = log.replace(f"{loss}=1", f"{loss}=-1")
                self.assertIn("malformed_record", codes(analyze(negative)))

    def test_invalid_records_are_failures(self):
        records = [
            "[benchmark-frame] end_ms=NaN duration_ms=1",
            "[benchmark-frame] end_ms=10 duration_ms=0",
            "[benchmark-frame] end_ms=10 duration_ms=10 duration_ms=10",
            "[benchmark-metric] category=walker start_ms=0 end_ms=100 run_ms=1",
            "[benchmark-metric] category=walker start_ms=0 end_ms=100 op=mean run_ms=1",
            "[benchmark-metric] category=cpu start_ms=0 end_ms=100 op=sum cpu_ms=1",
            "[benchmark-metric] category=spu start_ms=0 end_ms=100 op=sum run_ms=-1",
            "[benchmark-metric] category=candidate name=demo start_ms=0 end_ms=100 op=sum hits=inf",
            "[benchmark-new] foo=3",
        ]
        for record in records:
            with self.subTest(record=record):
                r = analyze(FRAMES + record + "\n")
                self.assertEqual(r["status"], "fail")
                self.assertIn("malformed_record", codes(r))


class NativeObservationsTest(unittest.TestCase):
    def test_unaligned_native_metrics_are_reported_with_separate_clocks(self):
        log = FRAMES + """
[perf] t=2s wall=1000ms flips=1 fifo-flips=1 presents=1 draws=12 | walk n=2 10.1ms methods=42 | flush n=3 (present 1, ref 1, other 1) 2.0ms gpuwait 1.0ms | present 2.0ms swap 0.1ms | refpoll=4 | usleep gcm n=2 3.0ms other 4.0ms
[perf-thr] cpu=150ms threads=8: rsx=50 ppu:Render Thread=70
[perf-walk] t=2s draw 15.1ms (vtx 2.0, pso 1.0 new 2/0.5ms, const 0.1, tex 1.2 new 3/0.2ms) clear 0.1 | submit 2.5ms chunks 3 | gpuwait rb 1.0ms other 0.5ms | wb nv0039 1/2.0ms sync 2/3.0ms store 0.2ms | m2mf 0.1ms | tex-hash 0.2ms | pso: pre 0.1 units 0.2 key 0.3 find 0.4 memo-hits 5 | cbv copy 4 reuse 6
""" + "".join(f"[perf-gpu] t={i}s lists 4 busy 2.1 ms of 1000 longest 1.1 missed 0\n"
              for i in range(1, 11)) + "[perf-gt] t=10s frames 600 dropped 0 gpu 12.5 ms/frame" + GPU_TAIL + """
[spurs-cpu] Worker k1 cpu=14 ms/s yields=2/s sleeps=6/s
[spurs-ready] t=10s store=4/2 swap=3/1 cas=6/2 (calls/byte changed) per wid: none
[wacct] ---- t=10.0s window=10000ms flips=600
[wacct] thr=RSX walker wait=20ms park=8/3 pmrun=5/2 ksleep=3/1 gap:wid2=0.1ms*4 cpums=6 tid=7 cpus=1:3, | park:x=8.0ms/3
[usercmd-wait] t=10000 n=4 coll=2 replaced=0 wait_ms=2.5 max_ms=1.5 all_n=8 all_coll=4 all_replaced=0
[cellGcmSys] park idle wake: sleeps 8, park waits woken by the PUT 2, released before 1, timed out 3; PUTs over a published park 4
"""
        r = analyze(log)
        self.assertEqual(r["status"], "pass")
        self.assertIsNone(r["cpu"])
        self.assertIsNone(r["walker"])
        self.assertEqual(r["gpu"]["status"], "unavailable")
        native = r["native"]
        self.assertEqual(native["rsx_perf"]["clock"], "rsx_perf")
        self.assertEqual(native["rsx_perf"]["samples"][0]["walker"]["run_ms"], 10.1)
        self.assertEqual(native["rsx_perf"]["samples"][0]["fence"]["cpu_wait_ms"], 1)
        self.assertEqual(native["rsx_perf"]["thread_samples"][0]["top_threads"][1]["name"], "ppu:Render Thread")
        split = native["rsx_perf"]["walker_split"][0]
        self.assertEqual(split["writeback_sync_ms"], 3)
        self.assertEqual(split["candidates"]["ld_cb_reuse"]["reuse"], 6)
        self.assertEqual(native["gpu_monitor"]["timestamp_samples"][0]["status"], "exact")
        self.assertEqual(native["gpu_monitor"]["fence_poll_samples"][0]["status"], "approximate")
        self.assertEqual(native["spurs_cpu"]["samples"][0]["sleeps_per_s"], 6)
        self.assertTrue(native["spurs_ready"]["samples"][0]["cumulative"])
        thread = native["wait_accounting"]["windows"][0]["threads"][0]
        self.assertEqual(thread["cpu_ms"], 6)
        self.assertEqual(thread["host_tid"], 7)
        self.assertEqual(thread["logical_cpu_samples"], [{"cpu": 1, "samples": 3}])
        self.assertEqual(thread["gaps"], [{"workload": 2, "mean_ms": 0.1, "count": 4}])
        self.assertEqual(thread["kinds"]["park"], {"ms": 8, "count": 3})
        self.assertEqual(thread["sites"], [{"kind": "park", "site": "x", "ms": 8.0, "count": 3}])
        self.assertNotIn("job", thread["kinds"])
        self.assertEqual(native["usercmd_wait"]["clock"], "ps3_ms_now")
        self.assertEqual(native["park_wake"]["samples"][0]["put_wakes"], 2)

    def test_park_idle_counts_and_host_clock_are_not_invented_window_metrics(self):
        record = ("[park-idle] t_ms=123456 sleeps=8 parks=9 direct=2 pre_wait=1 timeout=3 "
                  "spurious=4 wait_failed=1 shutdown=2 overlaps=6 signaled=6 park_ns=12345 "
                  "progress=3 wake_progress_ns=500 wake_progress_max_ns=300\n")
        legacy = ("[cellGcmSys] park idle wake: sleeps 8, park waits woken by the PUT 2, "
                  "released before 1, timed out 3; PUTs over a published park 4\n")
        result = analyze(FRAMES + legacy + record)
        self.assertEqual(result["status"], "pass")
        self.assertIsNone(result["candidates"])
        group = result["native"]["park_wake"]
        self.assertIsNone(group["clock"])
        self.assertEqual(group["samples"][0]["put_wakes"], 2)
        self.assertEqual(group["samples"][1], {
            "cumulative": True, "clock": "GetTickCount64", "end_ms": 123456,
            "sleeps": 8, "parks": 9, "direct_signaled_releases": 2, "released_before_wait": 1,
            "timeouts": 3, "spurious_signals": 4, "wait_failures": 1, "shutdowns": 2,
            "overlapping_puts": 6, "wait_signals": 6, "park_time_ns": 12345, "walker_progress": 3,
            "wake_progress_ns": 500, "wake_progress_max_ns": 300,
        })

    def test_malformed_park_idle_record_is_failure(self):
        record = ("[park-idle] t_ms=1 sleeps=0 parks=1 direct=0 pre_wait=0 timeout=1 "
                  "spurious=0 wait_failed=0 shutdown=0 overlaps=0 signaled=0 park_ns=100 "
                  "progress=0 wake_progress_ns=0 wake_progress_max_ns=0")
        for bad in (record.replace(" park_ns=100", ""),
                    record + " direct=3", record + " extra=0", record.replace("direct=0", "direct=-1"),
                    record.replace("parks=1", "parks=18446744073709551616"),
                    record.replace("timeout=1", "timeout=nan")):
            with self.subTest(record=bad):
                result = analyze(FRAMES + bad + "\n")
                self.assertEqual(result["status"], "fail")
                self.assertIn("malformed_candidate", codes(result))

    def test_missing_wacct_cpu_stays_missing(self):
        log = FRAMES + "[wacct] ---- t=10s window=10000ms flips=20\n[wacct] thr=worker wait=2ms park=2/1 |\n"
        r = analyze(log)
        self.assertEqual(r["status"], "pass")
        thread = r["native"]["wait_accounting"]["windows"][0]["threads"][0]
        self.assertIsNone(thread["cpu_ms"])
        self.assertEqual(thread["sites"], [])
        malformed = "[wacct] ---- t=10s window=10000ms flips=20\n[wacct] thr=worker wait=2ms park=bad |\n"
        self.assertIn("malformed_wacct", codes(analyze(FRAMES + malformed)))

    def test_wacct_sites_preserve_kinds_names_times_counts_and_order(self):
        log = FRAMES + """
[wacct] ---- t=10s window=10000ms flips=20
[wacct] thr=worker name wait=20ms | hle:cellFsRead=12.3ms/4 sc:sc141=0ms/0 usleep:lr00ABCDEF=.5ms/8 label:label4=1e-1ms/2 pmrun:wid3=2ms/1 job:ovl9=3ms/6 ksleep:0=4.0ms/10 park:0=5ms/11 hle:?=6ms/12 hle:with spaces=7ms/13 hle:ns::function=8ms/14 hle:cellFsRead=9ms/15
"""
        r = analyze(log)
        self.assertEqual(r["status"], "pass")
        sites = r["native"]["wait_accounting"]["windows"][0]["threads"][0]["sites"]
        self.assertEqual(sites, [
            {"kind": "hle", "site": "cellFsRead", "ms": 12.3, "count": 4},
            {"kind": "sc", "site": "sc141", "ms": 0, "count": 0},
            {"kind": "usleep", "site": "lr00ABCDEF", "ms": 0.5, "count": 8},
            {"kind": "label", "site": "label4", "ms": 0.1, "count": 2},
            {"kind": "pmrun", "site": "wid3", "ms": 2, "count": 1},
            {"kind": "job", "site": "ovl9", "ms": 3, "count": 6},
            {"kind": "ksleep", "site": "0", "ms": 4.0, "count": 10},
            {"kind": "park", "site": "0", "ms": 5, "count": 11},
            {"kind": "hle", "site": "?", "ms": 6, "count": 12},
            {"kind": "hle", "site": "with spaces", "ms": 7, "count": 13},
            {"kind": "hle", "site": "ns::function", "ms": 8, "count": 14},
            {"kind": "hle", "site": "cellFsRead", "ms": 9, "count": 15},
        ])
        max_uint64 = 18446744073709551615
        edge = (FRAMES + "[wacct] ---- t=10s window=10000ms flips=20\n"
                f"[wacct] thr=worker wait=2ms | park:0=0ms/{max_uint64}\n")
        r = analyze(edge)
        self.assertEqual(r["status"], "pass")
        self.assertEqual(r["native"]["wait_accounting"]["windows"][0]["threads"][0]["sites"][0]["count"],
                         max_uint64)

    def test_malformed_wacct_sites_fail_instead_of_being_dropped(self):
        bad_sites = [
            "garbage", "park:=1ms/2", "park:0=oopsms/2", "park:0=NaNms/2", "park:0=1e999ms/2",
            "park:0=1e-9999ms/2",
            "park:0=-1ms/2", "park:0=1ms/-2", "park:0=1ms/", "park:0=1ms/2junk",
            "park:0=1ms/2 garbage", "park:0=1ms/2 park:1=2ms/",
            "park:broken hle:valid=2ms/1",
        ]
        header = FRAMES + "[wacct] ---- t=10s window=10000ms flips=20\n"
        for sites in bad_sites:
            with self.subTest(sites=sites):
                r = analyze(header + f"[wacct] thr=worker wait=2ms | {sites}\n")
                self.assertEqual(r["status"], "fail")
                self.assertIn("malformed_wacct", codes(r))
                self.assertEqual(r["native"]["wait_accounting"]["windows"][0]["threads"], [])

    def test_bad_wacct_window_does_not_inherit_the_previous_header(self):
        log = (FRAMES + "[wacct] ---- t=10s window=10000ms flips=20\n"
               f"[wacct] ---- t=20s window={'9' * 401}ms flips=20\n"
               "[wacct] thr=worker wait=2ms | park:0=2ms/1\n")
        r = analyze(log)
        self.assertEqual(r["status"], "fail")
        self.assertIn("malformed_wacct", codes(r))
        self.assertEqual(r["native"]["wait_accounting"]["windows"][0]["threads"], [])

    def test_bad_native_samples_and_dropped_gpu_samples_fail(self):
        lines = [
            "[perf] t=2s wall=1000ms flips=1",
            "[perf] wall=1000ms flips=1",
            ("[perf] t=2s wall=1000ms flips=1 fifo-flips=1 presents=1 draws=12 | walk n=2 10.1ms methods=42 "
             "| flush n=3 (present 1, ref 1, other 1) 2.0ms gpuwait 1.0ms | present 2.0ms swap 0.1ms "
             "| refpoll=4 | usleep gcm n=2 3.0ms other 4.0ms junk"),
            "[perf-walk] t=2s draw 3",
            "[perf-walk] draw 3",
            "[perf-thr] cpu=12ms threads=2: broken",
            "[wacct] thr=worker wait=2ms |",
            "[wacct] --- t=10s",
            "[perf-gpu] t=2s lists 4 busy 2 ms of 1000 longest 1 missed 1",
            "[perf-gpu] lists 4",
            "[perf-gt] CreateQueryHeap failed",
            "[perf-gt] t=10s frames 600 dropped 1 gpu 12.5 ms/frame" + GPU_TAIL,
            "[perf-gt] t=10s frames 0 dropped 0 gpu 0.0 ms/frame" + GPU_TAIL,
            "[perf-gt] t=10s frames 600 dropped 0 gpu 12.5 ms/frame",
            "[perf-gt] t=10s frames 600 dropped 0 gpu 12.5 ms/frame" + GPU_TAIL,
        ]
        for line in lines:
            with self.subTest(line=line):
                self.assertEqual(analyze(FRAMES + line + "\n")["status"], "fail")

    def test_native_instrumentation_announcements_are_not_samples(self):
        log = (FRAMES + "[perf] RSX_PERF=1: per-second host timing on (walker tid 1)\n"
               "[pace] fps limit 60 (vblank tick 8 ms)\n"
               "[wacct] on: 10000 ms windows (KZ3 patch 1032)\n"
               "[perf-gt] GPU timestamps on: ring 32768, freq 1000000 Hz\n")
        self.assertEqual(analyze(log)["status"], "pass")

    def test_native_gpu_timestamp_needs_all_loss_free_fence_polls(self):
        polls = "".join(f"[perf-gpu] t={i}s lists 1 busy 1 ms of 1000 longest 1 missed {int(i == 3)}\n"
                        for i in range(1, 11))
        r = analyze(FRAMES + polls + "[perf-gt] t=10s frames 600 dropped 0 gpu 12.5 ms/frame" + GPU_TAIL + "\n")
        self.assertEqual(r["status"], "fail")
        sample = r["native"]["gpu_monitor"]["timestamp_samples"][0]
        self.assertEqual(sample["status"], "unavailable")
        self.assertIsNone(sample["gpu_ms_per_frame"])


class FailureAndCompatibilityTest(unittest.TestCase):
    def test_crash_pad_warnings_exit_and_liveness_fail_without_a_baseline(self):
        cases = [
            ("[CRASH] code=0xC0000005", "crash_marker"),
            ("[null-call] stopped", "crash_marker"),
            ("[HardExit] exited", "crash_marker"),
            ("[HARDEXIT] exited", "crash_marker"),
            ("[fatal] stopped", "crash_marker"),
            ("[FATAL] stopped", "crash_marker"),
            ("[exit] SPU pool did not drain; forced guest exit code 3", "crash_marker"),
            ("[exit] forced exit with undrained SPU workers", "crash_marker"),
            ("[cellPad] PAD_SCRIPT: WARNING event dropped", "pad_script_warning"),
            ("[cellPad] PAD_SCRIPT_FILE: WARNING cannot read script", "pad_script_warning"),
            ("[exit code -1]", "nonzero_exit"),
            ("[exit code", "malformed_exit"),
        ]
        for line, code in cases:
            with self.subTest(line=line):
                r = analyze(FRAMES + line + "\n")
                self.assertEqual(r["status"], "fail")
                self.assertIn(code, codes(r))
        self.assertIn("not_alive", codes(analyze(FRAMES, alive=False)))

    def test_oversized_native_integers_produce_structured_cli_failures(self):
        walk = ("[perf-walk] t=2s draw 15.1ms (vtx 2, pso 1 new 2/0.5ms, const 0.1, tex 1 new 3/0.2ms) "
                "clear 0.1 | submit 2.5ms chunks 3 | gpuwait rb 1 other 0.5 | wb nv0039 1/2ms "
                "sync 2/3ms store 0.2ms | m2mf 0.1ms | tex-hash 0.2ms | pso: pre 0.1 units 0.2 "
                "key 0.3 find 0.4 memo-hits {huge}")
        cmd = [sys.executable, "-B", str(SCRIPT), "-", "--benchmark-v1", "--window", "0,0.1"]
        for digits in (401, 5000):
            huge = "9" * digits
            cases = {
                "flip_count": f"[flip] 1 at 0 ms\n[flip] {huge} at 100 ms\n",
                "flip_time": FRAMES + f"[flip] 1 at {huge} ms\n",
                "exit_code": FRAMES + f"[exit code -{huge}]\n",
                "exit_code_positive": FRAMES + f"[exit code {huge}]\n",
                "exit_code_zeros": FRAMES + f"[exit code {'0' * digits}]\n",
                "pace_count": FRAMES + f"[pace] t=10000 n={huge} p50=16 p99=20 max=25 sd=2 low1=45\n",
                "perf_threads": FRAMES + f"[perf-thr] cpu=1ms threads={huge}:\n",
                "perf_cpu_time": FRAMES + f"[perf-thr] cpu={huge}ms threads=1:\n",
                "gpu_poll": FRAMES + f"[perf-gpu] t=1s lists {huge} busy 1 ms of 1000 longest 1 missed 0\n",
                "gpu_ignored_number": (FRAMES + "[perf-gt] t=10s frames 1 dropped 0 gpu 1 ms/frame" +
                                       GPU_TAIL.replace("draw 11.0", f"draw {huge}") + "\n"),
                "walk_ignored_count": FRAMES + walk.format(huge=huge) + "\n",
                "spurs_thread": FRAMES + f"[spurs-cpu] worker k{huge} cpu=1 ms/s yields=1/s sleeps=1/s\n",
                "spurs_workload": (FRAMES + "[spurs-ready] t=10s store=1/1 swap=1/1 cas=1/1 "
                                   f"(calls/byte changed) per wid: w{huge}:1/1,1/1,1/1\n"),
                "wacct_header": FRAMES + f"[wacct] ---- t=10s window={huge}ms flips=1\n",
                "wacct_site": (FRAMES + "[wacct] ---- t=10s window=10000ms flips=1\n"
                               f"[wacct] thr=worker wait=2ms | park:0=1ms/{huge}\n"),
                "wacct_site_time": (FRAMES + "[wacct] ---- t=10s window=10000ms flips=1\n"
                                    f"[wacct] thr=worker wait=2ms | park:0={huge}.0ms/1\n"),
                "wacct_site_id": (FRAMES + "[wacct] ---- t=10s window=10000ms flips=1\n"
                                  f"[wacct] thr=worker wait=2ms | pmrun:wid{huge}=1ms/1\n"),
                "wacct_kind": (FRAMES + "[wacct] ---- t=10s window=10000ms flips=1\n"
                               f"[wacct] thr=worker wait=2ms park={huge}/1 |\n"),
                "usercmd": (FRAMES + f"[usercmd-wait] t=1 n={huge} coll=0 replaced=0 wait_ms=0 max_ms=0 "
                            "all_n=1 all_coll=0 all_replaced=0\n"),
                "park_wake": (FRAMES + f"[cellGcmSys] park idle wake: sleeps {huge}, park waits woken by the PUT 0, "
                              "released before 0, timed out 0; PUTs over a published park 0\n"),
            }
            for name, log in cases.items():
                with self.subTest(digits=digits, source=name):
                    process = subprocess.run(cmd, input=log, text=True, capture_output=True, cwd=ROOT, check=False)
                    self.assertEqual(process.returncode, 1, process.stderr)
                    result = json.loads(process.stdout)
                    self.assertEqual(result["schema"], "kz3-benchmark-v1")
                    self.assertEqual(result["status"], "fail")
                    self.assertTrue(result["failures"])
                    self.assertNotIn("Traceback", process.stderr)

    def test_native_integer_bounds_and_pace_arithmetic_are_guarded(self):
        max_uint64 = 18446744073709551615
        valid = f"[flip] 1 at 0 ms\n[flip] {max_uint64} at 100 ms\n"
        result = analyze(valid)
        self.assertEqual(result["status"], "pass")
        self.assertTrue(result["frames"]["fps_mean"] > 0)
        self.assertEqual(analyze(valid.replace(str(max_uint64), str(max_uint64 + 1)))["status"], "fail")
        self.assertEqual(analyze(FRAMES + f"[perf-thr] cpu={max_uint64 + 1}ms threads=1:\n")["status"], "fail")
        tiny = "[pace] t=10000 n=1 p50=1e-320 p99=1e-320 max=1 sd=0 low1=1\n"
        self.assertEqual(analyze(tiny, window=(0, 10))["status"], "fail")

    def test_native_integer_fields_share_the_same_bounds(self):
        header = FRAMES + "[wacct] ---- t=10s window=10000ms flips=20\n"
        perf = ("[perf] t=2s wall=1000ms flips=1 fifo-flips=1 presents=1 draws={n} | walk n=2 10.1ms methods=42 "
                "| flush n=3 (present 1, ref 1, other 1) 2ms gpuwait 1ms | present 2ms swap 0.1ms "
                "| refpoll=4 | usleep gcm n=2 3ms other 4ms\n")
        cases = [
            FRAMES + perf,
            FRAMES + "[perf-gt] t=10s frames {n} dropped 0 gpu 12.5 ms/frame" + GPU_TAIL + "\n",
            header + "[wacct] thr=worker wait={n}ms |\n",
            header + "[wacct] thr=worker wait=2ms cpums={n} |\n",
            header + "[wacct] thr=worker wait=2ms tid={n} |\n",
            header + "[wacct] thr=worker wait=2ms kskip={n} |\n",
            header + "[wacct] thr=worker wait=2ms gap:wid{n}=0.1ms*4 |\n",
            header + "[wacct] thr=worker wait=2ms gap:wid1=0.1ms*{n} |\n",
            header + "[wacct] thr=worker wait=2ms cpus={n}:3, |\n",
            header + "[wacct] thr=worker wait=2ms cpus=1:{n}, |\n",
            header + "[wacct] thr=worker wait=2ms | hle:nid{n}=1ms/1\n",
        ]
        for digits in (21, 401, 5000):
            number = "9" * digits
            for record in cases:
                with self.subTest(digits=digits, record=record[:100]):
                    r = analyze(record.format(n=number))
                    self.assertEqual(r["status"], "fail")
                    self.assertTrue(r["failures"])

    def test_arithmetic_and_serialization_fallbacks_remain_structured(self):
        with mock.patch.object(metrics.BenchmarkV1, "finish", side_effect=OverflowError):
            r = analyze(FRAMES + "[fatal] synthetic\n")
        self.assertEqual(r["status"], "fail")
        self.assertIn("crash_marker", codes(r))
        self.assertIn("numeric_overflow", codes(r))

        invalid_result = {"status": "fail", "value": float("inf"),
                          "failures": [{"code": "crash_marker", "line": 5, "message": "crash marker"}]}
        out = io.StringIO()
        with mock.patch.object(metrics, "measure_benchmark", return_value=invalid_result), \
                mock.patch.object(sys, "argv", ["kz3_metrics.py", "-", "--benchmark-v1", "--window", "0,0.1"]), \
                redirect_stdout(out):
            with self.assertRaises(SystemExit) as caught:
                metrics.main()
        self.assertEqual(caught.exception.code, 1)
        fallback = json.loads(out.getvalue())
        self.assertIn("crash_marker", codes(fallback))
        self.assertIn("numeric_overflow", codes(fallback))

    def test_invalid_windows_are_rejected(self):
        for window in (None, (), (0,), (0, 1, 2), (-1, 2), (1, 1), (2, 1), (0, float("nan")), (0, float("inf"))):
            with self.subTest(window=window):
                with self.assertRaises(ValueError):
                    metrics._bm_window(window)

    def test_unterminated_final_record_is_a_failure(self):
        r = analyze(FRAMES.rstrip("\n"))
        self.assertEqual(r["status"], "fail")
        self.assertIn("truncated_log", codes(r))

    def test_legacy_boot_output_and_measure_are_unchanged(self):
        log = ("[flip] 1 at 1000 ms\n[flip] 11 at 2000 ms\n"
               "[cellPad] PAD_SCRIPT: WARNING dropped\n"
               "[pace] t=10000 n=600 p50=16.62 p99=20 max=25 sd=2 low1=45\n")
        with mock.patch.object(metrics, "blocks", side_effect=lambda _: iter([log])):
            m = metrics.measure("synthetic.log", True)
            self.assertEqual(m["fps_avg"], 10)
            self.assertEqual(m["fps_low1pct"], 45)
            self.assertEqual(m["script_dropped"], 1)
            out = io.StringIO()
            with mock.patch.object(sys, "argv", ["kz3_metrics.py", "synthetic.log"]), redirect_stdout(out):
                with self.assertRaises(SystemExit) as caught:
                    metrics.main()
            self.assertEqual(caught.exception.code, 0)
        self.assertIn("metric", out.getvalue())
        self.assertIn("PASS (no baseline yet)", out.getvalue())
        self.assertNotIn("kz3-benchmark-v1", out.getvalue())

    def test_real_cli_json_exit_codes_and_out_file(self):
        name = f".metrics-test-{uuid4().hex}.json"
        output = ROOT / name
        cmd = [sys.executable, "-B", str(SCRIPT), "-", "--benchmark-v1", "--window", "0,0.1", "--out", name]
        try:
            ok = subprocess.run(cmd, input=FRAMES, text=True, capture_output=True, cwd=ROOT, check=False)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            self.assertEqual(json.loads(ok.stdout)["status"], "pass")
            self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["status"], "pass")
            bad = subprocess.run(cmd, input=FRAMES + "[CRASH] synthetic\n", text=True, capture_output=True,
                                 cwd=ROOT, check=False)
            self.assertEqual(bad.returncode, 1, bad.stderr)
            self.assertEqual(json.loads(bad.stdout)["status"], "fail")
            self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["status"], "fail")
            for marker in ("[HardExit]", "[fatal]"):
                bad = subprocess.run(cmd, input=FRAMES + marker + " synthetic\n", text=True, capture_output=True,
                                     cwd=ROOT, check=False)
                self.assertEqual(bad.returncode, 1, bad.stderr)
                self.assertIn("crash_marker", codes(json.loads(bad.stdout)))
                self.assertIn("crash_marker", codes(json.loads(output.read_text(encoding="utf-8"))))
            huge_exit = subprocess.run(cmd, input=FRAMES + f"[exit code {'9' * 5000}]\n", text=True,
                                       capture_output=True, cwd=ROOT, check=False)
            self.assertEqual(huge_exit.returncode, 1, huge_exit.stderr)
            self.assertIn("malformed_exit", codes(json.loads(huge_exit.stdout)))
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), json.loads(huge_exit.stdout))
            malformed = subprocess.run(cmd + ["--window", "0,nan"], input=FRAMES, text=True,
                                       capture_output=True, cwd=ROOT, check=False)
            self.assertNotEqual(malformed.returncode, 0)
            self.assertEqual(json.loads(malformed.stdout)["failures"][0]["code"], "invalid_window")
            self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["status"], "fail")
            missing_cmd = cmd.copy()
            missing_cmd[3] = f".nonexistent-{uuid4().hex}.log"
            missing = subprocess.run(missing_cmd, text=True, capture_output=True, cwd=ROOT, check=False)
            self.assertNotEqual(missing.returncode, 0)
            self.assertEqual(json.loads(missing.stdout)["failures"][0]["code"], "input_error")
            self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["status"], "fail")
        finally:
            if output.exists():
                output.unlink()


if __name__ == "__main__":
    unittest.main()
