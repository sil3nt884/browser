# Copyright 2026 The Umbra Authors. BSD-style license; see LICENSE.
"""Apply ChoriumBootstrap to a decoded APK with the three compatibility fixes.

This guarded Java-bytecode repackage accompanies the normal Java source.
Native libraries and assets remain unchanged. Prefer a full source build
when the Chromium checkout is available; verify, sign and test the APK.
"""
import argparse
import re
from pathlib import Path
from patch_android_frame_rate import METHOD

BOOT = 'Lorg/umbra/chorium/runtime/ChoriumBootstrap;'
ACTIVITY = 'Lorg/umbra/chorium/runtime/ChoriumActivity;'

BOOTSTRAP = r'''
.class final Lorg/umbra/chorium/runtime/ChoriumBootstrap;
.super Ljava/lang/Object;
.source "ChoriumBootstrap.java"
.field private static final TAG:Ljava/lang/String; = "ChoriumBootstrap"
.field private static sAttached:Z
.field private static sBrowserProcess:Z
.field private static sServicesStarted:Z
.field private static sBrowserPrepared:Z

.method private constructor <init>()V
    .locals 0
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method static attach(Landroid/app/Application;)V
    .locals 7
    sget-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sAttached:Z
    if-nez v0, :done
    invoke-static {p0}, Lorg/chromium/base/ContextUtils;->initApplicationContext(Landroid/content/Context;)V
    invoke-virtual {p0}, Landroid/content/Context;->getPackageName()Ljava/lang/String;
    move-result-object v0
    invoke-static {}, Lorg/chromium/base/ContextUtils;->getProcessName()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/lang/String;->equals(Ljava/lang/Object;)Z
    move-result v0
    sput-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sBrowserProcess:Z
    invoke-static {}, Lorg/chromium/ui/base/ResourceBundle;->setNoAvailableLocalePaks()V
    invoke-static {}, Lorg/chromium/base/library_loader/LibraryLoader;->getInstance()Lorg/chromium/base/library_loader/LibraryLoader;
    move-result-object v0
    sget-boolean v1, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sBrowserProcess:Z
    if-eqz v1, :child_type
    const/4 v1, 0x1
    goto :set_type
    :child_type
    const/4 v1, 0x2
    :set_type
    invoke-virtual {v0, v1}, Lorg/chromium/base/library_loader/LibraryLoader;->setLibraryProcessType(I)V
    sget-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sBrowserProcess:Z
    if-eqz v0, :attached
    invoke-virtual {p0}, Landroid/content/Context;->getPackageName()Ljava/lang/String;
    move-result-object v0
    move-object v1, v0
    const/4 v2, 0x0
    const/4 v3, 0x2
    const/4 v4, 0x0
    const/4 v5, 0x0
    const/4 v6, 0x0
    invoke-static/range {v0 .. v6}, Lorg/chromium/content_public/browser/ChildProcessCreationParams;->set(Ljava/lang/String;Ljava/lang/String;ZIZZZ)V
    const-string v0, "chorium"
    invoke-static {v0}, Lorg/chromium/base/PathUtils;->setPrivateDataDirectorySuffix(Ljava/lang/String;)V
    invoke-static {p0}, Lorg/chromium/base/ApplicationStatus;->initialize(Landroid/app/Application;)V
    invoke-static {}, Lorg/chromium/base/CommandLine;->isInitialized()Z
    move-result v0
    if-nez v0, :command_line_ready
    const/4 v0, 0x0
    invoke-static {v0}, Lorg/chromium/base/CommandLine;->init([Ljava/lang/String;)V
    :command_line_ready
    invoke-static {}, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->configurePageFrameRate()V
    invoke-static {}, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->configureInstalledAppApi()V
    :attached
    const/4 v0, 0x1
    sput-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sAttached:Z
    :done
    return-void
.end method

.method static startServices()V
    .locals 2
    sget-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sAttached:Z
    if-nez v0, :attached
    new-instance v0, Ljava/lang/IllegalStateException;
    const-string v1, "Bootstrap not attached"
    invoke-direct {v0, v1}, Ljava/lang/IllegalStateException;-><init>(Ljava/lang/String;)V
    throw v0
    :attached
    sget-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sBrowserProcess:Z
    if-eqz v0, :done
    sget-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sServicesStarted:Z
    if-nez v0, :done
    invoke-static {}, Lorg/chromium/net/NetworkChangeNotifier;->init()Lorg/chromium/net/NetworkChangeNotifier;
    const/4 v0, 0x1
    invoke-static {v0}, Lorg/chromium/net/NetworkChangeNotifier;->setAutoDetectConnectivityState(Z)V
    sput-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sServicesStarted:Z
    :done
    return-void
.end method

.method static prepareBrowser(Landroid/app/Activity;)Z
    .locals 3
    sget-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sAttached:Z
    if-eqz v0, :invalid
    sget-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sBrowserProcess:Z
    if-eqz v0, :invalid
    sget-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sServicesStarted:Z
    if-eqz v0, :invalid
    sget-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sBrowserPrepared:Z
    if-nez v0, :ready
    :try_start
    invoke-static {p0}, Lorg/chromium/content_public/browser/DeviceUtils;->updateDeviceSpecificUserAgentSwitch(Landroid/content/Context;)V
    invoke-static {}, Lorg/chromium/base/library_loader/LibraryLoader;->getInstance()Lorg/chromium/base/library_loader/LibraryLoader;
    move-result-object v0
    invoke-virtual {v0}, Lorg/chromium/base/library_loader/LibraryLoader;->ensureInitialized()V
    :try_end
    .catch Ljava/lang/RuntimeException; {:try_start .. :try_end} :failed
    .catch Ljava/lang/LinkageError; {:try_start .. :try_end} :failed
    const/4 v0, 0x1
    sput-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sBrowserPrepared:Z
    :ready
    const/4 v0, 0x1
    return v0
    :failed
    move-exception v0
    const-string v1, "ChoriumBootstrap"
    const-string v2, "Native browser initialization failed"
    invoke-static {v1, v2, v0}, Lorg/chromium/base/Log;->e(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)V
    const/4 v0, 0x0
    return v0
    :invalid
    const-string v0, "ChoriumBootstrap"
    const-string v1, "Browser preparation called outside browser startup"
    invoke-static {v0, v1}, Lorg/chromium/base/Log;->e(Ljava/lang/String;Ljava/lang/String;)V
    const/4 v0, 0x0
    return v0
.end method

.method static startBrowser(Lorg/chromium/content_public/browser/BrowserStartupController$StartupCallback;)V
    .locals 6
    sget-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sBrowserPrepared:Z
    if-nez v0, :prepared
    new-instance v0, Ljava/lang/IllegalStateException;
    const-string v1, "Browser not prepared"
    invoke-direct {v0, v1}, Ljava/lang/IllegalStateException;-><init>(Ljava/lang/String;)V
    throw v0
    :prepared
    invoke-static {}, Lorg/chromium/content_public/browser/BrowserStartupController;->getInstance()Lorg/chromium/content_public/browser/BrowserStartupController;
    move-result-object v0
    const/4 v1, 0x1
    const/4 v2, 0x1
    const/4 v3, 0x0
    const/4 v4, 0x0
    move-object v5, p0
    invoke-interface/range {v0 .. v5}, Lorg/chromium/content_public/browser/BrowserStartupController;->startBrowserProcessesAsync(IZZZLorg/chromium/content_public/browser/BrowserStartupController$StartupCallback;)V
    return-void
.end method
'''

