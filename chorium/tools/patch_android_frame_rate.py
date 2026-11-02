# Copyright 2026 The Umbra Authors. BSD-style license; see LICENSE.
"""Apply ChoriumActivity.configurePageFrameRate to an Apktool-decoded APK.

Use after patch_android_renderer_compat.py on the original October APK.
Rebuild, align, sign, compare payloads and test before distributing.
"""
import argparse
from pathlib import Path

METHOD = r'''
.method private static configurePageFrameRate()V
    .locals 5
    invoke-static {}, Lorg/chromium/base/CommandLine;->getInstance()Lorg/chromium/base/CommandLine;
    move-result-object v0
    const-string v1, "disable-features"
    invoke-virtual {v0, v1}, Lorg/chromium/base/CommandLine;->getSwitchValue(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v1
    const-string v2, "ThrottleMainFrameTo60Hz"
    if-eqz v1, :empty
    invoke-virtual {v1}, Ljava/lang/String;->isEmpty()Z
    move-result v3
    if-nez v3, :empty
    const-string v3, ","
    invoke-virtual {v3, v1}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v4
    invoke-virtual {v4, v3}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v4
    const-string v3, ",ThrottleMainFrameTo60Hz,"
    invoke-virtual {v4, v3}, Ljava/lang/String;->contains(Ljava/lang/CharSequence;)Z
    move-result v3
    if-nez v3, :apply
    const-string v3, ",ThrottleMainFrameTo60Hz"
    invoke-virtual {v1, v3}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v1
    goto :apply
    :empty
    move-object v1, v2
    :apply
    const-string v2, "disable-features"
    invoke-virtual {v0, v2, v1}, Lorg/chromium/base/CommandLine;->appendSwitchWithValue(Ljava/lang/String;Ljava/lang/String;)V
    return-void
.end method
'''

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('decoded', type=Path)
    args = ap.parse_args()
    files = list(args.decoded.glob('smali*/org/umbra/chorium/runtime/ChoriumActivity.smali'))
    if len(files) != 1:
        ap.error('expected exactly one ChoriumActivity')
    path = files[0]
    original = path.read_text(encoding='utf-8')
    anchor = '    invoke-static {p0}, Lorg/chromium/content_public/browser/DeviceUtils;->updateDeviceSpecificUserAgentSwitch(Landroid/content/Context;)V'
    if 'configurePageFrameRate()V' in original or original.count(anchor) != 1:
        ap.error('already patched or Activity startup changed')
    call = '    invoke-static {}, Lorg/umbra/chorium/runtime/ChoriumActivity;->configurePageFrameRate()V\n\n'
    path.write_text(original.replace(anchor, call + anchor) + METHOD, encoding='utf-8')
    print(f'Patched {path}')

if __name__ == '__main__':
    main()
