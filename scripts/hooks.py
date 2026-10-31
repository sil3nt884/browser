#!/usr/bin/env python3
# Copyright 2026 The Umbra Authors.
"""Applies Umbra's small set of edits ("hooks") to a Chromium checkout.

Why not .patch files? Unified diffs break whenever nearby lines move, which in
Chromium is every few weeks. Each hook here instead finds a *structural anchor*
(a function signature, a target name, an enum entry) and inserts or replaces a
line next to it. When an anchor genuinely disappears the script stops, names the
hook, file and anchor, and changes nothing -- see docs/HOOKS.md for what each
hook is for and how to re-anchor it.

Usage:
  hooks.py apply  --src ~/chromium/src [--platform win|android|linux]
  hooks.py check  --src ~/chromium/src [--platform ...]   # dry run
  hooks.py revert --src ~/chromium/src                     # git checkout touched files
  hooks.py list
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import re
import subprocess
import sys
from typing import Callable, Optional, Sequence

MARK = "[umbra]"


@dataclasses.dataclass
class Edit:
  """One change inside a file.

  kind:
    'insert_after'  -- insert `text` on new line(s) after the line matching
                       `anchor` (searched from the end of `within`, if given).
    'insert_before' -- same, before the matching line.
    'replace'       -- re.sub(anchor, text, count=1) on the region.
  `anchor` may be a list: alternatives tried in order.
  `text` may contain {indent}, replaced by the anchor line's indentation
  (plus `extra_indent`).
  """
  kind: str
  anchor: str | Sequence[str]
  text: str
  within: Optional[str] = None
  extra_indent: str = ""


@dataclasses.dataclass
class Hook:
  name: str
  path: str
  edits: list[Edit]
  why: str
  required: bool = True
  platforms: tuple[str, ...] = ("win", "android", "linux", "mac")
  comment: str = "//"
  # Optional custom "already applied" test. Defaults to looking for the marker.
  applied: Optional[Callable[[str], bool]] = None

  def marker(self) -> str:
    return f"{self.comment} {MARK}"

  def is_applied(self, content: str) -> bool:
    if self.applied:
      return self.applied(content)
    return f"{MARK} {self.name}" in content


def _cc_include(header: str) -> Edit:
  # After the file's own header include (first #include line), which is
  # always present and always first.
  return Edit("insert_after", r'^#include\s+"[^"]+"\s*$',
              f'#include "{header}"')


HOOKS: list[Hook] = [
    Hook(
        name="packaged-vs-version",
        path="build/vs_toolchain.py",
        why="Honor the explicitly selected version of a custom Windows toolchain.",
        platforms=("win",),
        comment="#",
        edits=[Edit(
            "insert_after", r"^    supported_versions = list\(MSVS_VERSIONS.keys\(\)\)",
            "{indent}custom_version = os.environ.get('CHORIUM_VS_VERSION')\n"
            "{indent}if custom_version:\n"
            "{indent}    if custom_version not in supported_versions:\n"
            "{indent}        raise ValueError('Unsupported CHORIUM_VS_VERSION')\n"
            "{indent}    return custom_version",
        )],
    ),
    Hook(
        name="startup",
        path="chrome/app/chrome_main_delegate.cc",
        why="Start Tor and lock the network down before anything else runs.",
        edits=[
            _cc_include("umbra/app/umbra_startup.h"),
            Edit("insert_after",
                 r"ChromeMainDelegate::BasicStartupComplete\(\)\s*\{",
                 "{indent}umbra::OnBasicStartupComplete();",
                 extra_indent="  "),
        ],
    ),
    Hook(
        name="prefs",
        path="chrome/browser/profiles/pref_service_builder_utils.cc",
        why="Override profile pref defaults (forced incognito, no suggest...).",
        edits=[
            _cc_include("umbra/browser/umbra_prefs.h"),
            # After the keyed-service prefs are registered too (e.g.
            # plugins.always_open_pdf_externally comes from PluginPrefsFactory),
            # so every profile pref exists by the time defaults are changed.
            Edit("insert_after",
                 r"^\s*BrowserContextDependencyManager::GetInstance\(\)\s*"
                 r"->RegisterProfilePrefsForServices\(pref_registry\);",
                 "{indent}umbra::OverrideProfilePrefDefaults(pref_registry);"),
        ],
    ),
    Hook(
        name="renderer",
        path="chrome/renderer/chrome_content_renderer_client.cc",
        why="Attach the preload/video injector to every new frame.",
        edits=[
            _cc_include("umbra/renderer/preload_injector.h"),
            Edit("insert_after",
                 r"void ChromeContentRendererClient::RenderFrameCreated\(\s*"
                 r"content::RenderFrame\*\s*render_frame\)\s*\{",
                 "{indent}umbra::PreloadInjector::MaybeAttach(render_frame);",
                 extra_indent="  "),
        ],
    ),
    Hook(
        name="isolated-world-id",
        path="chrome/common/chrome_isolated_world_ids.h",
        why="Reserve an isolated JS world for the preload injector.",
        edits=[
            Edit("insert_before", r"^\s*ISOLATED_WORLD_ID_EXTENSIONS\b",
                 "{indent}ISOLATED_WORLD_ID_UMBRA_PRELOAD,"),
        ],
    ),
    Hook(
        name="browser-deps",
        path="chrome/browser/BUILD.gn",
        comment="#",
        why="Link //umbra/app and //umbra/browser into the browser.",
        edits=[
            Edit("insert_after", r"^\s*deps\s*\+?=\s*\[",
                 '{indent}"//umbra/app",\n{indent}"//umbra/browser",',
                 # source_set since M157; static_library before.
                 within=r'^(?:static_library|source_set)\("browser"\)\s*\{',
                 extra_indent="  "),
        ],
    ),
    Hook(
        name="renderer-deps",
        path="chrome/renderer/BUILD.gn",
        comment="#",
        why="Link //umbra/renderer into the renderer.",
        edits=[
            Edit("insert_after", r"^\s*deps\s*\+?=\s*\[",
                 '{indent}"//umbra/renderer",',
                 within=r'^(?:static_library|source_set)\("renderer"\)\s*\{',
                 extra_indent="  "),
        ],
    ),
    Hook(
        name="android-assets",
        path="chrome/android/BUILD.gn",
        comment="#",
        platforms=("android",),
        why="Package libumbra_tor.so into the APK as an uncompressed asset.",
        edits=[
            Edit("insert_after", r"^\s*deps\s*\+?=\s*\[",
                 '{indent}"//umbra/tor_core:runtime_assets",',
                 within=r'^\s*android_library\("chrome_java"\)\s*\{',
                 extra_indent="  "),
        ],
    ),
    Hook(
        name="media-buffer",
        path="third_party/blink/renderer/platform/media/multi_buffer_data_source.h",
        required=False,
        why="Buffer ~60 s ahead instead of ~10 s for progressive (non-MSE) video.",
        edits=[
            # media::Tuneable<int> {name, min, default, max}: raise the default.
            # 60 is the Tuneable's own max, so it stays in range.
            Edit("replace",
                 r'(preload_seconds_\s*=\s*\{\s*'
                 r'"SrcMediaMultiBufferPreloadSeconds",\s*\d+,\s*)\d+',
                 r"\g<1>60"),
        ],
    ),
    Hook(
        name="media-buffer-cap",
        path="third_party/blink/renderer/platform/media/multi_buffer_data_source.cc",
        required=False,
        why="Let the 60 s read-ahead use up to 200 MB instead of 50 MB.",
        edits=[
            Edit("replace", r"(kMaxBufferPreload\s*=\s*)\d+(\s*<<\s*20)",
                 r"\g<1>200\g<2>"),
        ],
    ),
    Hook(
        name="win-installer",
        path="chrome/installer/mini_installer/chrome.release",
        comment="#",
        required=False,
        platforms=("win",),
        why="Ship umbra_tor.dll in the Windows installer next to chrome.dll.",
        edits=[
            Edit("insert_after", r"^chrome\.dll:\s*%\(VersionDir\)s\\\s*$",
                 "umbra_tor.dll: %(VersionDir)s\\"),
        ],
    ),
    Hook(
        name="branding",
        path="chrome/app/theme/chromium/BRANDING",
        comment="",
        required=False,
        why="Product name shown in window titles, installer and about page.",
        applied=lambda c: "PRODUCT_FULLNAME=Umbra" in c,
        edits=[
            Edit("replace", r"^PRODUCT_FULLNAME=.*$", "PRODUCT_FULLNAME=Umbra"),
            Edit("replace", r"^PRODUCT_SHORTNAME=.*$", "PRODUCT_SHORTNAME=Umbra"),
            Edit("replace", r"^PRODUCT_INSTALLER_FULLNAME=.*$",
                 "PRODUCT_INSTALLER_FULLNAME=Umbra Installer"),
            Edit("replace", r"^PRODUCT_INSTALLER_SHORTNAME=.*$",
                 "PRODUCT_INSTALLER_SHORTNAME=Umbra Installer"),
        ],
    ),
]


class AnchorError(Exception):
  pass


def _line_start(content: str, pos: int) -> int:
  return content.rfind("\n", 0, pos) + 1


def _line_end(content: str, pos: int) -> int:
  end = content.find("\n", pos)
  return len(content) if end == -1 else end


def _indent_of(line: str) -> str:
  return line[: len(line) - len(line.lstrip())]


def _find(pattern_list, content: str, start: int):
  for pat in pattern_list:
    m = re.compile(pat, re.MULTILINE).search(content, start)
    if m:
      return m
  return None


def apply_edit(content: str, edit: Edit, hook: Hook) -> str:
  anchors = [edit.anchor] if isinstance(edit.anchor, str) else list(edit.anchor)

  start = 0
  if edit.within:
    w = re.compile(edit.within, re.MULTILINE).search(content)
    if not w:
      raise AnchorError(f"region not found: /{edit.within}/")
    start = w.end()

  if edit.kind == "replace":
    m = _find(anchors, content, start)
    if not m:
      raise AnchorError(f"pattern not found: /{anchors[0]}/")
    new = m.expand(edit.text)
    out = content[: m.start()] + new + content[m.end():]
    if hook.comment:
      end = _line_end(out, m.start())
      out = out[:end] + f"  {hook.marker()} {hook.name}" + out[end:]
    return out

  m = _find(anchors, content, start)
  if not m:
    raise AnchorError(f"anchor not found: /{anchors[0]}/")
  # A leading \s* in an anchor can swallow blank lines; measure indentation
  # from the line holding the first real character of the match.
  first = m.start() + (len(m.group(0)) - len(m.group(0).lstrip()))

  if edit.kind == "insert_after":
    # Anchor may span lines (function signatures); insert after its last line.
    ls = _line_start(content, first)
    anchor_line = content[ls:_line_end(content, ls)]
    insert_at = _line_end(content, m.end() - 1 if m.end() > m.start() else m.end())
    indent = _indent_of(anchor_line) + edit.extra_indent
  elif edit.kind == "insert_before":
    ls = _line_start(content, first)
    anchor_line = content[ls:_line_end(content, ls)]
    insert_at = ls - 1 if ls > 0 else 0
    indent = _indent_of(anchor_line) + edit.extra_indent
  else:
    raise ValueError(edit.kind)

  text = edit.text.replace("{indent}", indent)
  if hook.comment:
    text = f"{indent}{hook.marker()} {hook.name}\n{text}"
  if edit.kind == "insert_before" and ls == 0:
    return text + "\n" + content
  return content[:insert_at] + "\n" + text + content[insert_at:]


@dataclasses.dataclass
class Result:
  hook: Hook
  status: str  # applied | already | skipped | failed | missing-file
  detail: str = ""
  new_content: Optional[str] = None


def plan(src: str, platform: str, only: str | None = None) -> list[Result]:
  results = []
  for hook in HOOKS:
    if only and hook.name != only:
      continue
    if platform not in hook.platforms:
      results.append(Result(hook, "skipped", f"not used on {platform}"))
      continue
    path = os.path.join(src, hook.path)
    if not os.path.isfile(path):
      results.append(Result(hook, "missing-file", hook.path))
      continue
    with open(path, encoding="utf-8", newline="") as f:
      content = f.read()
    if hook.is_applied(content):
      results.append(Result(hook, "already"))
      continue
    new = content
    try:
      for edit in hook.edits:
        new = apply_edit(new, edit, hook)
    except AnchorError as e:
      results.append(Result(hook, "failed", str(e)))
      continue
    results.append(Result(hook, "applied", new_content=new))
  return results


def report(results: list[Result]) -> bool:
  ok = True
  for r in results:
    bad = r.status in ("failed", "missing-file")
    tag = {"applied": "OK  ", "already": "done", "skipped": "skip"}.get(
        r.status, "FAIL" if r.hook.required else "warn")
    line = f"[{tag}] {r.hook.name:<18} {r.hook.path}"
    if r.detail:
      line += f"\n         {r.detail}"
    if bad:
      line += f"\n         purpose: {r.hook.why}\n         see docs/HOOKS.md#{r.hook.name}"
      if r.hook.required:
        ok = False
    print(line)
  return ok


def cmd_apply(src: str, platform: str, dry_run: bool, only: str | None = None) -> int:
  results = plan(src, platform, only)
  ok = report(results)
  if not ok:
    print("\nRequired hooks failed; nothing was written.", file=sys.stderr)
    return 1
  if dry_run:
    print("\nDry run: all required hooks can be applied.")
    return 0
  for r in results:
    if r.status == "applied" and r.new_content is not None:
      with open(os.path.join(src, r.hook.path), "w", encoding="utf-8",
                newline="") as f:
        f.write(r.new_content)
  print("\nHooks applied.")
  return 0


def cmd_revert(src: str, only: str | None = None) -> int:
  paths = []
  for hook in HOOKS:
    if only and hook.name != only:
      continue
    p = os.path.join(src, hook.path)
    if os.path.isfile(p):
      with open(p, encoding="utf-8", newline="") as f:
        if hook.is_applied(f.read()):
          paths.append(hook.path)
  if not paths:
    print("Nothing to revert.")
    return 0
  # Hook files may live in sub-repositories (e.g. third_party/blink is in
  # src, but be safe and run git from each file's directory).
  for rel in paths:
    full = os.path.join(src, rel)
    subprocess.check_call(["git", "checkout", "--", os.path.basename(full)],
                          cwd=os.path.dirname(full))
    print(f"reverted {rel}")
  return 0


def main(argv=None) -> int:
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument("command", choices=["apply", "check", "revert", "list"])
  ap.add_argument("--src", help="Chromium src/ directory")
  ap.add_argument("--only", choices=[h.name for h in HOOKS],
                  help="apply/check a single hook")
  ap.add_argument("--platform", default=_default_platform(),
                  choices=["win", "android", "linux", "mac"])
  args = ap.parse_args(argv)

  if args.command == "list":
    for h in HOOKS:
      if args.only and h.name != args.only:
        continue
      req = "required" if h.required else "optional"
      print(f"{h.name:<18} {req:<9} {','.join(h.platforms):<22} {h.path}\n"
            f"{'':<18} {h.why}")
    return 0
  if not args.src or not os.path.isdir(args.src):
    ap.error("--src must point at Chromium's src/ directory")
  if args.command == "revert":
    return cmd_revert(args.src, only=args.only)
  return cmd_apply(args.src, args.platform, dry_run=args.command == "check", only=args.only)


def _default_platform() -> str:
  if sys.platform.startswith("win"):
    return "win"
  if sys.platform == "darwin":
    return "mac"
  return "linux"


if __name__ == "__main__":
  sys.exit(main())
