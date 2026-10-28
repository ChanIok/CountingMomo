"""名称锚点驱动的逐屏扫描：一次标定、固定大步、有界并行模板识别。"""

import argparse
import json
from datetime import datetime
from pathlib import Path
from time import monotonic, perf_counter

from layout import load_layout
from report import write_reports
from scan import positive_int
from recognition import DEFAULT_FONT
from scrolling import (CALIBRATION_NOTCHES, MAX_WHEEL_NOTCHES, calibrated_notches,
                       target_displacement, verify_response)
from stability import PageWaitError, frame_image, frame_info, wait_frame, wait_page
from tracking import assign_records, make_anchor
from window_input import (activate, choose_scroll_point, client_geometry, enable_physical_coordinates,
                          find_window, list_windows, position_pointer, prepare_scan_window, scroll_down, stop_pressed,
                          user32, window_pid, window_title)


class ScanStopped(Exception):
    pass


def positive_float(text):
    import math

    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("必须是有限正数")
    return value


def parse_args():
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="扫描当前图鉴分类；请手动滚到顶部。F8 停止。")
    parser.add_argument("--list-windows", action="store_true")
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--hwnd", type=lambda value: int(value, 0))
    target.add_argument("--window-title", default="无限暖暖")
    parser.add_argument("--category")
    parser.add_argument("--layout", type=Path, default=root / "layouts/portrait_1_4.json")
    parser.add_argument("--output", type=Path, help="新建结果目录，不覆盖已有文件")
    parser.add_argument("--capture-only", action="store_true", help="只保存 WGC 客户区截图，不移动鼠标或滚动")
    parser.add_argument("--font", type=Path, default=DEFAULT_FONT, help="游戏TrueType字体文件")
    parser.add_argument("--batch-size", type=positive_int, default=16)
    parser.add_argument("--queue-pages", type=positive_int, default=2)
    parser.add_argument("--wheel-notches", type=positive_int, help="手动固定幅度；省略时先小步标定一次，再固定大步")
    parser.add_argument("--overlap-rows", type=positive_int, default=6, help="标定大步时预留的重叠行数，至少 2 行")
    parser.add_argument("--scroll-point", nargs=2, type=int, metavar=("X", "Y"), help="参考分辨率下的客户区坐标；默认选择可见列表右侧")
    parser.add_argument("--max-scrolls", type=positive_int, default=1000)
    parser.add_argument("--stable-seconds", type=positive_float, default=0.25, help="名称及星形前景在不同原始帧中保持稳定的时长")
    parser.add_argument("--frame-timeout", type=positive_float, default=8.0, help="一次采样事务超时，不在超时后追加输入")
    parser.add_argument("--expected-count", type=positive_int, help="含未拥有的当前分类总数")
    args = parser.parse_args()
    if not args.list_windows and not args.capture_only and not args.category:
        parser.error("扫描需要 --category")
    if args.frame_timeout <= args.stable_seconds:
        parser.error("--frame-timeout 必须大于 --stable-seconds")
    if args.overlap_rows < 2:
        parser.error("--overlap-rows 至少为 2")
    if args.wheel_notches is not None and args.wheel_notches > MAX_WHEEL_NOTCHES:
        parser.error(f"--wheel-notches 不能超过 {MAX_WHEEL_NOTCHES}")
    return args, root


