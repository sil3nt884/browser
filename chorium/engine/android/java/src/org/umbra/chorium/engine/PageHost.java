// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

package org.umbra.chorium.engine;

import android.content.Context;
import android.database.Cursor;
import android.graphics.Rect;
import android.net.Uri;
import android.provider.MediaStore;
import android.view.ActionMode;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.webkit.MimeTypeMap;
import android.widget.FrameLayout;

import org.chromium.base.ThreadUtils;
import org.chromium.base.ContextUtils;
import org.chromium.components.download.DownloadCollectionBridge;
import org.chromium.components.embedder_support.view.ContentView;
import org.chromium.components.embedder_support.view.ContentViewRenderView;
import org.chromium.content_public.browser.ActionModeCallback;
import org.chromium.content_public.browser.ActionModeCallbackHelper;
import org.chromium.content_public.browser.SelectionMenuItem;
import org.chromium.content_public.browser.SelectionPopupController;
import org.chromium.content_public.browser.Visibility;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.OverscrollAction;
import org.chromium.ui.OverscrollActivationStatus;
import org.chromium.ui.OverscrollRefreshHandler;
import org.chromium.ui.base.BackGestureEventSwipeEdge;
import org.chromium.ui.base.ViewAndroidDelegate;
import org.chromium.ui.base.WindowAndroid;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.NativeMethods;

import java.util.HashMap;
import java.util.Map;
import java.util.Locale;

/**
 * Where Chorium's pages show on Android. Each page is a ContentView (real
 * touch, scrolling, zoom and keyboard); one ContentViewRenderView draws the
 * page that is showing. The runtime adds this view to its layout and the
 * engine places it where the app draws the page (chorium_android_show_page).
 */