APPLICATION = r'''
.class public Lorg/umbra/chorium/runtime/ChoriumApplication;
.super Landroid/app/Application;
.source "ChoriumApplication.java"
.method public constructor <init>()V
    .locals 0
    invoke-direct {p0}, Landroid/app/Application;-><init>()V
    return-void
.end method
.method protected attachBaseContext(Landroid/content/Context;)V
    .locals 0
    invoke-super {p0, p1}, Landroid/app/Application;->attachBaseContext(Landroid/content/Context;)V
    invoke-static {p0}, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->attach(Landroid/app/Application;)V
    return-void
.end method
.method public onCreate()V
    .locals 0
    invoke-super {p0}, Landroid/app/Application;->onCreate()V
    invoke-static {}, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->startServices()V
    return-void
.end method
'''

OWNERSHIP = r'''
.method static claimRuntime(Lorg/umbra/chorium/runtime/ChoriumActivity;)V
    .locals 2
    sget-boolean v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sBrowserPrepared:Z
    if-nez v0, :prepared
    new-instance v0, Ljava/lang/IllegalStateException;
    const-string v1, "Browser not prepared"
    invoke-direct {v0, v1}, Ljava/lang/IllegalStateException;-><init>(Ljava/lang/String;)V
    throw v0
    :prepared
    sget-object v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sRuntimeOwner:Lorg/umbra/chorium/runtime/ChoriumActivity;
    if-eqz v0, :claim
    if-eq v0, p0, :claim
    invoke-virtual {v0}, Lorg/umbra/chorium/runtime/ChoriumActivity;->stopRuntime()V
    :claim
    sput-object p0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sRuntimeOwner:Lorg/umbra/chorium/runtime/ChoriumActivity;
    return-void
.end method
.method static releaseRuntime(Lorg/umbra/chorium/runtime/ChoriumActivity;)V
    .locals 1
    sget-object v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sRuntimeOwner:Lorg/umbra/chorium/runtime/ChoriumActivity;
    if-ne v0, p0, :done
    const/4 v0, 0x0
    sput-object v0, Lorg/umbra/chorium/runtime/ChoriumBootstrap;->sRuntimeOwner:Lorg/umbra/chorium/runtime/ChoriumActivity;
    invoke-virtual {p0}, Lorg/umbra/chorium/runtime/ChoriumActivity;->stopRuntime()V
    :done
    return-void
.end method
'''