def main():
    args, root = parse_args()
    enable_physical_coordinates()
    if args.list_windows:
        for window in list_windows():
            print(f"{window['hwnd']:#x}\t{window['title']}")
        return 0
    config = load_layout(args.layout)
    if not args.capture_only and args.scroll_point is None and "scroll_rect" not in config:
        raise ValueError("实时布局需要 scroll_rect，或用 --scroll-point 指定位置。")
    target_pixels = target_displacement(config, args.overlap_rows) if not args.capture_only else None
    hwnd = find_window(args.hwnd, args.window_title)
    pid = window_pid(hwnd)
    ref_width, ref_height = config["reference_size"]
    # 仅捕获入口保持只读；正常扫描会在创建 WGC 前应用布局的参考尺寸。
    size = client_geometry(hwnd)[0] if args.capture_only else (ref_width, ref_height)
    if args.capture_only and abs(size[1] - ref_height * size[0] / ref_width) > 2:
        raise ValueError(f"客户区 {size} 与布局 {ref_width}×{ref_height} 宽高比不符。")
    output = args.output or root / "results" / datetime.now().strftime("live_%Y%m%d_%H%M%S_%f")
    output.mkdir(parents=True, exist_ok=False)
    for name in ("frames", "crops"):
        (output / name).mkdir()
    started = perf_counter()
    records, events = [], []
    timings = {"queue_wait_seconds": 0.0}
    session = {"status": "incomplete", "message": "采集尚未完成。", "scroll_count": 0,
               "expected_count": args.expected_count, "start_at_top": "user_required",
               "scroll_mode": "fixed" if args.wheel_notches is not None else "calibrate_once",
               "target_pixels": target_pixels, "target_overlap_rows": args.overlap_rows,
               "window_hwnd": hwnd, "window_title": window_title(hwnd), "window_pid": pid}
    capture = worker = None
    foreground_required = False
    geometry_required = args.capture_only
    latest_image = None
    last_point = None

    def check_stop():
        if stop_pressed():
            raise ScanStopped("按下 F8，已停止采集。")
        if worker is not None:
            worker.check()
        if window_pid(hwnd) != pid or (geometry_required and client_geometry(hwnd)[0] != size):
            raise RuntimeError("目标窗口或客户区尺寸发生变化。")
        if foreground_required and user32.GetForegroundWindow() != hwnd:
            raise RuntimeError("游戏失去前台焦点，已停止采集。")

    def scroll_point():
        nonlocal last_point
        session["client_origin"] = list(client_geometry(hwnd)[1])
        scale = size[0] / ref_width
        if args.scroll_point is not None:
            point = tuple(round(value * scale) for value in args.scroll_point)
        else:
            point = choose_scroll_point(hwnd, [round(value * scale) for value in config["scroll_rect"]])
        session["scroll_point"] = list(point)
        if point != last_point:
            print(f"滚动点（客户区）：{point}。", flush=True)
            last_point = point
        return point

    def checkpoint():
        (output / "session.json").write_text(json.dumps(
            {"session": session, "accepted_count": len(records), "frames": events},
            ensure_ascii=False, indent=2), encoding="utf-8")

    def journal_transaction(value):
        with (output / "transactions.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(value, ensure_ascii=False) + "\n")

    def accept(observation, first_new, global_start, frame_id):
        samples = assign_records(observation.page.rows[first_new:], global_start,
                                 frame_id, args.category, first_new)
        if samples:
            timings["queue_wait_seconds"] += worker.submit(PageJob(frame_id, observation.page.image, samples), check_stop)
            records.extend(sample.record for sample in samples)
            print(f"{frame_id} 新增 {len(samples)} 件，累计采集 {len(records)} 件。", flush=True)
        return len(samples)

    def check_completion(page):
        if page.bottom["at_bottom"]:
            page.image.save(output / "end_frame.png")
        if args.expected_count is not None and len(records) > args.expected_count:
            raise RuntimeError(f"已采集 {len(records)} 件，超过指定总数 {args.expected_count}。")
        if not page.bottom["at_bottom"]:
            return False
        if args.expected_count is not None and len(records) != args.expected_count:
            raise RuntimeError(f"滚动条已到底，但采集 {len(records)}/{args.expected_count} 件，数量不符。")
        session.update(status="count_verified" if args.expected_count is not None else "bottom_reached",
                       message="滚动条已到底，当前分类采集结束。")
        return True

    try:
        if not args.capture_only:
            setup_started = perf_counter()
            session["window_setup"] = prepare_scan_window(hwnd, size, check_stop, args.frame_timeout)
            timings["window_setup_seconds"] = perf_counter() - setup_started
            geometry_required = True
            from stream_worker import PageJob, RecognitionWorker

            worker = RecognitionWorker(output, config, args.font, args.batch_size, args.queue_pages)
            while not worker.ready.wait(0.1):
                check_stop()
            check_stop()
            focus_started = perf_counter()
            activate(hwnd, check_stop)
            timings["focus_wait_seconds"] = perf_counter() - focus_started
            foreground_required = True
            # 首屏之前就离开卡片热区，名称锚点和星星采样不受鼠标悬停影响。
            position_pointer(hwnd, pid, scroll_point(), size)
        from wgc_capture import WindowCapture

        capture = WindowCapture(hwnd)
        if args.capture_only:
            frame_image(wait_frame(capture, check_stop, args.frame_timeout)).save(output / "capture.png")
            session.update(status="capture_only", message="已保存 WGC 客户区截图，未移动鼠标或发送输入。")
        else:
            observation = wait_page(capture, config, check_stop, args.stable_seconds, args.frame_timeout)
            latest_image = observation.page.image
            previous = observation.page
            previous_start, accepted_rows = 0, len(previous.rows)
            count = accept(observation, 0, 0, "f0001")
            events.append({"frame_id": "f0001", "row_y": [r.y for r in previous.rows],
                           "global_start": 0, "new_cards": count, "scrollbar": previous.bottom,
                           "frame": observation.frame})
            journal_transaction({"kind": "initial", "trace": observation.trace})
            checkpoint()
            page_notches, rate = args.wheel_notches, None
            for index in range(1, args.max_scrolls + 1):
                if check_completion(previous):
                    break
                check_stop()
                anchor = make_anchor(previous, config, args.overlap_rows)
                calibrating = page_notches is None
                notches = CALIBRATION_NOTCHES if calibrating else page_notches
                plan = {"mode": "calibration" if calibrating else "fixed", "wheel_notches": notches,
                        "anchor_y": anchor.y, "anchor_rows": len(anchor.templates),
                        "expected_pixels": notches * rate if rate is not None else None}
                baseline = capture.snapshot()
                plan["before_frame"] = frame_info(baseline)
                session["last_transaction"] = plan
                print(f"第 {index} 次滚动：{notches} 格（{'小步标定' if calibrating else '固定幅度'}），等待本次响应。", flush=True)
                # 这次事务返回前，循环内没有其他输入发送路径。
                plan["input_started_at"] = monotonic()
                scroll_down(hwnd, pid, scroll_point(), notches, size)
                plan["input_sent_at"] = monotonic()
                session["scroll_count"] = index
                observation = wait_page(capture, config, check_stop, args.stable_seconds, args.frame_timeout,
                                        previous=previous, anchor=anchor, after_sequence=baseline.sequence)
                latest_image = observation.page.image
                current = observation.page
                plan.update(actual_pixels=observation.displacement, after_frame=observation.frame,
                            anchor=observation.anchor)
                journal_transaction({"kind": "scroll", "plan": plan, "trace": observation.trace})
                if rate is not None:
                    verify_response(observation.displacement, notches * rate, current.bottom["at_bottom"], config["row_step"])
                if calibrating and not current.bottom["at_bottom"]:
                    page_notches, rate = calibrated_notches(observation.displacement, target_pixels)
                    session["calibration"] = {"pixels_per_notch": rate, "page_notches": page_notches,
                                              "predicted_pixels": page_notches * rate}
                    print(f"标定完成：{rate:.2f}px/格，后续固定 {page_notches} 格，预计 {page_notches * rate:.0f}px/次。", flush=True)
                current_start = previous_start + observation.advance
                if current_start > accepted_rows:
                    raise RuntimeError("相邻画面间存在未覆盖的行。")
                first_new = max(0, accepted_rows - current_start)
                frame_id = f"f{index + 1:04d}"
                count = accept(observation, first_new, current_start + first_new, frame_id)
                overlap = min(len(previous.rows) - observation.advance, len(current.rows))
                print(f"本次移动 {observation.displacement}px，接续重叠 {overlap} 行。", flush=True)
                events.append({"frame_id": frame_id, "row_y": [r.y for r in current.rows],
                               "global_start": current_start, "advance": observation.advance,
                               "overlap": overlap, "new_cards": count, "scrollbar": current.bottom,
                               "scroll": plan, "frame": observation.frame})
                accepted_rows = max(accepted_rows, current_start + len(current.rows))
                previous, previous_start = current, current_start
                checkpoint()
            else:
                if not check_completion(previous):
                    session.update(status="scroll_limit", message="达到滚动次数上限，保存部分结果。")
    except PageWaitError as error:
        session.update(status="failed", message=str(error))
        latest_image = error.image
        journal_transaction({"kind": "failed", "plan": session.get("last_transaction"), "trace": error.trace})
    except (ScanStopped, KeyboardInterrupt) as error:
        session.update(status="stopped", message=str(error) or "按下 Ctrl+C，已停止采集。")
    except Exception as error:
        session.update(status="failed", message=str(error))
    finally:
        if session["status"] == "failed" and latest_image is not None:
            latest_image.save(output / "unmatched_frame.png")
        if capture is not None:
            try:
                capture.close()
            except Exception as error:
                session.update(status="failed", message=f"停止捕获失败：{error}")
        timings["capture_phase_seconds"] = perf_counter() - started
        if worker is not None:
            print("采集已停止，等待已入队名称识别完成并导出。", flush=True)
            worker.finish()
            timings.update(worker.timings)
            if worker.error is not None:
                session.update(status="failed", message=f"识别线程失败：{worker.error}")
            elif worker.completed != len(records):
                session.update(status="failed", message=f"采集 {len(records)} 件，仅处理 {worker.completed} 件。")
            session["processed_count"] = worker.completed
        session["accepted_count"] = len(records)
        timings["before_export_seconds"] = perf_counter() - started
        checkpoint()
        if not args.capture_only:
            metadata = {"created_at": datetime.now().astimezone().isoformat(), "category": args.category,
                        "image_size": list(size), "layout": config, "layout_only": False,
                        "recognition": worker.recognition_info if worker is not None else None,
                        "batch_size": args.batch_size,
                        "ownership_method": "rightmost_star_foreground_luma", "scope": "streaming_category",
                        "timings": timings, "session": session, "frames": events}
            write_reports(None, records, metadata, output)
    print(f"{session['status']}：{session['message']}", flush=True)
    print(f"结果目录：{output.resolve()}", flush=True)
    return 0 if session["status"] in ("bottom_reached", "count_verified", "capture_only") else (1 if session["status"] == "failed" else 2)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, RuntimeError) as error:
        print(f"无法开始扫描：{error}")
        raise SystemExit(1)