@JNINamespace("chorium::android")
public class PageHost extends FrameLayout {
    /** Worker thread: reserve a pending file in the shared Downloads collection. */
    @CalledByNative
    private static String[] createDownloadTarget(String name, String mime, String url) {
        if (mime.isEmpty() || mime.equals("application/octet-stream")) {
            int dot = name.lastIndexOf('.');
            String inferred = dot < 0 ? null : MimeTypeMap.getSingleton()
                    .getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase(Locale.ROOT));
            if (inferred != null) mime = inferred;
        }
        String uri = DownloadCollectionBridge.createIntermediateUriForPublish(name, mime, url, "");
        if (uri == null) return new String[] {"", ""};
        try (Cursor cursor = ContextUtils.getApplicationContext().getContentResolver().query(
                Uri.parse(uri), new String[] {MediaStore.MediaColumns.DISPLAY_NAME},
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) name = cursor.getString(0);
        }
        return new String[] {uri, name};
    }

    private static PageHost sInstance;

    private final WindowAndroid mWindow;
    private final ContentViewRenderView mRenderView;
    private final Map<Long, Page> mPages = new HashMap<>();
    private Page mShown;
    private boolean mActivityVisible = true;

    private static class Page {
        final WebContents webContents;
        final ContentView view;

        Page(WebContents webContents, ContentView view) {
            this.webContents = webContents;
            this.view = view;
        }
    }

    // chorium.wit's overscroll-kind and overscroll-phase.
    private static final int KIND_REFRESH = 0;
    private static final int KIND_BACK = 1;
    private static final int KIND_FORWARD = 2;
    private static final int PHASE_START = 0;
    private static final int PHASE_PULL = 1;
    private static final int PHASE_RELEASE = 2;
    private static final int PHASE_CANCEL = 3;

    /**
     * Pull to refresh and swipe back/forward, as Chromium reports them: passed to the app (as
     * "overscroll" events), which draws the indicator and acts on release.
     */
    private static class Overscroll implements OverscrollRefreshHandler {
        private final long mKey;
        private final WebContents mWebContents;
        private final float mDensity;
        private int mKind = -1;

        Overscroll(long key, WebContents webContents, float density) {
            mKey = key;
            mWebContents = webContents;
            mDensity = density;
        }

        @Override
        public boolean start(@OverscrollAction int type, @BackGestureEventSwipeEdge int edge) {
            if (type == OverscrollAction.PULL_TO_REFRESH) {
                mKind = KIND_REFRESH;
            } else if (type == OverscrollAction.HISTORY_NAVIGATION) {
                boolean back = edge == BackGestureEventSwipeEdge.LEFT;
                boolean can = back ? mWebContents.getNavigationController().canGoBack()
                                   : mWebContents.getNavigationController().canGoForward();
                if (!can) return false;
                mKind = back ? KIND_BACK : KIND_FORWARD;
            } else {
                return false;
            }
            send(PHASE_START, 0, 0);
            return true;
        }

        @Override
        public void pull(float xDelta, float yDelta) {
            if (mKind >= 0) send(PHASE_PULL, xDelta / mDensity, yDelta / mDensity);
        }

        @Override
        public void release(@OverscrollActivationStatus int status) {
            if (mKind < 0) return;
            boolean act = status == OverscrollActivationStatus.ALLOW_ACTIVATION
                    || status == OverscrollActivationStatus.FORCE_ACTIVATION;
            send(act ? PHASE_RELEASE : PHASE_CANCEL, 0, 0);
            mKind = -1;
        }

        @Override
        public void reset() {
            if (mKind < 0) return;
            send(PHASE_CANCEL, 0, 0);
            mKind = -1;
        }

        @Override
        public void setEnabled(boolean enabled) {}

        private void send(int phase, float dx, float dy) {
            PageHostJni.get().overscroll(mKey, mKind, phase, dx, dy);
        }
    }

    /** Call once the native library is loaded; there is one per process. */
    public PageHost(Context context, WindowAndroid window) {
        super(context);
        mWindow = window;
        mRenderView = new ContentViewRenderView(context);
        mRenderView.onNativeLibraryLoaded(window);
        addView(mRenderView, new LayoutParams(LayoutParams.MATCH_PARENT,
                LayoutParams.MATCH_PARENT));
        setVisibility(View.INVISIBLE);
        sInstance = this;
    }

    public void destroy() {
        mRenderView.destroy();
        if (sInstance == this) sInstance = null;
    }

    @CalledByNative
    private static void addPage(long key, WebContents webContents) {
        ThreadUtils.assertOnUiThread();
        PageHost host = sInstance;
        if (host == null) return;
        ContentView view = ContentView.createContentView(host.getContext(), webContents);
        webContents.setDelegates("", ViewAndroidDelegate.createBasicDelegate(view), view,
                host.mWindow, WebContents.createDefaultInternalsHolder());
        webContents.updateWebContentsVisibility(Visibility.HIDDEN);
        // Text selection: handles, and the copy/paste bar. Without this,
        // touching a text field crashes (no drawable for the caret handle).
        SelectionPopupController.fromWebContents(webContents)
                .setActionModeCallback(defaultActionModeCallback(webContents));
        webContents.setOverscrollRefreshHandler(new Overscroll(
                key, webContents, host.getResources().getDisplayMetrics().density));
        host.mPages.put(key, new Page(webContents, view));
    }

    /** The system's copy/paste bar, as content shell sets it up. */
    private static ActionModeCallback defaultActionModeCallback(WebContents webContents) {
        final ActionModeCallbackHelper helper =
                SelectionPopupController.fromWebContents(webContents)
                        .getActionModeCallbackHelper();
        return new ActionModeCallback() {
            @Override
            public boolean onCreateActionMode(ActionMode mode, Menu menu) {
                helper.onCreateActionMode(mode, menu);
                return true;
            }

            @Override
            public boolean onPrepareActionMode(ActionMode mode, Menu menu) {
                return helper.onPrepareActionMode(mode, menu);
            }

            @Override
            public boolean onActionItemClicked(ActionMode mode, MenuItem item) {
                return helper.onActionItemClicked(mode, item);
            }

            @Override
            public boolean onDropdownItemClicked(SelectionMenuItem item, boolean closeMenu) {
                return helper.onDropdownItemClicked(item, closeMenu);
            }

            @Override
            public void onDestroyActionMode(ActionMode mode) {
                helper.onDestroyActionMode();
            }

            @Override
            public void onGetContentRect(ActionMode mode, View view, Rect outRect) {
                helper.onGetContentRect(mode, view, outRect);
            }
        };
    }

    @CalledByNative
    private static void removePage(long key) {
        PageHost host = sInstance;
        if (host == null) return;
        Page page = host.mPages.remove(key);
        // Its handle is freed next; nothing may call back with |key| after.
        if (page != null) page.webContents.setOverscrollRefreshHandler(null);
        if (page != null && page == host.mShown) {
            host.removeView(page.view);
            host.mShown = null;
            host.setVisibility(View.INVISIBLE);
        }
    }

    @CalledByNative
    private static void showPage(long key, int x, int y, int width, int height) {
        PageHost host = sInstance;
        if (host == null) return;
        Page page = host.mPages.get(key);
        if (page == null) return;
        host.place(x, y, width, height);
        if (page != host.mShown) {
            if (host.mShown != null) {
                host.removeView(host.mShown.view);
                host.mShown.webContents.updateWebContentsVisibility(Visibility.HIDDEN);
            }
            host.addView(page.view, new LayoutParams(LayoutParams.MATCH_PARENT,
                    LayoutParams.MATCH_PARENT));
            host.mRenderView.setCurrentWebContents(page.webContents);
            page.webContents.updateWebContentsVisibility(
                    host.mActivityVisible ? Visibility.VISIBLE : Visibility.HIDDEN);
            host.mShown = page;
        }
        host.setVisibility(View.VISIBLE);
    }

    /** The activity started or stopped: the shown page renders only while it's seen. */
    public void setActivityVisible(boolean visible) {
        mActivityVisible = visible;
        if (mShown != null) {
            mShown.webContents.updateWebContentsVisibility(
                    visible ? Visibility.VISIBLE : Visibility.HIDDEN);
        }
    }

    /** Some video (protected video especially) plays in an overlay under the page. */
    @CalledByNative
    private static void setOverlayMode(boolean useOverlayMode) {
        PageHost host = sInstance;
        if (host != null) host.mRenderView.setOverlayVideoMode(useOverlayMode);
    }

    @CalledByNative
    private static void hidePages() {
        PageHost host = sInstance;
        if (host != null) host.setVisibility(View.INVISIBLE);
    }

    /** Moves this view to the rect (pixels of its parent) without relayout churn. */
    private void place(int x, int y, int width, int height) {
        FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) getLayoutParams();
        if (params == null) return;
        if (params.leftMargin == x && params.topMargin == y && params.width == width
                && params.height == height) {
            return;
        }
        params.leftMargin = x;
        params.topMargin = y;
        params.width = width;
        params.height = height;
        setLayoutParams(params);
    }

    @NativeMethods
    interface Natives {
        void overscroll(long key, int kind, int phase, float dx, float dy);
    }
}
