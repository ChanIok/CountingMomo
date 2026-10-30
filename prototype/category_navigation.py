"""仅遍历图鉴分类：图标定位、选中框确认、截图留证；不读取服装。"""

import argparse
import ctypes
import json
import sys
from dataclasses import dataclass
from datetime import datetime
from math import ceil
from pathlib import Path
from time import monotonic, sleep

import cv2
import numpy as np
from PIL import Image


TEMPLATE_ROOT = Path(__file__).resolve().parent / "category_templates"


class NavigationStopped(Exception):
    pass


@dataclass
class IconMatch:
    point: tuple[int, int]
    score: float
    margin: float


def scroll_notches(distance, pixels_per_notch, limit):
    return min(limit, max(1, ceil(distance / pixels_per_notch)))


def crop(pixels, box):
    left, top, right, bottom = box
    return pixels[top:bottom, left:right]


def gray(pixels):
    return cv2.cvtColor(pixels[:, :, :3], cv2.COLOR_BGR2GRAY)


def icon_signal(pixels):
    # 去掉菜单的缓慢背景渐变；保留亮色图标笔画，不包含外侧选中框。
    value = gray(pixels).astype(np.float32)
    return np.maximum(value - cv2.GaussianBlur(value, (0, 0), 3), 0)


def load_catalog():
    config = json.loads((TEMPLATE_ROOT / "categories.json").read_text(encoding="utf-8"))
    for group in config["groups"]:
        entries = ([group["entry"]] if "entry" in group else []) + group["categories"]
        for entry in entries:
            path = TEMPLATE_ROOT / entry["template"]
            with Image.open(path) as image:
                pixels = np.asarray(image.convert("RGB"))[:, :, ::-1].copy()
            entry["signal"] = icon_signal(pixels)
            left, top, _, _ = entry["crop"]
            x, y = entry["source_point"]
            entry["click_offset"] = (x - left, y - top)
    return config


