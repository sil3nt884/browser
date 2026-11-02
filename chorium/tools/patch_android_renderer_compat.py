# Copyright 2026 The Umbra Authors. BSD-style license; see LICENSE.
"""Apply the Java configureRendererServices fix to an Apktool-decoded release.

This is a compatibility repackaging path when the native build cache is gone.
It changes the same method as ChoriumApplication.java, without changing native
libraries, assets, resources, permissions, or isolatedProcess declarations.
Build normally from source when the Chromium checkout is available. Repacked
APKs must be aligned, signed with the original certificate, and checked/tested.
"""
import argparse
from pathlib import Path

OWNER = "Lorg/umbra/chorium/runtime/ChoriumApplication;"
ANCHOR = '    const-string v0, "chorium"'
METHOD = """
.method private configureRendererServices()V
    .locals 7
    invoke-virtual {p0}, Landroid/content/Context;->getPackageName()Ljava/lang/String;
    move-result-object v0
    move-object v1, v0
    const/4 v2, 0x0
    const/4 v3, 0x2
    const/4 v4, 0x0
    const/4 v5, 0x0
    const/4 v6, 0x0
    invoke-static/range {v0 .. v6}, Lorg/chromium/content_public/browser/ChildProcessCreationParams;->set(Ljava/lang/String;Ljava/lang/String;ZIZZZ)V
    return-void
.end method
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("decoded", type=Path)
    args = parser.parse_args()
    candidates = list(args.decoded.glob("smali*/org/umbra/chorium/runtime/ChoriumApplication.smali"))
    if len(candidates) != 1:
        parser.error("expected exactly one original ChoriumApplication class")
    path = candidates[0]
    original = path.read_text(encoding="utf-8")
    if "configureRendererServices()V" in original:
        parser.error("renderer compatibility fix already present")
    if original.count(ANCHOR) != 1 or ".method protected attachBaseContext(Landroid/content/Context;)V" not in original:
        parser.error("original Application startup anchor changed")
    api = list(args.decoded.glob("smali*/org/chromium/content_public/browser/ChildProcessCreationParams.smali"))
    process_types = list(args.decoded.glob("smali*/org/chromium/base/library_loader/LibraryProcessType.smali"))
    if len(api) != 1 or "public static set(Ljava/lang/String;Ljava/lang/String;ZIZZZ)V" not in api[0].read_text(encoding="utf-8"):
        parser.error("original child-service configuration API not available")
    if len(process_types) != 1 or "PROCESS_CHILD:I = 0x2" not in process_types[0].read_text(encoding="utf-8"):
        parser.error("original child-process constant changed")
    call = f"    invoke-direct {{p0}}, {OWNER}->configureRendererServices()V\n\n"
    path.write_text(original.replace(ANCHOR, call + ANCHOR) + METHOD, encoding="utf-8")
    print(f"Patched renderer-service configuration in {path}")


if __name__ == "__main__":
    main()