def patch_runtime_ownership(decoded):
    bootstraps = list(decoded.glob('smali*/org/umbra/chorium/runtime/ChoriumBootstrap.smali'))
    activities = list(decoded.glob('smali*/org/umbra/chorium/runtime/ChoriumActivity.smali'))
    if len(bootstraps) != 1 or len(activities) != 1:
        raise ValueError('expected one bootstrap and Activity')
    bootstrap, activity = bootstraps[0], activities[0]
    boot, text = bootstrap.read_text(encoding='utf-8'), activity.read_text(encoding='utf-8')
    if 'sRuntimeOwner' in boot or 'stopRuntime()V' in text:
        raise ValueError('runtime ownership already patched')
    start = '.method private startApp()V\n    .locals 11'
    if text.count(start) != 1:
        raise ValueError('startApp register layout changed')
    entry = f'''
    invoke-virtual {{p0}}, Landroid/app/Activity;->isFinishing()Z
    move-result v0
    if-nez v0, :bootstrap_cancelled
    invoke-virtual {{p0}}, Landroid/app/Activity;->isDestroyed()Z
    move-result v0
    if-eqz v0, :bootstrap_claim
    :bootstrap_cancelled
    return-void
    :bootstrap_claim
    invoke-static {{p0}}, {BOOT}->claimRuntime({ACTIVITY})V
'''
    text = text.replace(start, start+'\n'+entry)
    destroy = re.search(r'\.method protected onDestroy\(\)V.*?\.end method', text, re.S)
    if not destroy:
        raise ValueError('missing Activity destructor')
    method = destroy.group()
    teardown = re.search(r'    iget-boolean v0, p0, '+re.escape(ACTIVITY)+r'->mRunning:Z.*?    :cond_0', method, re.S)
    if not teardown or '->stop()V' not in teardown.group():
        raise ValueError('Activity runtime teardown changed')
    body = teardown.group()
    # Preserve the original native stop and mRunning guard in stopRuntime.
    stop = '\n.method stopRuntime()V\n    .locals 1\n'+body+'\n    return-void\n.end method\n'
    replacement = f'    invoke-static {{p0}}, {BOOT}->releaseRuntime({ACTIVITY})V\n\n    :cond_0'
    method = method.replace(body, replacement)
    text = text[:destroy.start()]+method+text[destroy.end():]+stop
    field = '.field private static sRuntimeOwner:Lorg/umbra/chorium/runtime/ChoriumActivity;'
    boot = boot.replace('.field private static sBrowserPrepared:Z', '.field private static sBrowserPrepared:Z\n'+field)+OWNERSHIP
    bootstrap.write_text(boot, encoding='utf-8')
    activity.write_text(text, encoding='utf-8')
    print('Patched Activity runtime ownership and cancelled-start guard')

def patch_retired_views(decoded):
    views = list(decoded.glob('smali*/org/umbra/chorium/runtime/AppView.smali'))
    activities = list(decoded.glob('smali*/org/umbra/chorium/runtime/ChoriumActivity.smali'))
    if len(views) != 1 or len(activities) != 1:
        raise ValueError('expected one AppView and Activity')
    view, activity = views[0], activities[0]
    vtext, text = view.read_text(encoding='utf-8'), activity.read_text(encoding='utf-8')
    if 'setStopped()V' in vtext:
        raise ValueError('views already retired')
    match = re.search(r'\.method stopRuntime\(\)V.*?\.end method', text, re.S)
    if not match:
        raise ValueError('runtime ownership fix required first')
    method = match.group()
    anchor = '    invoke-static {}, Lorg/umbra/chorium/runtime/ChoriumNativesJni;->get()Lorg/umbra/chorium/runtime/ChoriumNatives$Natives;'
    if method.count(anchor) != 1:
        raise ValueError('native stop anchor changed')
    before = r'''    iget-object v0, p0, Lorg/umbra/chorium/runtime/ChoriumActivity;->mAppView:Lorg/umbra/chorium/runtime/AppView;
    invoke-virtual {v0}, Lorg/umbra/chorium/runtime/AppView;->setStopped()V
    iget-object v0, p0, Lorg/umbra/chorium/runtime/ChoriumActivity;->mPageHost:Lorg/umbra/chorium/engine/PageHost;
    if-eqz v0, :bootstrap_no_host
    invoke-virtual {v0}, Lorg/umbra/chorium/engine/PageHost;->destroy()V
    const/4 v0, 0x0
    iput-object v0, p0, Lorg/umbra/chorium/runtime/ChoriumActivity;->mPageHost:Lorg/umbra/chorium/engine/PageHost;
    :bootstrap_no_host
'''
    text = text[:match.start()]+method.replace(anchor, before+anchor)+text[match.end():]
    vtext += r'''
.method setStopped()V
    .locals 1
    const/4 v0, 0x0
    iput-boolean v0, p0, Lorg/umbra/chorium/runtime/AppView;->mStarted:Z
    iput-boolean v0, p0, Lorg/umbra/chorium/runtime/AppView;->mFramePending:Z
    return-void
.end method
'''
    view.write_text(vtext, encoding='utf-8')
    activity.write_text(text, encoding='utf-8')
    print('Patched view retirement before WebContents teardown')