def locate_icon(pixels, entry, bounds, config, *, child=False):
    signal = icon_signal(crop(pixels, bounds))
    template = entry["signal"]
    height, width = template.shape
    if signal.shape[0] < height or signal.shape[1] < width:
        return None
    response = cv2.matchTemplate(signal, template, cv2.TM_CCOEFF_NORMED)
    response = np.nan_to_num(response, nan=-1, posinf=-1, neginf=-1)
    _, score, _, (x, y) = cv2.minMaxLoc(response)
    # 相邻像素属于同一定位峰；只与远处的另一个图标位置比较。
    remaining = response.copy()
    remaining[max(0, y - height // 2):y + height // 2 + 1,
              max(0, x - width // 2):x + width // 2 + 1] = -1
    second = float(remaining.max())
    dx, dy = entry["click_offset"]
    point = (bounds[0] + x + dx, bounds[1] + y + dy)
    if score < config["match_threshold"] or score - second < config["match_margin"]:
        return None
    # 渐隐区、父入口下面只露出一部分的子类不点击，滚到整个按钮可见再处理。
    half = 32 if child else 40
    vertical = bounds if child else config["menu_rect"]
    if point[1] - half < vertical[1] or point[1] + half >= vertical[3]:
        return None
    return IconMatch(point, float(score), float(score - second))


def selection_contrast(pixels, point, *, child=False):
    """四条边都出现局部亮线才算选中；鼠标须先移出菜单，排除悬停框。"""
    image = gray(pixels).astype(np.float32)
    x, y = point
    half = 32 if child else 40
    span = half - 12

    def horizontal(at):
        values = image[at - 8:at + 9, x - span:x + span + 1].mean(axis=1)
        return float(np.max(values[4:13] - (values[:9] + values[8:]) * 0.5))

    def vertical(at):
        values = image[y - span:y + span + 1, at - 8:at + 9].mean(axis=0)
        return float(np.max(values[4:13] - (values[:9] + values[8:]) * 0.5))

    return min(horizontal(y - half), horizontal(y + half),
               vertical(x - half), vertical(x + half))


class Navigator:
    def __init__(self, args, config, output, session):
        # 离线导入图像函数不会加载 Win32、查找窗口或启动 WGC。
        import window_input

        self.win = window_input
        self.args, self.config = args, config
        self.output, self.session = output, session
        self.hwnd = args.hwnd
        self.pid = self.win.window_pid(self.hwnd)
        self.size = tuple(config["reference_size"])
        self.capture = self.last_frame = None
        self.pointer_parked = False
        self.foreground_required = False
        self.stage = "初始化"

    def check(self):
        if self.win.stop_pressed():
            raise NavigationStopped("按下 F8，已停止分类遍历。")
        if self.win.window_pid(self.hwnd) != self.pid:
            raise RuntimeError("目标窗口发生变化。")
        if self.win.client_geometry(self.hwnd)[0] != self.size:
            raise RuntimeError(f"需要客户区 {self.size[0]}×{self.size[1]}，脚本不会自动调整窗口。")
        if self.foreground_required and self.win.user32.GetForegroundWindow() != self.hwnd:
            raise RuntimeError("游戏失去前台焦点，已停止。")

    def journal(self, **event):
        with (self.output / "events.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps({"at": monotonic(), "stage": self.stage, **event},
                                    ensure_ascii=False) + "\n")

    def checkpoint(self):
        visited = {entry["id"] for entry in self.session["visited"]}
        self.session["remaining"] = [entry for entry in self.session["planned"]
                                     if entry["id"] not in visited]
        (self.output / "session.json").write_text(
            json.dumps(self.session, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    def park_pointer(self):
        self.win.position_pointer(self.hwnd, self.pid, (290, 90), self.size)
        self.pointer_parked = True

    def input(self, point, *, wheel=None):
        self.check()
        self.win.position_pointer(self.hwnd, self.pid, point, self.size)
        self.pointer_parked = False
        # 先让游戏更新鼠标命中区域和悬停，再取输入前基准；不能把悬停变化当成滚动。
        positioned_at = monotonic()
        while monotonic() - positioned_at < 0.15:
            self.check()
            sleep(0.01)
        self.check()
        marker = self.capture.snapshot()
        self.journal(action="click" if wheel is None else "wheel", point=point, notches=wheel)
        self.check()
        sent_at = monotonic()
        if wheel is None:
            packets = (self.win.Input * 2)(
                self.win.Input(type=0, mi=self.win.MouseInput(dwFlags=0x0002)),
                self.win.Input(type=0, mi=self.win.MouseInput(dwFlags=0x0004)))
            count = self.win.user32.SendInput(2, packets, ctypes.sizeof(self.win.Input))
            if count != 2:
                # 若只注入了按下，必须释放本脚本按下的鼠标键。
                if count == 1:
                    self.win.user32.SendInput(1, ctypes.byref(packets[1]), ctypes.sizeof(self.win.Input))
                raise RuntimeError("点击输入失败，请确认终端与游戏权限一致。")
        else:
            packet = self.win.Input(type=0, mi=self.win.MouseInput(
                mouseData=(-120 * wheel) & 0xFFFFFFFF, dwFlags=0x0800))
            if self.win.user32.SendInput(1, ctypes.byref(packet), ctypes.sizeof(packet)) != 1:
                raise RuntimeError("滚轮输入失败，请确认终端与游戏权限一致。")
        if wheel is None:
            # 点击后才移开鼠标，供选中框确认；滚动时一直留在滚动区域。
            while monotonic() - sent_at < 0.12:
                self.check()
                sleep(0.01)
            self.park_pointer()
        return marker, sent_at

    def stable_sample(self, frame):
        return [cv2.GaussianBlur(gray(crop(frame.pixels, self.config[key])), (3, 3), 0)
                for key in ("menu_rect", "header_rect")]

    def wait(self, *, marker=None, sent_at=0, predicate=None, moved=False, motion_box=None):
        deadline = monotonic() + self.args.timeout
        last_sequence = marker.sequence if marker else 0
        last_source = marker.source_time if marker else -1
        motion_box = motion_box or self.config["menu_rect"]
        before = gray(crop(marker.pixels, motion_box)) if moved else None
        previous = stable_since = None
        while monotonic() < deadline:
            self.check()
            frame = self.capture.snapshot()
            if frame is None or frame.sequence <= last_sequence:
                sleep(0.02)
                continue
            last_sequence = frame.sequence
            self.last_frame = frame
            if frame.source_time <= last_source or frame.received_at < sent_at:
                continue
            last_source = frame.source_time
            sample = self.stable_sample(frame)
            ready = not moved or np.mean(cv2.absdiff(
                before, gray(crop(frame.pixels, motion_box)))) > 0.6
            if ready and predicate is not None:
                ready = bool(predicate(frame.pixels))
            now = monotonic()
            if not ready:
                previous = stable_since = None
                continue
            if previous is None or any(np.mean(cv2.absdiff(a, b)) > 0.7
                                       for a, b in zip(previous, sample)):
                previous, stable_since = sample, now
            elif now - stable_since >= 0.35:
                return frame
        raise TimeoutError(f"{self.stage}：等待响应或选中状态稳定超时；未追加输入。")

    def bounds(self, pixels, group):
        if "entry" not in group:
            return self.config["root_search_rect"]
        parent = locate_icon(pixels, group["entry"], self.config["root_search_rect"], self.config)
        if parent is None:
            return None
        bottom = self.config["menu_rect"][3]
        if group["id"] == "accessories":
            makeup = self.config["groups"][2]["entry"]
            next_parent = locate_icon(pixels, makeup, self.config["root_search_rect"], self.config)
            if next_parent is None:
                return None
            # 折叠时两个父入口也可能同时可见，但它们之间没有子列表。
            if next_parent.point[1] - parent.point[1] < 400:
                return None
            bottom = next_parent.point[1] - 38
        else:
            # 最后一个分组没有后继父入口，用样例中展开区的高度判断是否已露出。
            bottom = parent.point[1] + group["viewport_bottom_offset"]
            if bottom > self.config["menu_rect"][3]:
                return None
        if bottom - (parent.point[1] + 43) < 300:
            return None
        return [188, parent.point[1] + 43, 272, bottom]

    def reveal_group(self, group, frame):
        """展开是内联插入，不会自动把父入口移到顶部；先滚外层露出子列表。"""
        for count in range(self.args.max_scrolls + 1):
            self.check()
            if self.bounds(frame.pixels, group) is not None:
                return frame
            if count == self.args.max_scrolls:
                break
            parent = locate_icon(frame.pixels, group["entry"],
                                 self.config["root_search_rect"], self.config)
            if parent is None:
                raise RuntimeError(f"{group['name']}父入口丢失，未发送滚轮。")
            self.stage = f"露出{group['name']}子列表（滚动外层菜单）"
            # 顶部首槽始终在展开子列表上方；不悬停父按钮，以免图标放大后无法重定位。
            point = tuple(self.config["outer_scroll_point"])
            distance = parent.point[1] - group["expanded_parent_y"]
            if distance <= 0:
                raise RuntimeError(f"{group['name']}父入口已到展开目标位置，但子列表边界未确认，已停止。")
            notches = scroll_notches(distance, self.config["wheel_pixels_per_notch"], 24)
            self.journal(action="scroll_plan", target=group["id"], parent_point=parent.point,
                         target_parent_y=group["expanded_parent_y"], notches=notches)
            marker, sent_at = self.input(point, wheel=notches)
            frame = self.wait(marker=marker, sent_at=sent_at, moved=True)
        raise RuntimeError(f"{group['name']}：达到外层滚动上限，子列表仍未完整进入可视区。")

    def seek(self, entry, group, frame, *, direction=1, root=False):
        child = "entry" in group and not root
        for count in range(self.args.max_scrolls + 1):
            self.check()
            bounds = self.bounds(frame.pixels, group) if child else self.config["root_search_rect"]
            if bounds is None:
                raise RuntimeError(f"{group['name']}菜单边界不明，已停止，未发送滚轮。")
            match = locate_icon(frame.pixels, entry, bounds, self.config, child=child)
            if match is not None:
                return frame, match
            expected = None
            if child:
                expected = self.reject_missed_child(entry, group, frame, bounds)
            if count == self.args.max_scrolls:
                break
            self.stage = f"寻找{group['name']} / {entry['name']}（{'向下' if direction > 0 else '向上'}滚动）"
            # 使用子列表中部，避开上方父入口和下方另一分组；不贴右侧边缘。
            if child:
                point = (229, (bounds[1] + bounds[3]) // 2)
                rate = self.config["wheel_pixels_per_notch"]
                # 将目标带到上部安全区，后续几个分类可直接点击；保留至少一行重叠。
                limit = max(1, (bounds[3] - bounds[1] - self.config["child_row_step"]) // rate)
                distance = direction * (expected[1] - (bounds[1] + 80)) if expected else 6 * rate
                if distance <= 0:
                    raise RuntimeError(f"{entry['name']}已越过预期滚动方向，停止并保留现场。")
                notches = scroll_notches(distance, rate, limit)
            else:
                point = tuple(self.config["outer_scroll_point"])
                notches = self.config["root_seek_notches"]
            self.journal(action="scroll_plan", target=entry["id"], expected_point=expected,
                         notches=direction * notches)
            marker, sent_at = self.input(point, wheel=direction * notches)
            frame = self.wait(marker=marker, sent_at=sent_at, moved=True, motion_box=bounds)
        raise RuntimeError(f"{entry['name']}：达到滚动上限 {self.args.max_scrolls}，未找到完整图标。")

    def reject_missed_child(self, entry, group, frame, bounds):
        # 顺序和行距用于规划滚动及发现漏识别；最终点击仍必须通过目标图标定位。
        entries = group["categories"]
        index = next(i for i, item in enumerate(entries) if item["id"] == entry["id"])
        step = self.config["child_row_step"]
        neighbors = sorted((i for i in range(len(entries)) if i != index),
                           key=lambda i: (abs(i - index), i))
        for neighbor_index in neighbors:
            neighbor = locate_icon(frame.pixels, entries[neighbor_index], bounds, self.config, child=True)
            if neighbor is None:
                continue
            expected_y = neighbor.point[1] + (index - neighbor_index) * step
            if bounds[1] + 37 <= expected_y < bounds[3] - 37:
                self.save_region(frame.pixels, self.config["menu_rect"], "unmatched_category_menu.png")
                self.journal(action="unmatched_visible_category", target=entry["id"],
                             expected_point=[neighbor.point[0], expected_y],
                             neighbor=entries[neighbor_index]["id"])
                raise RuntimeError(f"{entry['name']}应已完整进入可视区，但图标未通过匹配；"
                                   "已保存 unmatched_category_menu.png，未继续滚过该分类。")
            return (neighbor.point[0], expected_y)
        return None

    def selected(self, pixels, entry, group, *, root=False):
        child = "entry" in group and not root
        bounds = self.bounds(pixels, group) if child else self.config["root_search_rect"]
        if bounds is None:
            return False
        match = locate_icon(pixels, entry, bounds, self.config, child=child)
        return match is not None and selection_contrast(
            pixels, match.point, child=child) >= self.config["selection_contrast"]

    def open_group(self, group, frame):
        entry = group["entry"]
        frame, match = self.seek(entry, group, frame, root=True)
        self.stage = f"展开{group['name']}"
        marker, sent_at = self.input(match.point)
        frame = self.wait(marker=marker, sent_at=sent_at,
                          predicate=lambda pixels: self.selected(pixels, entry, group, root=True))
        frame = self.reveal_group(group, frame)
        bounds = self.bounds(frame.pixels, group)
        if not any(locate_icon(frame.pixels, child, bounds, self.config, child=True) is not None
                   for child in group["categories"]):
            raise RuntimeError(f"{group['name']}展开区域内未找到子类图标，已停止。")
        # 展开可能保留上次的子列表位置；向上找到首项后才开始顺序遍历。
        frame, _ = self.seek(group["categories"][0], group, frame, direction=-1)
        return frame

    def visit(self, entry, group, frame):
        self.stage = f"切换{group['name']} / {entry['name']}"
        frame, match = self.seek(entry, group, frame)
        self.stage = f"确认{group['name']} / {entry['name']}"
        # 滚动后的悬停框不作为“已经选中”的证据，直接点击目标，之后移开确认。
        if not self.pointer_parked or not self.selected(frame.pixels, entry, group):
            marker, sent_at = self.input(match.point)
            frame = self.wait(marker=marker, sent_at=sent_at,
                              predicate=lambda pixels: self.selected(pixels, entry, group))
        # 已选中项也正常记为访问；空分类不以是否存在物品判定成功。
        bounds = self.bounds(frame.pixels, group)
        match = locate_icon(frame.pixels, entry, bounds, self.config, child="entry" in group)
        number = len(self.session["visited"]) + 1
        prefix = f"{number:02}_{entry['id']}"
        self.save_region(frame.pixels, self.config["menu_rect"], f"{prefix}_menu.png")
        self.save_region(frame.pixels, self.config["header_rect"], f"{prefix}_title.png")
        record = {"id": entry["id"], "group": group["name"], "name": entry["name"],
                  "status": "visited", "point": match.point, "match_score": match.score,
                  "match_margin": match.margin, "sample_empty": entry.get("sample_empty", False),
                  "sequence": frame.sequence, "source_time": frame.source_time,
                  "menu_image": f"{prefix}_menu.png", "title_image": f"{prefix}_title.png"}
        self.session["visited"].append(record)
        self.journal(action="visited", **record)
        self.checkpoint()
        print(f"[{number}/{len(self.session['planned'])}] {group['name']} / {entry['name']} 已访问", flush=True)
        return frame

    def save_region(self, pixels, bounds, filename):
        rgb = cv2.cvtColor(crop(pixels, bounds)[:, :, :3], cv2.COLOR_BGR2RGB)
        Image.fromarray(rgb).save(self.output / filename)

    def run(self):
        self.check()
        from wgc_capture import WindowCapture

        print("请确认图鉴分组已收起、侧栏位于顶部。30 秒内切回游戏；F8 停止。", flush=True)
        self.win.user32.SetForegroundWindow(self.hwnd)
        deadline = monotonic() + 30
        while self.win.user32.GetForegroundWindow() != self.hwnd:
            self.check()
            if monotonic() >= deadline:
                raise TimeoutError("等待游戏前台超时。")
            sleep(0.05)
        self.foreground_required = True
        self.park_pointer()
        self.capture = WindowCapture(self.hwnd)
        frame = self.wait()
        # 所有分段入口都要求相同的已知初始状态，避免在错误界面发送滚轮。
        regular = self.config["groups"][0]
        if any(locate_icon(frame.pixels, entry, self.config["root_search_rect"], self.config) is None
               for entry in regular["categories"][:3]):
            raise RuntimeError("初始侧栏不符：请收起饰品/妆容分组，并将普通分类列表滚到顶部。")
        self.session["status"] = "running"
        self.checkpoint()
        for group in self.config["groups"]:
            if self.args.group != "all" and self.args.group != group["id"]:
                continue
            if "entry" in group:
                frame = self.open_group(group, frame)
            for entry in group["categories"]:
                frame = self.visit(entry, group, frame)


def positive_int(text):
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("必须为正整数")
    return value


def positive_float(text):
    value = float(text)
    if not np.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("必须为有限正数")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hwnd", type=lambda value: int(value, 0), help="游戏窗口句柄，支持 0x 十六进制")
    parser.add_argument("--list-windows", action="store_true", help="只列出窗口，不捕获或发送输入")
    parser.add_argument("--group", choices=("all", "clothing", "accessories", "makeup"), default="all")
    parser.add_argument("--output", type=Path, help="新结果目录，不覆盖已有目录")
    parser.add_argument("--max-scrolls", type=positive_int, default=30, help="寻找单个目标的滚轮事务次数上限，非格数")
    parser.add_argument("--timeout", type=positive_float, default=8, help="单次输入的确认超时秒数")
    args = parser.parse_args()
    if not args.list_windows and args.hwnd is None:
        parser.error("请用 --hwnd 指定游戏窗口；--list-windows 可查看句柄")
    if args.timeout <= 0.35:
        parser.error("--timeout 必须大于稳定等待时长 0.35 秒")
    import window_input

    window_input.enable_physical_coordinates()
    if args.list_windows:
        for window in window_input.list_windows():
            print(f"{window['hwnd']:#x}\t{window['title']}")
        return 0
    config = load_catalog()
    output = args.output or Path(__file__).resolve().parent / "results" / datetime.now().strftime(
        "categories_%Y%m%d_%H%M%S_%f")
    output.mkdir(parents=True, exist_ok=False)
    planned = [{"id": entry["id"], "group": group["name"], "name": entry["name"]}
               for group in config["groups"] if args.group in ("all", group["id"])
               for entry in group["categories"]]
    session = {"status": "starting", "message": "", "hwnd": args.hwnd,
               "reference_size": config["reference_size"], "group": args.group,
               "confirmation": "selection_frame; titles_saved_for_manual_review",
               "planned": planned, "visited": [], "remaining": planned.copy()}
    navigator = None
    exit_code = 1
    try:
        navigator = Navigator(args, config, output, session)
        navigator.checkpoint()
        navigator.run()
        session.update(status="completed", message="分类遍历完成；请核对保存的分类标题截图。")
        exit_code = 0
    except (NavigationStopped, KeyboardInterrupt):
        session.update(status="stopped", message="用户停止，已保留访问记录。")
        exit_code = 2
    except Exception as error:
        session.update(status="failed", message=str(error))
    finally:
        if navigator is not None:
            if session["status"] != "completed" and navigator.last_frame is not None:
                try:
                    navigator.save_region(navigator.last_frame.pixels,
                                          [0, 0, *config["reference_size"]], "failure_frame.png")
                except Exception as error:
                    session["evidence_error"] = str(error)
            if navigator.capture is not None:
                try:
                    navigator.capture.close()
                except Exception as error:
                    session["capture_close_error"] = str(error)
            session["last_stage"] = navigator.stage
        visited = {entry["id"] for entry in session["visited"]}
        session["remaining"] = [entry for entry in planned if entry["id"] not in visited]
        (output / "session.json").write_text(
            json.dumps(session, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{session['message']}\n已访问 {len(session['visited'])}/{len(planned)}；结果：{output}", flush=True)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
