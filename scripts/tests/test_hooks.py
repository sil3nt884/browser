#!/usr/bin/env python3
# Copyright 2026 The Umbra Authors.
"""Tests for scripts/hooks.py against Chromium-shaped fixture files.

The fixtures mimic the structure of the real Chromium files closely enough to
exercise every anchor. They do not prove the anchors still match the Chromium
revision you sync; `hooks.py check --src ...` does that.

Run: python3 scripts/tests/test_hooks.py
"""

import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import hooks  # noqa: E402

FIXTURES = os.path.join(HERE, "fixtures", "src")


def read(src, rel):
  with open(os.path.join(src, rel), encoding="utf-8") as f:
    return f.read()


class HooksTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.mkdtemp()
    self.src = os.path.join(self.tmp, "src")
    shutil.copytree(FIXTURES, self.src)

  def tearDown(self):
    shutil.rmtree(self.tmp)

  def apply(self, platform="android"):
    return hooks.cmd_apply(self.src, platform, dry_run=False)

  def test_all_hooks_apply_on_android(self):
    self.assertEqual(self.apply("android"), 0)

    cc = read(self.src, "chrome/app/chrome_main_delegate.cc")
    self.assertIn('#include "umbra/app/umbra_startup.h"', cc)
    # Call is the first statement inside BasicStartupComplete, indented.
    body = cc.split("BasicStartupComplete() {", 1)[1]
    first_stmt = [l for l in body.splitlines() if l.strip()][1]
    self.assertEqual(first_stmt, "  umbra::OnBasicStartupComplete();")

    prefs = read(self.src,
                 "chrome/browser/profiles/pref_service_builder_utils.cc")
    self.assertIn('#include "umbra/browser/umbra_prefs.h"', prefs)
    # After the keyed-service registration, not the SimpleDependencyManager one.
    self.assertIn("      ->RegisterProfilePrefsForServices(pref_registry);\n"
                  "  // [umbra] prefs\n"
                  "  umbra::OverrideProfilePrefDefaults(pref_registry);\n}",
                  prefs)

    rcc = read(self.src, "chrome/renderer/chrome_content_renderer_client.cc")
    self.assertIn("    content::RenderFrame* render_frame) {\n"
                  "  // [umbra] renderer\n"
                  "  umbra::PreloadInjector::MaybeAttach(render_frame);", rcc)

    ids = read(self.src, "chrome/common/chrome_isolated_world_ids.h")
    self.assertLess(ids.index("ISOLATED_WORLD_ID_UMBRA_PRELOAD,"),
                    ids.index("ISOLATED_WORLD_ID_EXTENSIONS"))
    self.assertIn("  ISOLATED_WORLD_ID_UMBRA_PRELOAD,", ids)

    gn = read(self.src, "chrome/browser/BUILD.gn")
    browser_block = gn.split('source_set("browser")', 1)[1].split(
        'source_set("other")', 1)[0]
    self.assertIn('    "//umbra/app",\n    "//umbra/browser",', browser_block)
    # Must land in deps of the right target, not "other".
    other_block = gn.split('source_set("other")', 1)[1]
    self.assertNotIn("umbra", other_block)

    rgn = read(self.src, "chrome/renderer/BUILD.gn")
    self.assertIn('  deps = [\n    # [umbra] renderer-deps\n    "//umbra/renderer",',
                  rgn)
    self.assertNotIn("umbra", rgn.split("deps = [", 1)[0])

    agn = read(self.src, "chrome/android/BUILD.gn")
    java = agn.split('android_library("chrome_java")', 1)[1]
    self.assertIn('"//umbra/tor_core:runtime_assets"', java)
    self.assertNotIn("umbra", agn.split('android_library("chrome_java")', 1)[0])

    media_h = read(self.src,
                   "third_party/blink/renderer/platform/media/"
                   "multi_buffer_data_source.h")
    self.assertIn('"SrcMediaMultiBufferPreloadSeconds",'
                  "  // [umbra] media-buffer\n"
                  "                                           0, 60, 60};",
                  media_h)
    self.assertIn('"SrcMediaMultiBufferKeepAfterPlaybackSeconds", 0, 2, 60}',
                  media_h)  # untouched
    media = read(self.src,
                 "third_party/blink/renderer/platform/media/"
                 "multi_buffer_data_source.cc")
    self.assertIn("kMaxBufferPreload = 200 << 20;", media)
    self.assertIn("kMinBufferPreload = 2 << 20;", media)  # untouched

    brand = read(self.src, "chrome/app/theme/chromium/BRANDING")
    self.assertIn("PRODUCT_FULLNAME=Umbra\n", brand)

    # Windows-only hook left alone on Android.
    rel = read(self.src, "chrome/installer/mini_installer/chrome.release")
    self.assertNotIn("umbra_tor.dll", rel)

  def test_windows_skips_android_hook_and_patches_installer(self):
    self.assertEqual(self.apply("win"), 0)
    rel = read(self.src, "chrome/installer/mini_installer/chrome.release")
    self.assertIn("chrome.dll: %(VersionDir)s\\\n# [umbra] win-installer\n"
                  "umbra_tor.dll: %(VersionDir)s\\\n", rel)
    agn = read(self.src, "chrome/android/BUILD.gn")
    self.assertNotIn("umbra", agn)

  def test_idempotent(self):
    self.assertEqual(self.apply(), 0)
    before = {h.path: read(self.src, h.path) for h in hooks.HOOKS}
    self.assertEqual(self.apply(), 0)
    for h in hooks.HOOKS:
      self.assertEqual(before[h.path], read(self.src, h.path), h.name)
    statuses = {r.hook.name: r.status
                for r in hooks.plan(self.src, "android")}
    self.assertTrue(all(s in ("already", "skipped") for s in statuses.values()),
                    statuses)

  def test_missing_required_anchor_writes_nothing(self):
    path = os.path.join(self.src, "chrome/app/chrome_main_delegate.cc")
    with open(path, "w") as f:
      f.write('#include "chrome/app/chrome_main_delegate.h"\n'
              "int SomethingElse() { return 0; }\n")
    snapshot = {h.path: read(self.src, h.path) for h in hooks.HOOKS}
    self.assertEqual(self.apply(), 1)
    for h in hooks.HOOKS:
      self.assertEqual(snapshot[h.path], read(self.src, h.path),
                       f"{h.name} was modified despite failure")

  def test_missing_optional_anchor_is_only_a_warning(self):
    path = os.path.join(self.src, "third_party/blink/renderer/platform/media/"
                        "multi_buffer_data_source.cc")
    with open(path, "w") as f:
      f.write("namespace blink {}\n")
    self.assertEqual(self.apply(), 0)
    self.assertIn('"//umbra/renderer"',
                  read(self.src, "chrome/renderer/BUILD.gn"))

  def test_check_is_dry_run(self):
    snapshot = {h.path: read(self.src, h.path) for h in hooks.HOOKS}
    self.assertEqual(hooks.cmd_apply(self.src, "android", dry_run=True), 0)
    for h in hooks.HOOKS:
      self.assertEqual(snapshot[h.path], read(self.src, h.path))


if __name__ == "__main__":
  unittest.main(verbosity=2)