def patch(decoded):
    def find(name):
        paths = list(decoded.glob('smali*/org/umbra/chorium/runtime/'+name+'.smali'))
        if len(paths) != 1:
            raise ValueError('expected exactly one '+name)
        return paths[0]
    application, activity = find('ChoriumApplication'), find('ChoriumActivity')
    bootstrap = application.with_name('ChoriumBootstrap.smali')
    if bootstrap.exists():
        raise ValueError('bootstrap already present')
    app_text, text = application.read_text(encoding='utf-8'), activity.read_text(encoding='utf-8')
    if 'configureRendererServices()V' not in app_text:
        raise ValueError('renderer fix is required first')
    for name in ['configurePageFrameRate', 'configureInstalledAppApi']:
        pattern = r'\.method private static '+name+r'\(\)V.*?\.end method'
        text, count = re.subn(pattern, '', text, flags=re.S)
        if count != 1:
            raise ValueError('missing or unexpected compatibility method: '+name)
    pattern = (r'    invoke-static \{\}, Lorg/chromium/base/CommandLine;->isInitialized\(\)Z.*?'
               r'    invoke-virtual \{v0\}, Lorg/chromium/base/library_loader/LibraryLoader;->ensureInitialized\(\)V')
    replacement = f'''    invoke-static {{p0}}, {BOOT}->prepareBrowser(Landroid/app/Activity;)Z
    move-result v0
    if-nez v0, :bootstrap_ready
    const-string v0, "The engine didn't start"
    const/4 v1, 0x1
    invoke-static {{p0, v0, v1}}, Landroid/widget/Toast;->makeText(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;
    move-result-object v0
    invoke-virtual {{v0}}, Landroid/widget/Toast;->show()V
    invoke-virtual {{p0}}, Landroid/app/Activity;->finish()V
    return-void
    :bootstrap_ready
    const/4 v1, 0x0'''
    text, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise ValueError('Activity native startup anchor changed')
    pattern = (r'    invoke-static \{\}, Lorg/chromium/content_public/browser/BrowserStartupController;->getInstance\(\)Lorg/chromium/content_public/browser/BrowserStartupController;.*?'
               r'    invoke-interface/range \{v2 \.\. v7\}, Lorg/chromium/content_public/browser/BrowserStartupController;->startBrowserProcessesAsync\(IZZZLorg/chromium/content_public/browser/BrowserStartupController\$StartupCallback;\)V')
    replacement = f'''    new-instance v7, Lorg/umbra/chorium/runtime/ChoriumActivity$1;
    invoke-direct {{v7, p0}}, Lorg/umbra/chorium/runtime/ChoriumActivity$1;-><init>({ACTIVITY})V
    invoke-static {{v7}}, {BOOT}->startBrowser(Lorg/chromium/content_public/browser/BrowserStartupController$StartupCallback;)V'''
    text, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise ValueError('Activity asynchronous startup anchor changed')
    # Keep the already-verified switch merge implementation; equivalent to
    # the generic disableFeature helper in the Java source.
    flags = METHOD + (METHOD.replace('configurePageFrameRate', 'configureInstalledAppApi')
                     .replace('disable-features', 'disable-blink-features')
                     .replace('ThrottleMainFrameTo60Hz', 'InstalledApp'))
    bootstrap.write_text(BOOTSTRAP+flags, encoding='utf-8')
    application.write_text(APPLICATION, encoding='utf-8')
    activity.write_text(text, encoding='utf-8')
    patch_runtime_ownership(decoded)
    patch_retired_views(decoded)
    print('Patched Application, Activity and process-aware bootstrap')

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('decoded', type=Path)
    args = ap.parse_args()
    try:
        patch(args.decoded)
    except ValueError as error:
        ap.error(str(error))
