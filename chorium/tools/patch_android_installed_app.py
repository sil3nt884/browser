# Copyright 2026 The Umbra Authors. BSD-style license; see LICENSE.
"""Disable the unsupported InstalledApp API in an Apktool-decoded Chorium APK.

Run after patch_android_renderer_compat.py and patch_android_frame_rate.py.
The switch merge matches ChoriumActivity.disableBrowserFeature.
"""
import argparse
from pathlib import Path
from patch_android_frame_rate import METHOD

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('decoded', type=Path)
    args = ap.parse_args()
    files = list(args.decoded.glob('smali*/org/umbra/chorium/runtime/ChoriumActivity.smali'))
    if len(files) != 1:
        ap.error('expected exactly one ChoriumActivity')
    path = files[0]
    text = path.read_text(encoding='utf-8')
    anchor = '    invoke-static {}, Lorg/umbra/chorium/runtime/ChoriumActivity;->configurePageFrameRate()V'
    if 'configureInstalledAppApi()V' in text or text.count(anchor) != 1:
        ap.error('already patched or frame-rate startup hook missing')
    method = (METHOD.replace('configurePageFrameRate', 'configureInstalledAppApi')
              .replace('disable-features', 'disable-blink-features')
              .replace('ThrottleMainFrameTo60Hz', 'InstalledApp'))
    call = '    invoke-static {}, Lorg/umbra/chorium/runtime/ChoriumActivity;->configureInstalledAppApi()V'
    path.write_text(text.replace(anchor, anchor+'\n\n'+call)+method, encoding='utf-8')
    print(f'Patched {path}')

if __name__ == '__main__':
    main()
